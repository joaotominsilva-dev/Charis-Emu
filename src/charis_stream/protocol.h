// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Charis Stream: the PC (Charis Switch) sends its screen and sound to the Switch app over UDP,
// and the Switch sends its controller back. Shared by both sides; plain C++17, little endian.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CharisStream {

constexpr uint16_t HostPort = 48010;   // PC listens: discovery, control and controller input
constexpr uint16_t ClientPort = 48011; // Switch listens: announcements, video and audio
constexpr uint32_t Magic = 0x53524843; // "CHRS"
constexpr uint8_t ProtocolVersion = 1;
constexpr size_t MaxVideoBytes = 1200; // video payload per datagram, under a 1500 byte MTU
constexpr int KeepaliveMs = 500;
constexpr int TimeoutMs = 3000;

enum class Type : uint8_t {
    Discover = 1,        // Switch -> broadcast: is a Charis Switch around?
    Announce = 2,        // PC -> Switch: name and screen size
    Start = 3,           // Switch -> PC: begin streaming with these settings
    Keepalive = 4,       // Switch -> PC, every KeepaliveMs while connected
    RequestKeyframe = 5, // Switch -> PC: lost video, send a full frame
    Stop = 6,            // either side: the session ends
    GoHome = 7,          // Switch -> PC: close the game and show the Charis home screen
    Video = 10,          // PC -> Switch: one piece of an H.264 frame
    Audio = 11,          // PC -> Switch: one Opus packet, 48 kHz stereo
    Input = 20,          // Switch -> PC: full controller state
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint8_t version;
    Type type;
    uint16_t reserved;
};

struct Announce {
    char name[48];
    uint16_t width;
    uint16_t height;
};

struct StartRequest {
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t reserved;
    uint32_t bitrate_kbps;
};

struct VideoChunk {
    uint32_t frame;      // increases by one per encoded frame
    uint16_t index;      // piece number inside the frame
    uint16_t count;      // pieces in the frame
    uint32_t frame_size; // bytes in the whole frame
    uint8_t keyframe;
    uint8_t reserved[3];
};

struct AudioChunk {
    uint32_t sequence;
};

struct InputState {
    uint32_t sequence;
    uint64_t buttons; // Button bits below
    int16_t left_x;   // -32767..32767, up is positive
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
};
#pragma pack(pop)

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
    // Not a libnx bit: the Switch app sends it for its "Home" combination
    ButtonHome = 1ull << 40,
};

inline size_t WriteHeader(uint8_t* out, Type type) {
    const Header header{Magic, ProtocolVersion, type, 0};
    std::memcpy(out, &header, sizeof(header));
    return sizeof(header);
}

// Returns the payload size after the header, or -1 when this is not a Charis Stream packet.
inline long ReadHeader(const uint8_t* data, size_t size, Type& type) {
    if (size < sizeof(Header)) {
        return -1;
    }
    Header header;
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != Magic || header.version != ProtocolVersion) {
        return -1;
    }
    type = header.type;
    return static_cast<long>(size - sizeof(Header));
}

} // namespace CharisStream
