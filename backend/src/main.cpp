#include "aidoku_client.hpp"
#include "aidoku_db.hpp"
#include "appload_ipc.hpp"
#include "cbz_extractor.hpp"
#include "json.hpp"
#include "mangadex_client.hpp"
#include "state_store.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <filesystem>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Frontend -> Backend request message types
constexpr int32_t REQ_BOOTSTRAP = 1;
constexpr int32_t REQ_REFRESH_LIBRARY = 2;
constexpr int32_t REQ_SEARCH = 3;
constexpr int32_t REQ_MANGA_DETAILS = 4;
constexpr int32_t REQ_TOGGLE_LIBRARY = 5;
constexpr int32_t REQ_OPEN_CHAPTER = 6;
constexpr int32_t REQ_SAVE_PROGRESS = 7;
constexpr int32_t REQ_MARK_CHAPTER_READ = 8;
constexpr int32_t REQ_DOWNLOAD_CHAPTERS = 9;
constexpr int32_t REQ_CANCEL_DOWNLOAD = 10;
constexpr int32_t REQ_DELETE_DOWNLOAD = 11;
constexpr int32_t REQ_LIST_SOURCES = 12;
constexpr int32_t REQ_INSTALL_SOURCE = 13;
constexpr int32_t REQ_UPDATE_SETTINGS = 14;
constexpr int32_t REQ_SAVE_SOURCE_CONFIG = 15;

// Backend -> Frontend event message types
constexpr int32_t EVT_BOOTSTRAP = 100;
constexpr int32_t EVT_LIBRARY_UPDATED = 101;
constexpr int32_t EVT_SEARCH_RESULTS = 102;
constexpr int32_t EVT_MANGA_DETAILS = 103;
constexpr int32_t EVT_READER_LOADING = 104;
constexpr int32_t EVT_CHAPTER_PAGES = 105;
constexpr int32_t EVT_DOWNLOADS_UPDATED = 106;
constexpr int32_t EVT_SOURCES_LIST = 107;
constexpr int32_t EVT_COVER_UPDATED = 108;
constexpr int32_t EVT_TOAST = 109;

struct DownloadTask {
    std::string key;
    std::string sourceId;
    std::string sourceName;
    std::string mangaId;
    std::string mangaTitle;
    std::string chapterId;
    double chapterNum = 0.0;
    bool hasChapterNum = false;
    std::string chapterTitle;
    std::string status = "Queued";
    int progressPct = 0;
};

struct CoverTask {
    std::string sourceId;
    std::string mangaId;
    std::string title;
    std::string coverUrl;
};

class Rm2MangaApp {
    appload::IpcConnection ipc_;
    store::StateStore state_;
    aidoku::Client aidoku_;
    aidoku::CoverDb coverDb_;
    mangadex::Client mangadex_;
    extractor::PageCache pageCache_;

    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> shutdownJoined_{false};
    std::atomic<uint64_t> searchSeq_{0};
    std::atomic<uint64_t> readerSeq_{0};

    // Short-lived discover cache so switching installed source pills is instant
    std::mutex discoverMu_;
    Json::Value discoverCache_ = Json::Value::array();
    int64_t discoverCacheTs_ = 0;

    // Pinned chapter slugs in /tmp/rm2manga_pages (current + preloaded next)
    std::mutex pinMu_;
    std::string activeChapterSlug_;
    std::string preloadedChapterSlug_;
    std::set<std::string> preloadingKeys_;

    // Download queue
    std::mutex dlMu_;
    std::condition_variable dlCv_;
    std::deque<DownloadTask> dlQueue_;
    std::unique_ptr<DownloadTask> activeDl_;
    std::atomic<bool> cancelActiveDl_{false};
    std::thread dlWorker_;

    // Cover art queue
    std::mutex coverMu_;
    std::condition_variable coverCv_;
    std::deque<CoverTask> coverQueue_;
    std::set<std::string> attemptedCovers_;
    std::thread coverWorker_;

    // Track request worker threads for deterministic join on teardown
    struct WorkerSlot {
        std::thread th;
        std::atomic<bool> done{false};
    };
    std::mutex asyncMu_;
    std::list<WorkerSlot> workers_;
    bool disableCovers_ = (std::getenv("RM2MANGA_DISABLE_COVERS") != nullptr);

public:
    explicit Rm2MangaApp(const fs::path& workDir)
        : state_(workDir / "data"),
          aidoku_(workDir / "data" / "aidoku"),
          coverDb_(fs::path(aidoku_.homeDir()) / "database.db"),
          pageCache_(std::getenv("RM2MANGA_PAGE_CACHE_DIR")
                         ? std::getenv("RM2MANGA_PAGE_CACHE_DIR")
                         : "/tmp/rm2manga_pages") {}

    ~Rm2MangaApp() {
        shutdown();
    }

    bool start(const std::string& socketPath) {
        if (!ipc_.connectTo(socketPath)) {
            return false;
        }
        dlWorker_ = std::thread([this]() { downloadLoop(); });
        coverWorker_ = std::thread([this]() { coverLoop(); });
        return true;
    }

    void run() {
        ipc_.runLoop([this](int32_t msgType, const std::string& contents) {
            handleIncoming(msgType, contents);
        });
        shutdown();
    }

    void shutdown() {
        shuttingDown_ = true;
        bool expected = false;
        if (!shutdownJoined_.compare_exchange_strong(expected, true)) return;

        cancelActiveDl_ = true;
        dlCv_.notify_all();
        coverCv_.notify_all();
        ipc_.closeSocket();

        if (dlWorker_.joinable()) dlWorker_.join();
        if (coverWorker_.joinable()) coverWorker_.join();

        while (true) {
            std::list<WorkerSlot> batch;
            {
                std::lock_guard<std::mutex> lock(asyncMu_);
                if (workers_.empty()) break;
                batch.splice(batch.end(), workers_);
            }
            for (auto& w : batch) {
                if (w.th.joinable()) {
                    w.th.join();
                }
            }
        }
    }

private:
    void spawnAsync(std::function<void()> fn) {
        if (shuttingDown_.load()) return;

        std::list<WorkerSlot> finished;
        {
            std::lock_guard<std::mutex> lock(asyncMu_);
            if (shuttingDown_.load()) return;

            for (auto it = workers_.begin(); it != workers_.end();) {
                if (it->done.load() && it->th.get_id() != std::this_thread::get_id()) {
                    auto cur = it++;
                    finished.splice(finished.end(), workers_, cur);
                } else {
                    ++it;
                }
            }

            workers_.emplace_back();
            WorkerSlot& slot = workers_.back();
            slot.th = std::thread([this, &slot, fn = std::move(fn)]() {
                if (!shuttingDown_.load()) {
                    fn();
                }
                slot.done.store(true);
            });
        }

        for (auto& w : finished) {
            if (w.th.joinable()) {
                w.th.join();
            }
        }
    }

