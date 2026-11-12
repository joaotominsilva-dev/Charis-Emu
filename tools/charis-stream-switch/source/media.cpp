// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>

#include <opus/opus.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include "media.h"

namespace {

constexpr size_t MaxQueued = 4; // frames waiting for the decoder before it is clearly behind

#ifdef __SWITCH__
enum AVPixelFormat PickFormat(AVCodecContext*, const enum AVPixelFormat* formats) {
    for (const enum AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_NVTEGRA) {
            return *f;
        }
    }
    return formats[0];
}
#endif

} // namespace

VideoDecoder::~VideoDecoder() {
    Close();
}

bool VideoDecoder::OpenCodec(bool use_hardware) {
    const AVCodec* decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (decoder == nullptr) {
        SetStatus("Este app não tem decodificador H.264");
        return false;
    }
    codec = avcodec_alloc_context3(decoder);
    codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec->flags2 |= AV_CODEC_FLAG2_FAST;
    hardware = false;
#ifdef __SWITCH__
    if (use_hardware) {
        // The Switch's own video chip does the decoding
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0) == 0) {
            codec->hw_device_ctx = device;
            codec->get_format = PickFormat;
            hardware = true;
        }
    }
    if (!hardware) {
        // The processor does it: a little more delay, but it works everywhere
        codec->thread_count = 3;
        codec->thread_type = FF_THREAD_FRAME;
    }
#else
    (void)use_hardware;
    codec->thread_count = 2;
    codec->thread_type = FF_THREAD_SLICE;
#endif
    if (avcodec_open2(codec, decoder, nullptr) < 0) {
        avcodec_free_context(&codec);
        if (hardware) {
            return OpenCodec(false);
        }
        SetStatus("O decodificador de vídeo não abriu");
        return false;
    }
    hardware_failures = 0;
    SetStatus(hardware ? "Vídeo pelo chip de vídeo do Switch" : "Vídeo pelo processador");
    return true;
}

void VideoDecoder::SetStatus(const std::string& text) {
    std::scoped_lock lock{status_mutex};
    status = text;
}

std::string VideoDecoder::Status() const {
    std::scoped_lock lock{status_mutex};
    return status;
}

bool VideoDecoder::Open() {
    if (!OpenCodec(true)) {
        return false;
    }
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    converted = av_frame_alloc();
    newest = av_frame_alloc();
    showing = av_frame_alloc();
    running = true;
    thread = std::thread([this] { Loop(); });
    return true;
}

void VideoDecoder::Close() {
    if (running.exchange(false)) {
        {
            std::scoped_lock lock{queue_mutex};
            queue_ready.notify_all();
        }
        thread.join();
    }
    if (texture != nullptr) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    av_frame_free(&showing);
    av_frame_free(&newest);
    av_frame_free(&converted);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
}

void VideoDecoder::Submit(const uint8_t* data, size_t size, bool keyframe) {
    std::scoped_lock lock{queue_mutex};
    if (queue.size() >= MaxQueued) {
        // Too far behind to catch up smoothly: start again from the next keyframe
        queue.clear();
        skip_to_keyframe = true;
        wants_keyframe = true;
    }
    if (skip_to_keyframe && !keyframe) {
        return;
    }
    skip_to_keyframe = false;
    queue.push_back(Encoded{std::vector<uint8_t>(data, data + size), keyframe});
    queue_ready.notify_one();
}

void VideoDecoder::Reset() {
    {
        std::scoped_lock lock{queue_mutex};
        queue.clear();
        skip_to_keyframe = true;
    }
    reset_requested = true;
    std::scoped_lock lock{picture_mutex};
    fresh = false;
    has_picture = false;
}

void VideoDecoder::Loop() {
    int submitted_without_picture = 0;
    while (running) {
        Encoded next;
        {
            std::unique_lock lock{queue_mutex};
            queue_ready.wait(lock, [this] { return !running || !queue.empty(); });
            if (!running) {
                break;
            }
            next = std::move(queue.front());
            queue.pop_front();
        }
        if (reset_requested.exchange(false)) {
            avcodec_flush_buffers(codec);
        }
        const auto started = std::chrono::steady_clock::now();
        av_packet_unref(packet);
        if (av_new_packet(packet, static_cast<int>(next.data.size())) < 0) {
            continue;
        }
        std::memcpy(packet->data, next.data.data(), next.data.size());
        if (next.keyframe) {
            packet->flags |= AV_PKT_FLAG_KEY;
        }
        bool failed = avcodec_send_packet(codec, packet) < 0;
        bool got_picture = false;
        if (failed) {
            wants_keyframe = true;
            std::scoped_lock lock{queue_mutex};
            skip_to_keyframe = true;
        }
        while (avcodec_receive_frame(codec, frame) == 0) {
            AVFrame* picture = frame;
            if (frame->hw_frames_ctx != nullptr) {
                av_frame_unref(converted);
                if (av_hwframe_transfer_data(converted, frame, 0) < 0) {
                    av_frame_unref(frame);
                    failed = true;
                    continue;
                }
                picture = converted;
            }
            got_picture = true;
            if (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                wants_keyframe = true;
            }
            {
                std::scoped_lock lock{picture_mutex};
                av_frame_unref(newest);
                av_frame_move_ref(newest, picture);
                fresh = true;
            }
            if (picture != frame) {
                av_frame_unref(frame);
            }
            ++decoded;
        }
        // The video chip keeps failing: switch to the processor and start from a keyframe
        if (hardware) {
            hardware_failures = got_picture ? 0 : hardware_failures + (failed ? 1 : 0);
            submitted_without_picture = decoded == 0 ? submitted_without_picture + 1 : 0;
            if (hardware_failures >= 15 || (decoded == 0 && hardware_failures >= 5) ||
                submitted_without_picture > 150) {
                submitted_without_picture = 0;
                avcodec_free_context(&codec);
                if (!OpenCodec(false)) {
                    break;
                }
                std::scoped_lock lock{queue_mutex};
                queue.clear();
                skip_to_keyframe = true;
                wants_keyframe = true;
                continue;
            }
        }
        const float ms = std::chrono::duration<float, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count();
        decode_ms = decode_ms * 0.9f + ms * 0.1f;
    }
}

