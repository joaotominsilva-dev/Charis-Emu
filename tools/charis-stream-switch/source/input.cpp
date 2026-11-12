// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <SDL2/SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "input.h"

using namespace CharisStream;

namespace {

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#ifdef __SWITCH__
PadState pad;
#endif

// Stick to -1..1 with a dead zone and a gentle curve for fine pointing
float Curve(int value) {
    const float v = std::clamp(value / 32767.0f, -1.0f, 1.0f);
    const float dead = 0.12f;
    const float magnitude = std::fabs(v);
    if (magnitude < dead) {
        return 0;
    }
    const float scaled = (magnitude - dead) / (1.0f - dead);
    return std::copysign(scaled * scaled * 0.7f + scaled * 0.3f, v);
}

} // namespace

void Pads::Open() {
#ifdef __SWITCH__
    padConfigureInput(2, HidNpadStyleSet_NpadStandard);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical);
    // Handheld, a Pro Controller or a Joy-Con pair as player 1; a second player's controller
    // drives the same game controller too, so a pair split in two still works
    padInitialize(&pad, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_Handheld);
    hidInitializeTouchScreen();
#endif
}

PadSnapshot Pads::Read() {
    PadSnapshot out;
#ifdef __SWITCH__
    // Two Joy-Cons registered as separate controllers are joined back into one pair
    const int64_t now = Now();
    if (now - last_merge_check > 1500) {
        last_merge_check = now;
        const u32 first = hidGetNpadStyleSet(HidNpadIdType_No1);
        const u32 second = hidGetNpadStyleSet(HidNpadIdType_No2);
        const u32 single = HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight;
        if ((first & single) && !(first & HidNpadStyleTag_NpadJoyDual) && (second & single) &&
            !(second & HidNpadStyleTag_NpadJoyDual)) {
            hidMergeSingleJoyAsDualJoy(HidNpadIdType_No1, HidNpadIdType_No2);
        }
    }
    padUpdate(&pad);
    out.buttons = padGetButtons(&pad) & 0xFFFF;
    const HidAnalogStickState left = padGetStickPos(&pad, 0);
    const HidAnalogStickState right = padGetStickPos(&pad, 1);
    out.left_x = std::clamp(left.x, -32767, 32767);
    out.left_y = std::clamp(left.y, -32767, 32767);
    out.right_x = std::clamp(right.x, -32767, 32767);
    out.right_y = std::clamp(right.y, -32767, 32767);

    HidTouchScreenState touch{};
    if (hidGetTouchScreenStates(&touch, 1) > 0) {
        out.touch_count = std::min<int>(touch.count, MaxTouches);
        for (int i = 0; i < out.touch_count; ++i) {
            out.touches[i].id = static_cast<int>(touch.touches[i].finger_id);
            out.touches[i].x = std::clamp<int>(touch.touches[i].x, 0, 1279);
            out.touches[i].y = std::clamp<int>(touch.touches[i].y, 0, 719);
        }
    }
#else
    // A PC test run: keyboard, a game controller and the mouse as the touch screen
    (void)last_merge_check;
    if (gamepad == nullptr && SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
        gamepad = SDL_GameControllerOpen(0);
    }
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const auto key = [&](SDL_Scancode code, uint64_t bit) {
        if (keys[code]) {
            out.buttons |= bit;
        }
    };
    key(SDL_SCANCODE_X, ButtonA);
    key(SDL_SCANCODE_Z, ButtonB);
    key(SDL_SCANCODE_S, ButtonX);
    key(SDL_SCANCODE_A, ButtonY);
    key(SDL_SCANCODE_Q, ButtonL);
    key(SDL_SCANCODE_W, ButtonR);
    key(SDL_SCANCODE_RETURN, ButtonPlus);
    key(SDL_SCANCODE_BACKSPACE, ButtonMinus);
    key(SDL_SCANCODE_LEFT, ButtonLeft);
    key(SDL_SCANCODE_RIGHT, ButtonRight);
    key(SDL_SCANCODE_UP, ButtonUp);
    key(SDL_SCANCODE_DOWN, ButtonDown);
    if (const char* test = std::getenv("CHARIS_TEST_BUTTONS")) {
        out.buttons |= std::strtoull(test, nullptr, 0);
    }
    if (const char* stick = std::getenv("CHARIS_TEST_STICK")) {
        out.left_x = std::atoi(stick);
    }
    int mx = 0;
    int my = 0;
    if (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) {
        out.touch_count = 1;
        out.touches[0] = Touch{0, std::clamp(mx, 0, 1279), std::clamp(my, 0, 719)};
    }
    if (const char* touch = std::getenv("CHARIS_TEST_TOUCH")) {
        int tx = 0;
        int ty = 0;
        if (std::sscanf(touch, "%d,%d", &tx, &ty) == 2) {
            out.touch_count = 1;
            out.touches[0] = Touch{0, tx, ty};
        }
    }
#endif
    return out;
}

