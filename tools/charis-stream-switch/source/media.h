// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Turning what arrives from the PC into picture and sound. The video is decoded on its own
// thread (on the Switch's video chip), so the network and the screen never wait for it; the
// screen always shows the newest picture.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct OpusDecoder;

class VideoDecoder {
public:
    ~VideoDecoder();
    bool Open();
    void Close();

    // Network thread: one whole H.264 frame
    void Submit(const uint8_t* data, size_t size, bool keyframe);
    // Screen thread: puts the newest picture in the texture; true when there is one to show
    bool Present(SDL_Renderer* renderer);
    SDL_Texture* Texture() const {
        return texture;
    }
    // The decoder lost its place (a frame could not be decoded); ask the PC for a keyframe
    bool TakeKeyframeRequest() {
        return wants_keyframe.exchange(false);
    }
    void Reset();

    uint32_t Decoded() const {
        return decoded;
    }
    // How the picture is being decoded, for the screen while it opens and for the log
    std::string Status() const;
    float DecodeMs() const {
        return decode_ms;
    }

private:
    void Loop();
    bool OpenCodec(bool hardware);
    bool Upload(SDL_Renderer* renderer, AVFrame* picture);
    void SetStatus(const std::string& text);

    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* converted = nullptr;

    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex queue_mutex;
    std::condition_variable queue_ready;
    struct Encoded {
        std::vector<uint8_t> data;
        bool keyframe = false;
    };
    std::deque<Encoded> queue;
    bool skip_to_keyframe = true;
    std::atomic<bool> reset_requested{false};

    std::mutex picture_mutex;
    AVFrame* newest = nullptr; // decoded, waiting for the screen
    bool fresh = false;
    AVFrame* showing = nullptr;

    SDL_Texture* texture = nullptr;
    int texture_format = -1;
    int texture_w = 0;
    int texture_h = 0;
    bool has_picture = false;

    bool hardware = false;
    int hardware_failures = 0;
    bool nv12_textures = true; // false when the renderer cannot take NV12: planar copy instead
    std::vector<uint8_t> planar_u, planar_v;
    mutable std::mutex status_mutex;
    std::string status = "Preparando o vídeo";

    std::atomic<bool> wants_keyframe{false};
    std::atomic<uint32_t> decoded{0};
    std::atomic<float> decode_ms{0};
};

class AudioPlayer {
public:
    ~AudioPlayer();
    bool Open();
    void Close();
    // Network thread: one Opus packet (10 ms)
    void Play(const uint8_t* data, size_t size);
    void Reset();

private:
    SDL_AudioDeviceID device = 0;
    OpusDecoder* decoder = nullptr;
    std::vector<int16_t> pcm = std::vector<int16_t>(5760 * 2);
    bool primed = false;
};
