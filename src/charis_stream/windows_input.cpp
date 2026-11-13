// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include "charis_stream/sources.h"

#ifndef _WIN32

namespace CharisStream {
void DesktopInput::Apply(const InputState&, bool) {}
void DesktopInput::Release() {}
void DesktopInput::TouchAsMouse(const InputState&) {}
} // namespace CharisStream

#else

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <vector>

#include <windows.h>

namespace CharisStream {

namespace {

INPUT KeyInput(WORD virtual_key, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (virtual_key == VK_LEFT || virtual_key == VK_RIGHT || virtual_key == VK_UP ||
        virtual_key == VK_DOWN || virtual_key == VK_LWIN) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    return input;
}

void Press(std::vector<INPUT>& inputs, std::initializer_list<WORD> keys) {
    for (const WORD key : keys) {
        inputs.push_back(KeyInput(key, false));
    }
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
        inputs.push_back(KeyInput(*it, true));
    }
}

void TypeCharacter(std::vector<INPUT>& inputs, uint32_t code) {
    if (code == '\n' || code == '\r') {
        Press(inputs, {VK_RETURN});
        return;
    }
    if (code == '\t') {
        Press(inputs, {VK_TAB});
        return;
    }
    if (code < 0x20 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return;
    }
    WORD units[2];
    int count = 1;
    if (code >= 0x10000) {
        const uint32_t value = code - 0x10000;
        units[0] = static_cast<WORD>(0xD800 + (value >> 10));
        units[1] = static_cast<WORD>(0xDC00 + (value & 0x3FF));
        count = 2;
    } else {
        units[0] = static_cast<WORD>(code);
    }
    for (int i = 0; i < count; ++i) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = units[i];
        input.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(input);
        input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
}

void SpecialKey(std::vector<INPUT>& inputs, uint32_t key) {
    switch (key) {
    case KeyEnter:
        Press(inputs, {VK_RETURN});
        break;
    case KeyBackspace:
        Press(inputs, {VK_BACK});
        break;
    case KeyEscape:
        Press(inputs, {VK_ESCAPE});
        break;
    case KeyTab:
        Press(inputs, {VK_TAB});
        break;
    case KeyLeft:
        Press(inputs, {VK_LEFT});
        break;
    case KeyRight:
        Press(inputs, {VK_RIGHT});
        break;
    case KeyUp:
        Press(inputs, {VK_UP});
        break;
    case KeyDown:
        Press(inputs, {VK_DOWN});
        break;
    case KeyWindows:
        Press(inputs, {VK_LWIN});
        break;
    case KeyAltTab:
        Press(inputs, {VK_MENU, VK_TAB});
        break;
    case KeyAltF4:
        Press(inputs, {VK_MENU, VK_F4});
        break;
    default:
        break;
    }
}

void MouseButtonInput(std::vector<INPUT>& inputs, DWORD flag) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    inputs.push_back(input);
}

void Send(std::vector<INPUT>& inputs) {
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

void TypeKeys(const InputState& state, std::vector<INPUT>& inputs, uint32_t& last_key_sequence) {
    // Each key event comes in several packets in a row; type each one once, in order
    const int count = std::min<int>(state.key_count, MaxKeyEvents);
    for (int i = 0; i < count; ++i) {
        const KeyEvent& event = state.keys[i];
        if (static_cast<int32_t>(event.sequence - last_key_sequence) <= 0) {
            continue;
        }
        if (event.kind == KeyText) {
            TypeCharacter(inputs, event.code);
        } else {
            SpecialKey(inputs, event.code);
        }
        last_key_sequence = event.sequence;
    }
}

} // namespace

void DesktopInput::Apply(const InputState& state, bool with_pointer) {
    std::vector<INPUT> inputs;
    if (!with_pointer) {
        // Game mode: forget the pointer so it is not moved when desktop mode comes back
        have_previous = false;
        TypeKeys(state, inputs, last_key_sequence);
        Send(inputs);
        return;
    }

    // The Switch owns the pointer only while it moves it, so the PC's own mouse still works
    const bool moved = !have_previous || state.mouse_x != mouse_x || state.mouse_y != mouse_y;
    if (moved) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        // 0..65535 across the main monitor, the one the Switch is watching
        input.mi.dx = state.mouse_x;
        input.mi.dy = state.mouse_y;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs.push_back(input);
        mouse_x = state.mouse_x;
        mouse_y = state.mouse_y;
    }

    const uint8_t changed = mouse_buttons ^ state.mouse_buttons;
    if (changed & MouseLeft) {
        MouseButtonInput(inputs, (state.mouse_buttons & MouseLeft) ? MOUSEEVENTF_LEFTDOWN
                                                                   : MOUSEEVENTF_LEFTUP);
    }
    if (changed & MouseRight) {
        MouseButtonInput(inputs, (state.mouse_buttons & MouseRight) ? MOUSEEVENTF_RIGHTDOWN
                                                                    : MOUSEEVENTF_RIGHTUP);
    }
    mouse_buttons = state.mouse_buttons;

    if (have_previous && state.wheel != wheel) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>((state.wheel - wheel) * WHEEL_DELTA);
        inputs.push_back(input);
    }
    wheel = state.wheel;
    have_previous = true;

    TypeKeys(state, inputs, last_key_sequence);
    Send(inputs);
}


void DesktopInput::TouchAsMouse(const InputState& state) {
    std::vector<INPUT> inputs;
    if (state.touch_count > 0) {
        INPUT move{};
        move.type = INPUT_MOUSE;
        move.mi.dx = state.touches[0].x;
        move.mi.dy = state.touches[0].y;
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        inputs.push_back(move);
        if (!touch_down) {
            MouseButtonInput(inputs, MOUSEEVENTF_LEFTDOWN);
            touch_down = true;
        }
    } else if (touch_down) {
        MouseButtonInput(inputs, MOUSEEVENTF_LEFTUP);
        touch_down = false;
    }
    Send(inputs);
}

void DesktopInput::Release() {
    std::vector<INPUT> inputs;
    if (touch_down) {
        MouseButtonInput(inputs, MOUSEEVENTF_LEFTUP);
        touch_down = false;
    }
    if (mouse_buttons & MouseLeft) {
        MouseButtonInput(inputs, MOUSEEVENTF_LEFTUP);
    }
    if (mouse_buttons & MouseRight) {
        MouseButtonInput(inputs, MOUSEEVENTF_RIGHTUP);
    }
    mouse_buttons = 0;
    Send(inputs);
}

} // namespace CharisStream

#endif
