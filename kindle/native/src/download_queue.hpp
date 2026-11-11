#pragma once

#include "types.hpp"
#include "source_manager.hpp"
#include "zip_writer.hpp"
#include "../../../backend/src/util.hpp"
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <filesystem>
#include <functional>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

namespace kindle {

struct DownloadJob {
    UnifiedManga manga;
    UnifiedChapter chapter;
    bool auto_open = false;
    int start_page = 0;
    bool silent_preload = false;
};

struct DownloadResult {
    UnifiedManga manga;
    UnifiedChapter chapter;
    std::string cbz_path;
    std::string error;
    bool ok = false;
    bool cancelled = false;
    bool auto_open = false;
    int start_page = 0;
    bool silent_preload = false;
};

struct DownloadSnapshot {
    bool active = false;
    DownloadJob current;
    std::string status;
    int pct = 0;
    std::vector<DownloadJob> queued;
};

class DownloadQueue {
private:
    SourceManager& sources_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<DownloadJob> queue_;
    std::deque<DownloadResult> completed_;

    bool has_active_ = false;
    DownloadJob active_job_;
    std::string active_status_;
    int active_pct_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_current_{false};
    std::thread worker_;
    std::function<void()> on_update_;

    static bool sameJob(const DownloadJob& a, const std::string& mangaTitle,
                        const std::string& chId, const std::string& chNum) {
        if (a.manga.title != mangaTitle) return false;
        if (!chId.empty() && a.chapter.id == chId) return true;
        if (!chNum.empty() && a.chapter.chapterNum == chNum) return true;
        return false;
    }

public:
    explicit DownloadQueue(SourceManager& sources) : sources_(sources) {}
    ~DownloadQueue() { stop(); }

    void start(std::function<void()> onUpdate) {
        on_update_ = std::move(onUpdate);
        if (running_.load()) return;
        running_ = true;
        worker_ = std::thread(&DownloadQueue::workerLoop, this);
    }

    void stop() {
        if (running_.load()) {
            running_ = false;
            cancel_current_ = true;
            cv_.notify_all();
            if (worker_.joinable()) {
                worker_.join();
            }
        }
    }

    bool enqueue(const DownloadJob& job) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (has_active_ && sameJob(active_job_, job.manga.title, job.chapter.id, job.chapter.chapterNum)) {
                if (job.auto_open) {
                    active_job_.auto_open = true;
                    active_job_.start_page = job.start_page;
                    active_job_.silent_preload = false;
                }
                return false;
            }
            for (auto& q : queue_) {
                if (sameJob(q, job.manga.title, job.chapter.id, job.chapter.chapterNum)) {
                    if (job.auto_open) {
                        q.auto_open = true;
                        q.start_page = job.start_page;
                        q.silent_preload = false;
                    }
                    return false;
                }
            }
            if (job.auto_open) {
                queue_.push_front(job);
            } else {
                queue_.push_back(job);
            }
        }
        cv_.notify_one();
        if (on_update_) on_update_();
        return true;
    }

    bool isQueuedOrActive(const std::string& mangaTitle, const std::string& chId, const std::string& chNum) {
        std::lock_guard<std::mutex> lock(mu_);
        if (has_active_ && sameJob(active_job_, mangaTitle, chId, chNum)) return true;
        for (const auto& q : queue_) {
            if (sameJob(q, mangaTitle, chId, chNum)) return true;
        }
        return false;
    }

    void cancelCurrent() {
        cancel_current_ = true;
        if (on_update_) on_update_();
    }

    void cancelAll() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.clear();
            cancel_current_ = true;
        }
        if (on_update_) on_update_();
    }

    void removeQueuedAt(size_t idx) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (idx < queue_.size()) {
                queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(idx));
            }
        }
        if (on_update_) on_update_();
    }

    DownloadSnapshot snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        DownloadSnapshot s;
        s.active = has_active_;
        s.current = active_job_;
        s.status = active_status_;
        s.pct = active_pct_;
        s.queued.assign(queue_.begin(), queue_.end());
        return s;
    }

    bool popCompleted(DownloadResult& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (completed_.empty()) return false;
        out = std::move(completed_.front());
        completed_.pop_front();
        return true;
    }

