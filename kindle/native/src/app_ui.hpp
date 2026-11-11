#pragma once

#include "canvas.hpp"
#include "display_zelda.hpp"
#include "input_kindle.hpp"
#include "zip_writer.hpp"
#include "font_manager.hpp"
#include "state_manager.hpp"
#include "source_manager.hpp"
#include "frontlight.hpp"
#include "cover_cache.hpp"
#include "reader_cache.hpp"
#include "download_queue.hpp"
#include "ui_components.hpp"
#include "../third_party/stb_image.h"
#include "../../../backend/src/cbz_extractor.hpp"
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>

namespace fs = std::filesystem;

namespace kindle {

enum class ViewState {
    LIBRARY = 0,
    RECENT,
    BROWSE,
    DETAIL,
    READER,
    DOWNLOADS,
    SETTINGS,
    SOURCES
};

enum class LibraryFilter {
    ALL = 0,
    UNREAD,
    OFFLINE
};

enum class ChapterFilter {
    ALL = 0,
    UNREAD,
    OFFLINE
};

class AppUI {
public:
    static constexpr int SCREEN_W = DisplayZelda::WIDTH;   // 1264
    static constexpr int SCREEN_H = DisplayZelda::HEIGHT;  // 1680
    static constexpr int NAV_H = 90;
    static constexpr int TOP_H = 80;

private:
    DisplayZelda display_;
    InputKindle input_;
    Canvas canvas_{SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, nullptr};
    FontManager font_;
    StateManager state_;
    SourceManager sources_;
    CoverCache cover_cache_;
    ReaderPageCache reader_cache_;
    DownloadQueue download_queue_{sources_};

    std::atomic<bool> is_running_{true};
    std::atomic<bool> shutting_down_{false};
    ViewState current_view_ = ViewState::LIBRARY;
    ViewState detail_origin_view_ = ViewState::LIBRARY;
    ViewState reader_origin_view_ = ViewState::DETAIL;
    int selected_tab_ = 0; // 0: Library, 1: Recent, 2: Browse, 3: Downloads, 4: Settings

    // Manga data
    std::vector<UnifiedManga> library_mangas_;
    std::vector<UnifiedManga> browse_mangas_;
    UnifiedManga selected_manga_;
    std::vector<UnifiedChapter> selected_chapters_;

    // Library & Browse view state
    LibraryFilter library_filter_ = LibraryFilter::ALL;
    int library_page_ = 0;
    int browse_page_ = 0;
    int source_pill_offset_ = 0;
    static constexpr int BROWSE_GRID_PER_PAGE = 6; // 3x2 Tankobon cover grid

    // Chapter and reader state
    ChapterFilter chapter_filter_ = ChapterFilter::ALL;
    int current_chapter_idx_ = -1;
    int reader_current_page_ = 0;
    int reader_total_pages_ = 0;
    std::vector<std::string> reader_page_files_;
    extractor::PageCache page_cache_{"/mnt/us/manga/.cache/pages"};
    bool reader_show_overlay_ = false;
    int reader_page_turn_counter_ = 0;
    bool reader_zoom_2x_ = false;
    float reader_pan_x_ = 0.5f;
    float reader_pan_y_ = 0.5f;
    bool waiting_for_chapter_open_ = false;
    std::string waiting_chapter_num_;

    // Chapter list navigation & sorting
    int chapter_page_ = 0;
    bool detail_sort_descending_ = true;
    static constexpr int CHAPTERS_PER_PAGE = 10;

    // Recent & Downloads pagination
    int recent_page_ = 0;
    int downloads_page_ = 0;
    static constexpr int RECENT_PER_PAGE = 8;
    static constexpr int DOWNLOADS_PER_PAGE = 8;

    // Search and Keyboard state
    enum class KeyboardTarget {
        SEARCH = 0,
        REPO_URL = 1
    };
    KeyboardTarget kb_target_ = KeyboardTarget::SEARCH;
    bool show_keyboard_ = false;
    bool kb_shift_active_ = false;
    bool kb_symbols_mode_ = false;
    std::string search_query_;
    std::string status_banner_;

    // Sources and Extension Catalog state
    enum class SourcesSubTab {
        INSTALLED = 0,
        AVAILABLE = 1,
        REPOSITORIES = 2
    };
    SourcesSubTab sources_sub_tab_ = SourcesSubTab::INSTALLED;
    std::vector<SourceInfo> available_aidoku_sources_;
    bool available_sources_loaded_ = false;
    int available_sources_page_ = 0;
    static constexpr int AVAILABLE_PER_PAGE = 7;
    std::string repo_url_input_;

    // Touch gesture tracking for swipes & 2x zoom drag
    int touch_down_x_ = 0;
    int touch_down_y_ = 0;
    int touch_last_x_ = 0;
    int touch_last_y_ = 0;
    int touch_max_move_ = 0;
    bool touch_is_down_ = false;

    // Library Removal Modal state
    bool show_remove_modal_ = false;
    UnifiedManga manga_to_remove_;
    int remove_modal_dl_count_ = 0;

    // Non-blocking Async Network Task Engine (search, popular, chapter refresh, library sync)
    std::mutex async_mu_;
    std::thread async_thread_;
    std::atomic<uint64_t> async_seq_{0};
    std::atomic<bool> async_busy_{false};
    std::string async_label_;
    std::function<void()> pending_ui_callback_;

public:
    AppUI() = default;

    ~AppUI() {
        shutdownWorkers();
    }

    void shutdownWorkers() {
        shutting_down_ = true;
        async_seq_.fetch_add(1);
        cover_cache_.stop();
        reader_cache_.stop();
        download_queue_.stop();
        if (async_thread_.joinable()) {
            async_thread_.join();
        }
        input_.stop();
    }

    bool init() {
        if (!std::getenv("RM2MANGA_HTTP_HELPER")) {
            ::setenv("RM2MANGA_HTTP_HELPER", "/mnt/us/extensions/rm2manga/bin/fetch.sh", 1);
        }

        std::error_code ec;
        fs::create_directories("/mnt/us/manga/.cache/pages", ec);
        fs::create_directories("/mnt/us/manga/.cache/tmp", ec);
        fs::create_directories("/mnt/us/manga/.cache/covers", ec);

        // Clean up any stale rm2manga files from /tmp to protect Kindle's 64MB RAM tmpfs
        if (fs::exists("/tmp", ec)) {
            for (const auto& p : fs::directory_iterator("/tmp", ec)) {
                std::string fname = p.path().filename().string();
                if (fname.rfind("rm2manga", 0) == 0) {
                    fs::remove_all(p.path(), ec);
                }
            }
        }

        if (!display_.init()) return false;
        canvas_ = Canvas(SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, display_.getBuffer());
        if (!input_.start()) return false;

        font_.init();
        state_.load();
        input_.setRotation(state_.settings().touch_rotation);

        // Start background caches & workers wired to zero-CPU event wakeup
        reader_cache_.start(SCREEN_W, SCREEN_H, DisplayZelda::STRIDE);
        cover_cache_.start([this]() { input_.wake(); });
        download_queue_.start([this]() { input_.wake(); });

        loadLibraryFromDisk();
        renderAll();
        display_.refreshFull(WAVEFORM_GC16);
        return true;
    }

    void run() {
        InputEvent ev;
        while (is_running_) {
            if (input_.waitForEvent(ev, 300)) {
                if (ev.type == EventType::ASYNC_WAKE) {
                    processAsyncWake();
                } else {
                    handleInput(ev);
                }
            }
        }
    }

    void quit() {
        is_running_ = false;
        state_.save();
        shutdownWorkers();
    }

private:
    void launchAsyncTask(const std::string& label, std::function<std::function<void()>(uint64_t)> workFn) {
        uint64_t seq = async_seq_.fetch_add(1) + 1;
        async_busy_ = true;
        {
            std::lock_guard<std::mutex> lock(async_mu_);
            async_label_ = label;
        }
        if (async_thread_.joinable()) {
            async_thread_.detach();
        }
        async_thread_ = std::thread([this, seq, workFn = std::move(workFn)]() {
            auto uiContinuation = workFn(seq);
            if (shutting_down_.load() || async_seq_.load() != seq) return;
            {
                std::lock_guard<std::mutex> lock(async_mu_);
                pending_ui_callback_ = std::move(uiContinuation);
                async_busy_ = false;
                async_label_.clear();
            }
            input_.wake();
        });
    }

    void cancelAsyncTask() {
        async_seq_.fetch_add(1);
        async_busy_ = false;
        {
            std::lock_guard<std::mutex> lock(async_mu_);
            async_label_.clear();
            pending_ui_callback_ = nullptr;
        }
        status_banner_ = "Cancelled request.";
        renderAll();
        display_.refreshView();
    }

    void processAsyncWake() {
        bool needFullViewRefresh = false;

        // 1. Run completed async network UI continuation if any
        std::function<void()> uiCb;
        {
            std::lock_guard<std::mutex> lock(async_mu_);
            uiCb = std::move(pending_ui_callback_);
            pending_ui_callback_ = nullptr;
        }
        if (uiCb) {
            uiCb();
            needFullViewRefresh = true;
        }

        // 2. Drain completed chapter downloads
        DownloadResult dlRes;
        while (download_queue_.popCompleted(dlRes)) {
            if (dlRes.ok) {
                // Reconcile selected_chapters_ if matching manga
                if (dlRes.manga.title == selected_manga_.title) {
                    for (auto& ch : selected_chapters_) {
                        if ((!dlRes.chapter.id.empty() && ch.id == dlRes.chapter.id) ||
                            (!dlRes.chapter.chapterNum.empty() && ch.chapterNum == dlRes.chapter.chapterNum)) {
                            ch.isDownloaded = true;
                            ch.localPath = dlRes.cbz_path;
                        }
                    }
                }
                loadLibraryFromDisk();
                if (!dlRes.silent_preload) {
                    status_banner_ = "✓ Saved Ch. " + dlRes.chapter.chapterNum + " (" + dlRes.manga.title + ")";
                }

                if (dlRes.auto_open && !dlRes.cancelled) {
                    waiting_for_chapter_open_ = false;
                    // Open freshly downloaded CBZ immediately
                    if (selected_manga_.title != dlRes.manga.title) {
                        selected_manga_ = dlRes.manga;
                        loadMangaChapters(selected_manga_, false);
                    }
                    int foundIdx = -1;
                    for (size_t i = 0; i < selected_chapters_.size(); ++i) {
                        if ((!dlRes.chapter.id.empty() && selected_chapters_[i].id == dlRes.chapter.id) ||
                            selected_chapters_[i].localPath == dlRes.cbz_path ||
                            selected_chapters_[i].chapterNum == dlRes.chapter.chapterNum) {
                            foundIdx = static_cast<int>(i);
                            break;
                        }
                    }
                    if (foundIdx >= 0) {
                        openChapter(foundIdx, false, dlRes.start_page);
                        return;
                    }
                }
            } else if (!dlRes.cancelled && !dlRes.silent_preload) {
                waiting_for_chapter_open_ = false;
                status_banner_ = "Download failed (Ch. " + dlRes.chapter.chapterNum + "): " + dlRes.error;
                if (current_view_ == ViewState::READER) {
                    renderReader();
                    renderReaderToast("Failed Ch. " + dlRes.chapter.chapterNum);
                    return;
                }
            } else if (dlRes.cancelled) {
                waiting_for_chapter_open_ = false;
                status_banner_ = "Download cancelled.";
            }
            needFullViewRefresh = true;
        }

        // 3. Refresh active screen smoothly (covers, progress bars, queue updates)
        if (current_view_ == ViewState::READER) {
            if (waiting_for_chapter_open_) {
                auto snap = download_queue_.snapshot();
                if (snap.active) {
                    renderReaderDownloadProgress(snap.status, snap.pct);
                }
            }
            return;
        }

        if (needFullViewRefresh || current_view_ == ViewState::LIBRARY ||
            current_view_ == ViewState::BROWSE || current_view_ == ViewState::DETAIL ||
            current_view_ == ViewState::DOWNLOADS || current_view_ == ViewState::RECENT) {
            renderAll();
            display_.refreshView();
        }
    }

    int countDownloadedChapters(const std::string& mangaTitle) {
        if (mangaTitle.empty()) return 0;
        std::string dirPath = "/mnt/us/manga/" + mangaTitle;
        std::error_code ec;
        if (!fs::exists(dirPath, ec)) return 0;
        int count = 0;
        for (const auto& f : fs::directory_iterator(dirPath, ec)) {
            if (f.path().extension() == ".cbz") count++;
        }
        return count;
    }

    void loadLibraryFromDisk() {
        library_mangas_.clear();
        std::map<std::string, UnifiedManga> merged;

        for (const auto& sm : state_.getLibraryMangas()) {
            merged[sm.title] = sm;
        }

        std::string manga_root = "/mnt/us/manga";
        std::error_code ec;

        if (fs::exists(manga_root, ec)) {
            for (const auto& entry : fs::directory_iterator(manga_root, ec)) {
                if (!entry.is_directory(ec)) continue;
                std::string dirName = entry.path().filename().string();
                if (dirName.empty() || dirName[0] == '.' || dirName == "data") continue;
                if (state_.isUnpinned(dirName)) continue;

                int count = 0;
                fs::path firstCbz;
                for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                    if (f.path().extension() == ".cbz") {
                        if (firstCbz.empty()) firstCbz = f.path();
                        count++;
                    }
                }

                if (count == 0 && merged.find(dirName) == merged.end()) continue;

                UnifiedManga& m = merged[dirName];
                if (m.title.empty()) {
                    m.title = dirName;
                    m.id = dirName;
                }
                m.downloaded_count = count;

                fs::path localCover = entry.path() / "cover.jpg";
                fs::path localCoverPng = entry.path() / "cover.png";
                if (fs::exists(localCoverPng, ec) && fs::file_size(localCoverPng, ec) > 64) {
                    m.cover_url = localCoverPng.string();
                } else if (fs::exists(localCover, ec) && fs::file_size(localCover, ec) > 64) {
                    m.cover_url = localCover.string();
                } else if (m.cover_url.empty() && !firstCbz.empty()) {
                    // Extract cover once via native C++ PageCache instead of spawning shell pipelines
                    std::string err;
                    std::set<std::string> pinned;
                    auto pages = page_cache_.extractCbz(dirName + "_cover", firstCbz, pinned, &err);
                    if (!pages.empty()) {
                        std::string p0 = (pages[0].rfind("file://", 0) == 0) ? pages[0].substr(7) : pages[0];
                        fs::copy_file(p0, localCover, fs::copy_options::overwrite_existing, ec);
                        if (!ec) m.cover_url = localCover.string();
                    }
                }
            }
        }

        for (auto& kv : merged) {
            kv.second.unread_count = state_.countUnreadCachedChapters(kv.second);
            library_mangas_.push_back(std::move(kv.second));
        }

        std::sort(library_mangas_.begin(), library_mangas_.end(), [](const UnifiedManga& a, const UnifiedManga& b) {
            return util::naturalLess(a.title, b.title);
        });
    }

    std::vector<UnifiedManga> getFilteredLibrary() const {
        std::vector<UnifiedManga> out;
        out.reserve(library_mangas_.size());
        for (const auto& m : library_mangas_) {
            if (library_filter_ == LibraryFilter::UNREAD && m.unread_count <= 0) continue;
            if (library_filter_ == LibraryFilter::OFFLINE && m.downloaded_count <= 0) continue;
            out.push_back(m);
        }
        return out;
    }

