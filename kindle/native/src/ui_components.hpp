#pragma once

#include "canvas.hpp"
#include "font_manager.hpp"
#include "cover_cache.hpp"
#include "types.hpp"
#include <string>
#include <vector>

namespace kindle {

struct GridCardMetrics {
    static constexpr int COLS = 3;
    static constexpr int MARGIN_X = 28;
    static constexpr int GAP_X = 26;
    static constexpr int CARD_W = (1264 - 2 * MARGIN_X - (COLS - 1) * GAP_X) / COLS; // 385
    static constexpr int COVER_H = 540;
    static constexpr int TITLE_AREA_H = 68;
    static constexpr int CARD_H = COVER_H + TITLE_AREA_H; // 608

    static int colX(int col) {
        return MARGIN_X + col * (CARD_W + GAP_X);
    }
};

namespace ui {

inline void drawPillBadge(Canvas& canvas, FontManager& font,
                          int x, int y, const std::string& text,
                          bool inverted = true, int fontSize = 18, int padX = 10, int h = 32) {
    int tw = font.measureText(text, fontSize, true);
    int w = std::max(h, tw + padX * 2);
    canvas.fillRect(x, y, w, h, inverted ? 0x00 : 0xFF);
    canvas.drawRect(x, y, w, h, inverted ? 0xFF : 0x00, 2);
    font.drawText(canvas, x + (w - tw) / 2, y + (h - fontSize) / 2 - 1, text, fontSize, inverted ? 0xFF : 0x00, true);
}

// Draws a Mihon-style Tankobon cover card with unread/offline/source/pin badges and 2-line elided title
inline void drawMangaGridCard(Canvas& canvas,
                              FontManager& font,
                              CoverCache& covers,
                              int x, int y, int cardW, int coverH,
                              const UnifiedManga& manga,
                              const std::string& sourceBadge,
                              bool showPinButton,
                              bool isPinned,
                              bool showRemoveCorner = false) {
    // Subtle card drop-shadow & crisp cover frame
    canvas.fillRect(x + 4, y + 4, cardW, coverH, 0xD4);
    canvas.fillRect(x, y, cardW, coverH, 0xEC);

    bool hasImg = covers.tryBlitCover(canvas, x, y, cardW, coverH, manga.cover_url, manga.title, manga.id);
    if (!hasImg) {
        // Stylized monogram Tankobon fallback cover
        canvas.fillRect(x, y, cardW, coverH, 0xEAEAEA & 0xFF);
        canvas.fillRect(x + 14, y + 14, cardW - 28, coverH - 28, 0xF7);
        canvas.drawRect(x + 14, y + 14, cardW - 28, coverH - 28, 0x90, 1);

        size_t idx = 0;
        std::string mono = "?";
        if (!manga.title.empty()) {
            FontManager::nextCodepoint(manga.title, idx);
            mono = manga.title.substr(0, idx);
        }
        int mw = font.measureText(mono, 64, true);
        font.drawText(canvas, x + (cardW - mw) / 2, y + coverH / 2 - 46, mono, 64, 0x45, true);

        auto previewLines = font.wrapText(manga.title, 20, cardW - 44, 2, true);
        int py = y + coverH / 2 + 30;
        for (const auto& ln : previewLines) {
            int lw = font.measureText(ln, 20, true);
            font.drawText(canvas, x + (cardW - lw) / 2, py, ln, 20, 0x55, true);
            py += 25;
        }
    }
    canvas.drawRect(x, y, cardW, coverH, 0x00, 2);

    // Top-left badges: Unread count (black) + Offline downloaded count (white)
    int badgeX = x + 8;
    int badgeY = y + 8;
    if (manga.unread_count > 0) {
        std::string uStr = std::to_string(manga.unread_count);
        drawPillBadge(canvas, font, badgeX, badgeY, uStr, true, 18, 9, 32);
        badgeX += std::max(32, font.measureText(uStr, 18, true) + 18) + 6;
    }
    if (manga.downloaded_count > 0) {
        std::string dStr = "✓ " + std::to_string(manga.downloaded_count);
        drawPillBadge(canvas, font, badgeX, badgeY, dStr, false, 17, 8, 32);
    }

    // Top-right action badge: 1-tap [+ / ✓] Pin in Browse, or [✕] Remove in Library
    if (showPinButton) {
        std::string pinLabel = isPinned ? "✓ Saved" : "+ Pin";
        int pw = isPinned ? 94 : 80;
        int ph = 38;
        int px = x + cardW - pw - 8;
        int py = y + 8;
        font.drawButton(canvas, px, py, pw, ph, pinLabel, isPinned, 18, 0xFF);
    } else if (showRemoveCorner) {
        int rw = 40, rh = 38;
        int rx = x + cardW - rw - 8;
        int ry = y + 8;
        font.drawButton(canvas, rx, ry, rw, rh, "✕", false, 18, 0xFF);
    }

    // Bottom-left source label pill on cover if provided
    if (!sourceBadge.empty()) {
        std::string sb = font.elideText(sourceBadge, 16, cardW - 24, true);
        int sw = font.measureText(sb, 16, true) + 16;
        int sy = y + coverH - 34;
        canvas.fillRect(x + 6, sy, sw, 28, 0x00);
        canvas.drawRect(x + 6, sy, sw, 28, 0xFF, 1);
        font.drawText(canvas, x + 14, sy + 5, sb, 16, 0xFF, true);
    }

    // 2-line wrapped & pixel-elided title below cover
    auto lines = font.wrapText(manga.title, 22, cardW - 4, 2, true);
    int ty = y + coverH + 8;
    for (const auto& line : lines) {
        font.drawText(canvas, x + 2, ty, line, 22, 0x00, true);
        ty += 26;
    }
}

inline void drawPaginationBar(Canvas& canvas, FontManager& font,
                              int y, int curPage, int totalPages, const std::string& extraInfo = "") {
    if (totalPages <= 1 && extraInfo.empty()) return;
    const int btnW = 185;
    const int btnH = 50;

    bool hasPrev = (curPage > 0);
    bool hasNext = (curPage + 1 < totalPages);

    if (totalPages > 1) {
        if (hasPrev) {
            font.drawButton(canvas, 28, y, btnW, btnH, "◄ Prev", false, 21);
        } else {
            canvas.drawRect(28, y, btnW, btnH, 0xC8, 1);
            int tw = font.measureText("◄ Prev", 21, false);
            font.drawText(canvas, 28 + (btnW - tw) / 2, y + 14, "◄ Prev", 21, 0xB8, false);
        }

        if (hasNext) {
            font.drawButton(canvas, canvas.width() - btnW - 28, y, btnW, btnH, "Next ►", false, 21);
        } else {
            int rx = canvas.width() - btnW - 28;
            canvas.drawRect(rx, y, btnW, btnH, 0xC8, 1);
            int tw = font.measureText("Next ►", 21, false);
            font.drawText(canvas, rx + (btnW - tw) / 2, y + 14, "Next ►", 21, 0xB8, false);
        }
    }

    std::string center = "Page " + std::to_string(curPage + 1) + " / " + std::to_string(std::max(1, totalPages));
    if (!extraInfo.empty()) center += "  ·  " + extraInfo;
    center = font.elideText(center, 21, canvas.width() - 2 * btnW - 90, true);
    int cw = font.measureText(center, 21, true);
    font.drawText(canvas, (canvas.width() - cw) / 2, y + 14, center, 21, 0x20, true);
}

inline const std::vector<std::string>& urlQuickTokens() {
    static const std::vector<std::string> tokens = {
        "https://",
        "raw.githubusercontent.com/",
        "index.min.json",
        ".json",
        "/",
        "-",
        "_"
    };
    return tokens;
}

} // namespace ui
} // namespace kindle
