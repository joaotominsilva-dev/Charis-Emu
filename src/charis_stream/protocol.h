// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Charis Stream, protocol 2. The PC (Charis Switch) sends its screen and sound to the Switch app
// over UDP and the Switch sends its controller, touch, mouse and keys back. Both sides are logged
// in to the same Charis account; the stream key derived from that account authenticates the
// handshake and encrypts everything after it. Shared by both sides; plain C++17, little endian.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CharisStream {

constexpr uint16_t HostPort = 48010;   // PC listens here; LAN discovery is broadcast to it
constexpr uint32_t Magic = 0x32524843; // "CHR2"
constexpr size_t ChunkBytes = 1100;    // video payload per datagram, safe for internet MTUs
constexpr size_t MaxDatagram = 1400;
constexpr int KeepaliveMs = 500;
constexpr int TimeoutMs = 4000;

enum class Type : uint8_t {
    // In the clear: discovery and handshake
    Discover = 1, // Switch -> broadcast: is a PC of my account around?
    Announce = 2, // PC -> Switch
    Hello = 3,    // Switch -> PC: start a session (proves it has the account key)
    Welcome = 4,  // PC -> Switch: session accepted (proves the same)
    Punch = 5,    // either way: opens the sender's router for the other side

    // Encrypted with the session keys
    Start = 16,           // Switch -> PC: begin streaming with these settings
    Keepalive = 17,       // Switch -> PC, every KeepaliveMs
    RequestKeyframe = 18, // Switch -> PC: lost video, send a full frame
    Stop = 19,            // either side: the session ends
    GoHome = 20,          // Switch -> PC: close the game and show the Charis home screen
    SetBitrate = 21,      // Switch -> PC: the network asks for less (or allows more)
    Video = 32,           // PC -> Switch: one piece of an H.264 frame, or parity for it
    Audio = 33,           // PC -> Switch: one Opus packet, 48 kHz stereo
    Input = 40,           // Switch -> PC: controller, touch, mouse and keys
};

inline bool IsEncrypted(Type type) {
    return static_cast<uint8_t>(type) >= 16;
}

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    Type type;
    uint8_t reserved[3];
    uint64_t counter; // per sender, never repeats inside a session; part of the nonce
};

struct Discover {
    uint8_t tag[16];
};

struct Announce {
    uint8_t tag[16];
    uint8_t host_id[8];
    char name[48];
};

struct Hello {
    uint8_t tag[16];
    uint8_t client_nonce[16];
    uint8_t proof[16];
    char name[32];
};

struct Welcome {
    uint8_t client_nonce[16];
    uint8_t host_nonce[16];
    uint8_t proof[16];
};

struct StartRequest {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t reserved;
    uint32_t bitrate_kbps;
};

struct SetBitrate {
    uint32_t bitrate_kbps;
};

// A frame is cut into data_count pieces of ChunkBytes (the last may be shorter), followed by
// parity_count parity pieces. Parity piece p is the XOR of the data pieces whose index is
// p modulo parity_count, so one lost piece per group can be rebuilt, and a burst of up to
// parity_count lost pieces in a row still leaves at most one hole per group.
struct VideoChunk {
    uint32_t frame;        // increases by one per encoded frame
    uint16_t index;        // 0..data_count-1 data, then parity
    uint16_t data_count;
    uint16_t parity_count;
    uint16_t reserved;
    uint32_t frame_size;   // bytes in the whole frame
    uint8_t keyframe;
    uint8_t pad[3];
};

struct AudioChunk {
    uint32_t sequence;
};

struct TouchPoint {
    uint8_t id;
    uint8_t pad;
    uint16_t x; // 0..65535 across the picture
    uint16_t y;
};

struct KeyEvent {
    uint32_t sequence; // each event is repeated in several packets; the PC applies it once
    uint8_t kind;      // KeyKind
    uint8_t pad[3];
    uint32_t code;     // Unicode code point, or a Key value
};

constexpr int MaxTouches = 2;
constexpr int MaxKeyEvents = 8;

struct InputState {
    uint32_t sequence;
    uint8_t mode; // InputMode
    uint8_t touch_count;
    uint8_t key_count;
    uint8_t mouse_buttons; // MouseLeft | MouseRight
    // Game mode
    uint64_t buttons;      // Button bits below
    int16_t left_x;        // -32767..32767, up is positive
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
    TouchPoint touches[MaxTouches];
    // Desktop mode
    uint16_t mouse_x; // 0..65535 across the PC screen
    uint16_t mouse_y;
    int32_t wheel;    // running total of wheel steps; the PC applies the difference
    KeyEvent keys[MaxKeyEvents];
};
#pragma pack(pop)

enum InputMode : uint8_t {
    ModeGame = 0,    // controller and touch go to the emulated console
    ModeDesktop = 1, // mouse and keyboard for the PC itself
};

enum MouseButton : uint8_t {
    MouseLeft = 1,
    MouseRight = 2,
};

enum KeyKind : uint8_t {
    KeyText = 0,    // code is a Unicode character to type
    KeySpecial = 1, // code is a Key value, pressed and released
};

enum Key : uint32_t {
    KeyEnter = 1,
    KeyBackspace = 2,
    KeyEscape = 3,
    KeyTab = 4,
    KeyLeft = 5,
    KeyRight = 6,
    KeyUp = 7,
    KeyDown = 8,
    KeyWindows = 9,
    KeyAltTab = 10,
    KeyAltF4 = 11,
};

// libnx HidNpadButton bit order; bits 0-15 also match the emulator's virtual gamepad buttons.
enum Button : uint64_t {
    ButtonA = 1ull << 0,
    ButtonB = 1ull << 1,
    ButtonX = 1ull << 2,
    ButtonY = 1ull << 3,
    ButtonStickL = 1ull << 4,
    ButtonStickR = 1ull << 5,
    ButtonL = 1ull << 6,
    ButtonR = 1ull << 7,
    ButtonZL = 1ull << 8,
    ButtonZR = 1ull << 9,
    ButtonPlus = 1ull << 10,
    ButtonMinus = 1ull << 11,
    ButtonLeft = 1ull << 12,
    ButtonUp = 1ull << 13,
    ButtonRight = 1ull << 14,
    ButtonDown = 1ull << 15,
    // Not a libnx bit: the Switch app sends it for its "HOME do jogo" menu entry
    ButtonHome = 1ull << 40,
};

inline uint16_t ParityCount(uint16_t data_count) {
    return data_count == 0 ? 0 : static_cast<uint16_t>((data_count + 4) / 5); // about 20%
}

inline size_t WriteHeader(uint8_t* out, Type type, uint64_t counter = 0) {
    Header header{};
    header.magic = Magic;
    header.type = type;
    header.counter = counter;
    std::memcpy(out, &header, sizeof(header));
    return sizeof(header);
}

// Returns the payload size after the header, or -1 when this is not a Charis Stream packet.
inline long ReadHeader(const uint8_t* data, size_t size, Header& header) {
    if (size < sizeof(Header)) {
        return -1;
    }
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != Magic) {
        return -1;
    }
    return static_cast<long>(size - sizeof(Header));
}

} // namespace CharisStream