    std::vector<int> getFilteredChapterIndices() const {
        std::vector<int> indices;
        indices.reserve(selected_chapters_.size());
        for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
            const auto& ch = selected_chapters_[i];
            if (chapter_filter_ == ChapterFilter::UNREAD && ch.isRead) continue;
            if (chapter_filter_ == ChapterFilter::OFFLINE && !ch.isDownloaded) continue;
            indices.push_back(i);
        }
        return indices;
    }

    // --- ASYNC NETWORK OPERATIONS ---
    void fetchPopularAsync() {
        std::string srcId = sources_.getActiveSourceId();
        std::string srcName = sources_.getSourceDisplayName(srcId);
        status_banner_ = "Loading popular titles from " + srcName + "...";
        renderAll();
        display_.refreshView();

        launchAsyncTask("Loading " + srcName + "...", [this, srcId, srcName](uint64_t seq) -> std::function<void()> {
            std::string err;
            auto results = sources_.getPopularForSource(srcId, &err);
            return [this, seq, results = std::move(results), err, srcName]() mutable {
                if (async_seq_.load() != seq) return;
                browse_mangas_ = std::move(results);
                browse_page_ = 0;
                if (!err.empty() && browse_mangas_.empty()) {
                    status_banner_ = "Error (" + srcName + "): " + err;
                } else {
                    status_banner_.clear();
                }
            };
        });
    }

    void performSearchAsync(const std::string& query) {
        show_keyboard_ = false;
        std::string srcId = sources_.getActiveSourceId();
        std::string srcName = sources_.getSourceDisplayName(srcId);
        status_banner_ = "Searching " + srcName + " for \"" + query + "\"...";
        renderAll();
        display_.refreshView();

        launchAsyncTask("Searching " + srcName + "...", [this, srcId, query](uint64_t seq) -> std::function<void()> {
            std::string err;
            auto results = sources_.searchSource(srcId, query, &err);
            return [this, seq, results = std::move(results), err, query]() mutable {
                if (async_seq_.load() != seq) return;
                browse_mangas_ = std::move(results);
                browse_page_ = 0;
                if (!err.empty() && browse_mangas_.empty()) {
                    status_banner_ = "Search error: " + err;
                } else if (browse_mangas_.empty()) {
                    status_banner_ = "No results found for \"" + query + "\"";
                } else {
                    status_banner_.clear();
                }
            };
        });
    }

    void fetchChaptersAsync(const UnifiedManga& manga) {
        std::string srcId = manga.source_id.empty() ? sources_.getActiveSourceId() : manga.source_id;
        if (srcId == "local" || manga.id.empty()) return;

        status_banner_ = "Refreshing chapters from " + sources_.getSourceDisplayName(srcId) + "...";
        renderAll();
        display_.refreshView();

        launchAsyncTask("Fetching chapters...", [this, manga, srcId](uint64_t seq) -> std::function<void()> {
            std::string err;
            auto fetched = sources_.getChaptersForSource(srcId, manga.id, &err);
            return [this, seq, manga, srcId, fetched = std::move(fetched), err]() mutable {
                if (async_seq_.load() != seq) return;
                if (!fetched.empty()) {
                    selected_chapters_ = std::move(fetched);
                    reconcileChaptersLocalAndRead(manga);
                    sortChapters();
                    state_.saveChaptersCache(manga.title, srcId, manga.id, selected_chapters_);
                    loadLibraryFromDisk();
                    status_banner_ = "✓ Loaded " + std::to_string(selected_chapters_.size()) + " chapters";
                } else if (!err.empty()) {
                    status_banner_ = "Refresh error: " + err;
                } else {
                    status_banner_ = "No remote chapters returned.";
                }
            };
        });
    }

    void syncLibraryUpdatesAsync() {
        if (library_mangas_.empty()) return;
        std::vector<UnifiedManga> targets = library_mangas_;
        status_banner_ = "Syncing library chapters...";
        renderAll();
        display_.refreshView();

        launchAsyncTask("Syncing library...", [this, targets](uint64_t seq) -> std::function<void()> {
            int updatedCount = 0;
            for (const auto& m : targets) {
                if (shutting_down_.load() || async_seq_.load() != seq) break;
                if (m.source_id.empty() || m.source_id == "local" || m.id.empty()) continue;
                std::string err;
                auto chs = sources_.getChaptersForSource(m.source_id, m.id, &err);
                if (!chs.empty()) {
                    state_.saveChaptersCache(m.title, m.source_id, m.id, chs);
                    ++updatedCount;
                }
            }
            return [this, seq, updatedCount]() {
                if (async_seq_.load() != seq) return;
                loadLibraryFromDisk();
                status_banner_ = "✓ Synced " + std::to_string(updatedCount) + " library series";
            };
        });
    }

    // --- INPUT & GESTURE ROUTING ---
    void handlePageStep(int delta) {
        if (current_view_ == ViewState::READER) {
            bool inv = state_.settings().button_inverted;
            if ((delta > 0 && !inv) || (delta < 0 && inv)) nextPage();
            else prevPage();
            return;
        }
        if (current_view_ == ViewState::LIBRARY) {
            auto filtered = getFilteredLibrary();
            int perPage = state_.settings().library_grid_view ? 6 : 8;
            int totalPages = std::max(1, (static_cast<int>(filtered.size()) + perPage - 1) / perPage);
            int nextP = std::max(0, std::min(totalPages - 1, library_page_ + delta));
            if (nextP != library_page_) {
                library_page_ = nextP;
                renderAll();
                display_.refreshView();
            }
        } else if (current_view_ == ViewState::BROWSE) {
            if (delta > 0) nextBrowsePage();
            else prevBrowsePage();
        } else if (current_view_ == ViewState::DETAIL) {
            if (delta > 0) nextChapterPage();
            else prevChapterPage();
        } else if (current_view_ == ViewState::RECENT) {
            auto recent = state_.getRecentHistory();
            int totalPages = std::max(1, (static_cast<int>(recent.size()) + RECENT_PER_PAGE - 1) / RECENT_PER_PAGE);
            int nextP = std::max(0, std::min(totalPages - 1, recent_page_ + delta));
            if (nextP != recent_page_) {
                recent_page_ = nextP;
                renderAll();
                display_.refreshView();
            }
        } else if (current_view_ == ViewState::DOWNLOADS) {
            auto offline = getOfflineCbzList();
            int totalPages = std::max(1, (static_cast<int>(offline.size()) + DOWNLOADS_PER_PAGE - 1) / DOWNLOADS_PER_PAGE);
            int nextP = std::max(0, std::min(totalPages - 1, downloads_page_ + delta));
            if (nextP != downloads_page_) {
                downloads_page_ = nextP;
                renderAll();
                display_.refreshView();
            }
        } else if (current_view_ == ViewState::SOURCES && sources_sub_tab_ == SourcesSubTab::AVAILABLE) {
            int totalPages = std::max(1, (static_cast<int>(available_aidoku_sources_.size()) + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE);
            int nextP = std::max(0, std::min(totalPages - 1, available_sources_page_ + delta));
            if (nextP != available_sources_page_) {
                available_sources_page_ = nextP;
                renderAll();
                display_.refreshView();
            }
        }
    }

    void handleInput(const InputEvent& ev) {
        if (ev.type == EventType::KEY_NEXT_PAGE) {
            handlePageStep(+1);
            return;
        }
        if (ev.type == EventType::KEY_PREV_PAGE) {
            handlePageStep(-1);
            return;
        }
        if (ev.type == EventType::TOUCH_DOWN) {
            handleTouchDown(ev.x, ev.y);
            return;
        }
        if (ev.type == EventType::TOUCH_MOVE) {
            touch_last_x_ = ev.x;
            touch_last_y_ = ev.y;
            int dist = static_cast<int>(std::hypot(ev.x - touch_down_x_, ev.y - touch_down_y_));
            if (dist > touch_max_move_) touch_max_move_ = dist;
            return;
        }
        if (ev.type == EventType::TOUCH_UP) {
            touch_is_down_ = false;
            int dx = ev.x - touch_down_x_;
            int dy = ev.y - touch_down_y_;
            int dist = std::max(touch_max_move_, static_cast<int>(std::hypot(dx, dy)));

            // Handle 2x Zoom Pan or Swipe Gestures when finger dragged significantly
            if (!show_keyboard_ && !show_remove_modal_ && dist >= 55) {
                if (current_view_ == ViewState::READER && reader_zoom_2x_ && !reader_show_overlay_) {
                    reader_pan_x_ = std::max(0.0f, std::min(1.0f, reader_pan_x_ - static_cast<float>(dx) / (SCREEN_W * 0.8f)));
                    reader_pan_y_ = std::max(0.0f, std::min(1.0f, reader_pan_y_ - static_cast<float>(dy) / (SCREEN_H * 0.8f)));
                    renderReader();
                    display_.refreshView();
                    return;
                }
                if (current_view_ == ViewState::READER && std::abs(dx) > std::abs(dy) * 1.15) {
                    bool isRTL = (state_.settings().reading_direction == "RTL");
                    if (reader_show_overlay_) reader_show_overlay_ = false;
                    // Natural manga/book page swipe matching main's ReaderView.qml
                    bool goNext = isRTL ? (dx > 0) : (dx < 0);
                    if (goNext) nextPage();
                    else prevPage();
                    return;
                }
                // Horizontal or vertical swipe pagination across list/grid views
                if (current_view_ != ViewState::READER) {
                    if (std::abs(dy) >= std::abs(dx)) {
                        // Swipe up -> next page, swipe down -> prev page
                        handlePageStep(dy < 0 ? +1 : -1);
                    } else {
                        // Swipe left -> next page, swipe right -> prev page
                        handlePageStep(dx < 0 ? +1 : -1);
                    }
                    return;
                }
            }

            // Deadzone guard in Reader so accidental short drags (28..54px) don't misfire page taps
            if (current_view_ == ViewState::READER && !reader_show_overlay_ && dist >= 30) {
                return;
            }

            handleTouch(ev.x, ev.y);
            return;
        }
    }

    void handleTouchDown(int x, int y) {
        touch_down_x_ = x;
        touch_down_y_ = y;
        touch_last_x_ = x;
        touch_last_y_ = y;
        touch_max_move_ = 0;
        touch_is_down_ = true;

        if (show_keyboard_ && y >= KB_START_Y && y < KB_START_Y + 4 * KB_ROW_H) {
            highlightKeyAt(x, y, true);
            return;
        }

        if (y >= SCREEN_H - NAV_H && current_view_ != ViewState::READER) {
            int tab_w = SCREEN_W / 5;
            int tab = x / tab_w;
            if (tab >= 0 && tab < 5) {
                canvas_.fillRect(tab * tab_w + 5, SCREEN_H - NAV_H + 5, tab_w - 10, NAV_H - 10, 0x40);
                display_.update(tab * tab_w + 5, SCREEN_H - NAV_H + 5, tab_w - 10, NAV_H - 10, WAVEFORM_DU, UPDATE_PARTIAL);
            }
            return;
        }

        if (y >= 10 && y <= TOP_H - 10 && current_view_ != ViewState::READER && x >= SCREEN_W - 160) {
            canvas_.fillRect(SCREEN_W - 157, 15, 137, 50, 0x40);
            display_.update(SCREEN_W - 157, 15, 137, 50, WAVEFORM_DU, UPDATE_PARTIAL);
            return;
        }
    }

    void handleTouch(int x, int y) {
        if (show_remove_modal_) {
            handleRemoveModalTouch(x, y);
            return;
        }

        // Async loading banner [ Cancel ] tap check across views
        if (async_busy_.load() && current_view_ != ViewState::READER) {
            int bannerY = (current_view_ == ViewState::DETAIL) ? 338 : 150;
            if (y >= bannerY - 4 && y <= bannerY + 52 && x >= SCREEN_W - 195 && x <= SCREEN_W - 25) {
                cancelAsyncTask();
                return;
            }
        }

        // Bottom Navigation Bar touch (outside Reader)
        if (y >= SCREEN_H - NAV_H && current_view_ != ViewState::READER) {
            int tab_w = SCREEN_W / 5;
            int tapped_tab = x / tab_w;
            if (tapped_tab >= 0 && tapped_tab < 5) {
                input_.clear();
                show_keyboard_ = false;
                selected_tab_ = tapped_tab;
                status_banner_.clear();

                if (selected_tab_ == 0) {
                    current_view_ = ViewState::LIBRARY;
                    loadLibraryFromDisk();
                } else if (selected_tab_ == 1) {
                    current_view_ = ViewState::RECENT;
                } else if (selected_tab_ == 2) {
                    current_view_ = ViewState::BROWSE;
                    if (browse_mangas_.empty() && !async_busy_.load()) {
                        fetchPopularAsync();
                        return;
                    }
                } else if (selected_tab_ == 3) {
                    current_view_ = ViewState::DOWNLOADS;
                    loadLibraryFromDisk();
                } else if (selected_tab_ == 4) {
                    current_view_ = ViewState::SETTINGS;
                }
                renderAll();
                display_.refreshView();
            }
            return;
        }

        // Keyboard touch if active
        if (show_keyboard_) {
            int kbTop = (kb_target_ == KeyboardTarget::REPO_URL) ? (KB_Y - 62) : KB_Y;
            if (y >= kbTop && y < SCREEN_H - NAV_H) {
                handleKeyboardTouch(x, y);
                return;
            }
            if (current_view_ == ViewState::BROWSE && y >= 10 && y <= TOP_H) {
                if (x >= SCREEN_W - 160) { quit(); return; }
                if (x >= 20 && x <= 675) {
                    // Inline ✕ clear button inside search box
                    if (!search_query_.empty() && x >= 620) {
                        search_query_.clear();
                        renderSearchBarInput();
                    }
                    return;
                }
                if (x >= 685 && x <= 803) {
                    show_keyboard_ = false;
                    if (!search_query_.empty()) performSearchAsync(search_query_);
                    else { renderAll(); display_.refreshView(); }
                    return;
                }
            }
            show_keyboard_ = false;
            renderAll();
            display_.refreshView();
            return;
        }

        if (current_view_ == ViewState::LIBRARY) handleLibraryTouch(x, y);
        else if (current_view_ == ViewState::RECENT) handleRecentTouch(x, y);
        else if (current_view_ == ViewState::BROWSE) handleBrowseTouch(x, y);
        else if (current_view_ == ViewState::DETAIL) handleDetailTouch(x, y);
        else if (current_view_ == ViewState::DOWNLOADS) handleDownloadsTouch(x, y);
        else if (current_view_ == ViewState::SETTINGS) handleSettingsTouch(x, y);
        else if (current_view_ == ViewState::SOURCES) handleSourcesTouch(x, y);
        else if (current_view_ == ViewState::READER) handleReaderTouch(x, y);
    }

    void openMangaDetail(const UnifiedManga& manga, ViewState origin) {
        detail_origin_view_ = origin;
        selected_manga_ = manga;
        if (!selected_manga_.source_id.empty() && selected_manga_.source_id != "local" && selected_manga_.source_id != "all") {
            sources_.setActiveSourceId(selected_manga_.source_id);
        }
        chapter_filter_ = ChapterFilter::ALL;
        loadMangaChapters(selected_manga_, true);
        current_view_ = ViewState::DETAIL;
        renderAll();
        display_.refreshView();
    }

    std::string formatHeroInlineLabel(const MangaProgress& hero, int maxTextW) const {
        std::string chap = hero.chapter_title;
        if (!hero.manga_title.empty() && chap.size() >= hero.manga_title.size()) {
            bool prefixMatch = true;
            for (size_t i = 0; i < hero.manga_title.size(); ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(chap[i])));
                char b = static_cast<char>(std::tolower(static_cast<unsigned char>(hero.manga_title[i])));
                if (a != b) { prefixMatch = false; break; }
            }
            if (prefixMatch) {
                chap = chap.substr(hero.manga_title.size());
                while (!chap.empty() && (chap.front() == ' ' || chap.front() == '-' || chap.front() == ':')) {
                    chap.erase(chap.begin());
                }
            }
        }
        if (chap.rfind("Chapter ", 0) == 0 || chap.rfind("chapter ", 0) == 0) {
            chap = "Ch." + chap.substr(8);
        }
        size_t colonPos = chap.find(':');
        if (colonPos != std::string::npos && colonPos > 2) {
            chap = chap.substr(0, colonPos);
        }
        std::string shortTitle = font_.elideText(hero.manga_title, 19, 260, true);
        std::string shortChap = font_.elideText(chap, 19, 175, true);
        std::string label = "▶ " + shortTitle + (shortChap.empty() ? "" : (" · " + shortChap));
        return font_.elideText(label, 19, maxTextW, true);
    }

    void handleLibraryTouch(int x, int y) {
        // Header buttons: [↻ Sync] [Grid/List] [✕ Exit]
        if (y >= 10 && y <= TOP_H) {
            if (x >= SCREEN_W - 160) { quit(); return; }
            if (x >= SCREEN_W - 315 && x < SCREEN_W - 165) {
                state_.settings().library_grid_view = !state_.settings().library_grid_view;
                library_page_ = 0;
                state_.save();
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= SCREEN_W - 465 && x < SCREEN_W - 320) {
                syncLibraryUpdatesAsync();
                return;
            }
        }

        MangaProgress hero;
        bool hasHero = state_.getHeroManga(hero);
        int filterY = TOP_H + 12;

        // Filter pills + inline right-aligned [ ▶ Resume ] pill (y: 92..138)
        if (y >= filterY - 4 && y <= filterY + 50) {
            if (x >= 28 && x < 180) {
                library_filter_ = LibraryFilter::ALL;
                library_page_ = 0;
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= 182 && x < 334) {
                library_filter_ = LibraryFilter::UNREAD;
                library_page_ = 0;
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= 336 && x < 495) {
                library_filter_ = LibraryFilter::OFFLINE;
                library_page_ = 0;
                renderAll();
                display_.refreshView();
                return;
            }
            if (hasHero) {
                std::string resumeLbl = formatHeroInlineLabel(hero, 490);
                int resumeW = std::max(210, std::min(535, font_.measureText(resumeLbl, 19, true) + 38));
                int resumeX = SCREEN_W - 28 - resumeW;
                if (x >= resumeX - 8 && x <= SCREEN_W - 20) {
                    reader_origin_view_ = ViewState::LIBRARY;
                    resumeRecent(hero, ViewState::LIBRARY);
                    return;
                }
            }
            return;
        }

        auto filtered = getFilteredLibrary();
        bool gridMode = state_.settings().library_grid_view;
        int perPage = gridMode ? 6 : 8;
        int totalPages = std::max(1, (static_cast<int>(filtered.size()) + perPage - 1) / perPage);
        if (library_page_ >= totalPages) library_page_ = totalPages - 1;

        // Bottom pagination bar
        int pagY = SCREEN_H - NAV_H - 60;
        if (totalPages > 1 && y >= pagY - 6 && y <= pagY + 56) {
            if (x <= 230) { handlePageStep(-1); return; }
            if (x >= SCREEN_W - 230) { handlePageStep(+1); return; }
        }

        int contentY = async_busy_.load() ? 210 : (!status_banner_.empty() ? 194 : 160);
        int startIdx = library_page_ * perPage;
        int endIdx = std::min(static_cast<int>(filtered.size()), startIdx + perPage);

        if (gridMode) {
            int coverH = GridCardMetrics::COVER_H;
            int rowStride = coverH + GridCardMetrics::TITLE_AREA_H + 14;
            for (int i = startIdx; i < endIdx; ++i) {
                int rel = i - startIdx;
                int col = rel % GridCardMetrics::COLS;
                int row = rel / GridCardMetrics::COLS;
                int cx = GridCardMetrics::colX(col);
                int cy = contentY + row * rowStride;
                if (x >= cx && x <= cx + GridCardMetrics::CARD_W && y >= cy && y <= cy + coverH + GridCardMetrics::TITLE_AREA_H) {
                    // Top-right [✕] remove badge
                    if (x >= cx + GridCardMetrics::CARD_W - 56 && y <= cy + 54) {
                        manga_to_remove_ = filtered[i];
                        remove_modal_dl_count_ = countDownloadedChapters(manga_to_remove_.title);
                        show_remove_modal_ = true;
                        renderAll();
                        display_.refreshView();
                        return;
                    }
                    openMangaDetail(filtered[i], ViewState::LIBRARY);
                    return;
                }
            }
        } else {
            int itemH = 128, rowStride = 140;
            for (int i = startIdx; i < endIdx; ++i) {
                int itemY = contentY + (i - startIdx) * rowStride;
                if (y >= itemY && y < itemY + itemH) {
                    if (x >= SCREEN_W - 335 && x < SCREEN_W - 185) {
                        manga_to_remove_ = filtered[i];
                        remove_modal_dl_count_ = countDownloadedChapters(manga_to_remove_.title);
                        show_remove_modal_ = true;
                        renderAll();
                        display_.refreshView();
                        return;
                    }
                    openMangaDetail(filtered[i], ViewState::LIBRARY);
                    return;
                }
            }
        }
    }

    void handleRecentTouch(int x, int y) {
        auto recent = state_.getRecentHistory();
        if (y >= 10 && y <= TOP_H) {
            if (x >= SCREEN_W - 160) { quit(); return; }
            if (!recent.empty() && x >= SCREEN_W - 315 && x < SCREEN_W - 165) {
                state_.clearAllHistory();
                recent_page_ = 0;
                renderAll();
                display_.refreshView();
                return;
            }
        }

        if (recent.empty()) {
            int btnW = 280, btnH = 65, btnX = (SCREEN_W - btnW) / 2, btnY = 710;
            if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
                selected_tab_ = 2;
                current_view_ = ViewState::BROWSE;
                if (browse_mangas_.empty()) fetchPopularAsync();
                else { renderAll(); display_.refreshView(); }
            }
            return;
        }

        int totalPages = std::max(1, (static_cast<int>(recent.size()) + RECENT_PER_PAGE - 1) / RECENT_PER_PAGE);
        int pagY = SCREEN_H - NAV_H - 60;
        if (totalPages > 1 && y >= pagY - 6 && y <= pagY + 56) {
            if (x <= 230) { handlePageStep(-1); return; }
            if (x >= SCREEN_W - 230) { handlePageStep(+1); return; }
        }

        int yStart = TOP_H + 52;
        int itemH = 132, rowStride = 144;
        int startIdx = recent_page_ * RECENT_PER_PAGE;
        int endIdx = std::min(static_cast<int>(recent.size()), startIdx + RECENT_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int itemY = yStart + (i - startIdx) * rowStride;
            if (y >= itemY && y < itemY + itemH) {
                const auto& prog = recent[i];
                int btnW = 155, btnX = SCREEN_W - btnW - 42;
                if (x >= btnX && y >= itemY + 68) {
                    UnifiedManga m;
                    m.title = prog.manga_title;
                    m.id = prog.manga_id.empty() ? prog.manga_title : prog.manga_id;
                    m.source_id = prog.source_id;
                    m.cover_url = prog.cover_url;
                    for (const auto& lm : library_mangas_) {
                        if (lm.title == prog.manga_title) { m = lm; break; }
                    }
                    openMangaDetail(m, ViewState::RECENT);
                } else {
                    resumeRecent(prog, ViewState::RECENT);
                }
                return;
            }
        }
    }

    void handleBrowseTouch(int x, int y) {
        // Top Header: [Search box + ✕] [Search] [✦ Popular] [⚙ Sources] [✕ Exit]
        if (y >= 10 && y <= TOP_H - 5) {
            if (x >= SCREEN_W - 160) { quit(); return; }
            if (x >= 955 && x < SCREEN_W - 160) {
                show_keyboard_ = false;
                current_view_ = ViewState::SOURCES;
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= 813 && x < 950) {
                show_keyboard_ = false;
                search_query_.clear();
                fetchPopularAsync();
                return;
            }
            if (x >= 685 && x < 808) {
                if (!search_query_.empty()) performSearchAsync(search_query_);
                else {
                    kb_target_ = KeyboardTarget::SEARCH;
                    show_keyboard_ = true;
                    renderAll();
                    display_.refreshView();
                }
                return;
            }
            if (x >= 20 && x <= 675) {
                if (!search_query_.empty() && x >= 620) {
                    search_query_.clear();
                    fetchPopularAsync();
                    return;
                }
                kb_target_ = KeyboardTarget::SEARCH;
                show_keyboard_ = true;
                renderAll();
                display_.refreshView();
                return;
            }
        }

        // Scrollable Source Switcher Strip (All Sources + installed sources): y: 90..146
        if (y >= 88 && y <= 148) {
            std::vector<SourceInfo> pills;
            pills.push_back({"all", "All Sources", "", true, false, "", "all", ""});
            for (const auto& s : sources_.getSources()) pills.push_back(s);

            if (source_pill_offset_ > 0 && x < 72) {
                source_pill_offset_ = std::max(0, source_pill_offset_ - 1);
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= SCREEN_W - 72 && source_pill_offset_ + 1 < static_cast<int>(pills.size())) {
                source_pill_offset_++;
                renderAll();
                display_.refreshView();
                return;
            }

            int pillX = (source_pill_offset_ > 0) ? 78 : 28;
            for (size_t i = static_cast<size_t>(source_pill_offset_); i < pills.size(); ++i) {
                int pw = std::max(145, std::min(220, font_.measureText(pills[i].name, 20, true) + 38));
                if (pillX + pw > SCREEN_W - 76) break;
                if (x >= pillX && x <= pillX + pw) {
                    sources_.setActiveSourceId(pills[i].id);
                    browse_page_ = 0;
                    if (!search_query_.empty()) performSearchAsync(search_query_);
                    else fetchPopularAsync();
                    return;
                }
                pillX += pw + 12;
            }
            return;
        }

        // Pagination bar at bottom
        int totalItems = static_cast<int>(browse_mangas_.size());
        int totalPages = std::max(1, (totalItems + BROWSE_GRID_PER_PAGE - 1) / BROWSE_GRID_PER_PAGE);
        int pagY = SCREEN_H - NAV_H - 60;
        if (totalPages > 1 && y >= pagY - 6 && y <= pagY + 56) {
            if (x <= 230) { prevBrowsePage(); return; }
            if (x >= SCREEN_W - 230) { nextBrowsePage(); return; }
        }

        // 3x2 Tankobon Cover Grid hitboxes
        int gridStartY = async_busy_.load() ? 210 : (!status_banner_.empty() ? 194 : 160);
        int coverH = GridCardMetrics::COVER_H;
        int rowStride = coverH + GridCardMetrics::TITLE_AREA_H + 14;
        int startIdx = browse_page_ * BROWSE_GRID_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + BROWSE_GRID_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int rel = i - startIdx;
            int col = rel % GridCardMetrics::COLS;
            int row = rel / GridCardMetrics::COLS;
            int cx = GridCardMetrics::colX(col);
            int cy = gridStartY + row * rowStride;
            if (show_keyboard_ && cy + coverH + GridCardMetrics::TITLE_AREA_H > KB_Y) continue;
            if (x >= cx && x <= cx + GridCardMetrics::CARD_W &&
                y >= cy && y <= cy + coverH + GridCardMetrics::TITLE_AREA_H) {
                const auto& m = browse_mangas_[i];
                // 1-tap [+ Pin / ✓ Saved] badge in top-right of card cover
                if (x >= cx + GridCardMetrics::CARD_W - 108 && y <= cy + 54) {
                    bool added = state_.toggleLibrary(m);
                    loadLibraryFromDisk();
                    status_banner_ = added ? ("✓ Added \"" + m.title + "\" to Library")
                                           : ("Removed \"" + m.title + "\" from Library");
                    renderAll();
                    display_.refreshView();
                    return;
                }
                openMangaDetail(m, ViewState::BROWSE);
                return;
            }
        }
    }

    void handleDetailTouch(int x, int y) {
        if (y >= 10 && y <= TOP_H) {
            if (x >= SCREEN_W - 160) { quit(); return; }
            if (x >= 20 && x <= 175) {
                current_view_ = detail_origin_view_;
                status_banner_.clear();
                renderAll();
                display_.refreshView();
                return;
            }
            // Top-bar [↻ Refresh] chapters button
            if (x >= SCREEN_W - 335 && x < SCREEN_W - 165) {
                fetchChaptersAsync(selected_manga_);
                return;
            }
        }

        // Action Buttons Row at btnY = TOP_H + 188 (268..322)
        int btnY = TOP_H + 188;
        if (y >= btnY - 4 && y <= btnY + 56) {
            // [ ▶ Continue / Read ]
            if (x >= 180 && x <= 450) {
                MangaProgress prog;
                if (state_.getProgress(selected_manga_.source_id, selected_manga_.id, selected_manga_.title, prog)) {
                    resumeRecent(prog, ViewState::DETAIL);
                } else {
                    int firstIdx = getLowestUnreadChapterIndex();
                    if (firstIdx >= 0) {
                        reader_origin_view_ = ViewState::DETAIL;
                        openChapter(firstIdx, true, 0);
                    }
                }
                return;
            }
            // [ + Library / ✓ In Library ]
            if (x >= 462 && x <= 692) {
                bool inLib = state_.isInLibrary(selected_manga_.source_id, selected_manga_.id, selected_manga_.title);
                if (inLib) {
                    manga_to_remove_ = selected_manga_;
                    remove_modal_dl_count_ = countDownloadedChapters(selected_manga_.title);
                    show_remove_modal_ = true;
                } else {
                    state_.toggleLibrary(selected_manga_);
                    selected_manga_.in_library = true;
                    status_banner_ = "✓ Added \"" + selected_manga_.title + "\" to Library";
                    loadLibraryFromDisk();
                }
                renderAll();
                display_.refreshView();
                return;
            }
            // [ ⬇ Next 5 ]
            if (x >= 704 && x <= 904) {
                enqueueBatchChapters(5);
                return;
            }
            // [ ⬇ All Unread ]
            if (x >= 916 && x <= 1136) {
                enqueueBatchChapters(9999);
                return;
            }
        }

        // Active download bar [ Cancel ] button if visible (y: 334..396)
        auto dlSnap = download_queue_.snapshot();
        if (dlSnap.active && y >= 334 && y <= 398 && x >= SCREEN_W - 185 && x <= SCREEN_W - 28) {
            download_queue_.cancelAll();
            return;
        }

        // Chapter Filter & Sort strip: chHeadY = 408
        int chHeadY = 408;
        if (y >= chHeadY - 4 && y <= chHeadY + 50) {
            if (x >= 28 && x < 168) { chapter_filter_ = ChapterFilter::ALL; chapter_page_ = 0; }
            else if (x >= 176 && x < 326) { chapter_filter_ = ChapterFilter::UNREAD; chapter_page_ = 0; }
            else if (x >= 334 && x < 484) { chapter_filter_ = ChapterFilter::OFFLINE; chapter_page_ = 0; }
            else if (x >= SCREEN_W - 255 && x <= SCREEN_W - 28) {
                detail_sort_descending_ = !detail_sort_descending_;
                sortChapters();
                chapter_page_ = 0;
            }
            renderAll();
            display_.refreshView();
            return;
        }

        // Pagination bar at bottom
        auto filteredIdx = getFilteredChapterIndices();
        int totalPages = std::max(1, (static_cast<int>(filteredIdx.size()) + CHAPTERS_PER_PAGE - 1) / CHAPTERS_PER_PAGE);
        int pagY = SCREEN_H - NAV_H - 60;
        if (totalPages > 1 && y >= pagY - 6 && y <= pagY + 56) {
            if (x <= 230) { prevChapterPage(); return; }
            if (x >= SCREEN_W - 230) { nextChapterPage(); return; }
        }

        // Chapter rows: start_y = 472, CHAPTERS_PER_PAGE = 10, stride = 103
        int startY = 472;
        int itemH = 92;
        int rowStride = 103;
        if (y >= startY && y < startY + CHAPTERS_PER_PAGE * rowStride) {
            int row = (y - startY) / rowStride;
            int filtPos = chapter_page_ * CHAPTERS_PER_PAGE + row;
            if (filtPos >= 0 && filtPos < static_cast<int>(filteredIdx.size())) {
                int realIdx = filteredIdx[filtPos];
                auto& ch = selected_chapters_[realIdx];

                // Left [● / ✓] read-status toggle hitbox (x: 28..92)
                if (x >= 28 && x <= 92) {
                    ch.isRead = state_.toggleChapterRead(selected_manga_.source_id, selected_manga_.id, selected_manga_.title, ch);
                    loadLibraryFromDisk();
                    renderAll();
                    display_.refreshView();
                    return;
                }

                // Right [⬇ Save] button hitbox (x >= SCREEN_W - 185) when not yet downloaded
                if (!ch.isDownloaded && x >= SCREEN_W - 185) {
                    enqueueSingleChapter(realIdx, false, 0);
                    return;
                }

                // Tap chapter row or [▶ Read] -> open in Reader!
                reader_origin_view_ = ViewState::DETAIL;
                openChapter(realIdx, true, 0);
                return;
            }
        }
    }

    void handleRemoveModalTouch(int x, int y) {
        int dialogW = 860;
        int dialogH = (remove_modal_dl_count_ > 0) ? 460 : 370;
        int dialogX = (SCREEN_W - dialogW) / 2;
        int dialogY = (SCREEN_H - dialogH) / 2;

        if (remove_modal_dl_count_ > 0) {
            if (x >= dialogX + 45 && x <= dialogX + 415 && y >= dialogY + 265 && y <= dialogY + 325) {
                state_.removeFromLibrary(manga_to_remove_);
                show_remove_modal_ = false;
                status_banner_ = "Removed \"" + manga_to_remove_.title + "\" (offline files kept)";
                loadLibraryFromDisk();
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= dialogX + 445 && x <= dialogX + 815 && y >= dialogY + 265 && y <= dialogY + 325) {
                state_.removeFromLibrary(manga_to_remove_);
                state_.clearProgress(manga_to_remove_.title, manga_to_remove_.source_id, manga_to_remove_.id);
                std::error_code ec;
                fs::remove_all("/mnt/us/manga/" + manga_to_remove_.title, ec);
                show_remove_modal_ = false;
                status_banner_ = "Deleted offline files and removed \"" + manga_to_remove_.title + "\"";
                loadLibraryFromDisk();
                if (current_view_ == ViewState::DETAIL && selected_manga_.title == manga_to_remove_.title) {
                    loadMangaChapters(selected_manga_, false);
                }
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= dialogX + 245 && x <= dialogX + 615 && y >= dialogY + 355 && y <= dialogY + 415) {
                show_remove_modal_ = false;
                renderAll();
                display_.refreshView();
                return;
            }
        } else {
            if (x >= dialogX + 70 && x <= dialogX + 390 && y >= dialogY + 250 && y <= dialogY + 310) {
                state_.removeFromLibrary(manga_to_remove_);
                std::error_code ec;
                fs::remove_all("/mnt/us/manga/" + manga_to_remove_.title, ec);
                show_remove_modal_ = false;
                status_banner_ = "Removed \"" + manga_to_remove_.title + "\" from Library";
                loadLibraryFromDisk();
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= dialogX + 470 && x <= dialogX + 790 && y >= dialogY + 250 && y <= dialogY + 310) {
                show_remove_modal_ = false;
                renderAll();
                display_.refreshView();
                return;
            }
        }

        if (x < dialogX || x > dialogX + dialogW || y < dialogY || y > dialogY + dialogH) {
            show_remove_modal_ = false;
            renderAll();
            display_.refreshView();
        }
    }

    struct OfflineCbz {
        std::string manga_title;
        std::string chapter_stem;
        std::string full_path;
        double mb = 0.0;
    };

    std::vector<OfflineCbz> getOfflineCbzList() {
        std::vector<OfflineCbz> list;
        std::error_code ec;
        if (!fs::exists("/mnt/us/manga", ec)) return list;

        for (const auto& entry : fs::directory_iterator("/mnt/us/manga", ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string title = entry.path().filename().string();
            if (title.empty() || title[0] == '.' || title == "data") continue;

            for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                if (f.path().extension() == ".cbz") {
                    OfflineCbz item;
                    item.manga_title = title;
                    item.chapter_stem = f.path().stem().string();
                    item.full_path = f.path().string();
                    item.mb = static_cast<double>(fs::file_size(f.path(), ec)) / (1024.0 * 1024.0);
                    list.push_back(item);
                }
            }
        }
        std::sort(list.begin(), list.end(), [](const OfflineCbz& a, const OfflineCbz& b) {
            if (a.manga_title != b.manga_title) return util::naturalLess(a.manga_title, b.manga_title);
            return util::naturalLess(a.chapter_stem, b.chapter_stem);
        });
        return list;
    }

    double computeCacheSizeMb() const {
        std::error_code ec;
        uintmax_t bytes = 0;
        if (fs::exists("/mnt/us/manga/.cache", ec)) {
            for (const auto& entry : fs::recursive_directory_iterator("/mnt/us/manga/.cache", ec)) {
                if (entry.is_regular_file(ec)) bytes += entry.file_size(ec);
            }
        }
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    void handleDownloadsTouch(int x, int y) {
        if (y >= 10 && y <= TOP_H) {
            if (x >= SCREEN_W - 160) { quit(); return; }
            // [ Clear Cache ] button in Downloads header
            if (x >= SCREEN_W - 355 && x < SCREEN_W - 165) {
                page_cache_.clearAll();
                cover_cache_.clearMemoryCache();
                std::error_code ec;
                fs::remove_all("/mnt/us/manga/.cache/pages", ec);
                fs::remove_all("/mnt/us/manga/.cache/tmp", ec);
                fs::create_directories("/mnt/us/manga/.cache/pages", ec);
                fs::create_directories("/mnt/us/manga/.cache/tmp", ec);
                status_banner_ = "✓ Cleared temporary page cache";
                renderAll();
                display_.refreshView();
                return;
            }
        }

        auto snap = download_queue_.snapshot();
        int curY = TOP_H + 18;

        if (snap.active || !snap.queued.empty()) {
            // Active queue card: [Cancel] or [Cancel All]
            if (y >= curY && y <= curY + 92) {
                if (x >= SCREEN_W - 345 && x < SCREEN_W - 190) {
                    download_queue_.cancelCurrent();
                    return;
                }
                if (x >= SCREEN_W - 180 && x <= SCREEN_W - 28) {
                    download_queue_.cancelAll();
                    return;
                }
            }
            curY += 106;
        }

        auto offlineList = getOfflineCbzList();
        int totalPages = std::max(1, (static_cast<int>(offlineList.size()) + DOWNLOADS_PER_PAGE - 1) / DOWNLOADS_PER_PAGE);
        int pagY = SCREEN_H - NAV_H - 60;
        if (totalPages > 1 && y >= pagY - 6 && y <= pagY + 56) {
            if (x <= 230) { handlePageStep(-1); return; }
            if (x >= SCREEN_W - 230) { handlePageStep(+1); return; }
        }

        curY += 40; // section title offset
        int itemH = 98, rowStride = 110;
        int startIdx = downloads_page_ * DOWNLOADS_PER_PAGE;
        int endIdx = std::min(static_cast<int>(offlineList.size()), startIdx + DOWNLOADS_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int itemY = curY + (i - startIdx) * rowStride;
            if (y >= itemY && y < itemY + itemH) {
                const auto& item = offlineList[i];
                // Per-chapter [ Delete ] button (x: SCREEN_W - 330 .. SCREEN_W - 195)
                if (x >= SCREEN_W - 330 && x < SCREEN_W - 190) {
                    std::error_code ec;
                    fs::remove(item.full_path, ec);
                    status_banner_ = "Deleted " + item.manga_title + " (" + item.chapter_stem + ")";
                    loadLibraryFromDisk();
                    renderAll();
                    display_.refreshView();
                    return;
                }

                // [ ▶ Read ] or card tap -> open chapter and return to Downloads on Back!
                UnifiedManga m;
                m.title = item.manga_title;
                m.id = item.manga_title;
                for (const auto& lm : library_mangas_) {
                    if (lm.title == item.manga_title) { m = lm; break; }
                }
                selected_manga_ = m;
                loadMangaChapters(selected_manga_, false);
                reader_origin_view_ = ViewState::DOWNLOADS;
                for (size_t c = 0; c < selected_chapters_.size(); ++c) {
                    if (selected_chapters_[c].localPath == item.full_path ||
                        selected_chapters_[c].title == item.chapter_stem ||
                        selected_chapters_[c].chapterNum == item.chapter_stem) {
                        openChapter(static_cast<int>(c), false, 0);
                        return;
                    }
                }
                if (!selected_chapters_.empty()) openChapter(0, false, 0);
                return;
            }
        }
    }

    void handleSettingsTouch(int x, int y) {
        if (x >= SCREEN_W - 160 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Row 1: Frontlight [-] [+] (y: 170..230)
        if (y >= 165 && y <= 235) {
            if (x >= 340 && x <= 420) {
                state_.settings().frontlight = std::max(0, state_.settings().frontlight - 2);
                Frontlight::setIntensity(state_.settings().frontlight);
                state_.save();
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= 530 && x <= 610) {
                state_.settings().frontlight = std::min(24, state_.settings().frontlight + 2);
                Frontlight::setIntensity(state_.settings().frontlight);
                state_.save();
                renderAll();
                display_.refreshView();
                return;
            }
        }

        // Row 2: Reading Direction RTL / LTR (y: 245..310)
        if (y >= 245 && y <= 310 && x >= 340 && x <= 660) {
            state_.settings().reading_direction = (state_.settings().reading_direction == "RTL") ? "LTR" : "RTL";
            state_.save();
            renderAll();
            display_.refreshView();
            return;
        }

        // Row 3: Default Fit Mode (Crop Margins / Contain / Full Width) (y: 325..390)
        if (y >= 325 && y <= 390 && x >= 340 && x <= 680) {
            cycleFitMode();
            renderAll();
            display_.refreshView();
            return;
        }

        // Row 4: Grip / Touch Orientation 0 / 180 (y: 405..470)
        if (y >= 405 && y <= 470 && x >= 340 && x <= 680) {
            int newRot = (state_.settings().touch_rotation == 180) ? 0 : 180;
            state_.settings().touch_rotation = newRot;
            input_.setRotation(newRot);
            state_.save();
            status_banner_ = "Touch orientation: " + std::string(newRot == 180 ? "Buttons Left (180°)" : "Buttons Right (0°)");
            renderAll();
            display_.refreshView();
            return;
        }

        // Row 5: Mini Page Counter toggle + Physical Button Invert toggle (y: 485..550)
        if (y >= 485 && y <= 550) {
            if (x >= 340 && x <= 620) {
                state_.settings().show_mini_counter = !state_.settings().show_mini_counter;
                state_.save();
                renderAll();
                display_.refreshView();
                return;
            }
            if (x >= 640 && x <= 960) {
                state_.settings().button_inverted = !state_.settings().button_inverted;
                state_.save();
                renderAll();
                display_.refreshView();
                return;
            }
        }

        // Action buttons stack
        if (x >= 100 && x <= SCREEN_W - 100) {
            if (y >= 590 && y <= 665) { quit(); return; }
            if (y >= 680 && y <= 755) {
                page_cache_.clearAll();
                cover_cache_.clearMemoryCache();
                reader_cache_.invalidate();
                std::error_code ec;
                fs::remove_all("/mnt/us/manga/.cache", ec);
                fs::create_directories("/mnt/us/manga/.cache/pages", ec);
                fs::create_directories("/mnt/us/manga/.cache/tmp", ec);
                fs::create_directories("/mnt/us/manga/.cache/covers", ec);
                status_banner_ = "✓ Reader & cover cache cleared!";
                renderAll();
                display_.refreshView();
                return;
            }
            if (y >= 770 && y <= 845) {
                loadLibraryFromDisk();
                status_banner_ = "✓ Library rescanned (" + std::to_string(library_mangas_.size()) + " series)";
                renderAll();
                display_.refreshView();
                return;
            }
            if (y >= 860 && y <= 935) {
                status_banner_ = "✓ Screen refreshed (ghosting cleared)";
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void cycleFitMode() {
        std::string& fm = state_.settings().fit_mode;
        if (fm == "crop") fm = "contain";
        else if (fm == "contain") fm = "width";
        else fm = "crop";
        state_.settings().trim_margins = (fm == "crop");
        state_.save();
        reader_cache_.invalidate();
    }

    static std::string fitModeShortLabel(const std::string& fm) {
        if (fm == "crop") return "Fit: Crop";
        if (fm == "width") return "Fit: Width";
        return "Fit: Page";
    }

    void handleSourcesTouch(int x, int y) {
        if (x >= SCREEN_W - 160 && y >= 10 && y <= TOP_H) { quit(); return; }
        if (x >= SCREEN_W - 330 && x < SCREEN_W - 160 && y >= 10 && y <= TOP_H) {
            show_keyboard_ = false;
            current_view_ = ViewState::BROWSE;
            renderAll();
            display_.refreshView();
            return;
        }

        if (y >= 82 && y <= 140) {
            show_keyboard_ = false;
            if (x < 420) {
                sources_sub_tab_ = SourcesSubTab::INSTALLED;
                sources_.refreshSources();
            } else if (x < 835) {
                sources_sub_tab_ = SourcesSubTab::AVAILABLE;
                if (!available_sources_loaded_ && !async_busy_.load()) {
                    loadAvailableExtensionsAsync();
                    return;
                }
            } else {
                sources_sub_tab_ = SourcesSubTab::REPOSITORIES;
            }
            renderAll();
            display_.refreshView();
            return;
        }

        int contentY = 185;
        if (sources_sub_tab_ == SourcesSubTab::INSTALLED) handleInstalledSourcesTouch(x, y, contentY);
        else if (sources_sub_tab_ == SourcesSubTab::AVAILABLE) handleAvailableSourcesTouch(x, y, contentY);
        else handleRepositoriesTouch(x, y, contentY);
    }

    void loadAvailableExtensionsAsync() {
        status_banner_ = "Fetching Aidoku extensions catalog...";
        renderAll();
        display_.refreshView();
        launchAsyncTask("Fetching extensions...", [this](uint64_t seq) -> std::function<void()> {
            std::string err;
            auto list = sources_.getAvailableAidokuSources(&err);
            return [this, seq, list = std::move(list), err]() mutable {
                if (async_seq_.load() != seq) return;
                available_aidoku_sources_ = std::move(list);
                available_sources_loaded_ = true;
                if (!err.empty() && available_aidoku_sources_.empty()) status_banner_ = "Repository error: " + err;
                else status_banner_ = "✓ Loaded " + std::to_string(available_aidoku_sources_.size()) + " extensions";
            };
        });
    }

    void handleInstalledSourcesTouch(int x, int y, int startY) {
        const auto& srcList = sources_.getSources();
        int cardH = 118, maxVisible = 6;
        for (size_t i = 0; i < srcList.size() && static_cast<int>(i) < maxVisible; ++i) {
            int cy = startY + static_cast<int>(i) * (cardH + 12);
            if (y >= cy && y <= cy + cardH) {
                if (srcList[i].is_aidoku && x >= SCREEN_W - 365 && x < SCREEN_W - 210) {
                    std::string err;
                    if (sources_.uninstallAidokuSource(srcList[i].id, &err)) {
                        status_banner_ = "✓ Uninstalled " + srcList[i].name;
                        available_sources_loaded_ = false;
                    } else {
                        status_banner_ = "Failed: " + err;
                    }
                    renderAll();
                    display_.refreshView();
                    return;
                }
                sources_.setActiveSourceId(srcList[i].id);
                current_view_ = ViewState::BROWSE;
                fetchPopularAsync();
                return;
            }
        }

        int btnY = startY + std::min(static_cast<int>(srcList.size()), maxVisible) * (cardH + 12) + 20;
        if (y >= btnY && y <= btnY + 70 && x >= 35 && x <= SCREEN_W - 35) {
            sources_sub_tab_ = SourcesSubTab::AVAILABLE;
            if (!available_sources_loaded_) loadAvailableExtensionsAsync();
            else { renderAll(); display_.refreshView(); }
        }
    }

    void handleAvailableSourcesTouch(int x, int y, int startY) {
        if (!available_sources_loaded_ || available_aidoku_sources_.empty()) {
            if (y >= startY + 90 && y <= startY + 180) loadAvailableExtensionsAsync();
            return;
        }
        int totalItems = static_cast<int>(available_aidoku_sources_.size());
        int totalPages = std::max(1, (totalItems + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE);

        if (x >= 25 && x <= 285 && y >= startY && y <= startY + 55) {
            loadAvailableExtensionsAsync();
            return;
        }
        if (totalPages > 1 && y >= startY && y <= startY + 55) {
            if (x >= SCREEN_W - 320 && x <= SCREEN_W - 180) { handlePageStep(-1); return; }
            if (x >= SCREEN_W - 165 && x <= SCREEN_W - 25) { handlePageStep(+1); return; }
        }

        int listStartY = startY + 65, cardH = 115;
        int startIdx = available_sources_page_ * AVAILABLE_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + AVAILABLE_PER_PAGE);
        for (int i = startIdx; i < endIdx; ++i) {
            int cy = listStartY + (i - startIdx) * (cardH + 10);
            if (y >= cy && y <= cy + cardH && !available_aidoku_sources_[i].is_installed && x >= SCREEN_W - 240) {
                std::string id = available_aidoku_sources_[i].id;
                std::string name = available_aidoku_sources_[i].name;
                status_banner_ = "Installing " + name + "...";
                renderAll();
                display_.refreshView();
                launchAsyncTask("Installing " + name + "...", [this, i, id, name](uint64_t seq) -> std::function<void()> {
                    std::string err;
                    bool ok = sources_.installAidokuSource(id, &err);
                    return [this, seq, i, name, ok, err]() {
                        if (async_seq_.load() != seq) return;
                        if (ok) {
                            if (i < static_cast<int>(available_aidoku_sources_.size())) available_aidoku_sources_[i].is_installed = true;
                            status_banner_ = "✓ Installed " + name;
                        } else {
                            status_banner_ = "Install error: " + err;
                        }
                    };
                });
                return;
            }
        }
    }

    void handleRepositoriesTouch(int x, int y, int startY) {
        Json::Value cfg = sources_.getSourceConfig();
        const auto& sl = cfg["source_lists"].asArray();
        int curY = startY + 45;
        for (size_t i = 0; i < sl.size(); ++i) {
            std::string url = sl[i].str();
            if (y >= curY && y <= curY + 75 && x >= SCREEN_W - 190 && x <= SCREEN_W - 40) {
                std::string err;
                sources_.removeSourceListUrl(url, &err);
                available_sources_loaded_ = false;
                status_banner_ = "Removed repository URL.";
                renderAll();
                display_.refreshView();
                return;
            }
            curY += 88;
            if (curY > SCREEN_H - NAV_H - 400) break;
        }
        if (sl.empty()) curY += 95;

        int boxY = curY + 45, boxW = SCREEN_W - 270, boxH = 65;
        if (x >= 25 && x <= 25 + boxW && y >= boxY && y <= boxY + boxH) {
            kb_target_ = KeyboardTarget::REPO_URL;
            show_keyboard_ = true;
            renderAll();
            display_.refreshView();
            return;
        }
        if (x >= SCREEN_W - 225 && x <= SCREEN_W - 40 && y >= boxY && y <= boxY + boxH && !repo_url_input_.empty()) {
            addRepoUrlAsync(repo_url_input_);
            return;
        }
        int btnRowY = boxY + boxH + 20;
        if (x >= 25 && x <= SCREEN_W - 25 && y >= btnRowY && y <= btnRowY + 65) {
            addRepoUrlAsync("https://raw.githubusercontent.com/Aidoku-Community/legacy-sources/refs/heads/gh-pages/index.min.json");
        }
    }

    void addRepoUrlAsync(const std::string& url) {
        show_keyboard_ = false;
        status_banner_ = "Adding repository & syncing catalog...";
        renderAll();
        display_.refreshView();
        launchAsyncTask("Adding repository...", [this, url](uint64_t seq) -> std::function<void()> {
            std::string err;
            bool ok = sources_.addSourceListUrl(url, &err);
            std::vector<SourceInfo> avail;
            if (ok) avail = sources_.getAvailableAidokuSources(&err);
            return [this, seq, ok, err, avail = std::move(avail)]() mutable {
                if (async_seq_.load() != seq) return;
                if (ok) {
                    repo_url_input_.clear();
                    available_aidoku_sources_ = std::move(avail);
                    available_sources_loaded_ = true;
                    status_banner_ = "✓ Added repository (" + std::to_string(available_aidoku_sources_.size()) + " extensions)";
                } else {
                    status_banner_ = "Failed: " + err;
                }
            };
        });
    }

    void handleReaderTouch(int x, int y) {
        // If waiting on a live chapter download inside Reader, allow [ Cancel ] tap
        if (waiting_for_chapter_open_) {
            if (y >= SCREEN_H / 2 + 30 && y <= SCREEN_H / 2 + 115 &&
                x >= SCREEN_W / 2 - 120 && x <= SCREEN_W / 2 + 120) {
                waiting_for_chapter_open_ = false;
                download_queue_.cancelCurrent();
                current_view_ = reader_origin_view_;
                renderAll();
                display_.refreshView();
            }
            return;
        }

        // Floating [ ✕ Exit 2x Zoom ] pill at top-left when zoomed & HUD hidden
        if (reader_zoom_2x_ && !reader_show_overlay_ && x >= 15 && x <= 235 && y >= 15 && y <= 78) {
            reader_zoom_2x_ = false;
            reader_pan_x_ = 0.5f;
            reader_pan_y_ = 0.5f;
            renderReader();
            display_.refreshView();
            return;
        }

        if (reader_show_overlay_) {
            // 1. Top HUD Bar (y: 0..TOP_H + 8): [< Back] [Title] [⟳ Ghost] [Fit] [1x/2x]
            if (y <= TOP_H + 8) {
                if (x <= 175) {
                    reader_show_overlay_ = false;
                    reader_zoom_2x_ = false;
                    current_view_ = reader_origin_view_;
                    loadLibraryFromDisk();
                    input_.clear();
                    renderAll();
                    display_.refreshView();
                    return;
                }
                // [⟳ Ghost] at x: SCREEN_W - 430 .. SCREEN_W - 295
                if (x >= SCREEN_W - 430 && x < SCREEN_W - 292) {
                    reader_show_overlay_ = false;
                    renderReader();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                // [Fit: Crop/Page/Width] at x: SCREEN_W - 286 .. SCREEN_W - 132
                if (x >= SCREEN_W - 286 && x < SCREEN_W - 130) {
                    cycleFitMode();
                    renderReader();
                    display_.refreshView();
                    return;
                }
                // [1x / 2x] Zoom button at x >= SCREEN_W - 126
                if (x >= SCREEN_W - 126) {
                    reader_zoom_2x_ = !reader_zoom_2x_;
                    reader_pan_x_ = 0.5f;
                    reader_pan_y_ = 0.5f;
                    reader_show_overlay_ = false;
                    renderReader();
                    display_.refreshView();
                    return;
                }
                reader_show_overlay_ = false;
                renderReader();
                display_.refreshView();
                return;
            }

            // 2. Bottom HUD Panel (y: SCREEN_H - 220 .. SCREEN_H)
            int hudY = SCREEN_H - 220;
            if (y >= hudY) {
                // Interactive Page Scrubber Bar (y: hudY + 14 .. hudY + 68)
                if (y <= hudY + 70 && reader_total_pages_ > 1) {
                    int barX = 48, barW = SCREEN_W - 96;
                    float ratio = static_cast<float>(x - barX) / static_cast<float>(barW);
                    ratio = std::max(0.0f, std::min(1.0f, ratio));
                    if (state_.settings().reading_direction == "RTL") ratio = 1.0f - ratio;
                    int target = static_cast<int>(std::round(ratio * (reader_total_pages_ - 1)));
                    if (target != reader_current_page_) {
                        reader_current_page_ = target;
                        recordProgress();
                    }
                    renderReader();
                    display_.refreshView();
                    return;
                }

                // Controls Row 1 (y: hudY + 76 .. hudY + 138): Light [-] [+] | [RTL/LTR] | [Mini Counter]
                if (y >= hudY + 74 && y <= hudY + 140) {
                    if (x >= 145 && x <= 215) {
                        state_.settings().frontlight = std::max(0, state_.settings().frontlight - 2);
                        Frontlight::setIntensity(state_.settings().frontlight);
                        state_.save();
                    } else if (x >= 295 && x <= 365) {
                        state_.settings().frontlight = std::min(24, state_.settings().frontlight + 2);
                        Frontlight::setIntensity(state_.settings().frontlight);
                        state_.save();
                    } else if (x >= 400 && x <= 575) {
                        state_.settings().reading_direction = (state_.settings().reading_direction == "RTL") ? "LTR" : "RTL";
                        state_.save();
                    } else if (x >= 600 && x <= 830) {
                        state_.settings().show_mini_counter = !state_.settings().show_mini_counter;
                        state_.save();
                    } else if (x >= SCREEN_W - 220) {
                        reader_show_overlay_ = false;
                    }
                    renderReader();
                    display_.refreshView();
                    return;
                }

                // Controls Row 2 (y: hudY + 146 .. SCREEN_H): [« Prev Ch] [< Prev] [Page X/Y] [Next >] [Next Ch »]
                if (y > hudY + 140) {
                    bool isRTL = (state_.settings().reading_direction == "RTL");
                    if (x < 220) {
                        // Leftmost chapter jump button (RTL -> Next Ch, LTR -> Prev Ch)
                        int idx = isRTL ? getNextChapterIndex() : getPrevChapterIndex();
                        if (idx >= 0) { reader_show_overlay_ = false; openChapter(idx, true, 0); }
                        else renderReaderToast(isRTL ? "No newer chapter" : "No previous chapter");
                        return;
                    }
                    if (x >= 230 && x < 430) {
                        if (isRTL) nextPage(); else prevPage();
                        return;
                    }
                    if (x >= SCREEN_W - 430 && x < SCREEN_W - 230) {
                        if (isRTL) prevPage(); else nextPage();
                        return;
                    }
                    if (x >= SCREEN_W - 220) {
                        int idx = isRTL ? getPrevChapterIndex() : getNextChapterIndex();
                        if (idx >= 0) { reader_show_overlay_ = false; openChapter(idx, true, 0); }
                        else renderReaderToast(isRTL ? "No previous chapter" : "No newer chapter");
                        return;
                    }
                    reader_show_overlay_ = false;
                    renderReader();
                    display_.refreshView();
                    return;
                }
            }

            // Tap outside HUD dismisses overlay
            reader_show_overlay_ = false;
            renderReader();
            display_.refreshView();
            return;
        }

        // Fullscreen Reading Tap Zones: Top 9% or Center third opens HUD
        if (y <= 130 || (x >= SCREEN_W * 0.33 && x <= SCREEN_W * 0.67 && y >= SCREEN_H * 0.25 && y <= SCREEN_H * 0.78)) {
            reader_show_overlay_ = true;
            renderReader();
            display_.refreshView();
            return;
        }

        // Left / Right Page Turn Tap Zones
        bool isRTL = (state_.settings().reading_direction == "RTL");
        if (x < SCREEN_W * 0.38) {
            if (isRTL) nextPage(); else prevPage();
        } else if (x > SCREEN_W * 0.62) {
            if (isRTL) prevPage(); else nextPage();
        } else {
            reader_show_overlay_ = true;
            renderReader();
            display_.refreshView();
        }
    }

    // --- CHAPTER & READER ENGINE ---
    static double parseChapterNumber(const std::string& s) {
        if (s.empty()) return -1.0;
        try {
            size_t i = 0;
            while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.') ++i;
            if (i < s.size()) return std::stod(s.substr(i));
        } catch (...) {}
        return -1.0;
    }

    void reconcileChaptersLocalAndRead(const UnifiedManga& manga) {
        std::string dirPath = "/mnt/us/manga/" + manga.title;
        std::error_code ec;

        if (fs::exists(dirPath, ec)) {
            for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                if (f.path().extension() != ".cbz") continue;
                std::string stem = f.path().stem().string();
                double fileNum = parseChapterNumber(stem);
                bool matched = false;

                for (auto& ch : selected_chapters_) {
                    double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
                    std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
                    if (ch.title == stem || ch.chapterNum == stem || safeCh == stem ||
                        (fileNum >= 0 && chNum >= 0 && std::abs(fileNum - chNum) < 0.0001)) {
                        ch.isDownloaded = true;
                        ch.localPath = f.path().string();
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    UnifiedChapter localCh;
                    localCh.title = stem;
                    localCh.chapterNum = stem;
                    localCh.localPath = f.path().string();
                    localCh.isDownloaded = true;
                    selected_chapters_.push_back(localCh);
                }
            }
        }

        for (auto& ch : selected_chapters_) {
            ch.isRead = state_.isChapterRead(manga.source_id, manga.id, manga.title, ch);
        }
    }

    void loadMangaChapters(const UnifiedManga& manga, bool fetchRemoteIfEmpty = true) {
        selected_manga_ = manga;
        selected_chapters_ = state_.getCachedChapters(manga.title, manga.source_id, manga.id);
        chapter_page_ = 0;

        reconcileChaptersLocalAndRead(manga);
        sortChapters();

        if (selected_chapters_.empty() && fetchRemoteIfEmpty && !manga.id.empty() &&
            !manga.source_id.empty() && manga.source_id != "local") {
            fetchChaptersAsync(manga);
        }
    }

    int getLowestUnreadChapterIndex() const {
        if (selected_chapters_.empty()) return -1;
        int bestUnread = -1;
        double bestUnreadNum = 1e12;
        int bestAny = 0;
        double bestAnyNum = 1e12;

        for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
            const auto& ch = selected_chapters_[i];
            double n = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            double key = (n >= 0) ? n : (1e9 + i);
            if (key < bestAnyNum) { bestAnyNum = key; bestAny = i; }
            if (!ch.isRead && key < bestUnreadNum) { bestUnreadNum = key; bestUnread = i; }
        }
        return (bestUnread >= 0) ? bestUnread : bestAny;
    }

    void enqueueSingleChapter(int chIdx, bool autoOpen, int startPage, bool silentPreload = false) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        DownloadJob job;
        job.manga = selected_manga_;
        job.chapter = selected_chapters_[chIdx];
        if (job.chapter.source_id.empty()) job.chapter.source_id = selected_manga_.source_id;
        job.auto_open = autoOpen;
        job.start_page = startPage;
        job.silent_preload = silentPreload;
        download_queue_.enqueue(job);
        if (!silentPreload) {
            status_banner_ = autoOpen
                ? ("Downloading Ch. " + job.chapter.chapterNum + " to open...")
                : ("Queued Ch. " + job.chapter.chapterNum + " for offline reading");
            renderAll();
            display_.refreshView();
        }
    }

    void enqueueBatchChapters(int maxCount) {
        if (selected_chapters_.empty()) return;
        // Collect unread, un-downloaded chapters in ascending reading order
        std::vector<int> order(selected_chapters_.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [this](int a, int b) {
            double na = parseChapterNumber(selected_chapters_[a].chapterNum.empty() ? selected_chapters_[a].title : selected_chapters_[a].chapterNum);
            double nb = parseChapterNumber(selected_chapters_[b].chapterNum.empty() ? selected_chapters_[b].title : selected_chapters_[b].chapterNum);
            if (na >= 0 && nb >= 0 && std::abs(na - nb) > 0.0001) return na < nb;
            return a > b;
        });

        int queued = 0;
        for (int idx : order) {
            const auto& ch = selected_chapters_[idx];
            if (ch.isDownloaded || ch.isRead) continue;
            DownloadJob job;
            job.manga = selected_manga_;
            job.chapter = ch;
            if (job.chapter.source_id.empty()) job.chapter.source_id = selected_manga_.source_id;
            if (download_queue_.enqueue(job)) {
                if (++queued >= maxCount) break;
            }
        }
        status_banner_ = (queued > 0)
            ? ("✓ Queued " + std::to_string(queued) + (queued == 1 ? " chapter" : " chapters") + " in background")
            : "All unread chapters are already downloaded or queued!";
        renderAll();
        display_.refreshView();
    }

    void resumeRecent(const MangaProgress& prog, ViewState origin) {
        reader_origin_view_ = origin;
        UnifiedManga m;
        m.title = prog.manga_title;
        m.id = prog.manga_id.empty() ? prog.manga_title : prog.manga_id;
        m.source_id = prog.source_id;
        m.cover_url = prog.cover_url;
        m.cover_path = prog.cover_path;

        for (const auto& lm : library_mangas_) {
            if ((!prog.source_id.empty() && lm.source_id == prog.source_id && lm.id == prog.manga_id) ||
                lm.title == prog.manga_title) {
                m = lm;
                break;
            }
        }
        selected_manga_ = m;
        loadMangaChapters(selected_manga_, false);

        double targetNum = (prog.chapter_num >= 0) ? prog.chapter_num : parseChapterNumber(prog.chapter_title);
        for (size_t i = 0; i < selected_chapters_.size(); ++i) {
            double chNum = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
            if ((!prog.chapter_id.empty() && selected_chapters_[i].id == prog.chapter_id) ||
                selected_chapters_[i].title == prog.chapter_title ||
                selected_chapters_[i].chapterNum == prog.chapter_title ||
                (chNum >= 0 && targetNum >= 0 && std::abs(chNum - targetNum) < 0.0001)) {
                openChapter(static_cast<int>(i), true, prog.page_index);
                return;
            }
        }
        int fallback = getLowestUnreadChapterIndex();
        if (fallback >= 0) openChapter(fallback, true, 0);
    }

    int getNextChapterIndex() const {
        if (current_chapter_idx_ < 0 || selected_chapters_.empty()) return -1;
        const auto& curCh = selected_chapters_[current_chapter_idx_];
        double curNum = parseChapterNumber(curCh.chapterNum.empty() ? curCh.title : curCh.chapterNum);
        if (curNum >= 0) {
            int bestIdx = -1;
            double bestNum = 1e9;
            for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
                double n = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
                if (n > curNum + 0.0001 && n < bestNum) { bestNum = n; bestIdx = i; }
            }
            return bestIdx;
        }
        return detail_sort_descending_
            ? ((current_chapter_idx_ > 0) ? (current_chapter_idx_ - 1) : -1)
            : ((current_chapter_idx_ + 1 < static_cast<int>(selected_chapters_.size())) ? (current_chapter_idx_ + 1) : -1);
    }

    int getPrevChapterIndex() const {
        if (current_chapter_idx_ < 0 || selected_chapters_.empty()) return -1;
        const auto& curCh = selected_chapters_[current_chapter_idx_];
        double curNum = parseChapterNumber(curCh.chapterNum.empty() ? curCh.title : curCh.chapterNum);
        if (curNum >= 0) {
            int bestIdx = -1;
            double bestNum = -1.0;
            for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
                double n = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
                if (n >= 0 && n < curNum - 0.0001 && n > bestNum) { bestNum = n; bestIdx = i; }
            }
            return bestIdx;
        }
        return detail_sort_descending_
            ? ((current_chapter_idx_ + 1 < static_cast<int>(selected_chapters_.size())) ? (current_chapter_idx_ + 1) : -1)
            : ((current_chapter_idx_ > 0) ? (current_chapter_idx_ - 1) : -1);
    }

    void triggerBackgroundPreload() {
        int nextIdx = getNextChapterIndex();
        if (nextIdx < 0 || nextIdx >= static_cast<int>(selected_chapters_.size())) return;
        if (selected_chapters_[nextIdx].isDownloaded) return;
        enqueueSingleChapter(nextIdx, false, 0, true);
    }

    void openChapter(int chIdx, bool autoOpenAfterDownload = true, int startPage = 0) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        current_chapter_idx_ = chIdx;
        auto& ch = selected_chapters_[chIdx];

        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
        fs::path cbzFile = destDir / (safeCh + ".cbz");
        std::error_code ec;

        if (fs::exists(cbzFile, ec)) {
            ch.isDownloaded = true;
            ch.localPath = cbzFile.string();
        } else {
            double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            if (chNum >= 0 && fs::exists(destDir, ec)) {
                for (const auto& f : fs::directory_iterator(destDir, ec)) {
                    if (f.path().extension() == ".cbz") {
                        double fNum = parseChapterNumber(f.path().stem().string());
                        if (fNum >= 0 && std::abs(fNum - chNum) < 0.0001) {
                            ch.isDownloaded = true;
                            ch.localPath = f.path().string();
                            break;
                        }
                    }
                }
            }
        }

        if (ch.isDownloaded && fs::exists(ch.localPath, ec)) {
            waiting_for_chapter_open_ = false;
            std::string err;
            std::set<std::string> pinned;
            reader_page_files_ = page_cache_.extractCbz(ch.id.empty() ? ch.title : ch.id, ch.localPath, pinned, &err);
            reader_total_pages_ = static_cast<int>(reader_page_files_.size());
            reader_current_page_ = std::min(startPage, std::max(0, reader_total_pages_ - 1));
            reader_show_overlay_ = false;
            reader_zoom_2x_ = false;
            reader_pan_x_ = 0.5f;
            reader_pan_y_ = 0.5f;

            reader_cache_.setChapterPages(reader_page_files_, reader_current_page_, state_.settings().fit_mode);
            current_view_ = ViewState::READER;
            recordProgress();
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);

            if (reader_total_pages_ > 0 && reader_current_page_ >= reader_total_pages_ - 5) {
                triggerBackgroundPreload();
            }
        } else {
            if (current_view_ == ViewState::READER && autoOpenAfterDownload) {
                waiting_for_chapter_open_ = true;
                waiting_chapter_num_ = ch.chapterNum;
                enqueueSingleChapter(chIdx, true, startPage, false);
                renderReaderDownloadProgress("Downloading Ch. " + ch.chapterNum + "...", 5);
            } else {
                enqueueSingleChapter(chIdx, autoOpenAfterDownload, startPage, false);
            }
        }
    }

    void nextPage() {
        if (reader_current_page_ + 1 < reader_total_pages_) {
            reader_current_page_++;
            reader_zoom_2x_ = false;
            recordProgress();
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % state_.settings().refresh_interval == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }
            if (reader_total_pages_ > 0 && reader_current_page_ >= reader_total_pages_ - 5) {
                triggerBackgroundPreload();
            }
        } else {
            int nextIdx = getNextChapterIndex();
            if (nextIdx >= 0) {
                openChapter(nextIdx, true, 0);
            } else {
                renderReaderToast("End of manga reached");
            }
        }
    }

    void prevPage() {
        if (reader_current_page_ > 0) {
            reader_current_page_--;
            reader_zoom_2x_ = false;
            recordProgress();
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % state_.settings().refresh_interval == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }
        } else {
            int prevIdx = getPrevChapterIndex();
            if (prevIdx >= 0) {
                openChapter(prevIdx, true, 999999);
            } else {
                renderReaderToast("Beginning of manga");
            }
        }
    }

    void recordProgress() {
        if (current_chapter_idx_ >= 0 && current_chapter_idx_ < static_cast<int>(selected_chapters_.size())) {
            auto& ch = selected_chapters_[current_chapter_idx_];
            double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            state_.updateProgress(selected_manga_.source_id,
                                 selected_manga_.id,
                                 selected_manga_.title,
                                 ch.id,
                                 ch.title.empty() ? ("Chapter " + ch.chapterNum) : ch.title,
                                 chNum,
                                 reader_current_page_,
                                 reader_total_pages_,
                                 selected_manga_.cover_url,
                                 selected_manga_.cover_path);
            ch.isRead = state_.isChapterRead(selected_manga_.source_id, selected_manga_.id, selected_manga_.title, ch);
        }
    }

    void sortChapters() {
        if (selected_chapters_.empty()) return;
        std::sort(selected_chapters_.begin(), selected_chapters_.end(), [this](const UnifiedChapter& a, const UnifiedChapter& b) {
            double na = parseChapterNumber(a.chapterNum.empty() ? a.title : a.chapterNum);
            double nb = parseChapterNumber(b.chapterNum.empty() ? b.title : b.chapterNum);
            if (na >= 0 && nb >= 0 && std::abs(na - nb) > 0.0001) {
                return detail_sort_descending_ ? (na > nb) : (na < nb);
            }
            if (na >= 0 && nb < 0) return true;
            if (na < 0 && nb >= 0) return false;
            return detail_sort_descending_ ? util::naturalLess(b.title, a.title) : util::naturalLess(a.title, b.title);
        });
    }

    void nextBrowsePage() {
        int totalPages = std::max(1, (static_cast<int>(browse_mangas_.size()) + BROWSE_GRID_PER_PAGE - 1) / BROWSE_GRID_PER_PAGE);
        if (browse_page_ + 1 < totalPages) {
            browse_page_++;
            renderAll();
            display_.refreshView();
        }
    }

    void prevBrowsePage() {
        if (browse_page_ > 0) {
            browse_page_--;
            renderAll();
            display_.refreshView();
        }
    }

    void nextChapterPage() {
        auto filteredIdx = getFilteredChapterIndices();
        int totalPages = std::max(1, (static_cast<int>(filteredIdx.size()) + CHAPTERS_PER_PAGE - 1) / CHAPTERS_PER_PAGE);
        if (chapter_page_ + 1 < totalPages) {
            chapter_page_++;
            renderAll();
            display_.refreshView();
        }
    }

    void prevChapterPage() {
        if (chapter_page_ > 0) {
            chapter_page_--;
            renderAll();
            display_.refreshView();
        }
    }

    // --- KEYBOARD CONSTANTS & TOUCH ---
    static constexpr int KB_Y = 1130;
    static constexpr int KB_ROW_H = 105;
    static constexpr int KB_START_Y = KB_Y + 15;
    static constexpr int KB_KEY_H = 92;
    static constexpr int KB_KEY_W = 114;
    static constexpr int KB_KEY_GAP = 8;
    static constexpr int KB_KEY_STEP = 122;

    std::string& getActiveInputText() {
        return (kb_target_ == KeyboardTarget::REPO_URL) ? repo_url_input_ : search_query_;
    }

    void renderKey(int x, int y, int w, int h, const std::string& label, bool inverted = false, bool funcKey = false, int fontSize = 30) {
        uint8_t bg = inverted ? 0x00 : (funcKey ? 0xE6 : 0xFF);
        uint8_t fg = inverted ? 0xFF : 0x00;
        canvas_.fillRect(x, y, w, h, bg);
        canvas_.drawRect(x, y, w, h, 0x00, 2);
        int tw = font_.measureText(label, fontSize, true);
        font_.drawText(canvas_, x + (w - tw) / 2, y + (h - fontSize) / 2 - 1, label, fontSize, fg, true);
    }

    void highlightKeyAt(int x, int y, bool inverted = true) {
        if (y < KB_START_Y || y >= KB_START_Y + 4 * KB_ROW_H) return;
        int row = (y - KB_START_Y) / KB_ROW_H;
        if (row < 0 || row > 3) return;

        int kx = 0, ky = 0, kw = 0, kh = KB_KEY_H;
        std::string label;
        bool isFunc = false;
        int fontSize = 30;

        if (row == 0) {
            int col = std::max(0, std::min(9, (x - 20) / KB_KEY_STEP));
            kx = 26 + col * KB_KEY_STEP; ky = KB_START_Y; kw = KB_KEY_W; fontSize = 32;
            const char* syms[] = {"1","2","3","4","5","6","7","8","9","0"};
            const char* up[] = {"Q","W","E","R","T","Y","U","I","O","P"};
            const char* low[] = {"q","w","e","r","t","y","u","i","o","p"};
            label = kb_symbols_mode_ ? syms[col] : (kb_shift_active_ ? up[col] : low[col]);
        } else if (row == 1) {
            int col = std::max(0, std::min(8, (x - 60) / KB_KEY_STEP));
            kx = 87 + col * KB_KEY_STEP; ky = KB_START_Y + KB_ROW_H; kw = KB_KEY_W; fontSize = 32;
            const char* syms[] = {"-","/",":",";","(",")","$","&","@"};
            const char* up[] = {"A","S","D","F","G","H","J","K","L"};
            const char* low[] = {"a","s","d","f","g","h","j","k","l"};
            label = kb_symbols_mode_ ? syms[col] : (kb_shift_active_ ? up[col] : low[col]);
        } else if (row == 2) {
            ky = KB_START_Y + 2 * KB_ROW_H;
            if (x < 195) { kx = 26; kw = 175; label = kb_shift_active_ ? "⇧ Shift" : "⇧"; isFunc = true; fontSize = 26; }
            else if (x >= 1050) { kx = 1063; kw = 175; label = "⌫"; isFunc = true; fontSize = 28; }
            else {
                int col = std::max(0, std::min(6, (x - 195) / KB_KEY_STEP));
                kx = 209 + col * KB_KEY_STEP; kw = KB_KEY_W; fontSize = 32;
                const char* syms[] = {".",",","?","!","'","\"","_"};
                const char* up[] = {"Z","X","C","V","B","N","M"};
                const char* low[] = {"z","x","c","v","b","n","m"};
                label = kb_symbols_mode_ ? syms[col] : (kb_shift_active_ ? up[col] : low[col]);
            }
        } else {
            ky = KB_START_Y + 3 * KB_ROW_H;
            if (x < 170) { kx = 26; kw = 140; label = kb_symbols_mode_ ? "ABC" : "?123"; isFunc = true; fontSize = 24; }
            else if (x < 305) { kx = 174; kw = 130; label = "Clear"; isFunc = true; fontSize = 22; }
            else if (x < 805) { kx = 312; kw = 490; label = "space"; fontSize = 24; }
            else if (x < 940) { kx = 810; kw = 130; label = "Close"; isFunc = true; fontSize = 22; }
            else { kx = 948; kw = 290; label = (kb_target_ == KeyboardTarget::REPO_URL) ? "Done ↵" : "Search ↵"; fontSize = 26; }
        }
        renderKey(kx, ky, kw, kh, label, inverted, isFunc, fontSize);
        display_.update(kx, ky, kw, kh, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void renderSearchBarInput() {
        if (kb_target_ == KeyboardTarget::REPO_URL) {
            renderSources();
            display_.update(0, 0, SCREEN_W, KB_Y - 62, WAVEFORM_DU, UPDATE_PARTIAL);
            return;
        }
        int sbX = 20, sbY = 15, sbW = 655, sbH = 50;
        canvas_.fillRect(sbX + 2, sbY + 2, sbW - 4, sbH - 4, 0xFF);
        std::string disp = search_query_.empty()
            ? (show_keyboard_ ? "_" : "Search manga...")
            : (show_keyboard_ ? search_query_ + "_" : search_query_);
        disp = font_.elideText(disp, 24, sbW - 65, false);
        font_.drawText(canvas_, sbX + 14, sbY + 12, disp, 24, (search_query_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);
        if (!search_query_.empty()) {
            font_.drawText(canvas_, sbX + sbW - 40, sbY + 12, "✕", 24, 0x30, true);
        }
        display_.update(sbX, sbY, sbW, sbH, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void handleKeyboardTouch(int x, int y) {
        std::string& activeInput = getActiveInputText();

        // URL Quick-Token Strip when entering Aidoku Repository URLs (y: KB_Y - 62 .. KB_Y)
        if (kb_target_ == KeyboardTarget::REPO_URL && y >= KB_Y - 62 && y < KB_Y) {
            const auto& tokens = ui::urlQuickTokens();
            int tx = 24;
            for (const auto& tok : tokens) {
                int tw = font_.measureText(tok, 19, true) + 28;
                if (tx + tw > SCREEN_W - 20) break;
                if (x >= tx && x <= tx + tw) {
                    activeInput += tok;
                    renderSearchBarInput();
                    return;
                }
                tx += tw + 10;
            }
            return;
        }

        if (y < KB_START_Y || y >= KB_START_Y + 4 * KB_ROW_H) return;
        int row = (y - KB_START_Y) / KB_ROW_H;
        bool updatedKb = false;

        if (row == 0) {
            int col = std::max(0, std::min(9, (x - 20) / KB_KEY_STEP));
            const char sym0[] = "1234567890", low0[] = "qwertyuiop", up0[] = "QWERTYUIOP";
            activeInput += kb_symbols_mode_ ? sym0[col] : (kb_shift_active_ ? up0[col] : low0[col]);
            if (!kb_symbols_mode_ && kb_shift_active_) { kb_shift_active_ = false; updatedKb = true; }
        } else if (row == 1) {
            int col = std::max(0, std::min(8, (x - 70) / KB_KEY_STEP));
            const char sym1[] = "-/:;()$&@", low1[] = "asdfghjkl", up1[] = "ASDFGHJKL";
            activeInput += kb_symbols_mode_ ? sym1[col] : (kb_shift_active_ ? up1[col] : low1[col]);
            if (!kb_symbols_mode_ && kb_shift_active_) { kb_shift_active_ = false; updatedKb = true; }
        } else if (row == 2) {
            if (x < 205) {
                kb_shift_active_ = !kb_shift_active_;
                renderKeyboard();
                display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            }
            if (x >= 1060) {
                FontManager::popUtf8Codepoint(activeInput);
                renderSearchBarInput();
                renderKey(1063, KB_START_Y + 2 * KB_ROW_H, 175, KB_KEY_H, "⌫", false, true, 28);
                display_.update(1063, KB_START_Y + 2 * KB_ROW_H, 175, KB_KEY_H, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            }
            int col = std::max(0, std::min(6, (x - 205) / KB_KEY_STEP));
            const char sym2[] = ".,?!'\"_", low2[] = "zxcvbnm", up2[] = "ZXCVBNM";
            activeInput += kb_symbols_mode_ ? sym2[col] : (kb_shift_active_ ? up2[col] : low2[col]);
            if (!kb_symbols_mode_ && kb_shift_active_) { kb_shift_active_ = false; updatedKb = true; }
        } else {
            if (x < 165) {
                kb_symbols_mode_ = !kb_symbols_mode_;
                renderKeyboard();
                display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            }
            if (x < 305) {
                activeInput.clear();
                renderSearchBarInput();
                renderKey(174, KB_START_Y + 3 * KB_ROW_H, 130, KB_KEY_H, "Clear", false, true, 22);
                display_.update(174, KB_START_Y + 3 * KB_ROW_H, 130, KB_KEY_H, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            }
            if (x < 805) {
                activeInput += " ";
                renderSearchBarInput();
                renderKey(312, KB_START_Y + 3 * KB_ROW_H, 490, KB_KEY_H, "space", false, false, 24);
                display_.update(312, KB_START_Y + 3 * KB_ROW_H, 490, KB_KEY_H, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            }
            if (x < 940) {
                show_keyboard_ = false;
                renderAll();
                display_.refreshView();
                return;
            }
            show_keyboard_ = false;
            if (kb_target_ == KeyboardTarget::REPO_URL) {
                if (!repo_url_input_.empty()) addRepoUrlAsync(repo_url_input_);
                else { renderAll(); display_.refreshView(); }
            } else {
                if (!search_query_.empty()) performSearchAsync(search_query_);
                else { renderAll(); display_.refreshView(); }
            }
            return;
        }

        renderSearchBarInput();
        renderKeyboard();
        if (updatedKb) {
            display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
        } else {
            display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
        }
    }

    // --- RENDER PASSES ---
    void drawCoverThumbnail(int x, int y, int w, int h, const std::string& pathOrUrl, const std::string& fallbackTitle, const std::string& mangaId = "") {
        if (cover_cache_.tryBlitCover(canvas_, x, y, w, h, pathOrUrl, fallbackTitle, mangaId)) {
            canvas_.drawRect(x, y, w, h, 0x00, 2);
            return;
        }
        canvas_.fillRect(x, y, w, h, 0xEA);
        canvas_.drawRect(x, y, w, h, 0x00, 2);
        size_t idx = 0;
        std::string mono = "?";
        if (!fallbackTitle.empty()) {
            FontManager::nextCodepoint(fallbackTitle, idx);
            mono = fallbackTitle.substr(0, idx);
        }
        int mw = font_.measureText(mono, 32, true);
        font_.drawText(canvas_, x + (w - mw) / 2, y + (h - 32) / 2, mono, 32, 0x40, true);
    }

    void renderAsyncBannerIfBusy(int y) {
        if (!async_busy_.load()) return;
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(async_mu_);
            msg = async_label_;
        }
        if (msg.empty()) msg = status_banner_.empty() ? "Working..." : status_banner_;
        canvas_.fillRect(28, y, SCREEN_W - 56, 48, 0x00);
        font_.drawText(canvas_, 46, y + 12, font_.elideText(msg, 21, SCREEN_W - 260, true), 21, 0xFF, true);
        font_.drawButton(canvas_, SCREEN_W - 180, y + 6, 142, 36, "✕ Cancel", false, 18, 0xFF);
    }

    void renderAll() {
        canvas_.clear(0xFF);

        if (current_view_ == ViewState::LIBRARY) renderLibrary();
        else if (current_view_ == ViewState::RECENT) renderRecent();
        else if (current_view_ == ViewState::BROWSE) renderBrowse();
        else if (current_view_ == ViewState::DETAIL) renderDetail();
        else if (current_view_ == ViewState::DOWNLOADS) renderDownloads();
        else if (current_view_ == ViewState::SETTINGS) renderSettings();
        else if (current_view_ == ViewState::SOURCES) renderSources();
        else if (current_view_ == ViewState::READER) {
            renderReader();
            return;
        }

        if (show_keyboard_) {
            renderKeyboard();
        }

        renderBottomNav();

        if (show_remove_modal_) {
            renderRemoveModal();
        }
    }

    void renderRemoveModal() {
        int dialogW = 860;
        int dialogH = (remove_modal_dl_count_ > 0) ? 460 : 370;
        int dialogX = (SCREEN_W - dialogW) / 2;
        int dialogY = (SCREEN_H - dialogH) / 2;

        canvas_.fillRect(dialogX + 10, dialogY + 10, dialogW, dialogH, 0x66);
        canvas_.fillRect(dialogX, dialogY, dialogW, dialogH, 0xFF);
        canvas_.drawRect(dialogX, dialogY, dialogW, dialogH, 0x00, 3);

        font_.drawText(canvas_, dialogX + 45, dialogY + 42, "Remove from Library?", 32, 0x00, true);
        canvas_.drawHLine(dialogX + 45, dialogY + 82, dialogW - 90, 0x00, 2);

        std::string dispTitle = font_.elideText(manga_to_remove_.title, 28, dialogW - 90, true);
        font_.drawText(canvas_, dialogX + 45, dialogY + 120, dispTitle, 28, 0x00, true);

        if (remove_modal_dl_count_ > 0) {
            std::string msg = "This series has " + std::to_string(remove_modal_dl_count_) +
                              (remove_modal_dl_count_ == 1 ? " chapter" : " chapters") + " saved offline.";
            font_.drawText(canvas_, dialogX + 45, dialogY + 168, msg, 22, 0x35, false);
            font_.drawText(canvas_, dialogX + 45, dialogY + 204, "Keep offline files on disk or delete them to free storage?", 21, 0x60, false);

            font_.drawButton(canvas_, dialogX + 45, dialogY + 265, 370, 60, "Keep Files & Remove", false, 21);
            font_.drawButton(canvas_, dialogX + 445, dialogY + 265, 370, 60, "Delete Files & Remove", true, 21);
            font_.drawButton(canvas_, dialogX + 245, dialogY + 355, 370, 60, "Cancel", false, 22);
        } else {
            font_.drawText(canvas_, dialogX + 45, dialogY + 175, "Remove this manga from your pinned library?", 22, 0x40, false);
            font_.drawButton(canvas_, dialogX + 70, dialogY + 250, 320, 60, "Remove", true, 22);
            font_.drawButton(canvas_, dialogX + 470, dialogY + 250, 320, 60, "Cancel", false, 22);
        }
    }

    void renderLibrary() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 28, 22, "rm2manga", 34, 0x00, true);
        font_.drawText(canvas_, 215, 28, "Library", 25, 0x55, false);

        font_.drawButton(canvas_, SCREEN_W - 462, 15, 138, 50, "↻ Sync", false, 20);
        font_.drawButton(canvas_, SCREEN_W - 312, 15, 142, 50, state_.settings().library_grid_view ? "☰ List" : "⊞ Grid", false, 20);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        MangaProgress hero;
        bool hasHero = state_.getHeroManga(hero);
        int filterY = TOP_H + 12;

        // Filter Pills: All | Unread | Offline  +  Inline Right-Aligned [ ▶ Resume ] Pill
        std::string allLbl = "All (" + std::to_string(library_mangas_.size()) + ")";
        font_.drawButton(canvas_, 28, filterY, 148, 46, allLbl, library_filter_ == LibraryFilter::ALL, 19);
        font_.drawButton(canvas_, 186, filterY, 144, 46, "Unread", library_filter_ == LibraryFilter::UNREAD, 19);
        font_.drawButton(canvas_, 340, filterY, 150, 46, "Offline", library_filter_ == LibraryFilter::OFFLINE, 19);

        if (hasHero) {
            std::string resumeLbl = formatHeroInlineLabel(hero, 490);
            int resumeW = std::max(210, std::min(535, font_.measureText(resumeLbl, 19, true) + 38));
            int resumeX = SCREEN_W - 28 - resumeW;
            font_.drawButton(canvas_, resumeX, filterY, resumeW, 46, resumeLbl, true, 19);
        }

        int contentY = 160;
        if (async_busy_.load()) {
            renderAsyncBannerIfBusy(150);
            contentY = 210;
        } else if (!status_banner_.empty()) {
            font_.drawText(canvas_, 28, 154, font_.elideText(status_banner_, 20, SCREEN_W - 56, true), 20, 0x15, true);
            contentY = 194;
        }

        auto filtered = getFilteredLibrary();
        if (filtered.empty()) {
            std::string emptyTitle = library_mangas_.empty() ? "Your library is empty." : "No matching series in this filter.";
            int tw = font_.measureText(emptyTitle, 28, true);
            font_.drawText(canvas_, (SCREEN_W - tw) / 2, 650, emptyTitle, 28, 0x55, true);
            std::string hint = "Open Browse and tap [+ Pin] on any manga cover to add it here.";
            int hw = font_.measureText(hint, 22, false);
            font_.drawText(canvas_, (SCREEN_W - hw) / 2, 700, hint, 22, 0x75, false);
            return;
        }

        bool gridMode = state_.settings().library_grid_view;
        int perPage = gridMode ? 6 : 8;
        int totalPages = std::max(1, (static_cast<int>(filtered.size()) + perPage - 1) / perPage);
        if (library_page_ >= totalPages) library_page_ = totalPages - 1;
        if (library_page_ < 0) library_page_ = 0;

        int startIdx = library_page_ * perPage;
        int endIdx = std::min(static_cast<int>(filtered.size()), startIdx + perPage);

        if (gridMode) {
            int coverH = GridCardMetrics::COVER_H;
            int rowStride = coverH + GridCardMetrics::TITLE_AREA_H + 14;
            for (int i = startIdx; i < endIdx; ++i) {
                int rel = i - startIdx;
                int col = rel % GridCardMetrics::COLS;
                int row = rel / GridCardMetrics::COLS;
                int cx = GridCardMetrics::colX(col);
                int cy = contentY + row * rowStride;
                std::string srcName = filtered[i].source_id.empty() ? "" : sources_.getSourceDisplayName(filtered[i].source_id);
                ui::drawMangaGridCard(canvas_, font_, cover_cache_,
                                      cx, cy, GridCardMetrics::CARD_W, coverH,
                                      filtered[i], srcName, false, true, true);
            }
        } else {
            int itemH = 128, rowStride = 140;
            for (int i = startIdx; i < endIdx; ++i) {
                const auto& m = filtered[i];
                int itemY = contentY + (i - startIdx) * rowStride;
                canvas_.drawShadowCard(28, itemY, SCREEN_W - 56, itemH, 0xFF, 2, 3);
                drawCoverThumbnail(40, itemY + 10, 76, 108, m.cover_url, m.title, m.id);
                font_.drawText(canvas_, 136, itemY + 22, font_.elideText(m.title, 26, SCREEN_W - 495, true), 26, 0x00, true);

                std::string sub;
                if (m.unread_count > 0) sub += std::to_string(m.unread_count) + " unread  ·  ";
                if (m.downloaded_count > 0) sub += std::to_string(m.downloaded_count) + " offline";
                else sub += sources_.getSourceDisplayName(m.source_id);
                font_.drawText(canvas_, 136, itemY + 68, sub, 21, 0x55, false);

                font_.drawButton(canvas_, SCREEN_W - 330, itemY + 36, 132, 54, "Remove", false, 20);
                font_.drawButton(canvas_, SCREEN_W - 182, itemY + 36, 132, 54, "Open", true, 20);
            }
        }

        if (totalPages > 1) {
            ui::drawPaginationBar(canvas_, font_, SCREEN_H - NAV_H - 60, library_page_, totalPages,
                                  std::to_string(filtered.size()) + " series");
        }
    }

    void renderRecent() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 28, 22, "rm2manga", 34, 0x00, true);
        font_.drawText(canvas_, 215, 28, "Recent History", 25, 0x55, false);

        auto recent = state_.getRecentHistory();
        if (!recent.empty()) {
            font_.drawButton(canvas_, SCREEN_W - 312, 15, 142, 50, "✕ Clear", false, 20);
        }
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        if (recent.empty()) {
            std::string msg = "No reading history yet.";
            font_.drawText(canvas_, (SCREEN_W - font_.measureText(msg, 30, true)) / 2, 600, msg, 30, 0x50, true);
            std::string sub = "Series you read appear here with exact chapter & page progress.";
            font_.drawText(canvas_, (SCREEN_W - font_.measureText(sub, 22, false)) / 2, 650, sub, 22, 0x75, false);
            font_.drawButton(canvas_, (SCREEN_W - 280) / 2, 710, 280, 65, "✦ Browse Manga", true, 24);
            return;
        }

        int totalPages = std::max(1, (static_cast<int>(recent.size()) + RECENT_PER_PAGE - 1) / RECENT_PER_PAGE);
        if (recent_page_ >= totalPages) recent_page_ = totalPages - 1;
        font_.drawText(canvas_, 28, TOP_H + 16, "Recently Read (" + std::to_string(recent.size()) + ")", 22, 0x45, true);

        int yStart = TOP_H + 52;
        int itemH = 132, rowStride = 144;
        int startIdx = recent_page_ * RECENT_PER_PAGE;
        int endIdx = std::min(static_cast<int>(recent.size()), startIdx + RECENT_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int itemY = yStart + (i - startIdx) * rowStride;
            const auto& prog = recent[i];
            canvas_.drawShadowCard(28, itemY, SCREEN_W - 56, itemH, 0xFF, 2, 3);
            drawCoverThumbnail(40, itemY + 10, 78, 112, prog.cover_url, prog.manga_title, prog.manga_id);

            font_.drawText(canvas_, 136, itemY + 16, font_.elideText(prog.manga_title, 26, SCREEN_W - 365, true), 26, 0x00, true);
            std::string chText = prog.chapter_title.empty() ? "In progress" : prog.chapter_title;
            font_.drawText(canvas_, 136, itemY + 52, font_.elideText(chText, 21, SCREEN_W - 365, false), 21, 0x40, false);

            int pct = (prog.total_pages > 0) ? std::min(100, std::max(0, (prog.page_index + 1) * 100 / prog.total_pages)) : 0;
            std::string pgStr = "Page " + std::to_string(prog.page_index + 1);
            if (prog.total_pages > 0) pgStr += " / " + std::to_string(prog.total_pages) + " (" + std::to_string(pct) + "%)";
            std::string ago = util::formatTimeAgo(prog.last_read);
            if (!ago.empty()) pgStr += "  ·  " + ago;
            font_.drawText(canvas_, 136, itemY + 82, pgStr, 19, 0x60, false);

            int pbarW = 240, pbarH = 8, pbarX = 136, pbarY = itemY + 111;
            canvas_.drawRect(pbarX, pbarY, pbarW, pbarH, 0x70, 1);
            int fw = (pbarW - 2) * pct / 100;
            if (fw > 0) canvas_.fillRect(pbarX + 1, pbarY + 1, fw, pbarH - 2, 0x00);

            int btnW = 155, btnX = SCREEN_W - btnW - 42;
            font_.drawButton(canvas_, btnX, itemY + 12, btnW, 48, "▶ Resume", true, 20);
            font_.drawButton(canvas_, btnX, itemY + 68, btnW, 48, "Details", false, 19);
        }

        if (totalPages > 1) {
            ui::drawPaginationBar(canvas_, font_, SCREEN_H - NAV_H - 60, recent_page_, totalPages);
        }
    }

    void renderBrowse() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);

        // Search input box with inline ✕ clear icon
        int sbX = 20, sbY = 15, sbW = 655, sbH = 50;
        canvas_.fillRect(sbX, sbY, sbW, sbH, 0xFF);
        canvas_.drawRect(sbX, sbY, sbW, sbH, 0x00, show_keyboard_ ? 3 : 2);
        std::string sDisp = search_query_.empty()
            ? (show_keyboard_ ? "_" : "Search manga across sources...")
            : (show_keyboard_ ? search_query_ + "_" : search_query_);
        sDisp = font_.elideText(sDisp, 23, sbW - 65, false);
        font_.drawText(canvas_, sbX + 14, sbY + 12, sDisp, 23, (search_query_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);
        if (!search_query_.empty()) {
            font_.drawText(canvas_, sbX + sbW - 40, sbY + 12, "✕", 23, 0x30, true);
        }

        font_.drawButton(canvas_, 685, 15, 118, 50, "Search", true, 20);
        font_.drawButton(canvas_, 813, 15, 132, 50, "✦ Popular", search_query_.empty(), 19);
        font_.drawButton(canvas_, 955, 15, 142, 50, "⚙ Sources", false, 19);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 20);

        // Scrollable Source Pills Row (All Sources + installed)
        int pillY = TOP_H + 12;
        std::vector<SourceInfo> pills;
        pills.push_back({"all", "All Sources", "", true, false, "", "all", ""});
        for (const auto& s : sources_.getSources()) pills.push_back(s);
        if (source_pill_offset_ >= static_cast<int>(pills.size())) source_pill_offset_ = 0;

        if (source_pill_offset_ > 0) {
            font_.drawButton(canvas_, 20, pillY, 48, 46, "◄", false, 20);
        }
        int pillX = (source_pill_offset_ > 0) ? 78 : 28;
        bool hasMoreRight = false;
        for (size_t i = static_cast<size_t>(source_pill_offset_); i < pills.size(); ++i) {
            int pw = std::max(145, std::min(220, font_.measureText(pills[i].name, 20, true) + 38));
            if (pillX + pw > SCREEN_W - 76) {
                hasMoreRight = true;
                break;
            }
            bool active = (pills[i].id == sources_.getActiveSourceId());
            font_.drawButton(canvas_, pillX, pillY, pw, 46, pills[i].name, active, 19);
            pillX += pw + 12;
        }
        if (hasMoreRight) {
            font_.drawButton(canvas_, SCREEN_W - 68, pillY, 48, 46, "►", false, 20);
        }

        int gridStartY = 160;
        if (async_busy_.load()) {
            renderAsyncBannerIfBusy(150);
            gridStartY = 210;
        } else if (!status_banner_.empty()) {
            font_.drawText(canvas_, 28, 154, font_.elideText(status_banner_, 20, SCREEN_W - 56, true), 20, 0x15, true);
            gridStartY = 194;
        }

        int totalItems = static_cast<int>(browse_mangas_.size());
        int totalPages = std::max(1, (totalItems + BROWSE_GRID_PER_PAGE - 1) / BROWSE_GRID_PER_PAGE);
        if (browse_page_ >= totalPages) browse_page_ = totalPages - 1;
        if (browse_page_ < 0) browse_page_ = 0;

        if (browse_mangas_.empty() && !async_busy_.load()) {
            std::string msg = "Tap a source pill above or enter a search query.";
            font_.drawText(canvas_, (SCREEN_W - font_.measureText(msg, 25, false)) / 2, 560, msg, 25, 0x60, false);
            return;
        }

        int coverH = GridCardMetrics::COVER_H;
        int rowStride = coverH + GridCardMetrics::TITLE_AREA_H + 14;
        int startIdx = browse_page_ * BROWSE_GRID_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + BROWSE_GRID_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int rel = i - startIdx;
            int col = rel % GridCardMetrics::COLS;
            int row = rel / GridCardMetrics::COLS;
            int cx = GridCardMetrics::colX(col);
            int cy = gridStartY + row * rowStride;
            if (show_keyboard_ && cy + coverH + GridCardMetrics::TITLE_AREA_H > KB_Y) continue;

            const auto& m = browse_mangas_[i];
            bool pinned = state_.isInLibrary(m.source_id, m.id, m.title);
            std::string srcBadge = sources_.getSourceDisplayName(m.source_id.empty() ? sources_.getActiveSourceId() : m.source_id);
            ui::drawMangaGridCard(canvas_, font_, cover_cache_,
                                  cx, cy, GridCardMetrics::CARD_W, coverH,
                                  m, srcBadge, true, pinned, false);
        }

        if (!show_keyboard_ && totalPages > 1) {
            ui::drawPaginationBar(canvas_, font_, SCREEN_H - NAV_H - 60, browse_page_, totalPages,
                                  std::to_string(totalItems) + " titles");
        }
    }

    void renderSources() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 28, 22, "Manga Sources & Extensions", 31, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 322, 15, 152, 50, "◄ Browse", true, 21);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        int tabY = 82, tabH = 54;
        const auto& srcList = sources_.getSources();
        std::string instTab = "Installed (" + std::to_string(srcList.size()) + ")";
        std::string availTab = available_sources_loaded_
            ? ("Available (" + std::to_string(available_aidoku_sources_.size()) + ")")
            : "Available Catalog";

        font_.drawButton(canvas_, 25, tabY, 390, tabH, instTab, sources_sub_tab_ == SourcesSubTab::INSTALLED, 21);
        font_.drawButton(canvas_, 435, tabY, 390, tabH, availTab, sources_sub_tab_ == SourcesSubTab::AVAILABLE, 21);
        font_.drawButton(canvas_, 845, tabY, 390, tabH, "Repositories", sources_sub_tab_ == SourcesSubTab::REPOSITORIES, 21);
        canvas_.drawHLine(0, tabY + tabH + 8, SCREEN_W, 0x00, 2);

        if (async_busy_.load()) {
            renderAsyncBannerIfBusy(tabY + tabH + 14);
        } else if (!status_banner_.empty()) {
            font_.drawText(canvas_, 32, tabY + tabH + 18, font_.elideText(status_banner_, 21, SCREEN_W - 64, true), 21, 0x00, true);
        }

        int contentY = 185;
        if (sources_sub_tab_ == SourcesSubTab::INSTALLED) renderInstalledSourcesTab(contentY);
        else if (sources_sub_tab_ == SourcesSubTab::AVAILABLE) renderAvailableSourcesTab(contentY);
        else renderRepositoriesTab(contentY);
    }

    void renderInstalledSourcesTab(int startY) {
        const auto& srcList = sources_.getSources();
        int cardH = 118, maxVisible = 6;
        for (size_t i = 0; i < srcList.size() && static_cast<int>(i) < maxVisible; ++i) {
            int cy = startY + static_cast<int>(i) * (cardH + 12);
            bool isActive = (srcList[i].id == sources_.getActiveSourceId());
            canvas_.drawShadowCard(28, cy, SCREEN_W - 56, cardH, 0xFF, 2, 3);
            if (isActive) canvas_.fillRect(30, cy + 2, 8, cardH - 4, 0x00);

            font_.drawText(canvas_, 50, cy + 16, srcList[i].name, 27, 0x00, true);
            std::string meta = srcList[i].id + "  ·  " + srcList[i].version + "  ·  Lang: " + srcList[i].language +
                               "  ·  " + (srcList[i].is_aidoku ? "Aidoku WASM" : "Built-in C++");
            font_.drawText(canvas_, 50, cy + 52, meta, 19, 0x55, false);
            font_.drawText(canvas_, 50, cy + 80, font_.elideText(srcList[i].description.empty() ? "Online manga source" : srcList[i].description, 19, SCREEN_W - 440, false), 19, 0x75, false);

            font_.drawButton(canvas_, SCREEN_W - 198, cy + 30, 148, 56, isActive ? "✓ Active" : "Select", isActive, 21);
            if (srcList[i].is_aidoku) {
                font_.drawButton(canvas_, SCREEN_W - 360, cy + 30, 148, 56, "Uninstall", false, 19);
            }
        }
        int btnY = startY + std::min(static_cast<int>(srcList.size()), maxVisible) * (cardH + 12) + 20;
        if (btnY < SCREEN_H - NAV_H - 85) {
            font_.drawButton(canvas_, 35, btnY, SCREEN_W - 70, 64, "+ Browse & Install Community Extensions", false, 23);
        }
    }

    void renderAvailableSourcesTab(int startY) {
        if (!available_sources_loaded_) {
            font_.drawText(canvas_, 45, startY + 40, "Fetch community WASM extensions from configured repositories.", 24, 0x40, false);
            font_.drawButton(canvas_, 35, startY + 100, SCREEN_W - 70, 65, "↻ Load Extensions Catalog", true, 24);
            return;
        }
        if (available_aidoku_sources_.empty()) {
            font_.drawText(canvas_, 45, startY + 40, "No extensions found. Add a repository index URL in Repositories.", 23, 0x30, true);
            font_.drawButton(canvas_, 35, startY + 105, SCREEN_W - 70, 65, "↻ Retry Catalog Fetch", true, 24);
            return;
        }
        int totalItems = static_cast<int>(available_aidoku_sources_.size());
        int totalPages = std::max(1, (totalItems + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE);
        if (available_sources_page_ >= totalPages) available_sources_page_ = totalPages - 1;

        font_.drawButton(canvas_, 28, startY, 240, 50, "↻ Refresh List", false, 20);
        std::string info = "Page " + std::to_string(available_sources_page_ + 1) + " / " + std::to_string(totalPages) +
                           " (" + std::to_string(totalItems) + " available)";
        font_.drawText(canvas_, 290, startY + 14, info, 21, 0x30, true);
        if (totalPages > 1) {
            font_.drawButton(canvas_, SCREEN_W - 320, startY, 140, 50, "◄ Prev", false, 20);
            font_.drawButton(canvas_, SCREEN_W - 168, startY, 140, 50, "Next ►", false, 20);
        }

        int listStartY = startY + 65, cardH = 115;
        int startIdx = available_sources_page_ * AVAILABLE_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + AVAILABLE_PER_PAGE);
        for (int i = startIdx; i < endIdx; ++i) {
            const auto& s = available_aidoku_sources_[i];
            int cy = listStartY + (i - startIdx) * (cardH + 10);
            canvas_.drawShadowCard(28, cy, SCREEN_W - 56, cardH, 0xFF, 2, 2);
            font_.drawText(canvas_, 48, cy + 16, font_.elideText(s.name, 26, SCREEN_W - 280, true), 26, 0x00, true);
            font_.drawText(canvas_, 48, cy + 52, s.id + "  ·  " + s.version + "  ·  Lang: " + s.language, 19, 0x55, false);
            font_.drawText(canvas_, 48, cy + 80, font_.elideText(s.description.empty() ? "Aidoku WASM source" : s.description, 18, SCREEN_W - 280, false), 18, 0x75, false);
            font_.drawButton(canvas_, SCREEN_W - 200, cy + 28, 150, 56, s.is_installed ? "✓ Installed" : "+ Install", !s.is_installed, 20);
        }
    }

    void renderRepositoriesTab(int startY) {
        font_.drawText(canvas_, 32, startY, "AIDOKU REPOSITORY SOURCE LISTS", 23, 0x00, true);
        canvas_.drawHLine(32, startY + 30, SCREEN_W - 64, 0x00, 2);

        Json::Value cfg = sources_.getSourceConfig();
        const auto& sl = cfg["source_lists"].asArray();
        int curY = startY + 45;
        if (sl.empty()) {
            canvas_.drawRect(28, curY, SCREEN_W - 56, 76, 0x00, 2);
            font_.drawText(canvas_, 48, curY + 26, "No repository URLs configured yet.", 22, 0x50, false);
            curY += 95;
        } else {
            for (size_t i = 0; i < sl.size(); ++i) {
                std::string url = sl[i].str();
                canvas_.drawRect(28, curY, SCREEN_W - 56, 75, 0x00, 2);
                font_.drawText(canvas_, 46, curY + 25, font_.elideText(url, 20, SCREEN_W - 265, false), 20, 0x00, false);
                font_.drawButton(canvas_, SCREEN_W - 190, curY + 12, 140, 50, "Remove", false, 20);
                curY += 88;
                if (curY > SCREEN_H - NAV_H - 420) break;
            }
        }

        font_.drawText(canvas_, 32, curY + 12, "Add Repository Index URL:", 22, 0x00, true);
        int boxY = curY + 45, boxW = SCREEN_W - 270, boxH = 65;
        canvas_.fillRect(28, boxY, boxW, boxH, 0xFF);
        canvas_.drawRect(28, boxY, boxW, boxH, 0x00, (kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? 3 : 2);

        std::string inDisp = repo_url_input_.empty()
            ? ((kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? "_" : "Tap to enter URL (use quick tokens on keyboard)...")
            : (repo_url_input_ + ((kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? "_" : ""));
        font_.drawText(canvas_, 44, boxY + 20, font_.elideText(inDisp, 20, boxW - 32, false), 20, (repo_url_input_.empty() && !show_keyboard_) ? 0x75 : 0x00, false);
        font_.drawButton(canvas_, SCREEN_W - 225, boxY, 180, boxH, "+ Add URL", true, 21);

        if (!show_keyboard_) {
            int btnRowY = boxY + boxH + 20;
            font_.drawButton(canvas_, 28, btnRowY, SCREEN_W - 56, 62, "+ Quick Add Official Aidoku-Community Repository", false, 21);
        }
    }

    void renderDetail() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawButton(canvas_, 20, 15, 145, 50, "◄ Back", false, 21);
        font_.drawText(canvas_, 182, 24, font_.elideText(selected_manga_.title, 28, SCREEN_W - 535, true), 28, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 328, 15, 158, 50, "↻ Refresh", false, 20);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        // Cover card (136x198) + Metadata + 2-line wrapped Synopsis
        drawCoverThumbnail(28, TOP_H + 18, 136, 198, selected_manga_.cover_url, selected_manga_.title, selected_manga_.id);

        font_.drawText(canvas_, 182, TOP_H + 16, font_.elideText(selected_manga_.title, 27, SCREEN_W - 210, true), 27, 0x00, true);
        std::string srcName = sources_.getSourceDisplayName(selected_manga_.source_id.empty() ? sources_.getActiveSourceId() : selected_manga_.source_id);
        std::string meta = (selected_manga_.author.empty() ? srcName : selected_manga_.author) +
                           (selected_manga_.status.empty() ? "" : ("  ·  " + selected_manga_.status)) +
                           "  ·  " + srcName;
        font_.drawText(canvas_, 182, TOP_H + 52, font_.elideText(meta, 20, SCREEN_W - 210, false), 20, 0x45, false);

        // Synopsis (up to 3 wrapped lines)
        std::string desc = selected_manga_.description.empty()
            ? ("Total " + std::to_string(selected_chapters_.size()) + " chapters available.")
            : selected_manga_.description;
        auto synLines = font_.wrapText(desc, 19, SCREEN_W - 210, 3, false);
        int sy = TOP_H + 84;
        for (const auto& ln : synLines) {
            font_.drawText(canvas_, 182, sy, ln, 19, 0x55, false);
            sy += 24;
        }

        // Action Buttons Row at btnY = TOP_H + 188: [▶ Continue/Read] [+ Library] [⬇ Next 5] [⬇ Unread]
        int btnY = TOP_H + 188;
        MangaProgress prog;
        std::string readLabel = "▶ Read First";
        if (state_.getProgress(selected_manga_.source_id, selected_manga_.id, selected_manga_.title, prog)) {
            std::string chShort = prog.chapter_num >= 0
                ? ("Ch. " + std::to_string(static_cast<int>(prog.chapter_num)))
                : prog.chapter_title;
            readLabel = "▶ " + chShort + " (p." + std::to_string(prog.page_index + 1) + ")";
        } else {
            int firstIdx = getLowestUnreadChapterIndex();
            if (firstIdx >= 0 && !selected_chapters_[firstIdx].chapterNum.empty()) {
                readLabel = "▶ Read Ch. " + selected_chapters_[firstIdx].chapterNum;
            }
        }
        font_.drawButton(canvas_, 180, btnY, 268, 50, readLabel, true, 20);

        bool inLib = state_.isInLibrary(selected_manga_.source_id, selected_manga_.id, selected_manga_.title);
        font_.drawButton(canvas_, 462, btnY, 228, 50, inLib ? "✓ In Library" : "+ Library", inLib, 20);
        font_.drawButton(canvas_, 704, btnY, 198, 50, "⬇ Next 5", false, 19);
        font_.drawButton(canvas_, 916, btnY, 218, 50, "⬇ All Unread", false, 19);

        // Live Background Download / Async Banner Strip (y: 334..394)
        auto dlSnap = download_queue_.snapshot();
        if (async_busy_.load()) {
            renderAsyncBannerIfBusy(338);
        } else if (dlSnap.active) {
            canvas_.fillRect(28, 334, SCREEN_W - 56, 62, 0xF2);
            canvas_.drawRect(28, 334, SCREEN_W - 56, 62, 0x00, 2);
            std::string qExtra = dlSnap.queued.empty() ? "" : ("  [+" + std::to_string(dlSnap.queued.size()) + " queued]");
            font_.drawText(canvas_, 44, 342, font_.elideText(dlSnap.status + qExtra, 19, SCREEN_W - 245, true), 19, 0x00, true);
            int pbW = SCREEN_W - 245;
            canvas_.drawRect(44, 370, pbW, 14, 0x00, 2);
            int fw = std::max(0, std::min(pbW - 2, (pbW - 2) * dlSnap.pct / 100));
            if (fw > 0) canvas_.fillRect(45, 371, fw, 12, 0x00);
            font_.drawButton(canvas_, SCREEN_W - 178, 342, 138, 46, "✕ Cancel", false, 18);
        } else if (!status_banner_.empty()) {
            canvas_.fillRect(28, 340, SCREEN_W - 56, 48, 0xF6);
            canvas_.drawRect(28, 340, SCREEN_W - 56, 48, 0x00, 1);
            font_.drawText(canvas_, 44, 353, font_.elideText(status_banner_, 20, SCREEN_W - 90, true), 20, 0x00, true);
        }

        // Chapter Filter Pills + Sort Toggle Strip: chHeadY = 408
        int chHeadY = 408;
        std::string allChLbl = "All (" + std::to_string(selected_chapters_.size()) + ")";
        font_.drawButton(canvas_, 28, chHeadY, 138, 44, allChLbl, chapter_filter_ == ChapterFilter::ALL, 18);
        font_.drawButton(canvas_, 176, chHeadY, 146, 44, "Unread", chapter_filter_ == ChapterFilter::UNREAD, 18);
        font_.drawButton(canvas_, 332, chHeadY, 146, 44, "Offline", chapter_filter_ == ChapterFilter::OFFLINE, 18);

        std::string sortLabel = detail_sort_descending_ ? "⇅ Newest" : "⇅ Oldest";
        font_.drawButton(canvas_, SCREEN_W - 228, chHeadY, 200, 44, sortLabel, false, 19);
        canvas_.drawHLine(28, chHeadY + 54, SCREEN_W - 56, 0x00, 2);

        auto filteredIdx = getFilteredChapterIndices();
        int totalFiltered = static_cast<int>(filteredIdx.size());
        int totalPages = std::max(1, (totalFiltered + CHAPTERS_PER_PAGE - 1) / CHAPTERS_PER_PAGE);
        if (chapter_page_ >= totalPages) chapter_page_ = totalPages - 1;
        if (chapter_page_ < 0) chapter_page_ = 0;

        int startPos = chapter_page_ * CHAPTERS_PER_PAGE;
        int endPos = std::min(totalFiltered, startPos + CHAPTERS_PER_PAGE);

        int startY = 472, itemH = 92, rowStride = 103;
        for (int p = startPos; p < endPos; ++p) {
            int row = p - startPos;
            int y = startY + row * rowStride;
            const auto& ch = selected_chapters_[filteredIdx[p]];

            canvas_.fillRect(28, y, SCREEN_W - 56, itemH, ch.isRead ? 0xF9 : 0xFF);
            canvas_.drawRect(28, y, SCREEN_W - 56, itemH, ch.isRead ? 0x80 : 0x00, ch.isRead ? 1 : 2);

            // Tappable Left Read/Unread Status Badge (● unread vs ✓ read)
            font_.drawText(canvas_, 46, y + 30, ch.isRead ? "✓" : "●", 24, ch.isRead ? 0x70 : 0x00, true);

            std::string mainLbl = "Ch. " + ch.chapterNum + (ch.title.empty() ? "" : (" — " + ch.title));
            mainLbl = font_.elideText(mainLbl, 23, SCREEN_W - 430, !ch.isRead);
            font_.drawText(canvas_, 88, y + 18, mainLbl, 23, ch.isRead ? 0x68 : 0x00, !ch.isRead);

            std::string subLbl = ch.scanlator.empty() ? (ch.isDownloaded ? "Saved offline" : "Online") : ch.scanlator;
            if (prog.chapter_id == ch.id && !ch.id.empty() && prog.total_pages > 0) {
                subLbl += "  ·  Page " + std::to_string(prog.page_index + 1) + " / " + std::to_string(prog.total_pages);
            }
            font_.drawText(canvas_, 88, y + 52, font_.elideText(subLbl, 18, SCREEN_W - 430, false), 18, 0x70, false);

            bool queued = download_queue_.isQueuedOrActive(selected_manga_.title, ch.id, ch.chapterNum);
            if (ch.isDownloaded) {
                font_.drawButton(canvas_, SCREEN_W - 188, y + 20, 144, 50, "✓ Read", true, 20);
            } else if (queued) {
                font_.drawButton(canvas_, SCREEN_W - 188, y + 20, 144, 50, "Queued...", false, 18, 0xEC);
            } else {
                font_.drawButton(canvas_, SCREEN_W - 322, y + 20, 130, 50, "▶ Read", true, 20);
                font_.drawButton(canvas_, SCREEN_W - 180, y + 20, 136, 50, "⬇ Save", false, 19);
            }
        }

        if (totalPages > 1) {
            std::string range;
            if (startPos < endPos) {
                range = "Ch. " + selected_chapters_[filteredIdx[startPos]].chapterNum +
                        " .. " + selected_chapters_[filteredIdx[endPos - 1]].chapterNum;
            }
            ui::drawPaginationBar(canvas_, font_, SCREEN_H - NAV_H - 60, chapter_page_, totalPages, range);
        }
    }

    void renderDownloads() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 28, 22, "Downloads & Storage", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 350, 15, 180, 50, "Clear Cache", false, 19);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        int curY = TOP_H + 18;
        auto snap = download_queue_.snapshot();

        if (snap.active || !snap.queued.empty()) {
            canvas_.drawShadowCard(28, curY, SCREEN_W - 56, 90, 0xF4, 2, 3);
            std::string head = snap.active
                ? (snap.current.manga.title + " — " + snap.status)
                : ("Starting queue (" + std::to_string(snap.queued.size()) + " pending)...");
            if (!snap.queued.empty()) head += "  [+" + std::to_string(snap.queued.size()) + " queued]";
            font_.drawText(canvas_, 46, curY + 16, font_.elideText(head, 20, SCREEN_W - 415, true), 20, 0x00, true);

            int pbW = SCREEN_W - 420;
            canvas_.drawRect(46, curY + 54, pbW, 18, 0x00, 2);
            int fw = std::max(0, std::min(pbW - 2, (pbW - 2) * snap.pct / 100));
            if (fw > 0) canvas_.fillRect(47, curY + 55, fw, 16, 0x00);

            font_.drawButton(canvas_, SCREEN_W - 342, curY + 20, 146, 50, "Cancel", false, 19);
            font_.drawButton(canvas_, SCREEN_W - 184, curY + 20, 142, 50, "Cancel All", true, 19);
            curY += 106;
        }

        auto offlineList = getOfflineCbzList();
        double totalCbzMb = 0.0;
        for (const auto& it : offlineList) totalCbzMb += it.mb;
        double cacheMb = computeCacheSizeMb();

        char sumBuf[160];
        std::snprintf(sumBuf, sizeof(sumBuf), "Saved Offline: %zu chapters (%.1f MB)   ·   Temp Cache: %.1f MB",
                      offlineList.size(), totalCbzMb, cacheMb);
        font_.drawText(canvas_, 30, curY + 4, sumBuf, 20, 0x40, true);
        curY += 40;

        if (offlineList.empty() && !snap.active) {
            std::string msg = "No offline chapters saved yet.";
            font_.drawText(canvas_, (SCREEN_W - font_.measureText(msg, 28, true)) / 2, 580, msg, 28, 0x55, true);
            std::string hint = "Use [⬇ Save] or [⬇ Next 5] on any manga detail page to queue offline chapters.";
            font_.drawText(canvas_, (SCREEN_W - font_.measureText(hint, 21, false)) / 2, 632, hint, 21, 0x75, false);
            return;
        }

        int totalPages = std::max(1, (static_cast<int>(offlineList.size()) + DOWNLOADS_PER_PAGE - 1) / DOWNLOADS_PER_PAGE);
        if (downloads_page_ >= totalPages) downloads_page_ = totalPages - 1;
        int startIdx = downloads_page_ * DOWNLOADS_PER_PAGE;
        int endIdx = std::min(static_cast<int>(offlineList.size()), startIdx + DOWNLOADS_PER_PAGE);

        int itemH = 98, rowStride = 110;
        for (int i = startIdx; i < endIdx; ++i) {
            const auto& item = offlineList[i];
            int itemY = curY + (i - startIdx) * rowStride;
            canvas_.drawShadowCard(28, itemY, SCREEN_W - 56, itemH, 0xFF, 2, 2);
            font_.drawText(canvas_, 46, itemY + 18, font_.elideText(item.manga_title, 25, SCREEN_W - 410, true), 25, 0x00, true);

            char szBuf[96];
            std::snprintf(szBuf, sizeof(szBuf), "%s  ·  %.1f MB", item.chapter_stem.c_str(), item.mb);
            font_.drawText(canvas_, 46, itemY + 54, szBuf, 20, 0x55, false);

            font_.drawButton(canvas_, SCREEN_W - 328, itemY + 23, 132, 52, "✕ Delete", false, 19);
            font_.drawButton(canvas_, SCREEN_W - 182, itemY + 23, 138, 52, "▶ Read", true, 20);
        }

        if (totalPages > 1) {
            ui::drawPaginationBar(canvas_, font_, SCREEN_H - NAV_H - 60, downloads_page_, totalPages);
        }
    }

    void renderSettings() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 28, 22, "Settings & Device Controls", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 157, 15, 137, 50, "✕ Exit", false, 21);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 40, TOP_H + 14, status_banner_, 21, 0x00, true);
        }
        font_.drawText(canvas_, 40, TOP_H + 46, "Kindle Oasis (Zelda 1264×1680 @ 300 PPI · GLR16/DU Waveforms)", 21, 0x55, false);

        int y = 172;
        font_.drawText(canvas_, 40, y + 12, "Frontlight Brightness:", 25, 0x00, true);
        font_.drawButton(canvas_, 345, y, 70, 54, "-", false, 28);
        std::string flVal = std::to_string(state_.settings().frontlight) + " / 24";
        font_.drawText(canvas_, 435, y + 13, flVal, 24, 0x00, true);
        font_.drawButton(canvas_, 535, y, 70, 54, "+", false, 28);

        y = 252;
        font_.drawText(canvas_, 40, y + 12, "Reading Direction:", 25, 0x00, true);
        std::string modeLabel = (state_.settings().reading_direction == "RTL") ? "Manga (RTL)" : "Comic (LTR)";
        font_.drawButton(canvas_, 345, y, 305, 54, modeLabel, true, 21);

        y = 332;
        font_.drawText(canvas_, 40, y + 12, "Page Scaling / Margins:", 25, 0x00, true);
        std::string fitLbl = (state_.settings().fit_mode == "crop") ? "Auto-Crop Margins"
                           : ((state_.settings().fit_mode == "width") ? "Fit Full Width" : "Contain Full Page");
        font_.drawButton(canvas_, 345, y, 325, 54, fitLbl, true, 21);

        y = 412;
        font_.drawText(canvas_, 40, y + 12, "Grip / Touch Rotation:", 25, 0x00, true);
        std::string rotLabel = (state_.settings().touch_rotation == 180) ? "Buttons Left (180°)" : "Buttons Right (0°)";
        font_.drawButton(canvas_, 345, y, 325, 54, rotLabel, true, 21);

        y = 492;
        font_.drawText(canvas_, 40, y + 12, "Reader Options:", 25, 0x00, true);
        font_.drawButton(canvas_, 345, y, 270, 54, state_.settings().show_mini_counter ? "✓ Page Pill: ON" : "Page Pill: OFF", state_.settings().show_mini_counter, 20);
        font_.drawButton(canvas_, 635, y, 315, 54, state_.settings().button_inverted ? "✓ Invert Keys: ON" : "Invert Keys: OFF", state_.settings().button_inverted, 20);

        font_.drawButton(canvas_, 100, 590, SCREEN_W - 200, 72, "Exit rm2manga to Kindle Home", true, 23);
        font_.drawButton(canvas_, 100, 680, SCREEN_W - 200, 72, "Clear Reader & Cover Cache", false, 23);
        font_.drawButton(canvas_, 100, 770, SCREEN_W - 200, 72, "Rescan Manga Storage", false, 23);
        font_.drawButton(canvas_, 100, 860, SCREEN_W - 200, 72, "⟳ Full Refresh Screen (Clear Ghosting)", false, 23);
    }

    void renderReaderToast(const std::string& msg) {
        int w = 560, h = 80, x = (SCREEN_W - w) / 2, y = SCREEN_H / 2 - 40;
        canvas_.fillRect(x, y, w, h, 0x00);
        canvas_.drawRect(x, y, w, h, 0xFF, 2);
        int tw = font_.measureText(msg, 25, true);
        font_.drawText(canvas_, x + (w - tw) / 2, y + 25, msg, 25, 0xFF, true);
        display_.update(x, y, w, h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void renderReaderDownloadProgress(const std::string& status, int pct) {
        int w = 660, h = 230, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
        canvas_.fillRect(x, y, w, h, 0xFF);
        canvas_.drawRect(x, y, w, h, 0x00, 3);

        std::string s = font_.elideText(status, 23, w - 60, true);
        int tw = font_.measureText(s, 23, true);
        font_.drawText(canvas_, x + (w - tw) / 2, y + 26, s, 23, 0x00, true);

        int barW = w - 80, barH = 22, barX = x + 40, barY = y + 76;
        canvas_.drawRect(barX, barY, barW, barH, 0x00, 2);
        int fw = (barW - 4) * std::max(0, std::min(100, pct)) / 100;
        if (fw > 0) canvas_.fillRect(barX + 2, barY + 2, fw, barH - 4, 0x00);

        font_.drawButton(canvas_, x + (w - 200) / 2, y + 148, 200, 54, "✕ Cancel", false, 21);
        display_.update(x, y, w, h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void renderReader() {
        canvas_.clear(0xFF);

        // 1. Render pre-decoded / area-averaged / auto-cropped manga page
        if (!reader_page_files_.empty() && reader_current_page_ >= 0 &&
            reader_current_page_ < static_cast<int>(reader_page_files_.size())) {
            if (!reader_cache_.renderPage(canvas_, reader_current_page_, state_.settings().fit_mode,
                                          reader_zoom_2x_, reader_pan_x_, reader_pan_y_)) {
                font_.drawText(canvas_, SCREEN_W / 2 - 160, SCREEN_H / 2 - 20, "Failed to decode page", 28, 0x00, true);
            }
        } else {
            font_.drawText(canvas_, SCREEN_W / 2 - 150, SCREEN_H / 2 - 20, "No page image available", 28, 0x00, false);
        }

        // 2. Floating [✕ Exit 2x Zoom] pill when zoomed with HUD hidden (matching ReaderView.qml)
        if (reader_zoom_2x_ && !reader_show_overlay_) {
            font_.drawButton(canvas_, 20, 20, 210, 54, "✕ Exit 2x Zoom", true, 20);
        }

        // 3. Subtle fullscreen bottom-right mini page counter pill (e.g. "12 / 45")
        if (!reader_show_overlay_ && state_.settings().show_mini_counter && reader_total_pages_ > 0) {
            std::string mini = std::to_string(reader_current_page_ + 1) + " / " + std::to_string(reader_total_pages_);
            int mw = font_.measureText(mini, 18, true) + 22;
            int mx = SCREEN_W - mw - 14;
            int my = SCREEN_H - 42;
            canvas_.fillRect(mx, my, mw, 30, 0xFA);
            canvas_.drawRect(mx, my, mw, 30, 0x40, 1);
            font_.drawText(canvas_, mx + 11, my + 5, mini, 18, 0x20, true);
        }

        // 4. Full Reader HUD (Top Bar + Bottom Scrubber & Controls)
        if (reader_show_overlay_) {
            canvas_.fillRect(0, 0, SCREEN_W, TOP_H + 4, 0xF6);
            canvas_.drawHLine(0, TOP_H + 4, SCREEN_W, 0x00, 2);
            font_.drawButton(canvas_, 18, 16, 148, 52, "◄ Back", false, 21);

            std::string titleStr = selected_manga_.title;
            if (current_chapter_idx_ >= 0 && current_chapter_idx_ < static_cast<int>(selected_chapters_.size())) {
                const auto& ch = selected_chapters_[current_chapter_idx_];
                titleStr += " · Ch. " + (ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            }
            font_.drawText(canvas_, 182, 27, font_.elideText(titleStr, 24, SCREEN_W - 630, true), 24, 0x00, true);

            font_.drawButton(canvas_, SCREEN_W - 428, 16, 132, 52, "⟳ Ghost", false, 19);
            font_.drawButton(canvas_, SCREEN_W - 284, 16, 148, 52, fitModeShortLabel(state_.settings().fit_mode), false, 19);
            font_.drawButton(canvas_, SCREEN_W - 124, 16, 106, 52, reader_zoom_2x_ ? "2x ON" : "1x", reader_zoom_2x_, 20);

            // Bottom HUD Panel (height = 220)
            int hudY = SCREEN_H - 220;
            canvas_.fillRect(0, hudY, SCREEN_W, 220, 0xF6);
            canvas_.drawHLine(0, hudY, SCREEN_W, 0x00, 3);

            // Interactive RTL-aware Page Scrubber Track
            int barX = 48, barY = hudY + 34, barW = SCREEN_W - 96, barH = 14;
            canvas_.fillRect(barX, barY, barW, barH, 0xDF);
            canvas_.drawRect(barX, barY, barW, barH, 0x00, 2);
            bool isRTL = (state_.settings().reading_direction == "RTL");
            float progress = (reader_total_pages_ > 1)
                ? (static_cast<float>(reader_current_page_) / static_cast<float>(reader_total_pages_ - 1))
                : 0.0f;
            float knobRatio = isRTL ? (1.0f - progress) : progress;
            int knobX = barX + static_cast<int>(knobRatio * barW);
            if (isRTL) {
                canvas_.fillRect(knobX, barY + 2, barX + barW - knobX, barH - 4, 0x00);
            } else {
                canvas_.fillRect(barX + 2, barY + 2, std::max(0, knobX - barX), barH - 4, 0x00);
            }
            canvas_.fillRect(knobX - 12, barY - 9, 24, barH + 18, 0x00);
            canvas_.drawRect(knobX - 12, barY - 9, 24, barH + 18, 0xFF, 2);

            // Row 1: Light [-] [val] [+] | [RTL/LTR] | [Pill ON/OFF] | [Close HUD]
            int r1Y = hudY + 78;
            font_.drawText(canvas_, 44, r1Y + 13, "Light", 21, 0x00, true);
            font_.drawButton(canvas_, 145, r1Y, 64, 50, "-", false, 26);
            std::string flStr = std::to_string(state_.settings().frontlight);
            font_.drawText(canvas_, 240, r1Y + 13, flStr, 22, 0x00, true);
            font_.drawButton(canvas_, 295, r1Y, 64, 50, "+", false, 26);

            font_.drawButton(canvas_, 400, r1Y, 170, 50, isRTL ? "Mode: RTL" : "Mode: LTR", true, 19);
            font_.drawButton(canvas_, 595, r1Y, 225, 50, state_.settings().show_mini_counter ? "✓ Page Pill" : "Page Pill", false, 19);
            font_.drawButton(canvas_, SCREEN_W - 210, r1Y, 170, 50, "✕ Hide Menu", false, 19);

            // Row 2: Chapter & Page Navigation
            int r2Y = hudY + 146;
            font_.drawButton(canvas_, 30, r2Y, 185, 56, isRTL ? "◄ Next Ch" : "◄ Prev Ch", false, 20);
            font_.drawButton(canvas_, 230, r2Y, 185, 56, isRTL ? "◄ Next Pg" : "◄ Prev Pg", false, 20);

            std::string pgCenter = "Page " + std::to_string(reader_current_page_ + 1) + " of " + std::to_string(reader_total_pages_);
            int pw = font_.measureText(pgCenter, 22, true);
            font_.drawText(canvas_, (SCREEN_W - pw) / 2, r2Y + 16, pgCenter, 22, 0x00, true);

            font_.drawButton(canvas_, SCREEN_W - 415, r2Y, 185, 56, isRTL ? "Prev Pg ►" : "Next Pg ►", false, 20);
            font_.drawButton(canvas_, SCREEN_W - 215, r2Y, 185, 56, isRTL ? "Prev Ch ►" : "Next Ch ►", false, 20);
        }
    }

    void renderKeyboard() {
        // Optional URL Quick-Token Bar above keyboard when entering Repository URLs
        if (kb_target_ == KeyboardTarget::REPO_URL) {
            int stripY = KB_Y - 62;
            canvas_.fillRect(0, stripY, SCREEN_W, 62, 0xEAEAEA & 0xFF);
            canvas_.drawHLine(0, stripY, SCREEN_W, 0x00, 2);
            int tx = 24;
            for (const auto& tok : ui::urlQuickTokens()) {
                int tw = font_.measureText(tok, 19, true) + 28;
                if (tx + tw > SCREEN_W - 20) break;
                font_.drawButton(canvas_, tx, stripY + 9, tw, 44, tok, false, 19);
                tx += tw + 10;
            }
        }

        int kbH = SCREEN_H - NAV_H - KB_Y;
        canvas_.fillRect(0, KB_Y, SCREEN_W, kbH, 0xF2);
        canvas_.drawHLine(0, KB_Y, SCREEN_W, 0x00, 3);

        int y0 = KB_START_Y;
        const char* sym0[] = {"1","2","3","4","5","6","7","8","9","0"};
        const char* up0[] = {"Q","W","E","R","T","Y","U","I","O","P"};
        const char* low0[] = {"q","w","e","r","t","y","u","i","o","p"};
        for (int k = 0; k < 10; ++k) {
            renderKey(26 + k * KB_KEY_STEP, y0, KB_KEY_W, KB_KEY_H, kb_symbols_mode_ ? sym0[k] : (kb_shift_active_ ? up0[k] : low0[k]), false, false, 32);
        }

        int y1 = KB_START_Y + KB_ROW_H;
        const char* sym1[] = {"-","/",":",";","(",")","$","&","@"};
        const char* up1[] = {"A","S","D","F","G","H","J","K","L"};
        const char* low1[] = {"a","s","d","f","g","h","j","k","l"};
        for (int k = 0; k < 9; ++k) {
            renderKey(87 + k * KB_KEY_STEP, y1, KB_KEY_W, KB_KEY_H, kb_symbols_mode_ ? sym1[k] : (kb_shift_active_ ? up1[k] : low1[k]), false, false, 32);
        }

        int y2 = KB_START_Y + 2 * KB_ROW_H;
        renderKey(26, y2, 175, KB_KEY_H, kb_shift_active_ ? "⇧ Shift" : "⇧", kb_shift_active_, !kb_shift_active_, 26);
        const char* sym2[] = {".",",","?","!","'","\"","_"};
        const char* up2[] = {"Z","X","C","V","B","N","M"};
        const char* low2[] = {"z","x","c","v","b","n","m"};
        for (int k = 0; k < 7; ++k) {
            renderKey(209 + k * KB_KEY_STEP, y2, KB_KEY_W, KB_KEY_H, kb_symbols_mode_ ? sym2[k] : (kb_shift_active_ ? up2[k] : low2[k]), false, false, 32);
        }
        renderKey(1063, y2, 175, KB_KEY_H, "⌫", false, true, 28);

        int y3 = KB_START_Y + 3 * KB_ROW_H;
        renderKey(26, y3, 140, KB_KEY_H, kb_symbols_mode_ ? "ABC" : "?123", kb_symbols_mode_, !kb_symbols_mode_, 24);
        renderKey(174, y3, 130, KB_KEY_H, "Clear", false, true, 22);
        renderKey(312, y3, 490, KB_KEY_H, "space", false, false, 24);
        renderKey(810, y3, 130, KB_KEY_H, "Close", false, true, 22);
        renderKey(948, y3, 290, KB_KEY_H, (kb_target_ == KeyboardTarget::REPO_URL) ? "Done ↵" : "Search ↵", true, false, 26);
    }

    void renderBottomNav() {
        int navY = SCREEN_H - NAV_H;
        canvas_.fillRect(0, navY, SCREEN_W, NAV_H, 0xFF);
        canvas_.drawHLine(0, navY, SCREEN_W, 0x00, 2);

        auto dlSnap = download_queue_.snapshot();
        std::string dlLabel = "Downloads";
        if (dlSnap.active || !dlSnap.queued.empty()) {
            int count = (dlSnap.active ? 1 : 0) + static_cast<int>(dlSnap.queued.size());
            dlLabel = "Downloads (" + std::to_string(count) + ")";
        }

        const std::string tabs[5] = {"Library", "Recent", "Browse", dlLabel, "Settings"};
        int tabW = SCREEN_W / 5;

        for (int i = 0; i < 5; ++i) {
            int x = i * tabW;
            bool isActive = (selected_tab_ == i);
            if (isActive) {
                canvas_.fillRect(x + 6, navY + 6, tabW - 12, NAV_H - 12, 0x00);
                int tw = font_.measureText(tabs[i], 23, true);
                font_.drawText(canvas_, x + (tabW - tw) / 2, navY + 32, tabs[i], 23, 0xFF, true);
            } else {
                int tw = font_.measureText(tabs[i], 23, false);
                font_.drawText(canvas_, x + (tabW - tw) / 2, navY + 32, tabs[i], 23, 0x00, false);
            }
        }
    }
};

} // namespace kindle
