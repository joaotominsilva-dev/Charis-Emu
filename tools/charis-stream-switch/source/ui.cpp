// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include "ui.h"

bool Ui::Open(SDL_Renderer* renderer_, const char* regular_path, const char* bold_path) {
    renderer = renderer_;
    huge = TTF_OpenFont(bold_path, 52);
    title = TTF_OpenFont(bold_path, 34);
    bold = TTF_OpenFont(bold_path, 22);
    body = TTF_OpenFont(regular_path, 24);
    small = TTF_OpenFont(regular_path, 18);
    return huge && title && bold && body && small;
}

void Ui::Close() {
    for (TTF_Font* font : {huge, title, bold, body, small}) {
        if (font != nullptr) {
            TTF_CloseFont(font);
        }
    }
    huge = title = bold = body = small = nullptr;
}

void Ui::Fill(Color c) {
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderClear(renderer);
}

void Ui::Box(const Rect& r, int radius, Color c) {
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    roundedBoxRGBA(renderer, r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, radius, c.r, c.g, c.b, c.a);
}

void Ui::Outline(const Rect& r, int radius, Color c, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        roundedRectangleRGBA(renderer, r.x - i, r.y - i, r.x + r.w - 1 + i, r.y + r.h - 1 + i,
                             radius + i, c.r, c.g, c.b, c.a);
    }
}

void Ui::Circle(int x, int y, int radius, Color c) {
    filledCircleRGBA(renderer, x, y, radius, c.r, c.g, c.b, c.a);
    aacircleRGBA(renderer, x, y, radius, c.r, c.g, c.b, c.a);
}

void Ui::Shade(Uint8 alpha) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
    SDL_Rect all{0, 0, ScreenW, ScreenH};
    SDL_RenderFillRect(renderer, &all);
}

int Ui::Measure(TTF_Font* font, const std::string& text) {
    int w = 0;
    int h = 0;
    if (!text.empty()) {
        TTF_SizeUTF8(font, text.c_str(), &w, &h);
    }
    return w;
}

int Ui::Text(TTF_Font* font, const std::string& text, int x, int y, Color c, int align) {
    if (text.empty() || font == nullptr) {
        return 0;
    }
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), SDL_Color{c.r, c.g, c.b, c.a});
    if (surface == nullptr) {
        return 0;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    int left = x;
    if (align == 1) {
        left = x - surface->w / 2;
    } else if (align == 2) {
        left = x - surface->w;
    }
    SDL_Rect rect{left, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, nullptr, &rect);
    const int width = surface->w;
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
    return width;
}

int Ui::Paragraph(TTF_Font* font, const std::string& text, int x, int y, int max_width, Color c,
                  int line_gap) {
    const int line_height = TTF_FontHeight(font) + line_gap;
    std::string line;
    int used = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(' ', at);
        const bool last = end == std::string::npos;
        const std::string word = text.substr(at, last ? std::string::npos : end - at);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && Measure(font, candidate) > max_width) {
            Text(font, line, x, y + used, c);
            used += line_height;
            line = word;
        } else {
            line = candidate;
        }
        if (last) {
            break;
        }
        at = end + 1;
    }
    if (!line.empty()) {
        Text(font, line, x, y + used, c);
        used += line_height;
    }
    return used;
}

int Ui::Hint(int x, int y, const char* button, const std::string& label, bool on_dark) {
    const Color pill = on_dark ? SageLight : Charcoal;
    const Color glyph = on_dark ? Charcoal : Cream;
    Circle(x + 15, y + 15, 15, pill);
    const bool wide = std::strlen(button) > 1; // ZR, ZL: smaller so they fit the circle
    Text(wide ? small : bold, button, x + 15, y + (wide ? 5 : 3), glyph, 1);
    const int w = Text(body, label, x + 40, y + 1, on_dark ? Cream : Ink);
    return 40 + w + 28;
}

void Ui::Page(const std::string& eyebrow, const std::string& heading, int sheet_top) {
    Fill(Charcoal);
    Text(bold, eyebrow, 72, 58, OnDarkSoft);
    Text(huge, heading, 72, 92, Cream);
    Box({0, sheet_top, ScreenW, ScreenH - sheet_top + 40}, 32, Cream);
}

void Ui::Dots(int x, int y, Color c, Uint32 now_ms) {
    for (int i = 0; i < 3; ++i) {
        const int phase = static_cast<int>((now_ms / 180 + 3 - i) % 3);
        const int radius = phase == 0 ? 7 : 5;
        Color dot = c;
        dot.a = phase == 0 ? 255 : 130;
        Circle(x + i * 22, y, radius, dot);
    }
}
