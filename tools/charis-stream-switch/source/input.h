// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Reading the Switch's controllers and touch screen, and turning them into what the PC needs:
// a controller and touch for games, or a mouse and keyboard for the PC itself.

#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "charis_stream/protocol.h"

struct Touch {
    int id = 0;
    int x = 0; // 0..1279 on the Switch screen
    int y = 0; // 0..719
};

struct PadSnapshot {
    uint64_t buttons = 0; // CharisStream::Button bits
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0; // -32767..32767, up positive
    int touch_count = 0;
    Touch touches[CharisStream::MaxTouches];
};

class Pads {
public:
    void Open();
    PadSnapshot Read();

private:
#ifndef __SWITCH__
    void* gamepad = nullptr;
#endif
    int64_t last_merge_check = 0;
};

// Text typed on the Switch keyboard, and special keys, delivered to the PC in order. Every
// event rides in several packets in a row so a lost packet does not lose a key.
class KeyQueue {
public:
    void Text(const std::string& utf8);
    void Special(CharisStream::Key key);
    void Fill(CharisStream::InputState& state);
    void Clear() {
        events.clear();
    }

private:
    struct Pending {
        CharisStream::KeyEvent event;
        int sent = 0;
    };
    std::deque<Pending> events;
    uint32_t next_sequence = 1;
};

// The PC's mouse driven from the Switch: left stick moves it, right stick scrolls, the touch
// screen works like a laptop's touch screen (tap clicks, drag drags, hold right-clicks, two
// fingers scroll).
class DesktopPointer {
public:
    void Reset();
    // dt_ms since the last call. buttons as read this frame.
    void Update(const PadSnapshot& pad, uint64_t pressed, int dt_ms);
    void Fill(CharisStream::InputState& state) const;
    float X() const {
        return x;
    }
    float Y() const {
        return y;
    }

private:
    float x = 0.5f; // 0..1 across the PC screen
    float y = 0.5f;
    float scroll_rest = 0;
    int32_t wheel = 0;
    uint8_t buttons = 0;
    // Touch
    bool touching = false;
    bool dragging = false;
    bool long_pressed = false;
    int touch_start_x = 0, touch_start_y = 0;
    int64_t touch_start_ms = 0;
    int two_finger_y = -1;
    int click_frames = 0;     // a tap: hold the left button for a couple of frames
    int right_click_frames = 0;
    int64_t now_ms = 0;
};
