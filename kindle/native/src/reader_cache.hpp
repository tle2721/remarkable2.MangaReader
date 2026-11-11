#pragma once

#include "canvas.hpp"
#include "../third_party/stb_image.h"
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstring>
#include <algorithm>

namespace kindle {

class ReaderPageCache {
private:
    struct Slot {
        std::string filePath;
        std::string fitMode;
        int width = 0;
        int height = 0;
        int stride = 0;
        std::vector<uint8_t> buffer;
        bool valid = false;
    };

    struct RawDecoded {
        std::string filePath;
        int w = 0;
        int h = 0;
        int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
        std::vector<uint8_t> pixels;
    };

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> page_files_;
    int target_page_ = 0;
    std::string fit_mode_ = "crop";
    int screen_w_ = 1264;
    int screen_h_ = 1680;
    int stride_ = 1264;

    Slot slots_[3];
    int next_evict_ = 0;
    RawDecoded zoomed_raw_;

    std::atomic<bool> running_{false};
    std::thread worker_;

    static std::string stripFileScheme(const std::string& p) {
        if (p.rfind("file://", 0) == 0) return p.substr(7);
        return p;
    }

    static bool renderPageToBuffer(const std::string& rawPath,
                                   const std::string& fitMode,
                                   int screenW, int screenH, int stride,
                                   std::vector<uint8_t>& outBuf) {
        int imgW = 0, imgH = 0, channels = 0;
        unsigned char* data = stbi_load(rawPath.c_str(), &imgW, &imgH, &channels, 1);
        if (!data || imgW <= 0 || imgH <= 0) {
            if (data) stbi_image_free(data);
            return false;
        }

        outBuf.assign(static_cast<size_t>(stride * screenH), 0xFF);
        Canvas pageCanvas(screenW, screenH, stride, outBuf.data());

        int cropX = 0, cropY = 0, cropW = imgW, cropH = imgH;
        if (fitMode == "crop") {
            Canvas::detectContentBounds(data, imgW, imgH, cropX, cropY, cropW, cropH);
        }

        double scaleW = static_cast<double>(screenW) / cropW;
        double scaleH = static_cast<double>(screenH) / cropH;
        double scale = (fitMode == "width") ? scaleW : std::min(scaleW, scaleH);

        int dstW = std::max(1, static_cast<int>(cropW * scale));
        int dstH = std::max(1, static_cast<int>(cropH * scale));
        int offsetX = (screenW - dstW) / 2;
        int offsetY = (fitMode == "width" && dstH > screenH) ? 0 : ((screenH - dstH) / 2);

        pageCanvas.blitImageRegion(offsetX, offsetY, dstW, dstH,
                                   data, imgW, imgH,
                                   cropX, cropY, cropW, cropH, true);
        stbi_image_free(data);
        return true;
    }

public:
    ReaderPageCache() = default;
    ~ReaderPageCache() { stop(); }

    void start(int screenW, int screenH, int stride) {
        screen_w_ = screenW;
        screen_h_ = screenH;
        stride_ = stride;
        if (running_.load()) return;
        running_ = true;
        worker_ = std::thread(&ReaderPageCache::workerLoop, this);
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

    void setChapterPages(const std::vector<std::string>& files, int initialPage, const std::string& fitMode) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            page_files_ = files;
            target_page_ = initialPage;
            fit_mode_ = fitMode;
            for (auto& s : slots_) s.valid = false;
            zoomed_raw_.filePath.clear();
            zoomed_raw_.pixels.clear();
        }
        cv_.notify_one();
    }

    void invalidate() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& s : slots_) s.valid = false;
        zoomed_raw_.filePath.clear();
        zoomed_raw_.pixels.clear();
    }

    // Blits page to canvas (<2ms memcpy on cache hit) and schedules background N+1 / N-1 pre-decode
    bool renderPage(Canvas& canvas, int pageIdx, const std::string& fitMode,
                    bool zoom2x = false, float panX = 0.5f, float panY = 0.5f) {
        std::string rawPath;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (pageIdx < 0 || pageIdx >= static_cast<int>(page_files_.size())) return false;
            rawPath = stripFileScheme(page_files_[pageIdx]);
            target_page_ = pageIdx;
            fit_mode_ = fitMode;

            if (!zoom2x) {
                for (int i = 0; i < 3; ++i) {
                    const auto& s = slots_[i];
                    if (s.valid && s.filePath == rawPath && s.fitMode == fitMode &&
                        s.stride == canvas.stride() && s.height == canvas.height() &&
                        s.buffer.size() == static_cast<size_t>(canvas.stride() * canvas.height())) {
                        std::memcpy(canvas.buffer(), s.buffer.data(), s.buffer.size());
                        cv_.notify_one();
                        return true;
                    }
                }
            }
        }

        if (zoom2x) {
            bool ok = renderZoomedPage(canvas, rawPath, fitMode, panX, panY);
            cv_.notify_one();
            return ok;
        }

        // Cache miss: decode synchronously once, store in slot, and wake background worker for neighbors
        std::vector<uint8_t> rendered;
        if (!renderPageToBuffer(rawPath, fitMode, canvas.width(), canvas.height(), canvas.stride(), rendered)) {
            return false;
        }

        std::memcpy(canvas.buffer(), rendered.data(), rendered.size());
        {
            std::lock_guard<std::mutex> lock(mu_);
            storeSlotLocked(rawPath, fitMode, canvas.width(), canvas.height(), canvas.stride(), std::move(rendered));
        }
        cv_.notify_one();
        return true;
    }

