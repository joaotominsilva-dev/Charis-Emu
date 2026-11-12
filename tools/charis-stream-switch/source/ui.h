// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Drawing in the Charis look: graphite top, cream sheet, rounded cards, Archivo type.

#pragma once

#include <string>

#include <SDL2/SDL.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_ttf.h>

struct Color {
    Uint8 r, g, b, a;
};

constexpr Color Charcoal{0x20, 0x21, 0x1C, 255};
constexpr Color Charcoal2{0x2C, 0x2D, 0x27, 255};
constexpr Color Cream{0xF3, 0xF4, 0xEA, 255};
constexpr Color Cream2{0xE9, 0xEA, 0xDD, 255};
constexpr Color Cream3{0xDE, 0xDF, 0xD1, 255};
constexpr Color SageLight{0xC8, 0xC9, 0xB7, 255};
constexpr Color Ink{0x1D, 0x1E, 0x19, 255};
constexpr Color InkSoft{0x55, 0x56, 0x4B, 255};
constexpr Color OnDarkSoft{0xA9, 0xAA, 0x98, 255};
constexpr Color Danger{0x95, 0x35, 0x21, 255};
constexpr Color Good{0x4E, 0x6B, 0x3A, 255};

// The interface is laid out for 1280x720 and scaled by SDL when docked.
constexpr int ScreenW = 1280;
constexpr int ScreenH = 720;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool Contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

class Ui {
public:
    bool Open(SDL_Renderer* renderer, const char* regular_path, const char* bold_path);
    void Close();

    SDL_Renderer* renderer = nullptr;
    TTF_Font* huge = nullptr;  // 52 bold
    TTF_Font* title = nullptr; // 34 bold
    TTF_Font* bold = nullptr;  // 22 bold
    TTF_Font* body = nullptr;  // 24
    TTF_Font* small = nullptr; // 18

    void Fill(Color c);
    void Box(const Rect& r, int radius, Color c);
    void Outline(const Rect& r, int radius, Color c, int thickness = 3);
    void Circle(int x, int y, int radius, Color c);
    void Shade(Uint8 alpha);
    // Returns the width drawn. align: 0 left, 1 center, 2 right (x is that edge)
    int Text(TTF_Font* font, const std::string& text, int x, int y, Color c, int align = 0);
    // Breaks at spaces to fit max_width; returns the height used
    int Paragraph(TTF_Font* font, const std::string& text, int x, int y, int max_width, Color c,
                  int line_gap = 6);
    int Measure(TTF_Font* font, const std::string& text);
    // A round button glyph and its label, as on the Switch: (A) Conectar
    int Hint(int x, int y, const char* button, const std::string& label, bool on_dark);

    // The common frame: graphite header with the eyebrow and title, cream sheet below
    void Page(const std::string& eyebrow, const std::string& heading, int sheet_top = 200);
    // Little moving dots for "working" states
    void Dots(int x, int y, Color c, Uint32 now_ms);
};
