// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Charis Stream for Nintendo Switch (homebrew). Finds Charis Switch on the PC, shows its screen,
// plays its sound and sends the controller back. Also builds on Linux for testing.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_ttf.h>
#include <opus/opus.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "charis_stream/protocol.h"

using namespace CharisStream;

namespace {

// ---------- Charis palette ----------
struct Color {
    Uint8 r, g, b, a;
};
constexpr Color Charcoal{0x20, 0x21, 0x1C, 255};
constexpr Color Charcoal2{0x2C, 0x2D, 0x27, 255};
constexpr Color Cream{0xF3, 0xF4, 0xEA, 255};
constexpr Color Cream2{0xE9, 0xEA, 0xDD, 255};
constexpr Color SageLight{0xC8, 0xC9, 0xB7, 255};
constexpr Color Ink{0x1D, 0x1E, 0x19, 255};
constexpr Color InkSoft{0x55, 0x56, 0x4B, 255};
constexpr Color OnDarkSoft{0xA9, 0xAA, 0x98, 255};
constexpr Color Danger{0x95, 0x35, 0x21, 255};

constexpr int ScreenW = 1280;
constexpr int ScreenH = 720;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------- Drawing ----------
struct Ui {
    SDL_Renderer* renderer = nullptr;
    TTF_Font* title = nullptr;
    TTF_Font* body = nullptr;
    TTF_Font* small = nullptr;
    TTF_Font* bold = nullptr;

    void Fill(Color c) {
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_RenderClear(renderer);
    }
    void Box(int x, int y, int w, int h, int radius, Color c) {
        roundedBoxRGBA(renderer, x, y, x + w - 1, y + h - 1, radius, c.r, c.g, c.b, c.a);
    }
    // Returns the text width
    int Text(TTF_Font* font, const std::string& text, int x, int y, Color c, bool center = false) {
        if (text.empty()) {
            return 0;
        }
        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), SDL_Color{c.r, c.g, c.b, c.a});
        if (surface == nullptr) {
            return 0;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_Rect rect{center ? x - surface->w / 2 : x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, nullptr, &rect);
        const int width = surface->w;
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
        return width;
    }
    void Hint(int x, int y, const char* button, const std::string& label, bool dark) {
        const Color pill = dark ? SageLight : Charcoal;
        const Color glyph = dark ? Charcoal : Cream;
        filledCircleRGBA(renderer, x + 14, y + 14, 14, pill.r, pill.g, pill.b, 255);
        Text(bold, button, x + 14, y + 2, glyph, true);
        Text(body, label, x + 38, y + 1, dark ? Cream : Ink);
    }
};

// ---------- Network ----------
struct Net {
    int fd = -1;