private:
    bool renderZoomedPage(Canvas& canvas, const std::string& rawPath, const std::string& fitMode,
                          float panX, float panY) {
        std::unique_lock<std::mutex> lock(mu_);
        if (zoomed_raw_.filePath != rawPath || zoomed_raw_.pixels.empty()) {
            lock.unlock();
            int imgW = 0, imgH = 0, channels = 0;
            unsigned char* data = stbi_load(rawPath.c_str(), &imgW, &imgH, &channels, 1);
            if (!data || imgW <= 0 || imgH <= 0) {
                if (data) stbi_image_free(data);
                return false;
            }
            int cx = 0, cy = 0, cw = imgW, ch = imgH;
            if (fitMode == "crop") {
                Canvas::detectContentBounds(data, imgW, imgH, cx, cy, cw, ch);
            }
            std::vector<uint8_t> pix(data, data + static_cast<size_t>(imgW * imgH));
            stbi_image_free(data);

            lock.lock();
            zoomed_raw_ = {rawPath, imgW, imgH, cx, cy, cw, ch, std::move(pix)};
        }

        // Compute 2x viewport inside content bounds
        panX = std::max(0.0f, std::min(1.0f, panX));
        panY = std::max(0.0f, std::min(1.0f, panY));

        int viewW = std::max(1, zoomed_raw_.cropW / 2);
        int viewH = std::max(1, zoomed_raw_.cropH / 2);
        int vx = zoomed_raw_.cropX + static_cast<int>((zoomed_raw_.cropW - viewW) * panX);
        int vy = zoomed_raw_.cropY + static_cast<int>((zoomed_raw_.cropH - viewH) * panY);

        double scale = std::min(static_cast<double>(canvas.width()) / viewW,
                                static_cast<double>(canvas.height()) / viewH);
        int dstW = std::max(1, static_cast<int>(viewW * scale));
        int dstH = std::max(1, static_cast<int>(viewH * scale));
        int ox = (canvas.width() - dstW) / 2;
        int oy = (canvas.height() - dstH) / 2;

        canvas.clear(0xFF);
        canvas.blitImageRegion(ox, oy, dstW, dstH,
                               zoomed_raw_.pixels.data(), zoomed_raw_.w, zoomed_raw_.h,
                               vx, vy, viewW, viewH, true);
        return true;
    }

    bool hasSlotLocked(const std::string& rawPath, const std::string& fitMode) const {
        for (int i = 0; i < 3; ++i) {
            if (slots_[i].valid && slots_[i].filePath == rawPath && slots_[i].fitMode == fitMode) {
                return true;
            }
        }
        return false;
    }

    void storeSlotLocked(const std::string& rawPath, const std::string& fitMode,
                         int w, int h, int stride, std::vector<uint8_t>&& buf) {
        for (int i = 0; i < 3; ++i) {
            if (!slots_[i].valid || (slots_[i].filePath == rawPath && slots_[i].fitMode == fitMode)) {
                slots_[i] = {rawPath, fitMode, w, h, stride, std::move(buf), true};
                return;
            }
        }
        // Evict slot that isn't the current target page
        std::string curPath = (target_page_ >= 0 && target_page_ < static_cast<int>(page_files_.size()))
            ? stripFileScheme(page_files_[target_page_]) : "";
        for (int tries = 0; tries < 3; ++tries) {
            int idx = (next_evict_ + tries) % 3;
            if (slots_[idx].filePath != curPath) {
                slots_[idx] = {rawPath, fitMode, w, h, stride, std::move(buf), true};
                next_evict_ = (idx + 1) % 3;
                return;
            }
        }
        slots_[0] = {rawPath, fitMode, w, h, stride, std::move(buf), true};
    }

    void workerLoop() {
        while (running_.load()) {
            std::string jobPath;
            std::string jobFit;
            int w = 0, h = 0, stride = 0;

            {
                std::unique_lock<std::mutex> lock(mu_);
                auto findNextMissing = [&]() -> bool {
                    if (page_files_.empty()) return false;
                    // Priority: N, N+1, N-1
                    const int candidates[3] = {target_page_, target_page_ + 1, target_page_ - 1};
                    for (int p : candidates) {
                        if (p >= 0 && p < static_cast<int>(page_files_.size())) {
                            std::string path = stripFileScheme(page_files_[p]);
                            if (!hasSlotLocked(path, fit_mode_)) {
                                jobPath = path;
                                jobFit = fit_mode_;
                                w = screen_w_;
                                h = screen_h_;
                                stride = stride_;
                                return true;
                            }
                        }
                    }
                    return false;
                };

                cv_.wait(lock, [&]() { return !running_.load() || findNextMissing(); });
                if (!running_.load()) break;
                if (jobPath.empty() && !findNextMissing()) continue;
            }

            std::vector<uint8_t> decoded;
            if (renderPageToBuffer(jobPath, jobFit, w, h, stride, decoded) && running_.load()) {
                std::lock_guard<std::mutex> lock(mu_);
                storeSlotLocked(jobPath, jobFit, w, h, stride, std::move(decoded));
            }
        }
    }
};

} // namespace kindle
