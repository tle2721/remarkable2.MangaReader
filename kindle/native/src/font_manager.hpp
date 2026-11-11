#pragma once

#include "../third_party/stb_truetype.h"
#include "canvas.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace kindle {

class FontManager {
private:
    struct CachedGlyph {
        int advance = 0;
        int x0 = 0;
        int y0 = 0;
        int w = 0;
        int h = 0;
        std::vector<uint8_t> bmp;
    };

    std::vector<uint8_t> regular_data_;
    std::vector<uint8_t> bold_data_;
    std::vector<uint8_t> fallback_data_;
    stbtt_fontinfo regular_font_{};
    stbtt_fontinfo bold_font_{};
    stbtt_fontinfo fallback_font_{};
    bool regular_loaded_ = false;
    bool bold_loaded_ = false;
    bool fallback_loaded_ = false;

    mutable std::unordered_map<uint64_t, CachedGlyph> glyph_cache_;

    bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        std::streamsize sz = ifs.tellg();
        if (sz <= 0) return false;
        ifs.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(sz));
        if (ifs.read(reinterpret_cast<char*>(out.data()), sz)) {
            return true;
        }
        return false;
    }

    static bool isCustomIconCodepoint(uint32_t cp) {
        switch (cp) {
            case 0x2715: // ✕
            case 0x2716: // ✖
            case 0x2713: // ✓
            case 0x2714: // ✔
            case 0x2699: // ⚙
            case 0x25B6: // ▶
            case 0x25BA: // ►
            case 0x25C0: // ◀
            case 0x25C4: // ◄
            case 0x21C5: // ⇅
            case 0x21E7: // ⇧
            case 0x232B: // ⌫
            case 0x21BB: // ↻
            case 0x27F3: // ⟳
            case 0x2B07: // ⬇
            case 0x2193: // ↓
            case 0x21B5: // ↵
            case 0x25CF: // ●
            case 0x25CB: // ○
            case 0x2726: // ✦
            case 0x2630: // ☰
            case 0x229E: // ⊞
                return true;
            default:
                return false;
        }
    }

    // 4x supersampled procedural vector icon rasterizer so UI icons never render as missing tofu boxes
    static CachedGlyph rasterizeProceduralIcon(uint32_t cp, int pixelHeight, int baseline) {
        CachedGlyph g;
        int box = std::max(12, (pixelHeight * 78) / 100);
        g.w = box;
        g.h = box;
        g.x0 = 1;
        g.y0 = -baseline + (pixelHeight - box) / 2 + 1;
        g.advance = box + std::max(3, pixelHeight / 8);
        g.bmp.assign(static_cast<size_t>(box * box), 0);

        const int ss = 4;
        const int sw = box * ss;
        const int sh = box * ss;
        std::vector<uint8_t> hi(static_cast<size_t>(sw * sh), 0);

        auto setDot = [&](int x, int y) {
            if (x >= 0 && x < sw && y >= 0 && y < sh) {
                hi[static_cast<size_t>(y * sw + x)] = 1;
            }
        };

        auto drawThickSeg = [&](float x0, float y0, float x1, float y1, float radius) {
            float dx = x1 - x0;
            float dy = y1 - y0;
            float lenSq = dx * dx + dy * dy;
            int minX = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - radius - 1)));
            int maxX = std::min(sw - 1, static_cast<int>(std::ceil(std::max(x0, x1) + radius + 1)));
            int minY = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - radius - 1)));
            int maxY = std::min(sh - 1, static_cast<int>(std::ceil(std::max(y0, y1) + radius + 1)));
            float rSq = radius * radius;
            for (int py = minY; py <= maxY; ++py) {
                for (int px = minX; px <= maxX; ++px) {
                    float t = (lenSq > 0.001f) ? (((px - x0) * dx + (py - y0) * dy) / lenSq) : 0.0f;
                    t = std::max(0.0f, std::min(1.0f, t));
                    float cx = x0 + t * dx;
                    float cy = y0 + t * dy;
                    float distSq = (px - cx) * (px - cx) + (py - cy) * (py - cy);
                    if (distSq <= rSq) setDot(px, py);
                }
            }
        };

        auto fillTriangle = [&](float ax, float ay, float bx, float by, float cx, float cy) {
            auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
                return (px - x0) * (y1 - y0) - (py - y0) * (x1 - x0);
            };
            for (int py = 0; py < sh; ++py) {
                for (int px = 0; px < sw; ++px) {
                    float w0 = edge(ax, ay, bx, by, px + 0.5f, py + 0.5f);
                    float w1 = edge(bx, by, cx, cy, px + 0.5f, py + 0.5f);
                    float w2 = edge(cx, cy, ax, ay, px + 0.5f, py + 0.5f);
                    if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                        setDot(px, py);
                    }
                }
            }
        };

        const float W = static_cast<float>(sw);
        const float H = static_cast<float>(sh);
        const float stroke = std::max(3.2f, W * 0.095f);

        if (cp == 0x2715 || cp == 0x2716) { // ✕ Close
            drawThickSeg(W * 0.18f, H * 0.18f, W * 0.82f, H * 0.82f, stroke);
            drawThickSeg(W * 0.82f, H * 0.18f, W * 0.18f, H * 0.82f, stroke);
        } else if (cp == 0x2713 || cp == 0x2714) { // ✓ Checkmark
            drawThickSeg(W * 0.14f, H * 0.54f, W * 0.40f, H * 0.80f, stroke * 1.05f);
            drawThickSeg(W * 0.40f, H * 0.80f, W * 0.86f, H * 0.22f, stroke * 1.05f);
        } else if (cp == 0x25B6 || cp == 0x25BA) { // ▶ Play / Right triangle
            fillTriangle(W * 0.20f, H * 0.14f, W * 0.86f, H * 0.50f, W * 0.20f, H * 0.86f);
        } else if (cp == 0x25C0 || cp == 0x25C4) { // ◀ Left triangle
            fillTriangle(W * 0.80f, H * 0.14f, W * 0.14f, H * 0.50f, W * 0.80f, H * 0.86f);
        } else if (cp == 0x25CF) { // ● Filled dot
            float cx = W * 0.5f, cy = H * 0.5f, r = W * 0.32f;
            for (int py = 0; py < sh; ++py)
                for (int px = 0; px < sw; ++px)
                    if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r) setDot(px, py);
        } else if (cp == 0x25CB) { // ○ Hollow circle
            float cx = W * 0.5f, cy = H * 0.5f, rOuter = W * 0.36f, rInner = W * 0.23f;
            for (int py = 0; py < sh; ++py) {
                for (int px = 0; px < sw; ++px) {
                    float d2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);
                    if (d2 <= rOuter * rOuter && d2 >= rInner * rInner) setDot(px, py);
                }
            }
        } else if (cp == 0x2699) { // ⚙ Gear
            float cx = W * 0.5f, cy = H * 0.5f;
            float rOuter = W * 0.34f, rInner = W * 0.17f;
            for (int py = 0; py < sh; ++py) {
                for (int px = 0; px < sw; ++px) {
                    float d2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);
                    if (d2 <= rOuter * rOuter && d2 >= rInner * rInner) setDot(px, py);
                }
            }
            for (int k = 0; k < 8; ++k) {
                float ang = k * 3.14159265f * 0.25f;
                drawThickSeg(cx + std::cos(ang) * W * 0.28f, cy + std::sin(ang) * H * 0.28f,
                             cx + std::cos(ang) * W * 0.44f, cy + std::sin(ang) * H * 0.44f, stroke * 0.85f);
            }
        } else if (cp == 0x2B07 || cp == 0x2193) { // ⬇ Download arrow + tray
            drawThickSeg(W * 0.50f, H * 0.12f, W * 0.50f, H * 0.65f, stroke);
            fillTriangle(W * 0.22f, H * 0.48f, W * 0.78f, H * 0.48f, W * 0.50f, H * 0.78f);
            drawThickSeg(W * 0.18f, H * 0.88f, W * 0.82f, H * 0.88f, stroke * 0.9f);
        } else if (cp == 0x21BB || cp == 0x27F3) { // ↻ / ⟳ Refresh circular arrow
            float cx = W * 0.5f, cy = H * 0.52f;
            float rOuter = W * 0.37f, rInner = W * 0.23f;
            for (int py = 0; py < sh; ++py) {
                for (int px = 0; px < sw; ++px) {
                    float dx = px - cx, dy = py - cy;
                    float d2 = dx * dx + dy * dy;
                    if (d2 <= rOuter * rOuter && d2 >= rInner * rInner) {
                        float ang = std::atan2(dy, dx); // -pi..pi
                        if (ang < -0.85f || ang > -0.15f) setDot(px, py);
                    }
                }
            }
            fillTriangle(W * 0.56f, H * 0.10f, W * 0.88f, H * 0.14f, W * 0.76f, H * 0.42f);
        } else if (cp == 0x21C5) { // ⇅ Up/Down sort arrows
            drawThickSeg(W * 0.30f, H * 0.18f, W * 0.30f, H * 0.84f, stroke * 0.8f);
            fillTriangle(W * 0.10f, H * 0.36f, W * 0.50f, H * 0.36f, W * 0.30f, H * 0.10f);
            drawThickSeg(W * 0.70f, H * 0.16f, W * 0.70f, H * 0.82f, stroke * 0.8f);
            fillTriangle(W * 0.50f, H * 0.64f, W * 0.90f, H * 0.64f, W * 0.70f, H * 0.90f);
        } else if (cp == 0x21E7) { // ⇧ Shift arrow
            fillTriangle(W * 0.50f, H * 0.12f, W * 0.15f, H * 0.52f, W * 0.85f, H * 0.52f);
            drawThickSeg(W * 0.38f, H * 0.50f, W * 0.38f, H * 0.85f, stroke);
            drawThickSeg(W * 0.62f, H * 0.50f, W * 0.62f, H * 0.85f, stroke);
            drawThickSeg(W * 0.38f, H * 0.85f, W * 0.62f, H * 0.85f, stroke);
        } else if (cp == 0x232B) { // ⌫ Backspace
            drawThickSeg(W * 0.34f, H * 0.22f, W * 0.90f, H * 0.22f, stroke * 0.75f);
            drawThickSeg(W * 0.90f, H * 0.22f, W * 0.90f, H * 0.78f, stroke * 0.75f);
            drawThickSeg(W * 0.90f, H * 0.78f, W * 0.34f, H * 0.78f, stroke * 0.75f);
            drawThickSeg(W * 0.34f, H * 0.22f, W * 0.10f, H * 0.50f, stroke * 0.75f);
            drawThickSeg(W * 0.10f, H * 0.50f, W * 0.34f, H * 0.78f, stroke * 0.75f);
            drawThickSeg(W * 0.46f, H * 0.36f, W * 0.74f, H * 0.64f, stroke * 0.7f);
            drawThickSeg(W * 0.74f, H * 0.36f, W * 0.46f, H * 0.64f, stroke * 0.7f);
        } else if (cp == 0x21B5) { // ↵ Enter/Return
            drawThickSeg(W * 0.80f, H * 0.20f, W * 0.80f, H * 0.62f, stroke * 0.85f);
            drawThickSeg(W * 0.80f, H * 0.62f, W * 0.25f, H * 0.62f, stroke * 0.85f);
            fillTriangle(W * 0.36f, H * 0.42f, W * 0.36f, H * 0.82f, W * 0.12f, H * 0.62f);
        } else if (cp == 0x2726) { // ✦ 4-point star
            fillTriangle(W * 0.50f, H * 0.08f, W * 0.36f, H * 0.50f, W * 0.64f, H * 0.50f);
            fillTriangle(W * 0.50f, H * 0.92f, W * 0.36f, H * 0.50f, W * 0.64f, H * 0.50f);
            fillTriangle(W * 0.08f, H * 0.50f, W * 0.50f, H * 0.36f, W * 0.50f, H * 0.64f);
            fillTriangle(W * 0.92f, H * 0.50f, W * 0.50f, H * 0.36f, W * 0.50f, H * 0.64f);
        } else if (cp == 0x2630) { // ☰ List (3 crisp horizontal bars)
            drawThickSeg(W * 0.14f, H * 0.22f, W * 0.86f, H * 0.22f, stroke * 0.85f);
            drawThickSeg(W * 0.14f, H * 0.50f, W * 0.86f, H * 0.50f, stroke * 0.85f);
            drawThickSeg(W * 0.14f, H * 0.78f, W * 0.86f, H * 0.78f, stroke * 0.85f);
        } else if (cp == 0x229E) { // ⊞ Grid (2x2 Tankobon tile icon)
            auto fillBox = [&](float x0, float y0, float x1, float y1) {
                int minX = std::max(0, static_cast<int>(std::round(x0)));
                int maxX = std::min(sw - 1, static_cast<int>(std::round(x1)));
                int minY = std::max(0, static_cast<int>(std::round(y0)));
                int maxY = std::min(sh - 1, static_cast<int>(std::round(y1)));
                for (int py = minY; py <= maxY; ++py)
                    for (int px = minX; px <= maxX; ++px)
                        setDot(px, py);
            };
            fillBox(W * 0.12f, H * 0.12f, W * 0.44f, H * 0.44f);
            fillBox(W * 0.56f, H * 0.12f, W * 0.88f, H * 0.44f);
            fillBox(W * 0.12f, H * 0.56f, W * 0.44f, H * 0.88f);
            fillBox(W * 0.56f, H * 0.56f, W * 0.88f, H * 0.88f);
        }

        // Downsample 4x4 supersampled grid to 0..255 alpha
        for (int y = 0; y < box; ++y) {
            for (int x = 0; x < box; ++x) {
                int sum = 0;
                for (int dy = 0; dy < ss; ++dy) {
                    const uint8_t* row = hi.data() + (y * ss + dy) * sw + x * ss;
                    for (int dx = 0; dx < ss; ++dx) sum += row[dx];
                }
                g.bmp[static_cast<size_t>(y * box + x)] = static_cast<uint8_t>((sum * 255) / (ss * ss));
            }
        }
        return g;
    }

    int getBaseline(int pixelHeight, bool bold) const {
        if (!regular_loaded_) return pixelHeight;
        const stbtt_fontinfo* font = (bold && bold_loaded_) ? &bold_font_ : &regular_font_;
        float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(pixelHeight));
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
        return static_cast<int>(std::round(ascent * scale));
    }

    const CachedGlyph& getCachedGlyph(uint32_t cp, int pixelHeight, bool bold) const {
        if (glyph_cache_.size() > 4096) {
            glyph_cache_.clear();
        }
        uint64_t key = (static_cast<uint64_t>(bold ? 1 : 0) << 48) |
                       (static_cast<uint64_t>(pixelHeight & 0xFFFF) << 32) |
                       static_cast<uint64_t>(cp);
        auto it = glyph_cache_.find(key);
        if (it != glyph_cache_.end()) {
            return it->second;
        }

        int baseline = getBaseline(pixelHeight, bold);
        if (isCustomIconCodepoint(cp)) {
            auto inserted = glyph_cache_.emplace(key, rasterizeProceduralIcon(cp, pixelHeight, baseline));
            return inserted.first->second;
        }

        const stbtt_fontinfo* font = (bold && bold_loaded_) ? &bold_font_ : &regular_font_;
        if (fallback_loaded_ && stbtt_FindGlyphIndex(font, static_cast<int>(cp)) == 0) {
            if (stbtt_FindGlyphIndex(&fallback_font_, static_cast<int>(cp)) != 0) {
                font = &fallback_font_;
            }
        }

        float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(pixelHeight));
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(font, static_cast<int>(cp), &advance, &lsb);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);

        CachedGlyph cg;
        cg.advance = static_cast<int>(std::round(advance * scale));
        cg.x0 = x0;
        cg.y0 = y0;
        cg.w = std::max(0, x1 - x0);
        cg.h = std::max(0, y1 - y0);
        if (cg.w > 0 && cg.h > 0) {
            cg.bmp.resize(static_cast<size_t>(cg.w * cg.h), 0);
            stbtt_MakeCodepointBitmap(font, cg.bmp.data(), cg.w, cg.h, cg.w, scale, scale, static_cast<int>(cp));
        }

        auto inserted = glyph_cache_.emplace(key, std::move(cg));
        return inserted.first->second;
    }