void KeyQueue::Text(const std::string& utf8) {
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t code = c;
        int extra = 0;
        if (c >= 0xF0) {
            code = c & 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            code = c & 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            code = c & 0x1F;
            extra = 1;
        }
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) {
            code = (code << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        }
        KeyEvent event{};
        event.sequence = next_sequence++;
        event.kind = KeyText;
        event.code = code;
        events.push_back({event, 0});
    }
}

void KeyQueue::Special(Key key) {
    KeyEvent event{};
    event.sequence = next_sequence++;
    event.kind = KeySpecial;
    event.code = key;
    events.push_back({event, 0});
}

void KeyQueue::Fill(InputState& state) {
    constexpr int Repeats = 5;
    while (!events.empty() && events.front().sent >= Repeats) {
        events.pop_front();
    }
    state.key_count = 0;
    for (Pending& pending : events) {
        if (state.key_count >= MaxKeyEvents) {
            break;
        }
        state.keys[state.key_count++] = pending.event;
        ++pending.sent;
    }
}

void DesktopPointer::Reset() {
    buttons = 0;
    touching = dragging = long_pressed = false;
    two_finger_y = -1;
    click_frames = right_click_frames = 0;
    scroll_rest = 0;
}

void DesktopPointer::Update(const PadSnapshot& pad, uint64_t pressed, int dt_ms) {
    now_ms = Now();
    const float dt = std::clamp(dt_ms, 1, 100) / 1000.0f;

    // Left stick: about one screen width per second at full tilt
    const float speed = 1.05f;
    x = std::clamp(x + Curve(pad.left_x) * speed * dt, 0.0f, 1.0f);
    y = std::clamp(y - Curve(pad.left_y) * speed * dt * (16.0f / 9.0f), 0.0f, 1.0f);

    // Right stick: up to 14 wheel steps a second
    scroll_rest += Curve(pad.right_y) * 14.0f * dt;
    while (scroll_rest >= 1.0f) {
        ++wheel;
        scroll_rest -= 1.0f;
    }
    while (scroll_rest <= -1.0f) {
        --wheel;
        scroll_rest += 1.0f;
    }

    // Touch screen
    if (pad.touch_count >= 2) {
        // Two fingers: scroll like a laptop touch screen
        const int middle = (pad.touches[0].y + pad.touches[1].y) / 2;
        if (two_finger_y >= 0) {
            const int moved = middle - two_finger_y;
            if (std::abs(moved) >= 30) {
                wheel += moved / 30;
                two_finger_y += (moved / 30) * 30;
            }
        } else {
            two_finger_y = middle;
        }
        touching = true;
        dragging = true; // no click when the fingers lift
        buttons &= static_cast<uint8_t>(~MouseLeft);
    } else if (pad.touch_count == 1) {
        two_finger_y = -1;
        const Touch& t = pad.touches[0];
        x = t.x / 1279.0f;
        y = t.y / 719.0f;
        if (!touching) {
            touching = true;
            dragging = false;
            long_pressed = false;
            touch_start_x = t.x;
            touch_start_y = t.y;
            touch_start_ms = now_ms;
        } else if (!dragging && !long_pressed &&
                   (std::abs(t.x - touch_start_x) > 14 || std::abs(t.y - touch_start_y) > 14)) {
            dragging = true; // drag: hold the left button while the finger moves
            buttons |= MouseLeft;
        } else if (!dragging && !long_pressed && now_ms - touch_start_ms > 600) {
            long_pressed = true; // hold: right click
            right_click_frames = 3;
        }
    } else if (touching) {
        touching = false;
        two_finger_y = -1;
        if (dragging) {
            buttons &= static_cast<uint8_t>(~MouseLeft);
        } else if (!long_pressed) {
            click_frames = 3; // a tap
        }
        dragging = false;
    }

    // Buttons: A clicks (and drags while held), X right-clicks
    const bool a_held = (pad.buttons & ButtonA) != 0;
    const bool x_held = (pad.buttons & ButtonX) != 0;
    uint8_t held = 0;
    if (a_held || click_frames > 0 || (touching && dragging && pad.touch_count == 1)) {
        held |= MouseLeft;
    }
    if (x_held || right_click_frames > 0) {
        held |= MouseRight;
    }
    // A tap presses for a couple of frames, then lets go for one, so it is a clean click
    if (click_frames > 0) {
        --click_frames;
        if (click_frames == 0) {
            held &= static_cast<uint8_t>(~MouseLeft);
        }
    }
    if (right_click_frames > 0) {
        --right_click_frames;
        if (right_click_frames == 0) {
            held &= static_cast<uint8_t>(~MouseRight);
        }
    }
    buttons = held;
    (void)pressed;
}

void DesktopPointer::Fill(InputState& state) const {
    state.mode = ModeDesktop;
    state.mouse_x = static_cast<uint16_t>(std::lround(x * 65535.0f));
    state.mouse_y = static_cast<uint16_t>(std::lround(y * 65535.0f));
    state.mouse_buttons = buttons;
    state.wheel = wheel;
}