    void sendToast(const std::string& message, bool isError = false) {
        Json::Value ev = Json::Value::object();
        ev["message"] = message;
        ev["is_error"] = isError;
        ipc_.sendJson(EVT_TOAST, ev);
    }

    void invalidateDiscoverCache() {
        std::lock_guard<std::mutex> lock(discoverMu_);
        discoverCache_ = Json::Value::array();
        discoverCacheTs_ = 0;
    }

    void enqueueCoverFetch(const std::string& sourceId,
                           const std::string& mangaId,
                           const std::string& title,
                           const std::string& coverUrl = "") {
        if (disableCovers_) return;
        std::string mkey = store::makeMangaKey(sourceId, mangaId);
        Json::Value existing = state_.getManga(sourceId, mangaId);
        if (existing.isObject() && !existing["cover_path"].str().empty()) {
            std::string existingPath = existing["cover_path"].str();
            if (existingPath.rfind("file://", 0) == 0) {
                std::error_code ec;
                if (fs::exists(existingPath.substr(7), ec)) return;
            }
        }

        // The Aidoku runtime keeps each source's own cover URL in its SQLite DB
        std::string url = coverUrl;
        if (url.empty() && existing.isObject()) url = existing["cover_url"].str();
        if (url.empty()) url = coverDb_.lookupCoverUrl(sourceId, mangaId);
        if (url.empty() && title.empty()) return;

        std::lock_guard<std::mutex> lock(coverMu_);
        if (attemptedCovers_.count(mkey) > 0) return;
        attemptedCovers_.insert(mkey);
        coverQueue_.push_back(CoverTask{sourceId, mangaId, title, url});
        coverCv_.notify_one();
    }

    Json::Value buildInstalledSourcesList(bool* aidokuOnlineOut = nullptr) {
        Json::Value list = Json::Value::array();

        bool online = aidoku_.ensureRunning();
        if (aidokuOnlineOut) *aidokuOnlineOut = online;
        if (online) {
            std::string err;
            Json::Value installed = aidoku_.listInstalledSources(&err);
            if (installed.isArray()) {
                for (const auto& s : installed.asArray()) {
                    Json::Value copy = s;
                    copy["builtin"] = false;
                    list.push(std::move(copy));
                }
            }
        }
        return list;
    }

    void syncAidokuIntoStore(bool refreshRemoteChapters = false) {
        if (!aidoku_.ensureRunning()) return;

        std::string err;
        Json::Value lib = aidoku_.getLibrary(&err);
        if (err.empty() && lib.isArray()) {
            state_.syncAidokuLibraryList(lib);

            for (const auto& item : lib.asArray()) {
                std::string sid = item["source"]["id"].str();
                std::string sname = item["source"]["name"].str(sid);
                std::string mid = item["id"].str();
                std::string mtitle = item["title"].str(mid);
                if (sid.empty() || mid.empty()) continue;

                if (refreshRemoteChapters) {
                    aidoku_.refreshChapters(sid, mid, nullptr);
                }

                std::string chErr;
                Json::Value chs = aidoku_.listCachedChapters(sid, mid, &chErr);
                if (chErr.empty() && chs.isArray()) {
                    state_.mergeAndCacheChapters(sid, sname, mid, mtitle, chs);
                }
                enqueueCoverFetch(sid, mid, mtitle, "");
            }
        }
    }

    Json::Value serializeDownloadQueue() {
        std::lock_guard<std::mutex> lock(dlMu_);
        Json::Value arr = Json::Value::array();
        if (activeDl_) {
            Json::Value item = Json::Value::object();
            item["key"] = activeDl_->key;
            item["source_id"] = activeDl_->sourceId;
            item["manga_id"] = activeDl_->mangaId;
            item["manga_title"] = activeDl_->mangaTitle;
            item["chapter_id"] = activeDl_->chapterId;
            if (activeDl_->hasChapterNum) item["chapter_num"] = activeDl_->chapterNum;
            item["chapter_title"] = activeDl_->chapterTitle;
            item["status"] = activeDl_->status;
            item["progress_pct"] = activeDl_->progressPct;
            item["active"] = true;
            arr.push(std::move(item));
        }
        for (const auto& q : dlQueue_) {
            Json::Value item = Json::Value::object();
            item["key"] = q.key;
            item["source_id"] = q.sourceId;
            item["manga_id"] = q.mangaId;
            item["manga_title"] = q.mangaTitle;
            item["chapter_id"] = q.chapterId;
            if (q.hasChapterNum) item["chapter_num"] = q.chapterNum;
            item["chapter_title"] = q.chapterTitle;
            item["status"] = "Queued";
            item["progress_pct"] = 0;
            item["active"] = false;
            arr.push(std::move(item));
        }
        return arr;
    }

    void pushDownloadsEvent() {
        Json::Value ev = Json::Value::object();
        ev["queue"] = serializeDownloadQueue();
        ev["downloads"] = state_.listDownloads();
        uint64_t bytes = util::directorySizeBytes(pageCache_.root()) +
                         util::directorySizeBytes(state_.localChaptersDir()) +
                         util::directorySizeBytes(fs::path(aidoku_.homeDir()) / "downloads");
        ev["cache_bytes"] = static_cast<int64_t>(bytes);
        ipc_.sendJson(EVT_DOWNLOADS_UPDATED, ev);
    }

    void pushLibraryEvent() {
        Json::Value ev = Json::Value::object();
        ev["library"] = state_.listLibrary();
        ev["continue_reading"] = state_.getContinueReading();
        ev["recent"] = state_.listRecent();
        ipc_.sendJson(EVT_LIBRARY_UPDATED, ev);
    }

    void pushBootstrap() {
        // Send immediate local snapshot first so UI paints in <30ms
        {
            Json::Value fast = Json::Value::object();
            fast["library"] = state_.listLibrary();
            fast["continue_reading"] = state_.getContinueReading();
            fast["recent"] = state_.listRecent();
            fast["downloads"] = state_.listDownloads();
            fast["queue"] = serializeDownloadQueue();
            fast["settings"] = state_.getSettings();
            fast["installed_sources"] = Json::Value::array();
            fast["source_config"] = aidoku_.getSourceConfig();
            fast["aidoku_online"] = false;
            ipc_.sendJson(EVT_BOOTSTRAP, fast);
        }

        // Now sync Aidoku library & installed sources and push enriched state
        bool aidokuOnline = false;
        Json::Value sources = buildInstalledSourcesList(&aidokuOnline);
        if (aidokuOnline) {
            syncAidokuIntoStore(false);
        }

        for (const auto& m : state_.listMangasMissingCovers()) {
            enqueueCoverFetch(m["source_id"].str(), m["manga_id"].str(),
                              m["title"].str(), m["cover_url"].str());
        }

        Json::Value full = Json::Value::object();
        full["library"] = state_.listLibrary();
        full["continue_reading"] = state_.getContinueReading();
        full["recent"] = state_.listRecent();
        full["downloads"] = state_.listDownloads();
        full["queue"] = serializeDownloadQueue();
        full["settings"] = state_.getSettings();
        full["installed_sources"] = std::move(sources);
        full["source_config"] = aidoku_.getSourceConfig();
        full["aidoku_online"] = aidokuOnline;
        ipc_.sendJson(EVT_BOOTSTRAP, full);
    }