    bool Open() {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            return false;
        }
        const int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
        const int buffer = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(ClientPort);
        if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        return true;
    }

    void Send(uint32_t ip, Type type, const void* body = nullptr, size_t size = 0) {
        uint8_t packet[256];
        const size_t offset = WriteHeader(packet, type);
        if (body != nullptr && size > 0) {
            std::memcpy(packet + offset, body, size);
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = ip;
        to.sin_port = htons(HostPort);
        sendto(fd, packet, offset + size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
};

struct HostInfo {
    uint32_t ip = 0;
    std::string name;
    int64_t seen_ms = 0;
};

std::string IpText(uint32_t ip) {
    char text[INET_ADDRSTRLEN] = {};
    in_addr address{};
    address.s_addr = ip;
    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

// ---------- Video ----------
struct Video {
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw_frame = nullptr;
    AVPacket* packet = nullptr;
    SDL_Texture* texture = nullptr;
    int texture_format = -1;
    int texture_w = 0;
    int texture_h = 0;
    bool has_picture = false;

    // Reassembly of the frame being received
    uint32_t current = 0;
    bool receiving = false;
    std::vector<uint8_t> data;
    std::vector<uint8_t> got;
    uint16_t missing = 0;
    bool keyframe = false;
    bool waiting_keyframe = true;
    int64_t last_request_ms = 0;
    uint32_t frames_shown = 0;

    static enum AVPixelFormat PickFormat(AVCodecContext*, const enum AVPixelFormat* formats) {
#ifdef __SWITCH__
        for (const enum AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
            if (*f == AV_PIX_FMT_NVTEGRA) {
                return *f;
            }
        }
#endif
        return formats[0];
    }

    bool Open() {
        const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        codec = avcodec_alloc_context3(decoder);
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec->flags2 |= AV_CODEC_FLAG2_FAST;
#ifdef __SWITCH__
        // Hardware decoding on the Switch's own video chip
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) == 0) {
            codec->hw_device_ctx = device;
            codec->get_format = PickFormat;
        }
#else
        codec->thread_count = 4;
        codec->thread_type = FF_THREAD_SLICE;
#endif
        if (avcodec_open2(codec, decoder, nullptr) < 0) {
            return false;
        }
        frame = av_frame_alloc();
        sw_frame = av_frame_alloc();
        packet = av_packet_alloc();
        return true;
    }

    // Returns true when a new picture is ready in the texture.
    bool Decode(SDL_Renderer* renderer, const std::vector<uint8_t>& bytes) {
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(bytes.size())) < 0) {
            return false;
        }
        std::memcpy(packet->data, bytes.data(), bytes.size());
        if (avcodec_send_packet(codec, packet) < 0) {
            return false;
        }
        bool updated = false;
        while (avcodec_receive_frame(codec, frame) == 0) {
            AVFrame* picture = frame;
            if (frame->hw_frames_ctx != nullptr) {
                av_frame_unref(sw_frame);
                if (av_hwframe_transfer_data(sw_frame, frame, 0) < 0) {
                    av_frame_unref(frame);
                    continue;
                }
                picture = sw_frame;
            }
            updated = Upload(renderer, picture) || updated;
            av_frame_unref(frame);
        }
        return updated;
    }

    bool Upload(SDL_Renderer* renderer, AVFrame* picture) {
        const bool nv12 = picture->format == AV_PIX_FMT_NV12;
        const int format = nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
        if (!nv12 && picture->format != AV_PIX_FMT_YUV420P && picture->format != AV_PIX_FMT_YUVJ420P) {
            return false;
        }
        if (texture == nullptr || texture_format != format || texture_w != picture->width ||
            texture_h != picture->height) {
            if (texture != nullptr) {
                SDL_DestroyTexture(texture);
            }
            texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING,
                                        picture->width, picture->height);
            texture_format = format;
            texture_w = picture->width;
            texture_h = picture->height;
        }
        if (nv12) {
            SDL_UpdateNVTexture(texture, nullptr, picture->data[0], picture->linesize[0],
                                picture->data[1], picture->linesize[1]);
        } else {
            SDL_UpdateYUVTexture(texture, nullptr, picture->data[0], picture->linesize[0],
                                 picture->data[1], picture->linesize[1], picture->data[2],
                                 picture->linesize[2]);
        }
        has_picture = true;
        ++frames_shown;
        return true;
    }

    void Reset() {
        receiving = false;
        waiting_keyframe = true;
        has_picture = false;
        if (codec != nullptr) {
            avcodec_flush_buffers(codec);
        }
    }
};

// ---------- Audio ----------
struct Audio {
    SDL_AudioDeviceID device = 0;
    OpusDecoder* decoder = nullptr;
    std::vector<int16_t> pcm = std::vector<int16_t>(5760 * 2);

    bool Open() {
        SDL_AudioSpec want{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        int error = 0;
        decoder = opus_decoder_create(48000, 2, &error);
        if (device != 0) {
            SDL_PauseAudioDevice(device, 0);
        }
        return device != 0 && decoder != nullptr;
    }

    void Play(const uint8_t* data, size_t size) {
        if (device == 0 || decoder == nullptr) {
            return;
        }
        const int samples = opus_decode(decoder, data, static_cast<opus_int32>(size), pcm.data(),
                                        5760, 0);
        if (samples <= 0) {
            return;
        }
        // Keep the queue short: about 80 ms at most, so sound stays in sync with the picture
        if (SDL_GetQueuedAudioSize(device) > 48000 * 2 * 2 * 80 / 1000) {
            SDL_ClearQueuedAudio(device);
        }
        SDL_QueueAudio(device, pcm.data(), static_cast<Uint32>(samples * 2 * sizeof(int16_t)));
    }
};

// ---------- Controller ----------
struct Controller {
#ifdef __SWITCH__
    PadState pad{};
    int64_t last_merge_check = 0;
#else
    SDL_GameController* gamepad = nullptr;
#endif

