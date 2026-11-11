#pragma once

#include "canvas.hpp"
#include "../third_party/stb_image.h"
#include "../../../backend/src/util.hpp"
#include "../../../backend/src/mangadex_client.hpp"
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

namespace kindle {

class CoverCache {
public:
    struct ScaledThumb {
        int w = 0;
        int h = 0;
        std::vector<uint8_t> pixels;
    };

private:
    struct CoverRequest {
        std::string url;
        std::string destPath;
        std::string fallbackTitle;
    };

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<CoverRequest> queue_;
    std::unordered_set<std::string> pending_paths_;
    std::unordered_set<std::string> failed_paths_;
    std::unordered_map<std::string, ScaledThumb> ram_thumbs_;
    std::deque<std::string> lru_keys_;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::function<void()> on_ready_;
    mangadex::Client md_fallback_;

    static constexpr size_t MAX_RAM_THUMBS = 36;

public:
    CoverCache() = default;
    ~CoverCache() { stop(); }

    void start(std::function<void()> onReady) {
        on_ready_ = std::move(onReady);
        if (running_.load()) return;
        running_ = true;
        worker_ = std::thread(&CoverCache::workerLoop, this);
    }

    void stop() {
        if (running_.load()) {
            running_ = false;
            cv_.notify_all();
            if (worker_.joinable()) {
                worker_.join();
            }
        }
    }

    void clearMemoryCache() {
        std::lock_guard<std::mutex> lock(mu_);
        ram_thumbs_.clear();
        lru_keys_.clear();
        failed_paths_.clear();
    }

    static std::string makeSafeCachePath(const std::string& mangaId, const std::string& fallbackTitle) {
        std::string safe = mangaId.empty() ? fallbackTitle : mangaId;
        if (safe.empty()) safe = "cover";
        for (char& c : safe) {
            if (c == '/' || c == ':' || c == ' ' || c == '\\' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
        if (safe.size() > 96) {
            safe = safe.substr(0, 96);
        }
        return "/mnt/us/manga/.cache/covers/" + safe + ".jpg";
    }

    // Non-blocking: blits cached thumbnail if on disk/RAM; otherwise enqueues async download and returns false
    bool tryBlitCover(Canvas& canvas, int x, int y, int w, int h,
                      const std::string& pathOrUrl,
                      const std::string& fallbackTitle,
                      const std::string& mangaId) {
        if (w <= 0 || h <= 0) return false;

        std::string localPath = pathOrUrl;
        bool isRemote = (pathOrUrl.rfind("http://", 0) == 0 || pathOrUrl.rfind("https://", 0) == 0);
        if (isRemote || pathOrUrl.empty()) {
            localPath = makeSafeCachePath(mangaId, fallbackTitle);
        }

        const std::string ramKey = localPath + "#" + std::to_string(w) + "x" + std::to_string(h);
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = ram_thumbs_.find(ramKey);
            if (it != ram_thumbs_.end() && static_cast<int>(it->second.pixels.size()) == w * h) {
                canvas.blitImage(x, y, w, h, it->second.pixels.data(), w, h, false);
                return true;
            }
        }

        std::error_code ec;
        if (!localPath.empty() && fs::exists(localPath, ec) && fs::file_size(localPath, ec) > 64) {
            int cw = 0, ch = 0, channels = 0;
            unsigned char* data = stbi_load(localPath.c_str(), &cw, &ch, &channels, 1);
            if (data && cw > 0 && ch > 0) {
                // Pre-scale using Canvas's area-averaging filter into a compact w*h buffer
                std::vector<uint8_t> scaled(static_cast<size_t>(w * h), 0xFF);
                Canvas thumbCanvas(w, h, w, scaled.data());
                // Center-crop ("aspect fill") to match Tankobon cover proportions cleanly
                double scaleX = static_cast<double>(w) / cw;
                double scaleY = static_cast<double>(h) / ch;
                double scale = std::max(scaleX, scaleY);
                int cropW = std::min(cw, std::max(1, static_cast<int>(w / scale)));
                int cropH = std::min(ch, std::max(1, static_cast<int>(h / scale)));
                int cropX = (cw - cropW) / 2;
                int cropY = (ch - cropH) / 2;
                thumbCanvas.blitImageRegion(0, 0, w, h, data, cw, ch, cropX, cropY, cropW, cropH, false);
                stbi_image_free(data);

                canvas.blitImage(x, y, w, h, scaled.data(), w, h, false);

                std::lock_guard<std::mutex> lock(mu_);
                if (ram_thumbs_.size() >= MAX_RAM_THUMBS && !lru_keys_.empty()) {
                    ram_thumbs_.erase(lru_keys_.front());
                    lru_keys_.pop_front();
                }
                ram_thumbs_[ramKey] = {w, h, std::move(scaled)};
                lru_keys_.push_back(ramKey);
                return true;
            }
            if (data) stbi_image_free(data);
        }

        // Enqueue background fetch if remote URL or title fallback is available
        if (isRemote || !fallbackTitle.empty()) {
            enqueue(isRemote ? pathOrUrl : "", localPath, fallbackTitle);
        }
        return false;
    }

    void enqueue(const std::string& url, const std::string& destPath, const std::string& fallbackTitle) {
        if (destPath.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (pending_paths_.count(destPath) || failed_paths_.count(destPath)) return;
            if (url.empty() && fallbackTitle.empty()) return;
            // Keep queue bounded to most recent visible items
            if (queue_.size() >= 24) {
                pending_paths_.erase(queue_.front().destPath);
                queue_.pop_front();
            }
            pending_paths_.insert(destPath);
            queue_.push_back({url, destPath, fallbackTitle});
        }
        cv_.notify_one();
    }

private:
    void workerLoop() {
        std::error_code ec;
        fs::create_directories("/mnt/us/manga/.cache/covers", ec);

        while (running_.load()) {
            CoverRequest req;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]() { return !queue_.empty() || !running_.load(); });
                if (!running_.load()) break;
                req = std::move(queue_.front());
                queue_.pop_front();
            }

            bool ok = false;
            std::string tmpPath = req.destPath + ".part";
            if (!req.url.empty()) {
                if (util::httpsDownloadFile(req.url, tmpPath, 10) &&
                    fs::exists(tmpPath, ec) && fs::file_size(tmpPath, ec) > 128) {
                    fs::rename(tmpPath, req.destPath, ec);
                    ok = !ec;
                } else {
                    fs::remove(tmpPath, ec);
                }
            }

            // Fallback: if Aidoku/local item had no direct cover URL, look up cover by title on MangaDex
            if (!ok && req.url.empty() && !req.fallbackTitle.empty() && running_.load()) {
                Json::Value list = md_fallback_.queryMangaList(req.fallbackTitle, "search", nullptr);
                if (list.isArray() && !list.asArray().empty()) {
                    std::string mdCover = list.asArray()[0]["cover_url"].str();
                    if (!mdCover.empty() && util::httpsDownloadFile(mdCover, tmpPath, 10) &&
                        fs::exists(tmpPath, ec) && fs::file_size(tmpPath, ec) > 128) {
                        fs::rename(tmpPath, req.destPath, ec);
                        ok = !ec;
                    } else {
                        fs::remove(tmpPath, ec);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(mu_);
                pending_paths_.erase(req.destPath);
                if (!ok) {
                    failed_paths_.insert(req.destPath);
                }
            }

            if (ok && running_.load() && on_ready_) {
                on_ready_();
            }
        }
    }
};

} // namespace kindle