bool VideoDecoder::Present(SDL_Renderer* renderer) {
    {
        std::scoped_lock lock{picture_mutex};
        if (fresh) {
            av_frame_unref(showing);
            av_frame_move_ref(showing, newest);
            fresh = false;
        } else {
            return has_picture;
        }
    }
    if (Upload(renderer, showing)) {
        has_picture = true;
    }
    return has_picture;
}

bool VideoDecoder::Upload(SDL_Renderer* renderer, AVFrame* picture) {
    const bool nv12 = picture->format == AV_PIX_FMT_NV12;
    if (!nv12 && picture->format != AV_PIX_FMT_YUV420P && picture->format != AV_PIX_FMT_YUVJ420P) {
        return false;
    }
    const bool as_nv12 = nv12 && nv12_textures;
    const int format = as_nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV;
    if (texture == nullptr || texture_format != format || texture_w != picture->width ||
        texture_h != picture->height) {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(renderer, format, SDL_TEXTUREACCESS_STREAMING, picture->width,
                                    picture->height);
        if (texture == nullptr && as_nv12) {
            nv12_textures = false; // this renderer cannot: copy to planar and try again
            return Upload(renderer, picture);
        }
        if (texture == nullptr) {
            SetStatus(std::string("A tela não aceitou o vídeo: ") + SDL_GetError());
            return false;
        }
        texture_format = format;
        texture_w = picture->width;
        texture_h = picture->height;
    }
    if (nv12 && !as_nv12) {
        // NV12 to planar: split the interleaved colour plane in two
        const int w = picture->width / 2;
        const int h = picture->height / 2;
        planar_u.resize(static_cast<size_t>(w) * h);
        planar_v.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = picture->data[1] + y * picture->linesize[1];
            uint8_t* u = planar_u.data() + y * w;
            uint8_t* v = planar_v.data() + y * w;
            for (int x = 0; x < w; ++x) {
                u[x] = row[x * 2];
                v[x] = row[x * 2 + 1];
            }
        }
        SDL_UpdateYUVTexture(texture, nullptr, picture->data[0], picture->linesize[0],
                             planar_u.data(), w, planar_v.data(), w);
    } else if (nv12) {
        SDL_UpdateNVTexture(texture, nullptr, picture->data[0], picture->linesize[0],
                            picture->data[1], picture->linesize[1]);
    } else {
        SDL_UpdateYUVTexture(texture, nullptr, picture->data[0], picture->linesize[0],
                             picture->data[1], picture->linesize[1], picture->data[2],
                             picture->linesize[2]);
    }
    return true;
}

AudioPlayer::~AudioPlayer() {
    Close();
}

bool AudioPlayer::Open() {
    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 480;
    device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    int error = 0;
    decoder = opus_decoder_create(48000, 2, &error);
    if (device != 0) {
        SDL_PauseAudioDevice(device, 0);
    }
    return device != 0 && decoder != nullptr;
}

void AudioPlayer::Close() {
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
    if (decoder != nullptr) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }
}

void AudioPlayer::Reset() {
    if (device != 0) {
        SDL_ClearQueuedAudio(device);
    }
    primed = false;
}

void AudioPlayer::Play(const uint8_t* data, size_t size) {
    if (device == 0 || decoder == nullptr) {
        return;
    }
    const int samples =
        opus_decode(decoder, data, static_cast<opus_int32>(size), pcm.data(), 5760, 0);
    if (samples <= 0) {
        return;
    }
    constexpr Uint32 BytesPerMs = 48000 * 2 * 2 / 1000;
    const Uint32 queued = SDL_GetQueuedAudioSize(device);
    // Keep 30-60 ms queued: enough to ride out Wi-Fi bumps, little enough to stay in sync
    if (queued > 100 * BytesPerMs) {
        SDL_ClearQueuedAudio(device);
        primed = false;
    }
    if (!primed) {
        // A little silence first, so the first bump does not cut the sound
        std::vector<int16_t> silence(30 * BytesPerMs / sizeof(int16_t), 0);
        SDL_QueueAudio(device, silence.data(), static_cast<Uint32>(silence.size() * 2));
        primed = true;
    }
    SDL_QueueAudio(device, pcm.data(), static_cast<Uint32>(samples * 2 * sizeof(int16_t)));
}