    void Open() {
#ifdef __SWITCH__
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        hidSetNpadJoyHoldType(HidNpadJoyHoldType_Vertical);
        padInitialize(&pad, HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_Handheld);
#endif
    }

#ifdef __SWITCH__
    // Two Joy-Cons registered as separate controllers are joined back into one pair.
    void MergeJoyCons() {
        const int64_t now = NowMs();
        if (now - last_merge_check < 1500) {
            return;
        }
        last_merge_check = now;
        const u32 first = hidGetNpadStyleSet(HidNpadIdType_No1);
        const u32 second = hidGetNpadStyleSet(HidNpadIdType_No2);
        const bool first_single = (first & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
                                  !(first & HidNpadStyleTag_NpadJoyDual);
        const bool second_single = (second & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
                                   !(second & HidNpadStyleTag_NpadJoyDual);
        if (first_single && second_single) {
            hidMergeSingleJoyAsDualJoy(HidNpadIdType_No1, HidNpadIdType_No2);
        }
    }
#endif

    InputState Read(uint32_t sequence) {
        InputState state{};
        state.sequence = sequence;
#ifdef __SWITCH__
        MergeJoyCons();
        padUpdate(&pad);
        state.buttons = padGetButtons(&pad) & 0xFFFF;
        const HidAnalogStickState left = padGetStickPos(&pad, 0);
        const HidAnalogStickState right = padGetStickPos(&pad, 1);
        state.left_x = static_cast<int16_t>(std::clamp(left.x, -32767, 32767));
        state.left_y = static_cast<int16_t>(std::clamp(left.y, -32767, 32767));
        state.right_x = static_cast<int16_t>(std::clamp(right.x, -32767, 32767));
        state.right_y = static_cast<int16_t>(std::clamp(right.y, -32767, 32767));
#else
        if (gamepad == nullptr && SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
            gamepad = SDL_GameControllerOpen(0);
        }
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        auto key = [&](SDL_Scancode code, uint64_t bit) {
            if (keys[code]) {
                state.buttons |= bit;
            }
        };
        key(SDL_SCANCODE_X, ButtonA);
        key(SDL_SCANCODE_Z, ButtonB);
        key(SDL_SCANCODE_S, ButtonX);
        key(SDL_SCANCODE_A, ButtonY);
        key(SDL_SCANCODE_RETURN, ButtonPlus);
        key(SDL_SCANCODE_BACKSPACE, ButtonMinus);
        key(SDL_SCANCODE_LEFT, ButtonLeft);
        key(SDL_SCANCODE_RIGHT, ButtonRight);
        key(SDL_SCANCODE_UP, ButtonUp);
        key(SDL_SCANCODE_DOWN, ButtonDown);
        if (const char* test = std::getenv("CHARIS_TEST_BUTTONS")) {
            state.buttons |= std::strtoull(test, nullptr, 0);
        }
#endif
        return state;
    }
};

enum class Screen { Searching, Streaming, Menu, Message };

} // namespace

int main(int, char**) {
#ifdef __SWITCH__
    socketInitializeDefault();
    romfsInit();
    const char* font_regular = "romfs:/Archivo-Regular.ttf";
    const char* font_bold = "romfs:/Archivo-ExtraBold.ttf";
#else
    const char* font_regular = std::getenv("CHARIS_FONT") ? std::getenv("CHARIS_FONT") : "romfs/Archivo-Regular.ttf";
    const char* font_bold = std::getenv("CHARIS_FONT_BOLD") ? std::getenv("CHARIS_FONT_BOLD") : "romfs/Archivo-ExtraBold.ttf";
#endif

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    TTF_Init();
    SDL_Window* window = SDL_CreateWindow("Charis Stream", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, ScreenW, ScreenH, 0);
    Ui ui;
    ui.renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (ui.renderer == nullptr) {
        ui.renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_SetRenderDrawBlendMode(ui.renderer, SDL_BLENDMODE_BLEND);
    ui.title = TTF_OpenFont(font_bold, 52);
    ui.bold = TTF_OpenFont(font_bold, 22);
    ui.body = TTF_OpenFont(font_regular, 24);
    ui.small = TTF_OpenFont(font_regular, 18);

    Net net;
    const bool net_ok = net.Open();
    Video video;
    video.Open();
    Audio audio;
    audio.Open();
    Controller controller;
    controller.Open();

    std::vector<HostInfo> hosts;
    int selected = 0;
    HostInfo host;
    Screen screen = Screen::Searching;
    std::string message;
    int64_t last_discover = 0;
    int64_t first_found = 0;
    int64_t last_keepalive = 0;
    int64_t last_start = 0;
    int64_t last_packet = 0;
    int64_t combo_since = 0;
    int menu_choice = 0;
    uint32_t input_sequence = 0;
    uint64_t previous_buttons = 0;
    int home_frames = 0;
    std::vector<uint8_t> buffer(2048);
    const char* forced_host = std::getenv("CHARIS_HOST");
    const char* screenshot_path = std::getenv("CHARIS_SCREENSHOT");
    const int64_t started_ms = NowMs();
    bool running = true;

    const auto disconnect = [&](const std::string& why) {
        net.Send(host.ip, Type::Stop);
        video.Reset();
        host = {};
        hosts.clear();
        first_found = 0;
        message = why;
        screen = why.empty() ? Screen::Searching : Screen::Message;
    };

    while (running) {
#ifdef __SWITCH__
        if (!appletMainLoop()) {
            break;
        }
#endif
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }
        const int64_t now = NowMs();
        InputState input = controller.Read(input_sequence++);
        const uint64_t pressed = input.buttons & ~previous_buttons;
        previous_buttons = input.buttons;

        // ---- Receive everything waiting ----
        if (net_ok) {
            while (true) {
                sockaddr_in from{};
                socklen_t from_size = sizeof(from);
                const ssize_t got = recvfrom(net.fd, buffer.data(), buffer.size(), 0,
                                             reinterpret_cast<sockaddr*>(&from), &from_size);
                if (got <= 0) {
                    break;
                }
                Type type;
                const long payload = ReadHeader(buffer.data(), static_cast<size_t>(got), type);
                if (payload < 0) {
                    continue;
                }
                const uint8_t* body = buffer.data() + sizeof(Header);
                if (type == Type::Announce && payload >= static_cast<long>(sizeof(Announce))) {
                    Announce announce;
                    std::memcpy(&announce, body, sizeof(announce));
                    announce.name[sizeof(announce.name) - 1] = 0;
                    auto it = std::find_if(hosts.begin(), hosts.end(), [&](const HostInfo& h) {
                        return h.ip == from.sin_addr.s_addr;
                    });
                    if (it == hosts.end()) {
                        hosts.push_back({from.sin_addr.s_addr, announce.name, now});
                        if (first_found == 0) {
                            first_found = now;
                        }
                    } else {
                        it->seen_ms = now;
                    }
                    continue;
                }
                if (host.ip == 0 || from.sin_addr.s_addr != host.ip) {
                    continue;
                }
                last_packet = now;
                if (type == Type::Stop) {
                    disconnect("O PC parou a transmissão.");
                    break;
                }
                if (type == Type::Audio && payload > static_cast<long>(sizeof(AudioChunk))) {
                    audio.Play(body + sizeof(AudioChunk), static_cast<size_t>(payload) - sizeof(AudioChunk));
                    continue;
                }
                if (type != Type::Video || payload < static_cast<long>(sizeof(VideoChunk))) {
                    continue;
                }
                VideoChunk chunk;
                std::memcpy(&chunk, body, sizeof(chunk));
                const size_t bytes = static_cast<size_t>(payload) - sizeof(chunk);
                if (chunk.count == 0 || chunk.index >= chunk.count || chunk.frame_size > 8 * 1024 * 1024) {
                    continue;
                }
                if (!video.receiving || chunk.frame != video.current) {
                    if (video.receiving && video.missing > 0 &&
                        static_cast<int32_t>(chunk.frame - video.current) > 0) {
                        video.waiting_keyframe = true; // a frame was lost
                    }
                    if (video.receiving && static_cast<int32_t>(chunk.frame - video.current) < 0) {
                        continue; // late piece of an older frame
                    }
                    video.receiving = true;
                    video.current = chunk.frame;
                    video.data.assign(chunk.frame_size, 0);
                    video.got.assign(chunk.count, 0);
                    video.missing = chunk.count;
                    video.keyframe = chunk.keyframe != 0;
                }
                const size_t offset = static_cast<size_t>(chunk.index) * MaxVideoBytes;
                if (video.got[chunk.index] || offset + bytes > video.data.size()) {
                    continue;
                }
                std::memcpy(video.data.data() + offset, body + sizeof(chunk), bytes);
                video.got[chunk.index] = 1;
                if (--video.missing == 0) {
                    if (video.waiting_keyframe && !video.keyframe) {
                        continue;
                    }
                    video.waiting_keyframe = false;
                    video.Decode(ui.renderer, video.data);
                }
            }
        }

        // ---- State machine ----
        switch (screen) {
        case Screen::Searching:
            if (forced_host != nullptr && host.ip == 0) {
                host.ip = inet_addr(forced_host);
                host.name = forced_host;
                screen = Screen::Streaming;
                last_start = 0;
                last_packet = now;
                break;
            }
            if (now - last_discover > 1000) {
                last_discover = now;
                net.Send(htonl(INADDR_BROADCAST), Type::Discover);
            }
            hosts.erase(std::remove_if(hosts.begin(), hosts.end(),
                                       [&](const HostInfo& h) { return now - h.seen_ms > 5000; }),
                        hosts.end());
            selected = std::clamp(selected, 0, std::max(0, static_cast<int>(hosts.size()) - 1));
            if (pressed & ButtonDown) {
                selected = std::min(selected + 1, std::max(0, static_cast<int>(hosts.size()) - 1));
            }
            if (pressed & ButtonUp) {
                selected = std::max(selected - 1, 0);
            }
            if (!hosts.empty() && ((pressed & ButtonA) ||
                                   (hosts.size() == 1 && now - first_found > 1500))) {
                host = hosts[selected];
                screen = Screen::Streaming;
                last_start = 0;
                last_packet = now;
                video.Reset();
            }
            if (pressed & ButtonPlus) {
                running = false;
            }
            break;
        case Screen::Streaming:
        case Screen::Menu: {
            if (!video.has_picture && now - last_start > 1000) {
                last_start = now;
                const StartRequest start{1280, 720, 60, 0, 20000};
                net.Send(host.ip, Type::Start, &start, sizeof(start));
            }
            if (now - last_keepalive > KeepaliveMs) {
                last_keepalive = now;
                net.Send(host.ip, Type::Keepalive);
            }
            if (video.waiting_keyframe && now - video.last_request_ms > 250) {
                video.last_request_ms = now;
                net.Send(host.ip, Type::RequestKeyframe);
            }
            if (now - last_packet > TimeoutMs) {
                disconnect("A conexão com o PC caiu.");
                break;
            }
            // − and + together for a second: Charis menu (the game does not see it)
            const bool combo = (input.buttons & ButtonPlus) && (input.buttons & ButtonMinus);
            combo_since = combo ? (combo_since == 0 ? now : combo_since) : 0;
            if (screen == Screen::Streaming) {
                if (combo_since != 0 && now - combo_since > 900) {
                    screen = Screen::Menu;
                    menu_choice = 0;
                    combo_since = 0;
                }
                if (home_frames > 0) {
                    input.buttons |= ButtonHome;
                    --home_frames;
                }
                net.Send(host.ip, Type::Input, &input, sizeof(input));
            } else {
                InputState idle{};
                idle.sequence = input.sequence;
                net.Send(host.ip, Type::Input, &idle, sizeof(idle));
                if (pressed & ButtonDown) {
                    menu_choice = std::min(menu_choice + 1, 3);
                }
                if (pressed & ButtonUp) {
                    menu_choice = std::max(menu_choice - 1, 0);
                }
                if (pressed & ButtonB) {
                    screen = Screen::Streaming;
                }
                if (pressed & ButtonA) {
                    if (menu_choice == 0) {
                        screen = Screen::Streaming;
                    } else if (menu_choice == 1) {
                        // Close the game on the PC and show the Charis home screen
                        net.Send(host.ip, Type::GoHome);
                        screen = Screen::Streaming;
                    } else if (menu_choice == 2) {
                        screen = Screen::Streaming;
                        home_frames = 6;
                    } else {
                        disconnect("");
                    }
                }
            }
            break;
        }
        case Screen::Message:
            if (pressed & (ButtonA | ButtonB)) {
                screen = Screen::Searching;
                message.clear();
            }
            break;
        }

        // ---- Draw ----
        if ((screen == Screen::Streaming || screen == Screen::Menu) && video.has_picture) {
            SDL_SetRenderDrawColor(ui.renderer, 0, 0, 0, 255);
            SDL_RenderClear(ui.renderer);
            SDL_RenderCopy(ui.renderer, video.texture, nullptr, nullptr);
        } else {
            ui.Fill(Charcoal);
            ui.Text(ui.bold, "CHARIS STREAM", 72, 64, OnDarkSoft);
            if (screen == Screen::Streaming || screen == Screen::Menu) {
                ui.Text(ui.title, "Conectando...", 72, 100, Cream);
                ui.Text(ui.body, "Abrindo a tela de " + host.name, 72, 176, OnDarkSoft);
            } else if (screen == Screen::Message) {
                ui.Text(ui.title, "Transmissão encerrada", 72, 100, Cream);
                ui.Text(ui.body, message, 72, 176, OnDarkSoft);
                ui.Hint(72, ScreenH - 80, "A", "Procurar de novo", true);
            } else {
                ui.Text(ui.title, "Olá! Vamos jogar no PC?", 72, 100, Cream);
                ui.Box(0, 220, ScreenW, ScreenH - 220 + 40, 28, Cream);
                if (hosts.empty()) {
                    ui.Text(ui.bold, net_ok ? "Procurando o Charis Switch na sua rede..." :
                                              "Sem conexão de rede no Switch.", 72, 270, Ink);
                    ui.Text(ui.body, "No PC, abra o Charis Switch e clique em \"Transmitir para o Switch\".",
                            72, 312, InkSoft);
                    ui.Text(ui.body, "O PC e o Switch precisam estar na mesma rede.", 72, 348, InkSoft);
                } else {
                    ui.Text(ui.small, "PCS ENCONTRADOS", 72, 258, InkSoft);
                    for (size_t i = 0; i < hosts.size() && i < 4; ++i) {
                        const bool on = static_cast<int>(i) == selected;
                        const int y = 292 + static_cast<int>(i) * 92;
                        ui.Box(64, y, ScreenW - 128, 78, 20, on ? Charcoal : Cream2);
                        ui.Text(ui.bold, hosts[i].name, 96, y + 14, on ? Cream : Ink);
                        ui.Text(ui.small, IpText(hosts[i].ip), 96, y + 44, on ? OnDarkSoft : InkSoft);
                    }
                }
                ui.Hint(72, ScreenH - 64, "A", "Conectar", false);
                ui.Hint(260, ScreenH - 64, "+", "Sair", false);
            }
        }
        if (screen == Screen::Menu) {
            SDL_SetRenderDrawColor(ui.renderer, 0, 0, 0, 140);
            SDL_Rect shade{0, 0, ScreenW, ScreenH};
            SDL_RenderFillRect(ui.renderer, &shade);
            ui.Box(ScreenW - 520, 0, 560, ScreenH, 32, Charcoal);
            ui.Text(ui.small, "CHARIS STREAM", ScreenW - 484, 40, OnDarkSoft);
            ui.Text(ui.title, "Menu", ScreenW - 484, 66, Cream);
            const char* items[] = {"Continuar", "Voltar ao início", "Botão HOME do jogo",
                                   "Desconectar"};
            const char* details[] = {"Volta para o jogo", "Fecha o jogo e mostra seus jogos",
                                     "Abre o menu HOME dentro do jogo", "Encerra a transmissão"};
            ui.Box(ScreenW - 496, 150, 472, 4 * 86 + 16, 26, SageLight);
            for (int i = 0; i < 4; ++i) {
                const bool on = i == menu_choice;
                const int y = 158 + i * 86;
                ui.Box(ScreenW - 488, y, 456, 76, 20, on ? Charcoal : Cream);
                ui.Text(ui.bold, items[i], ScreenW - 464, y + 12, on ? Cream : Ink);
                ui.Text(ui.small, details[i], ScreenW - 464, y + 44, on ? OnDarkSoft : InkSoft);
            }
            ui.Hint(ScreenW - 484, ScreenH - 64, "A", "Selecionar", true);
            ui.Hint(ScreenW - 300, ScreenH - 64, "B", "Voltar ao jogo", true);
        }
        SDL_RenderPresent(ui.renderer);

        if (screenshot_path != nullptr && now - started_ms > 6000) {
            SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, ScreenW, ScreenH, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ui.renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
            SDL_SaveBMP(shot, screenshot_path);
            SDL_FreeSurface(shot);
            std::printf("frames shown: %u, host: %s, screen: %d\n", video.frames_shown,
                        host.name.c_str(), static_cast<int>(screen));
            running = false;
        }
#ifndef __SWITCH__
        SDL_Delay(1);
#endif
        (void)Danger;
        (void)Charcoal2;
    }

    if (host.ip != 0) {
        net.Send(host.ip, Type::Stop);
    }
    SDL_Quit();
#ifdef __SWITCH__
    romfsExit();
    socketExit();
#endif
    return 0;
}