    // Chronological ascending sort + deduplicated neighbor selection
    static std::vector<Json::Value> sortChaptersChronological(const Json::Value& chaptersArr) {
        std::vector<Json::Value> vec = chaptersArr.asArray();
        bool anyNum = false;
        for (const auto& c : vec) {
            if (c.has("chapter_num")) {
                anyNum = true;
                break;
            }
        }
        if (!anyNum) {
            return vec;
        }
        std::stable_sort(vec.begin(), vec.end(), [](const Json::Value& a, const Json::Value& b) {
            double na = a.has("chapter_num") ? a["chapter_num"].num(0) : -1.0;
            double nb = b.has("chapter_num") ? b["chapter_num"].num(0) : -1.0;
            if (std::fabs(na - nb) > 1e-4) return na < nb;
            double va = a.has("volume_num") ? a["volume_num"].num(0) : 0.0;
            double vb = b.has("volume_num") ? b["volume_num"].num(0) : 0.0;
            return va < vb;
        });
        return vec;
    }

    static void findPrevNextChapter(const Json::Value& chaptersArr,
                                    const std::string& currentChapterId,
                                    Json::Value& prevOut,
                                    Json::Value& nextOut) {
        prevOut = Json::Value();
        nextOut = Json::Value();
        if (!chaptersArr.isArray() || chaptersArr.empty()) return;

        std::vector<Json::Value> sorted = sortChaptersChronological(chaptersArr);
        int curIdx = -1;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i]["id"].str() == currentChapterId) {
                curIdx = static_cast<int>(i);
                break;
            }
        }
        if (curIdx < 0) return;

        const Json::Value& cur = sorted[static_cast<size_t>(curIdx)];
        bool hasNum = cur.has("chapter_num");
        double curNum = cur["chapter_num"].num(0.0);
        std::string curScan = cur["scanlator"].str();

        if (!hasNum) {
            if (curIdx > 0) prevOut = sorted[static_cast<size_t>(curIdx - 1)];
            if (curIdx + 1 < static_cast<int>(sorted.size())) nextOut = sorted[static_cast<size_t>(curIdx + 1)];
            return;
        }

        // Next chapter: find smallest chapter_num strictly greater than curNum
        for (size_t i = static_cast<size_t>(curIdx + 1); i < sorted.size(); ++i) {
            if (!sorted[i].has("chapter_num")) {
                nextOut = sorted[i];
                break;
            }
            double n = sorted[i]["chapter_num"].num(0.0);
            if (n > curNum + 0.0005) {
                // Collect candidates with this same target chapter_num and prefer downloaded or matching scanlator
                double targetNum = n;
                const Json::Value* best = &sorted[i];
                for (size_t j = i; j < sorted.size(); ++j) {
                    double nj = sorted[j]["chapter_num"].num(0.0);
                    if (std::fabs(nj - targetNum) > 0.0005) break;
                    if (sorted[j]["downloaded"].boolean(false)) {
                        best = &sorted[j];
                        break;
                    }
                    if (!curScan.empty() && sorted[j]["scanlator"].str() == curScan) {
                        best = &sorted[j];
                    }
                }
                nextOut = *best;
                break;
            }
        }

        // Previous chapter: find largest chapter_num strictly smaller than curNum
        for (int i = curIdx - 1; i >= 0; --i) {
            const auto& cand = sorted[static_cast<size_t>(i)];
            if (!cand.has("chapter_num")) {
                prevOut = cand;
                break;
            }
            double n = cand["chapter_num"].num(0.0);
            if (n < curNum - 0.0005) {
                double targetNum = n;
                const Json::Value* best = &cand;
                for (int j = i; j >= 0; --j) {
                    double nj = sorted[static_cast<size_t>(j)]["chapter_num"].num(0.0);
                    if (std::fabs(nj - targetNum) > 0.0005) break;
                    if (sorted[static_cast<size_t>(j)]["downloaded"].boolean(false)) {
                        best = &sorted[static_cast<size_t>(j)];
                        break;
                    }
                    if (!curScan.empty() && sorted[static_cast<size_t>(j)]["scanlator"].str() == curScan) {
                        best = &sorted[static_cast<size_t>(j)];
                    }
                }
                prevOut = *best;
                break;
            }
        }
    }

    std::set<std::string> currentPinnedSlugs() {
        std::lock_guard<std::mutex> lock(pinMu_);
        std::set<std::string> s;
        if (!activeChapterSlug_.empty()) s.insert(activeChapterSlug_);
        if (!preloadedChapterSlug_.empty()) s.insert(preloadedChapterSlug_);
        return s;
    }

    // Downloads (if needed) and prepares page file:// URLs for any chapter
    std::vector<std::string> resolveChapterPages(
        const std::string& sourceId,
        const std::string& sourceName,
        const std::string& mangaId,
        const std::string& mangaTitle,
        const std::string& chapterId,
        double chapterNum,
        bool hasChapterNum,
        const std::string& chapterTitle,
        const std::atomic<bool>* cancelFlag,
        const std::function<void(const std::string&, int)>& onProgress,
        std::string* errOut) {
        std::string ckey = store::makeChapterKey(sourceId, mangaId, chapterId);
        auto pinned = currentPinnedSlugs();

        // 1. Check if we already have a verified local path recorded
        Json::Value dlRec = state_.getDownloadRecord(sourceId, mangaId, chapterId);
        if (dlRec.isObject()) {
            std::string sp = dlRec["storage_path"].str();
            bool isFolder = dlRec["is_folder"].boolean(false);
            std::error_code ec;
            if (!sp.empty() && fs::exists(sp, ec)) {
                if (isFolder) {
                    return pageCache_.prepareFolder(sp, errOut);
                }
                return pageCache_.extractCbz(ckey, sp, pinned, errOut);
            }
        }

        // User-installed multi.mangadex Aidoku extension via native adapter
        if (sourceId == mangadex::kExtSourceId) {
            fs::path chFolder = state_.localChaptersDir() / mangadex::kExtSourceId / mangaId / chapterId;
            std::error_code ec;
            if (fs::exists(chFolder, ec) && !fs::is_empty(chFolder, ec)) {
                return pageCache_.prepareFolder(chFolder.string(), errOut);
            }
            if (onProgress) onProgress("Downloading chapter from MangaDex...", 20);
            std::string mdErr;
            bool ok = mangadex_.downloadChapterPages(
                chapterId, chFolder, cancelFlag, onProgress, &mdErr);
            if (!ok) {
                if (errOut) *errOut = mdErr.empty() ? "Failed downloading from MangaDex" : mdErr;
                return {};
            }
            state_.recordDownload(sourceId, "MangaDex", mangaId, mangaTitle,
                                  chapterId, chapterNum, hasChapterNum, chapterTitle,
                                  chFolder.string(), true);
            if (onProgress) onProgress("Preparing pages...", 95);
            return pageCache_.prepareFolder(chFolder.string(), errOut);
        }

        // 2. Aidoku chapter via local Aidoku server
        if (onProgress) onProgress("Preparing chapter from " + (sourceName.empty() ? sourceId : sourceName) + "...", 15);
        std::string dlErr;
        std::string cbzPath = aidoku_.downloadChapter(
            sourceId, mangaId, chapterId, chapterNum, hasChapterNum, cancelFlag,
            [&](const std::string& msg) {
                if (onProgress) onProgress(msg, 45);
            },
            &dlErr);
        if (cbzPath.empty()) {
            if (errOut) *errOut = dlErr.empty() ? "Failed to download chapter" : dlErr;
            return {};
        }

        state_.recordDownload(sourceId, sourceName, mangaId, mangaTitle,
                              chapterId, chapterNum, hasChapterNum, chapterTitle,
                              cbzPath, false);
        if (onProgress) onProgress("Extracting pages...", 85);
        return pageCache_.extractCbz(ckey, cbzPath, pinned, errOut);
    }

    void triggerBackgroundPreload(const std::string& sourceId,
                                  const std::string& sourceName,
                                  const std::string& mangaId,
                                  const std::string& mangaTitle,
                                  const Json::Value& nextChap) {
        if (!nextChap.isObject() || nextChap["id"].str().empty()) return;
        std::string nextCid = nextChap["id"].str();
        std::string ckey = store::makeChapterKey(sourceId, mangaId, nextCid);

        {
            std::lock_guard<std::mutex> lock(pinMu_);
            if (preloadingKeys_.count(ckey) > 0) return;
            preloadingKeys_.insert(ckey);
            preloadedChapterSlug_ = util::hashSlug(ckey);
        }

        double chNum = nextChap["chapter_num"].num(0.0);
        bool hasNum = nextChap.has("chapter_num");
        std::string chTitle = nextChap["title"].str();

        spawnAsync([this, sourceId, sourceName, mangaId, mangaTitle, nextCid, chNum, hasNum, chTitle, ckey]() {
            if (shuttingDown_.load()) return;
            std::string err;
            resolveChapterPages(sourceId, sourceName, mangaId, mangaTitle,
                                nextCid, chNum, hasNum, chTitle,
                                &shuttingDown_, nullptr, &err);
            std::lock_guard<std::mutex> lock(pinMu_);
            preloadingKeys_.erase(ckey);
        });
    }

    void downloadLoop() {
        while (!shuttingDown_.load()) {
            std::unique_ptr<DownloadTask> task;
            {
                std::unique_lock<std::mutex> lock(dlMu_);
                dlCv_.wait(lock, [this]() {
                    return shuttingDown_.load() || !dlQueue_.empty();
                });
                if (shuttingDown_.load()) break;
                task = std::make_unique<DownloadTask>(std::move(dlQueue_.front()));
                dlQueue_.pop_front();
                activeDl_ = std::move(task);
                cancelActiveDl_ = false;
                activeDl_->status = "Starting...";
                activeDl_->progressPct = 5;
            }
            pushDownloadsEvent();

            DownloadTask snapshot;
            {
                std::lock_guard<std::mutex> lock(dlMu_);
                snapshot = *activeDl_;
            }

            std::string err;
            resolveChapterPages(
                snapshot.sourceId, snapshot.sourceName,
                snapshot.mangaId, snapshot.mangaTitle,
                snapshot.chapterId, snapshot.chapterNum, snapshot.hasChapterNum,
                snapshot.chapterTitle,
                &cancelActiveDl_,
                [this](const std::string& st, int pct) {
                    {
                        std::lock_guard<std::mutex> lock(dlMu_);
                        if (activeDl_) {
                            activeDl_->status = st;
                            activeDl_->progressPct = pct;
                        }
                    }
                    pushDownloadsEvent();
                },
                &err);

            {
                std::lock_guard<std::mutex> lock(dlMu_);
                activeDl_.reset();
            }
            if (!err.empty() && err != "Download cancelled") {
                sendToast(snapshot.mangaTitle + ": " + err, true);
            }
            pushDownloadsEvent();
            pushLibraryEvent();
        }
    }

    void coverLoop() {
        while (!shuttingDown_.load()) {
            CoverTask task;
            {
                std::unique_lock<std::mutex> lock(coverMu_);
                coverCv_.wait(lock, [this]() {
                    return shuttingDown_.load() || !coverQueue_.empty();
                });
                if (shuttingDown_.load()) break;
                task = std::move(coverQueue_.front());
                coverQueue_.pop_front();
            }

            std::string mkey = store::makeMangaKey(task.sourceId, task.mangaId);
            fs::path dest = state_.coversDir() / (util::hashSlug(mkey) + ".jpg");
            std::error_code ec;

            std::string url = task.coverUrl;
            if (url.empty()) {
                url = coverDb_.lookupCoverUrl(task.sourceId, task.mangaId);
            }

            bool ok = false;
            if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 128) {
                ok = true;
            } else if (!url.empty()) {
                ok = util::httpsDownloadFile(url, dest, 20);
            }

            if (ok && !shuttingDown_.load()) {
                std::string uri = "file://" + fs::absolute(dest, ec).string();
                state_.setCoverPath(task.sourceId, task.mangaId, uri);

                Json::Value ev = Json::Value::object();
                ev["source_id"] = task.sourceId;
                ev["manga_id"] = task.mangaId;
                ev["cover_path"] = uri;
                ipc_.sendJson(EVT_COVER_UPDATED, ev);
            } else if (!ok) {
                std::lock_guard<std::mutex> lock(coverMu_);
                attemptedCovers_.erase(mkey);
            }
        }
    }

    void handleSearch(const Json::Value& req) {
        uint64_t mySeq = ++searchSeq_;
        std::string query = util::trim(req["query"].str());
        std::string filter = req["source_filter"].str("all");
        std::string mode = req["mode"].str(query.empty() ? "discover" : "search");
        if (mode == "popular" || mode == "latest") mode = "discover";
        bool forceRefresh = req["force_refresh"].boolean(false);

        spawnAsync([this, mySeq, query, filter, mode, forceRefresh]() {
            if (shuttingDown_.load() || searchSeq_.load() != mySeq) return;

            auto enrichItem = [this](Json::Value item) -> Json::Value {
                std::string sid = item["source"]["id"].str(item["source_id"].str());
                std::string sname = item["source"]["name"].str(item["source_name"].str(sid));
                std::string mid = item["id"].str();
                std::string title = item["title"].str(mid);
                item["source_id"] = sid;
                item["source_name"] = sname;

                Json::Value stored = state_.getManga(sid, mid);
                if (stored.isObject()) {
                    item["in_library"] = stored["in_library"].boolean(false);
                    if (!stored["cover_path"].str().empty()) {
                        item["cover_path"] = stored["cover_path"].str();
                    }
                } else {
                    item["in_library"] = false;
                }

                if (item["cover_url"].str().empty()) {
                    std::string dbUrl = coverDb_.lookupCoverUrl(sid, mid);
                    if (!dbUrl.empty()) item["cover_url"] = dbUrl;
                }
                if (item["cover_path"].str().empty()) {
                    enqueueCoverFetch(sid, mid, title, item["cover_url"].str());
                }
                return item;
            };

            Json::Value combined = Json::Value::array();
            std::string errors;

            bool online = false;
            Json::Value installed = buildInstalledSourcesList(&online);
            bool hasUserMangaDex = false;
            bool hasOtherAidoku = false;
            for (const auto& s : installed.asArray()) {
                if (s["id"].str() == mangadex::kExtSourceId) hasUserMangaDex = true;
                else hasOtherAidoku = true;
            }

            if (hasUserMangaDex && (filter == "all" || filter == mangadex::kExtSourceId)) {
                std::string mdErr;
                Json::Value mdItems = mangadex_.queryMangaList(query, mode, &mdErr);
                if (searchSeq_.load() != mySeq) return;
                if (mdItems.isArray()) {
                    for (const auto& m : mdItems.asArray()) {
                        combined.push(enrichItem(m));
                    }
                }
                if (!mdErr.empty() && filter == mangadex::kExtSourceId) {
                    errors = mdErr;
                }
            }

            if (online && hasOtherAidoku && filter != mangadex::kExtSourceId) {
                Json::Value rawItems = Json::Value::array();
                std::string aiErr;

                if (query.empty()) {
                    bool hitCache = false;
                    if (!forceRefresh) {
                        std::lock_guard<std::mutex> lock(discoverMu_);
                        if (discoverCache_.isArray() && !discoverCache_.empty() &&
                            (util::nowSeconds() - discoverCacheTs_) < 60) {
                            rawItems = discoverCache_;
                            hitCache = true;
                        }
                    }
                    if (!hitCache) {
                        rawItems = aidoku_.searchMangas("", &aiErr);
                        if (searchSeq_.load() != mySeq) return;
                        if (aiErr.empty() && rawItems.isArray() && !rawItems.empty()) {
                            std::lock_guard<std::mutex> lock(discoverMu_);
                            discoverCache_ = rawItems;
                            discoverCacheTs_ = util::nowSeconds();
                        }
                    }
                } else {
                    rawItems = aidoku_.searchMangas(query, &aiErr);
                }

                if (searchSeq_.load() != mySeq) return;

                size_t beforeAidoku = combined.size();
                for (const auto& m : rawItems.asArray()) {
                    std::string sid = m["source"]["id"].str(m["source_id"].str());
                    if (sid == mangadex::kExtSourceId) continue;
                    if (filter != "all" && sid != filter) continue;
                    combined.push(enrichItem(m));
                }

                // Fallback for search-only .aix plugins that return 0 items on empty query q=""
                if (query.empty() && combined.size() == beforeAidoku && aiErr.empty()) {
                    std::string fbErr;
                    Json::Value fbItems = aidoku_.searchMangas("a", &fbErr);
                    if (searchSeq_.load() != mySeq) return;
                    for (const auto& m : fbItems.asArray()) {
                        std::string sid = m["source"]["id"].str(m["source_id"].str());
                        if (sid == mangadex::kExtSourceId) continue;
                        if (filter != "all" && sid != filter) continue;
                        combined.push(enrichItem(m));
                    }
                }

                if (!aiErr.empty() && combined.empty() && filter != "all") {
                    errors = aiErr;
                }
            }

            if (searchSeq_.load() != mySeq) return;
            Json::Value done = Json::Value::object();
            done["query"] = query;
            done["source_filter"] = filter;
            done["mode"] = mode;
            done["results"] = std::move(combined);
            done["loading"] = false;
            done["error"] = errors;
            ipc_.sendJson(EVT_SEARCH_RESULTS, done);
        });
    }

    void handleMangaDetails(const Json::Value& req) {
        std::string sourceId = req["source_id"].str();
        std::string sourceName = req["source_name"].str(sourceId);
        std::string mangaId = req["manga_id"].str();
        std::string mangaTitle = req["manga_title"].str();
        std::string coverUrl = req["cover_url"].str();
        bool forceRefresh = req["force_refresh"].boolean(false);

        if (sourceId.empty() || mangaId.empty()) return;

        state_.upsertMangaMeta(sourceId, sourceName, mangaId, mangaTitle, coverUrl,
                               req["author"].str(), req["description"].str());
        enqueueCoverFetch(sourceId, mangaId, mangaTitle, coverUrl);

        spawnAsync([this, sourceId, sourceName, mangaId, mangaTitle, forceRefresh]() {
            if (shuttingDown_.load()) return;

            auto sendDetailsPayload = [this](const std::string& sid,
                                             const std::string& mid,
                                             const Json::Value& chapters,
                                             bool loading,
                                             const std::string& err) {
                Json::Value m = state_.getManga(sid, mid);
                Json::Value ev = Json::Value::object();
                ev["source_id"] = sid;
                ev["source_name"] = m["source_name"].str(sid);
                ev["manga_id"] = mid;
                ev["manga_title"] = m["title"].str(mid);
                ev["author"] = m["author"].str();
                ev["description"] = m["description"].str();
                ev["status"] = m["status"].str();
                ev["cover_path"] = m["cover_path"].str();
                ev["cover_url"] = m["cover_url"].str();
                ev["in_library"] = m["in_library"].boolean(false);
                ev["chapters"] = chapters;
                ev["resume_chapter_id"] = m["last_chapter_id"].str();
                ev["resume_page"] = m["last_page_index"].integer(0);
                ev["loading"] = loading;
                ev["error"] = err;
                ipc_.sendJson(EVT_MANGA_DETAILS, ev);
            };

            Json::Value cached = state_.getCachedChapters(sourceId, mangaId);
            if (!cached.empty()) {
                sendDetailsPayload(sourceId, mangaId, cached, forceRefresh, "");
                if (!forceRefresh) {
                    // Still refresh quietly if online? Only if forceRefresh or cache empty to keep opens instant!
                    return;
                }
            } else {
                sendDetailsPayload(sourceId, mangaId, Json::Value::array(), true, "");
            }

            std::string err;
            Json::Value freshChapters = Json::Value::array();

            if (sourceId == mangadex::kExtSourceId) {
                if (forceRefresh || cached.empty()) {
                    Json::Value details = mangadex_.getMangaDetails(mangaId, &err);
                    if (details.isObject()) {
                        state_.upsertMangaMeta(
                            sourceId, "MangaDex", mangaId,
                            details["title"].str(mangaTitle),
                            details["cover_url"].str(),
                            details["author"].str(),
                            details["description"].str());
                        if (!details["cover_url"].str().empty()) {
                            enqueueCoverFetch(sourceId, mangaId, details["title"].str(), details["cover_url"].str());
                        }
                    }
                    freshChapters = mangadex_.getChapters(mangaId, &err);
                }
            } else {
                if (forceRefresh || cached.empty()) {
                    Json::Value preCached = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    if (forceRefresh || !preCached.isArray() || preCached.empty()) {
                        aidoku_.refreshChapters(sourceId, mangaId, &err);
                    }
                }
                std::string listErr;
                freshChapters = aidoku_.listCachedChapters(sourceId, mangaId, &listErr);
                if (!listErr.empty() && err.empty()) err = listErr;
            }

            if (freshChapters.isArray() && !freshChapters.empty()) {
                Json::Value merged = state_.mergeAndCacheChapters(sourceId, sourceName, mangaId, mangaTitle, freshChapters);
                enqueueCoverFetch(sourceId, mangaId, mangaTitle, "");
                sendDetailsPayload(sourceId, mangaId, merged, false, "");
                pushLibraryEvent();
            } else {
                sendDetailsPayload(sourceId, mangaId, cached, false, err);
            }
        });
    }

    void handleOpenChapter(const Json::Value& req) {
        uint64_t mySeq = ++readerSeq_;
        std::string sourceId = req["source_id"].str();
        std::string sourceName = req["source_name"].str(sourceId);
        std::string mangaId = req["manga_id"].str();
        std::string mangaTitle = req["manga_title"].str();
        std::string chapterId = req["chapter_id"].str();
        double chapterNum = req["chapter_num"].num(0.0);
        bool hasChapterNum = req.has("chapter_num");
        std::string chapterTitle = req["chapter_title"].str();
        bool startAtEnd = req["start_at_end"].boolean(false);

        if (sourceId.empty() || mangaId.empty() || chapterId.empty()) return;

        std::string ckey = store::makeChapterKey(sourceId, mangaId, chapterId);
        {
            std::lock_guard<std::mutex> lock(pinMu_);
            activeChapterSlug_ = util::hashSlug(ckey);
        }

        spawnAsync([this, mySeq, sourceId, sourceName, mangaId, mangaTitle,
                     chapterId, chapterNum, hasChapterNum, chapterTitle, startAtEnd]() {
            if (shuttingDown_.load() || readerSeq_.load() != mySeq) return;

            auto sendLoading = [this, mySeq, &chapterId](const std::string& text, int pct) {
                if (readerSeq_.load() != mySeq) return;
                Json::Value ev = Json::Value::object();
                ev["chapter_id"] = chapterId;
                ev["status_text"] = text;
                ev["progress_pct"] = pct;
                ipc_.sendJson(EVT_READER_LOADING, ev);
            };

            sendLoading("Opening chapter...", 10);

            // Ensure chapters list is cached so prev/next chapter links work even on direct Continue Reading jump
            Json::Value chapters = state_.getCachedChapters(sourceId, mangaId);
            if (chapters.empty()) {
                if (sourceId == mangadex::kExtSourceId) {
                    std::string chErr;
                    Json::Value fetched = mangadex_.getChapters(mangaId, &chErr);
                    if (fetched.isArray() && !fetched.empty()) {
                        chapters = state_.mergeAndCacheChapters(sourceId, "MangaDex", mangaId, mangaTitle, fetched);
                    }
                } else if (aidoku_.ensureRunning()) {
                    Json::Value fetched = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    if (!fetched.isArray() || fetched.empty()) {
                        aidoku_.refreshChapters(sourceId, mangaId, nullptr);
                        fetched = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    }
                    if (fetched.isArray() && !fetched.empty()) {
                        chapters = state_.mergeAndCacheChapters(sourceId, sourceName, mangaId, mangaTitle, fetched);
                    }
                }
            }

            std::string err;
            auto pages = resolveChapterPages(
                sourceId, sourceName, mangaId, mangaTitle,
                chapterId, chapterNum, hasChapterNum, chapterTitle,
                &shuttingDown_, sendLoading, &err);

            if (readerSeq_.load() != mySeq) return;

            Json::Value prevCh, nextCh;
            findPrevNextChapter(chapters, chapterId, prevCh, nextCh);

            int initialPage = 0;
            if (!pages.empty()) {
                if (startAtEnd) {
                    initialPage = static_cast<int>(pages.size()) - 1;
                } else {
                    Json::Value m = state_.getManga(sourceId, mangaId);
                    if (m["chapter_pages"][chapterId].isObject()) {
                        int saved = static_cast<int>(m["chapter_pages"][chapterId]["page"].integer(0));
                        if (saved > 0 && saved < static_cast<int>(pages.size())) {
                            initialPage = saved;
                        }
                    } else if (m["last_chapter_id"].str() == chapterId) {
                        int saved = static_cast<int>(m["last_page_index"].integer(0));
                        if (saved > 0 && saved < static_cast<int>(pages.size())) {
                            initialPage = saved;
                        }
                    }
                }

                state_.saveProgress(sourceId, sourceName, mangaId, mangaTitle,
                                    chapterId, chapterNum, hasChapterNum, chapterTitle,
                                    initialPage, static_cast<int>(pages.size()), false);
                pushLibraryEvent();
                pushDownloadsEvent();

                // If opening near the end of a short chapter, start preloading next chapter immediately
                if (initialPage >= static_cast<int>(pages.size()) - 3 &&
                    state_.getSettings()["autoPreloadNext"].boolean(true)) {
                    triggerBackgroundPreload(sourceId, sourceName, mangaId, mangaTitle, nextCh);
                }
            }

            Json::Value ev = Json::Value::object();
            ev["source_id"] = sourceId;
            ev["source_name"] = sourceName;
            ev["manga_id"] = mangaId;
            ev["manga_title"] = mangaTitle;
            ev["chapter_id"] = chapterId;
            if (hasChapterNum) ev["chapter_num"] = chapterNum;
            ev["chapter_title"] = chapterTitle;

            Json::Value pagesArr = Json::Value::array();
            for (const auto& u : pages) pagesArr.push(u);
            ev["pages"] = std::move(pagesArr);
            ev["initial_page"] = initialPage;
            ev["prev_chapter"] = prevCh;
            ev["next_chapter"] = nextCh;
            ev["error"] = err;
            ipc_.sendJson(EVT_CHAPTER_PAGES, ev);
        });
    }

    void handleIncoming(int32_t msgType, const std::string& contents) {
        if (msgType == appload::MSG_SYSTEM_TERMINATE) {
            shuttingDown_ = true;
            return;
        }
        if (msgType == appload::MSG_SYSTEM_NEW_COORDINATOR) {
            spawnAsync([this]() { pushBootstrap(); });
            return;
        }
        if (msgType == appload::MSG_SYSTEM_LOST_COORDINATOR) {
            return;
        }

        bool ok = false;
        Json::Value req = contents.empty() ? Json::Value::object() : Json::parse(contents, &ok);

        switch (msgType) {
            case REQ_BOOTSTRAP: {
                spawnAsync([this]() { pushBootstrap(); });
                break;
            }
            case REQ_REFRESH_LIBRARY: {
                bool deep = req["deep"].boolean(false);
                spawnAsync([this, deep]() {
                    syncAidokuIntoStore(deep);
                    pushLibraryEvent();
                    sendToast(deep ? "Library & chapters refreshed" : "Library synced");
                });
                break;
            }
            case REQ_SEARCH: {
                handleSearch(req);
                break;
            }
            case REQ_MANGA_DETAILS: {
                handleMangaDetails(req);
                break;
            }
            case REQ_TOGGLE_LIBRARY: {
                std::string sid = req["source_id"].str();
                std::string sname = req["source_name"].str(sid);
                std::string mid = req["manga_id"].str();
                std::string title = req["manga_title"].str(mid);
                std::string coverUrl = req["cover_url"].str();
                bool add = req["add"].boolean(true);

                spawnAsync([this, sid, sname, mid, title, coverUrl, add, req]() {
                    std::string err;
                    if (sid != mangadex::kExtSourceId) {
                        if (add) aidoku_.addToLibrary(sid, mid, &err);
                        else aidoku_.removeFromLibrary(sid, mid, &err);
                    }
                    state_.upsertMangaMeta(sid, sname, mid, title, coverUrl,
                                           req["author"].str(), req["description"].str(), "",
                                           add ? 1 : 0);
                    if (add) enqueueCoverFetch(sid, mid, title, coverUrl);
                    pushLibraryEvent();
                    sendToast(add ? ("Added \"" + title + "\" to Library") : ("Removed from Library"));
                });
                break;
            }
            case REQ_OPEN_CHAPTER: {
                handleOpenChapter(req);
                break;
            }
            case REQ_SAVE_PROGRESS: {
                std::string sid = req["source_id"].str();
                std::string sname = req["source_name"].str(sid);
                std::string mid = req["manga_id"].str();
                std::string mtitle = req["manga_title"].str();
                std::string cid = req["chapter_id"].str();
                double chNum = req["chapter_num"].num(0.0);
                bool hasNum = req.has("chapter_num");
                std::string chTitle = req["chapter_title"].str();
                int pageIdx = static_cast<int>(req["page_index"].integer(0));
                int pageCount = static_cast<int>(req["page_count"].integer(0));
                bool completed = req["completed"].boolean(false) ||
                                 (pageCount > 0 && pageIdx >= pageCount - 1);

                state_.saveProgress(sid, sname, mid, mtitle, cid, chNum, hasNum, chTitle,
                                    pageIdx, pageCount, completed);

                if (completed && sid != mangadex::kExtSourceId) {
                    spawnAsync([this, sid, mid, cid]() {
                        aidoku_.markChapterRead(sid, mid, cid, nullptr);
                    });
                }

                // Pre-download & pre-extract next chapter when within 3 pages of end
                if (pageCount > 0 && pageIdx >= pageCount - 3 &&
                    state_.getSettings()["autoPreloadNext"].boolean(true)) {
                    Json::Value chs = state_.getCachedChapters(sid, mid);
                    Json::Value prevCh, nextCh;
                    findPrevNextChapter(chs, cid, prevCh, nextCh);
                    triggerBackgroundPreload(sid, sname, mid, mtitle, nextCh);
                }

                pushLibraryEvent();
                break;
            }
            case REQ_MARK_CHAPTER_READ: {
                std::string sid = req["source_id"].str();
                std::string mid = req["manga_id"].str();
                std::string cid = req["chapter_id"].str();
                bool read = req["read"].boolean(true);
                state_.markChapterRead(sid, mid, cid, read);
                if (read && sid != mangadex::kExtSourceId) {
                    spawnAsync([this, sid, mid, cid]() {
                        aidoku_.markChapterRead(sid, mid, cid, nullptr);
                    });
                }
                pushLibraryEvent();
                break;
            }
            case REQ_DOWNLOAD_CHAPTERS: {
                const Json::Value& items = req["items"];
                if (!items.isArray()) break;
                int added = 0;
                {
                    std::lock_guard<std::mutex> lock(dlMu_);
                    for (const auto& it : items.asArray()) {
                        std::string sid = it["source_id"].str();
                        std::string mid = it["manga_id"].str();
                        std::string cid = it["chapter_id"].str();
                        if (sid.empty() || mid.empty() || cid.empty()) continue;

                        std::string ckey = store::makeChapterKey(sid, mid, cid);
                        if (activeDl_ && activeDl_->key == ckey) continue;
                        bool dup = false;
                        for (const auto& q : dlQueue_) {
                            if (q.key == ckey) {
                                dup = true;
                                break;
                            }
                        }
                        if (dup) continue;

                        DownloadTask t;
                        t.key = ckey;
                        t.sourceId = sid;
                        t.sourceName = it["source_name"].str(sid);
                        t.mangaId = mid;
                        t.mangaTitle = it["manga_title"].str(mid);
                        t.chapterId = cid;
                        t.chapterNum = it["chapter_num"].num(0.0);
                        t.hasChapterNum = it.has("chapter_num");
                        t.chapterTitle = it["chapter_title"].str();
                        dlQueue_.push_back(std::move(t));
                        ++added;
                    }
                }
                if (added > 0) {
                    dlCv_.notify_one();
                    pushDownloadsEvent();
                    sendToast("Queued " + std::to_string(added) + (added == 1 ? " chapter" : " chapters"));
                }
                break;
            }
            case REQ_CANCEL_DOWNLOAD: {
                bool cancelAll = req["all"].boolean(false);
                std::string key = req["key"].str();
                {
                    std::lock_guard<std::mutex> lock(dlMu_);
                    if (cancelAll) {
                        dlQueue_.clear();
                        if (activeDl_) cancelActiveDl_ = true;
                    } else if (!key.empty()) {
                        if (activeDl_ && activeDl_->key == key) {
                            cancelActiveDl_ = true;
                        } else {
                            dlQueue_.erase(
                                std::remove_if(dlQueue_.begin(), dlQueue_.end(),
                                               [&](const DownloadTask& t) { return t.key == key; }),
                                dlQueue_.end());
                        }
                    }
                }
                pushDownloadsEvent();
                break;
            }
            case REQ_DELETE_DOWNLOAD: {
                if (req["clear_cache"].boolean(false)) {
                    pageCache_.clearAll();
                    pushDownloadsEvent();
                    sendToast("Cleared temporary page cache");
                } else {
                    state_.removeDownloadRecord(req["source_id"].str(),
                                                req["manga_id"].str(),
                                                req["chapter_id"].str());
                    pushDownloadsEvent();
                    pushLibraryEvent();
                }
                break;
            }
            case REQ_LIST_SOURCES: {
                bool includeAvail = req["include_available"].boolean(true);
                spawnAsync([this, includeAvail]() {
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = cfg["source_lists"].isArray() && !cfg["source_lists"].empty();
                    Json::Value ev = Json::Value::object();
                    bool online = false;
                    ev["installed"] = buildInstalledSourcesList(&online);
                    ev["available"] = Json::Value::array();
                    ev["source_config"] = cfg;
                    ev["loading"] = includeAvail && online && hasLists;
                    ev["error"] = "";
                    ipc_.sendJson(EVT_SOURCES_LIST, ev);

                    if (includeAvail && online && hasLists) {
                        std::string availErr;
                        Json::Value avail = aidoku_.listAvailableSources(&availErr);
                        Json::Value ev2 = Json::Value::object();
                        ev2["installed"] = buildInstalledSourcesList(nullptr);
                        ev2["available"] = avail.isArray() ? avail : Json::Value::array();
                        ev2["source_config"] = aidoku_.getSourceConfig();
                        ev2["loading"] = false;
                        ev2["error"] = availErr;
                        ipc_.sendJson(EVT_SOURCES_LIST, ev2);
                    }
                });
                break;
            }
            case REQ_INSTALL_SOURCE: {
                std::string sid = req["source_id"].str();
                bool install = req["install"].boolean(true);
                if (sid.empty()) break;

                spawnAsync([this, sid, install]() {
                    std::string err;
                    bool ok = install ? aidoku_.installSource(sid, &err) : aidoku_.uninstallSource(sid, &err);
                    invalidateDiscoverCache();
                    if (!ok) {
                        sendToast("Source error: " + err, true);
                    } else {
                        sendToast(install ? ("Installed " + sid) : ("Removed " + sid));
                    }
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = cfg["source_lists"].isArray() && !cfg["source_lists"].empty();
                    Json::Value ev = Json::Value::object();
                    ev["installed"] = buildInstalledSourcesList(nullptr);
                    Json::Value avail = hasLists ? aidoku_.listAvailableSources(nullptr) : Json::Value::array();
                    ev["available"] = avail.isArray() ? avail : Json::Value::array();
                    ev["source_config"] = cfg;
                    ev["loading"] = false;
                    ev["error"] = ok ? "" : err;
                    ipc_.sendJson(EVT_SOURCES_LIST, ev);
                });
                break;
            }
            case REQ_UPDATE_SETTINGS: {
                Json::Value updated = state_.updateSettings(req);
                Json::Value ev = Json::Value::object();
                ev["library"] = state_.listLibrary();
                ev["continue_reading"] = state_.getContinueReading();
                ev["recent"] = state_.listRecent();
                ev["downloads"] = state_.listDownloads();
                ev["queue"] = serializeDownloadQueue();
                ev["settings"] = updated;
                bool online = false;
                ev["installed_sources"] = buildInstalledSourcesList(&online);
                ev["source_config"] = aidoku_.getSourceConfig();
                ev["aidoku_online"] = online;
                ipc_.sendJson(EVT_BOOTSTRAP, ev);
                break;
            }
            case REQ_SAVE_SOURCE_CONFIG: {
                spawnAsync([this, req]() {
                    bool reloadDisk = req["reload_disk"].boolean(false);
                    std::string err;
                    if (reloadDisk) {
                        aidoku_.ensureSettingsInitialized();
                        aidoku_.restartServer();
                    } else {
                        if (!aidoku_.saveSourceConfig(req, &err)) {
                            sendToast(err, true);
                            Json::Value errEv = Json::Value::object();
                            errEv["installed"] = buildInstalledSourcesList(nullptr);
                            errEv["available"] = Json::Value::array();
                            errEv["source_config"] = aidoku_.getSourceConfig();
                            errEv["loading"] = false;
                            errEv["error"] = err;
                            ipc_.sendJson(EVT_SOURCES_LIST, errEv);
                            return;
                        }
                    }

                    invalidateDiscoverCache();
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = cfg["source_lists"].isArray() && !cfg["source_lists"].empty();
                    bool online = false;
                    Json::Value installed = buildInstalledSourcesList(&online);

                    if (online && hasLists) {
                        Json::Value loadingEv = Json::Value::object();
                        loadingEv["installed"] = installed;
                        loadingEv["available"] = Json::Value::array();
                        loadingEv["source_config"] = cfg;
                        loadingEv["loading"] = true;
                        loadingEv["error"] = "";
                        ipc_.sendJson(EVT_SOURCES_LIST, loadingEv);
                    }

                    std::string availErr;
                    Json::Value avail = (online && hasLists) ? aidoku_.listAvailableSources(&availErr)
                                                             : Json::Value::array();
                    Json::Value doneEv = Json::Value::object();
                    doneEv["installed"] = buildInstalledSourcesList(&online);
                    doneEv["available"] = avail.isArray() ? avail : Json::Value::array();
                    doneEv["source_config"] = aidoku_.getSourceConfig();
                    doneEv["loading"] = false;
                    doneEv["error"] = availErr;
                    ipc_.sendJson(EVT_SOURCES_LIST, doneEv);

                    if (!availErr.empty()) {
                        sendToast("Saved settings.json, fetch error: " + availErr, true);
                    } else if (reloadDisk) {
                        sendToast("Reloaded settings.json (" + std::to_string(doneEv["available"].size()) + " available)");
                    } else if (hasLists) {
                        sendToast("Saved source config (" + std::to_string(doneEv["available"].size()) + " sources available)");
                    } else {
                        sendToast("Cleared source lists");
                    }
                });
                break;
            }
            default:
                break;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <appload_socket_path>\n";
        return 1;
    }

    fs::path workDir = fs::current_path();
    if (const char* envDir = std::getenv("RM2MANGA_DATA_ROOT")) {
        workDir = envDir;
    }

    Rm2MangaApp app(workDir);
    if (!app.start(argv[1])) {
        return 1;
    }
    app.run();
    return 0;
}