public:
    FontManager() = default;

    bool init(const std::string& custom_reg = "", const std::string& custom_bold = "") {
        glyph_cache_.clear();
        const std::vector<std::string> reg_candidates = {
            custom_reg,
            "/usr/java/lib/fonts/Amazon-Ember-Regular.ttf",
            "/usr/java/lib/fonts/AmazonEmber-Regular.ttf",
            "/usr/java/lib/fonts/Amazon-Ember_Rg.ttf",
            "/usr/java/lib/fonts/AmazonEmber_Rg.ttf",
            "/usr/share/fonts/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/Amazon-Ember-Regular.ttf",
            "/usr/share/fonts/truetype/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/opentype/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/default/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/default/truetype/AmazonEmber-Regular.ttf",
            "/opt/amazon/ebook/fonts/AmazonEmber-Regular.ttf",
            "/opt/amazon/ebook/fonts/Amazon-Ember-Regular.ttf",
            "/var/local/java/lib/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/system/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/extensions/rm2manga/fonts/regular.ttf",
            "/usr/java/lib/fonts/Bookerly-Regular.ttf",
            "/usr/share/fonts/truetype/Bookerly-Regular.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_65_Medium.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_55_Roman.ttf",
            "/usr/java/lib/fonts/Helvetica-Roman.ttf",
            "/mnt/us/koreader/fonts/freefont/FreeSans.ttf",
            "/opt/koreader/fonts/freefont/FreeSans.ttf",
            "/mnt/us/koreader/fonts/noto/NotoSans-Regular.ttf"
        };

        for (const auto& p : reg_candidates) {
            if (p.empty()) continue;
            if (loadFile(p, regular_data_)) {
                if (stbtt_InitFont(&regular_font_, regular_data_.data(), 0)) {
                    regular_loaded_ = true;
                    break;
                }
            }
        }

        const std::vector<std::string> bold_candidates = {
            custom_bold,
            "/usr/java/lib/fonts/Amazon-Ember-Bold.ttf",
            "/usr/java/lib/fonts/AmazonEmber-Bold.ttf",
            "/usr/java/lib/fonts/Amazon-Ember_Bd.ttf",
            "/usr/java/lib/fonts/AmazonEmber_Bd.ttf",
            "/usr/share/fonts/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/Amazon-Ember-Bold.ttf",
            "/usr/share/fonts/truetype/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/opentype/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/default/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/default/truetype/AmazonEmber-Bold.ttf",
            "/opt/amazon/ebook/fonts/AmazonEmber-Bold.ttf",
            "/opt/amazon/ebook/fonts/Amazon-Ember-Bold.ttf",
            "/var/local/java/lib/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/system/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/extensions/rm2manga/fonts/bold.ttf",
            "/usr/java/lib/fonts/Bookerly-Bold.ttf",
            "/usr/share/fonts/truetype/Bookerly-Bold.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_75_Bold.ttf",
            "/usr/java/lib/fonts/Helvetica-Bold.ttf",
            "/mnt/us/koreader/fonts/freefont/FreeSansBold.ttf",
            "/opt/koreader/fonts/freefont/FreeSansBold.ttf"
        };

        for (const auto& p : bold_candidates) {
            if (p.empty()) continue;
            if (loadFile(p, bold_data_)) {
                if (stbtt_InitFont(&bold_font_, bold_data_.data(), 0)) {
                    bold_loaded_ = true;
                    break;
                }
            }
        }

        // Load broad Unicode / CJK fallback font if present on Kindle or KOReader
        const std::vector<std::string> fallback_candidates = {
            "/mnt/us/koreader/fonts/noto/NotoSansCJKsc-Regular.otf",
            "/mnt/us/koreader/fonts/droid/DroidSansFallbackFull.ttf",
            "/mnt/us/koreader/fonts/freefont/FreeSans.ttf",
            "/usr/java/lib/fonts/Code2000.ttf",
            "/usr/java/lib/fonts/TBGothic-Medium.ttf",
            "/usr/java/lib/fonts/MYingHeiTB-Medium.ttf",
            "/opt/koreader/fonts/freefont/FreeSans.ttf"
        };

        for (const auto& p : fallback_candidates) {
            if (loadFile(p, fallback_data_)) {
                int offset = stbtt_GetFontOffsetForIndex(fallback_data_.data(), 0);
                if (offset >= 0 && stbtt_InitFont(&fallback_font_, fallback_data_.data(), offset)) {
                    fallback_loaded_ = true;
                    break;
                }
            }
        }

        return regular_loaded_;
    }

    bool isLoaded() const { return regular_loaded_; }

    static inline uint32_t nextCodepoint(const std::string& str, size_t& i) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80) {
            i += 1;
            return c;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < str.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            i += 2;
            return cp;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < str.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            i += 3;
            return cp;
        }
        if ((c & 0xF8) == 0xF0 && i + 3 < str.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            i += 4;
            return cp;
        }
        i += 1;
        return c;
    }

    // Safely pop one UTF-8 codepoint from the end of a string (for Backspace)
    static void popUtf8Codepoint(std::string& s) {
        if (s.empty()) return;
        size_t pos = s.size() - 1;
        while (pos > 0 && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        s.erase(pos);
    }

    int measureText(const std::string& text, int pixelHeight, bool bold = false) const {
        if (!regular_loaded_) {
            int scale = std::max(1, pixelHeight / 16);
            return static_cast<int>(text.length()) * 8 * scale;
        }

        int totalWidth = 0;
        size_t i = 0;
        while (i < text.length()) {
            if (text[i] == '\n') {
                ++i;
                continue;
            }
            uint32_t cp = nextCodepoint(text, i);
            totalWidth += getCachedGlyph(cp, pixelHeight, bold).advance;
        }
        return totalWidth;
    }

    // Pixel-accurate UTF-8 safe text truncation with "..."
    std::string elideText(const std::string& text, int pixelHeight, int maxWidth, bool bold = false) const {
        if (text.empty() || maxWidth <= 0) return "";
        if (measureText(text, pixelHeight, bold) <= maxWidth) return text;

        const std::string ellipsis = "...";
        int ellW = measureText(ellipsis, pixelHeight, bold);
        if (ellW >= maxWidth) return "";

        int budget = maxWidth - ellW;
        int curW = 0;
        size_t i = 0;
        size_t lastSafeIdx = 0;
        while (i < text.size()) {
            size_t prevIdx = i;
            uint32_t cp = nextCodepoint(text, i);
            if (cp == '\n') cp = ' ';
            int adv = regular_loaded_ ? getCachedGlyph(cp, pixelHeight, bold).advance : (8 * std::max(1, pixelHeight / 16));
            if (curW + adv > budget) {
                return text.substr(0, prevIdx) + ellipsis;
            }
            curW += adv;
            lastSafeIdx = i;
        }
        return text.substr(0, lastSafeIdx) + ellipsis;
    }

    // Multi-line word wrapper with pixel-accurate eliding on the final line
    std::vector<std::string> wrapText(const std::string& text, int pixelHeight, int maxWidth, int maxLines, bool bold = false) const {
        std::vector<std::string> lines;
        if (text.empty() || maxWidth <= 0 || maxLines <= 0) return lines;

        // Collapse newlines into spaces for clean card/synopsis wrapping
        std::string cleaned;
        cleaned.reserve(text.size());
        for (char c : text) {
            if (c == '\r') continue;
            cleaned.push_back(c == '\n' || c == '\t' ? ' ' : c);
        }

        size_t pos = 0;
        while (pos < cleaned.size() && static_cast<int>(lines.size()) < maxLines) {
            while (pos < cleaned.size() && cleaned[pos] == ' ') ++pos;
            if (pos >= cleaned.size()) break;

            if (static_cast<int>(lines.size()) == maxLines - 1) {
                lines.push_back(elideText(cleaned.substr(pos), pixelHeight, maxWidth, bold));
                break;
            }

            size_t scan = pos;
            size_t lastSpace = std::string::npos;
            size_t lastFit = pos;
            int curW = 0;

            while (scan < cleaned.size()) {
                size_t charStart = scan;
                if (cleaned[scan] == ' ') lastSpace = scan;
                uint32_t cp = nextCodepoint(cleaned, scan);
                int adv = regular_loaded_ ? getCachedGlyph(cp, pixelHeight, bold).advance : (8 * std::max(1, pixelHeight / 16));
                if (curW + adv > maxWidth) {
                    scan = charStart;
                    break;
                }
                curW += adv;
                lastFit = scan;
            }

            if (scan >= cleaned.size()) {
                lines.push_back(cleaned.substr(pos));
                break;
            }

            if (lastSpace != std::string::npos && lastSpace > pos) {
                lines.push_back(cleaned.substr(pos, lastSpace - pos));
                pos = lastSpace + 1;
            } else if (lastFit > pos) {
                lines.push_back(cleaned.substr(pos, lastFit - pos));
                pos = lastFit;
            } else {
                // Single glyph wider than maxWidth
                nextCodepoint(cleaned, pos);
            }
        }
        return lines;
    }

    int drawText(Canvas& canvas, int x, int y, const std::string& text, int pixelHeight, uint8_t color = 0x00, bool bold = false) {
        if (!regular_loaded_) {
            int scale = std::max(1, pixelHeight / 16);
            canvas.drawText(x, y, text, scale, color);
            return measureText(text, pixelHeight, bold);
        }

        int baseline = getBaseline(pixelHeight, bold);
        uint8_t* rawBuf = canvas.buffer();
        const int canvasW = canvas.width();
        const int canvasH = canvas.height();
        const int stride = canvas.stride();

        int curX = x;
        size_t i = 0;
        while (i < text.length()) {
            if (text[i] == '\n') {
                curX = x;
                y += pixelHeight + 6;
                i++;
                continue;
            }

            uint32_t cp = nextCodepoint(text, i);
            const CachedGlyph& g = getCachedGlyph(cp, pixelHeight, bold);

            if (g.w > 0 && g.h > 0 && rawBuf) {
                int outY = y + baseline + g.y0;
                int outX = curX + g.x0;

                int rStart = std::max(0, -outY);
                int rEnd = std::min(g.h, canvasH - outY);
                int cStart = std::max(0, -outX);
                int cEnd = std::min(g.w, canvasW - outX);

                for (int r = rStart; r < rEnd; ++r) {
                    uint8_t* dstRow = rawBuf + (outY + r) * stride + outX;
                    const uint8_t* srcRow = g.bmp.data() + r * g.w;
                    for (int col = cStart; col < cEnd; ++col) {
                        uint8_t alpha = srcRow[col];
                        if (alpha == 0) continue;
                        if (alpha == 255) {
                            dstRow[col] = color;
                        } else {
                            uint8_t bg = dstRow[col];
                            dstRow[col] = static_cast<uint8_t>((bg * (255 - alpha) + color * alpha) / 255);
                        }
                    }
                }
            }

            curX += g.advance;
        }
        return curX - x;
    }

    void drawButton(Canvas& canvas, int x, int y, int w, int h, const std::string& label, bool selected = false, int fontSize = 24, uint8_t unselectedBg = 0xFF) {
        uint8_t bg = selected ? 0x00 : unselectedBg;
        uint8_t fg = selected ? 0xFF : 0x00;
        canvas.fillRect(x, y, w, h, bg);
        canvas.drawRect(x, y, w, h, 0x00, 2);

        std::string fitted = elideText(label, fontSize, std::max(10, w - 16), true);
        int text_w = measureText(fitted, fontSize, true);
        int text_x = x + (w - text_w) / 2;
        int text_y = y + (h - fontSize) / 2 - 1;
        drawText(canvas, text_x, text_y, fitted, fontSize, fg, true);
    }
};

} // namespace kindle