private:
    void reportProgress(const std::string& status, int pct, bool forceWake = false) {
        static auto lastWake = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_status_ = status;
            active_pct_ = std::max(0, std::min(100, pct));
        }
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastWake).count();
        if (forceWake || ms >= 320) {
            lastWake = now;
            if (on_update_) on_update_();
        }
    }

    void workerLoop() {
        while (running_.load()) {
            DownloadJob job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]() { return !queue_.empty() || !running_.load(); });
                if (!running_.load()) break;
                job = std::move(queue_.front());
                queue_.pop_front();
                has_active_ = true;
                active_job_ = job;
                active_status_ = "Connecting for Ch. " + job.chapter.chapterNum + "...";
                active_pct_ = 4;
                cancel_current_ = false;
            }
            if (on_update_) on_update_();

            DownloadResult res = executeJob(job);

            {
                std::lock_guard<std::mutex> lock(mu_);
                has_active_ = false;
                active_pct_ = 0;
                active_status_.clear();
                completed_.push_back(std::move(res));
            }
            if (on_update_) on_update_();
        }
    }

    DownloadResult executeJob(const DownloadJob& job) {
        DownloadResult res;
        res.manga = job.manga;
        res.chapter = job.chapter;
        res.auto_open = job.auto_open;
        res.start_page = job.start_page;
        res.silent_preload = job.silent_preload;

        std::string srcId = job.chapter.source_id.empty() ? job.manga.source_id : job.chapter.source_id;
        if (srcId.empty() || srcId == "all") srcId = "mangadex";
        res.chapter.source_id = srcId;

        fs::path destDir = "/mnt/us/manga/" + job.manga.title;
        std::string safeCh = job.chapter.chapterNum.empty() ? "chapter" : ("chapter_" + job.chapter.chapterNum);
        fs::path cbzFile = destDir / (safeCh + ".cbz");

        std::error_code ec;
        if (fs::exists(cbzFile, ec) && fs::file_size(cbzFile, ec) > 256) {
            res.ok = true;
            res.cbz_path = cbzFile.string();
            res.chapter.isDownloaded = true;
            res.chapter.localPath = res.cbz_path;
            return res;
        }

        std::string safeId = job.chapter.id.empty() ? job.chapter.title : job.chapter.id;
        for (char& c : safeId) {
            if (c == '/' || c == ':' || c == ' ' || c == '\\' || c == '?' || c == '&') c = '_';
        }
        if (safeId.size() > 80) safeId = safeId.substr(0, 80);
        fs::path tmpExtract = "/mnt/us/manga/.cache/tmp/rm2manga_dl_" + safeId;
        fs::remove_all(tmpExtract, ec);
        fs::create_directories(tmpExtract, ec);

        std::string err;
        bool ok = false;
        bool alreadyCbz = false;

        if (srcId == "mangafox" || srcId == "mangakatana") {
            reportProgress("Ch. " + job.chapter.chapterNum + ": fetching page list...", 10, true);
            std::vector<std::string> pageUrls = sources_.getChapterPageUrls(res.chapter, &err);
            if (!pageUrls.empty() && !cancel_current_.load() && running_.load()) {
                int total = static_cast<int>(pageUrls.size());
                int downloaded = 0;
                for (int p = 0; p < total; ++p) {
                    if (cancel_current_.load() || !running_.load()) break;
                    char fname[32];
                    std::snprintf(fname, sizeof(fname), "page_%03d.jpg", p + 1);
                    fs::path pageFile = tmpExtract / fname;
                    int pct = 12 + ((p + 1) * 76) / total;
                    reportProgress("Ch. " + job.chapter.chapterNum + ": page " +
                                   std::to_string(p + 1) + "/" + std::to_string(total), pct);
                    if (util::httpsDownloadFile(pageUrls[p], pageFile.string(), 30)) {
                        ++downloaded;
                    }
                }
                ok = !cancel_current_.load() && (downloaded > 0);
                if (!ok && err.empty() && !cancel_current_.load()) err = "Failed downloading chapter pages";
            } else if (err.empty() && !cancel_current_.load()) {
                err = "Empty chapter page list";
            }
        } else if (sources_.isAidokuSource(srcId)) {
            reportProgress("Ch. " + job.chapter.chapterNum + ": downloading via " + srcId + "...", 20, true);
            double num = -1.0;
            bool hasNum = false;
            if (!job.chapter.chapterNum.empty()) {
                char* end = nullptr;
                num = std::strtod(job.chapter.chapterNum.c_str(), &end);
                hasNum = (end != job.chapter.chapterNum.c_str());
            }
            std::string downloadedCbz = sources_.downloadAidokuChapter(
                srcId, job.manga.id, job.chapter.id, num, hasNum, &cancel_current_,
                [this, &job](const std::string& msg) {
                    reportProgress("Ch. " + job.chapter.chapterNum + ": " + msg, 55);
                }, &err);
            if (!cancel_current_.load() && !downloadedCbz.empty() && fs::exists(downloadedCbz, ec)) {
                fs::create_directories(destDir, ec);
                fs::copy_file(downloadedCbz, cbzFile, fs::copy_options::overwrite_existing, ec);
                ok = !ec;
                alreadyCbz = true;
            } else if (err.empty() && !cancel_current_.load()) {
                err = "Aidoku download failed";
            }
        } else {
            ok = sources_.mangadexClient().downloadChapterPages(
                job.chapter.id, tmpExtract, &cancel_current_,
                [this, &job](const std::string& msg, int pct) {
                    reportProgress("Ch. " + job.chapter.chapterNum + ": " + msg, pct);
                }, &err);
        }

        if (cancel_current_.load() || !running_.load()) {
            fs::remove_all(tmpExtract, ec);
            res.cancelled = true;
            res.error = "Cancelled";
            return res;
        }

        if (ok) {
            bool archiveOk = true;
            if (!alreadyCbz) {
                reportProgress("Ch. " + job.chapter.chapterNum + ": packing CBZ...", 92, true);
                fs::create_directories(destDir, ec);
                ZipStoreWriter zip;
                if (zip.open(cbzFile.string())) {
                    std::vector<fs::path> pageFiles;
                    for (const auto& entry : fs::directory_iterator(tmpExtract, ec)) {
                        if (entry.is_regular_file(ec)) pageFiles.push_back(entry.path());
                    }
                    std::sort(pageFiles.begin(), pageFiles.end(), [](const fs::path& a, const fs::path& b) {
                        return util::naturalLess(a.filename().string(), b.filename().string());
                    });
                    for (const auto& pf : pageFiles) {
                        std::string bytes;
                        if (util::readFileToString(pf, bytes)) {
                            std::vector<uint8_t> vec(bytes.begin(), bytes.end());
                            zip.add(pf.filename().string(), vec);
                        }
                    }
                    zip.close();

                    fs::path coverFile = destDir / "cover.jpg";
                    if (!fs::exists(coverFile, ec) && !pageFiles.empty()) {
                        fs::copy_file(pageFiles[0], coverFile, ec);
                    }
                } else {
                    archiveOk = false;
                    err = "Failed writing CBZ archive";
                }
            }
            fs::remove_all(tmpExtract, ec);

            if (archiveOk) {
                res.ok = true;
                res.cbz_path = cbzFile.string();
                res.chapter.isDownloaded = true;
                res.chapter.localPath = res.cbz_path;
                return res;
            }
        }

        fs::remove_all(tmpExtract, ec);
        res.ok = false;
        res.error = err.empty() ? "Download failed" : err;
        return res;
    }
};

} // namespace kindle
