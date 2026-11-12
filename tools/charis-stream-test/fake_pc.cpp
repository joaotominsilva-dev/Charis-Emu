// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for Charis Switch on the PC, for testing the Switch app on Linux: logs in to the
// account (CHARIS_USER / CHARIS_PASSWORD, server from CHARIS_ACCOUNT_URL), stays listed like
// the real PC, streams a moving test picture as real H.264 plus a tone, and prints the input
// it receives.

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opus/opus.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "charis_stream/account.h"
#include "charis_stream/crypto.h"
#include "charis_stream/host.h"
#include "charis_stream/presence.h"

using namespace CharisStream;

namespace {

std::atomic<bool> stop{false};

class CurlHttp final : public Http {
public:
    bool Post(const std::string& url, const std::string& body, std::string& response) override {
        const std::string command =
            "curl -s -m 10 -X POST --data-binary '" + body + "' '" + url + "'";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return false;
        }
        response.clear();
        char chunk[512];
        size_t got;
        while ((got = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
            response.append(chunk, got);
        }
        return pclose(pipe) == 0 && !response.empty();
    }
};

class PatternVideo final : public VideoSource {
public:
    bool Open(int width, int height, int fps, int kbps, std::string& error) override {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (codec == nullptr) {
            error = "libx264 not available";
            return false;
        }
        encoder = avcodec_alloc_context3(codec);
        encoder->width = width;
        encoder->height = height;
        encoder->time_base = AVRational{1, fps};
        encoder->framerate = AVRational{fps, 1};
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->gop_size = fps * 10;
        encoder->max_b_frames = 0;
        encoder->bit_rate = static_cast<int64_t>(kbps) * 1000;
        av_opt_set(encoder->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
        if (avcodec_open2(encoder, codec, nullptr) < 0) {
            error = "encoder did not open";
            return false;
        }
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;
        av_frame_get_buffer(frame, 0);
        packet = av_packet_alloc();
        interval = std::chrono::microseconds(1000000 / fps);
        next = std::chrono::steady_clock::now();
        std::printf("[pc] video %dx%d %d fps %d kbps\n", width, height, fps, kbps);
        return true;
    }

    bool Next(std::vector<uint8_t>& out, bool& keyframe, bool want_keyframe) override {
        std::this_thread::sleep_until(next);
        next += interval;
        av_frame_make_writable(frame);
        // Cream background, a graphite bar sweeping across, and a sage square in the middle
        const int bar = static_cast<int>((count * 12) % frame->width);
        for (int y = 0; y < frame->height; ++y) {
            uint8_t* row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < frame->width; ++x) {
                const bool on_bar = x >= bar && x < bar + 80;
                const bool middle = std::abs(x - frame->width / 2) < 120 &&
                                    std::abs(y - frame->height / 2) < 120;
                row[x] = on_bar ? 35 : (middle ? 150 : 235);
            }
        }
        for (int plane = 1; plane < 3; ++plane) {
            for (int y = 0; y < frame->height / 2; ++y) {
                std::memset(frame->data[plane] + y * frame->linesize[plane], 128,
                            static_cast<size_t>(frame->width / 2));
            }
        }
        frame->pts = count++;
        frame->pict_type = want_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(encoder, frame) < 0) {
            return false;
        }
        out.clear();
        while (avcodec_receive_packet(encoder, packet) == 0) {
            out.insert(out.end(), packet->data, packet->data + packet->size);
            keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            av_packet_unref(packet);
        }
        return true;
    }

    void SetBitrate(int kbps) override {
        std::printf("[pc] bitrate -> %d kbps\n", kbps);
    }

    void Close() override {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&encoder);
    }

private:
    AVCodecContext* encoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int64_t count = 0;
    std::chrono::microseconds interval{16666};
    std::chrono::steady_clock::time_point next;
};

class ToneAudio final : public AudioSource {
public:
    bool Open(std::string&) override {
        int error = 0;
        encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        next = std::chrono::steady_clock::now();
        return encoder != nullptr;
    }
    bool Next(std::vector<uint8_t>& out) override {
        std::this_thread::sleep_until(next);
        next += std::chrono::milliseconds(10);
        float pcm[480 * 2];
        for (int i = 0; i < 480; ++i) {
            const float v = 0.1f * std::sin(phase);
            phase += 2.0f * 3.14159265f * 440.0f / 48000.0f;
            pcm[i * 2] = pcm[i * 2 + 1] = v;
        }
        out.resize(1500);
        const int bytes = opus_encode_float(encoder, pcm, 480, out.data(), 1500);
        out.resize(bytes > 0 ? static_cast<size_t>(bytes) : 0);
        return true;
    }
    void Close() override {
        if (encoder != nullptr) {
            opus_encoder_destroy(encoder);
            encoder = nullptr;
        }
    }

private:
    OpusEncoder* encoder = nullptr;
    float phase = 0;
    std::chrono::steady_clock::time_point next;
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, [](int) { stop = true; });
    std::signal(SIGTERM, [](int) { stop = true; });
    const char* user = std::getenv("CHARIS_USER");
    const char* password = std::getenv("CHARIS_PASSWORD");
    if (user == nullptr || password == nullptr) {
        std::printf("set CHARIS_USER and CHARIS_PASSWORD\n");
        return 1;
    }
    const AccountKeys keys = DeriveAccountKeys(user, password);
    CurlHttp http;
    Account account{http};
    std::string token;
    std::string error = account.Login(user, keys.auth_hex, token, "pc", "PC de teste");
    bool create = false;
    if (!error.empty() && !account.Unavailable()) {
        error = account.Register(user, keys.auth_hex, token, "pc", "PC de teste");
    }
    if (!error.empty() && account.Unavailable()) {
        std::printf("[pc] no account server (%s): home network only\n", error.c_str());
        create = true;
    } else if (!error.empty()) {
        std::printf("[pc] login failed: %s\n", error.c_str());
        return 1;
    } else {
        std::printf("[pc] logged in as %s\n", user);
    }

    HostConfig config;
    config.name = "PC de teste";
    config.key = keys.stream_key;
    config.host_id = {1, 2, 3, 4, 5, 6, 7, 8};
    HostCallbacks callbacks;
    callbacks.make_video = [] { return std::make_unique<PatternVideo>(); };
    callbacks.make_audio = [] { return std::make_unique<ToneAudio>(); };
    InputState last{};
    uint32_t last_key_sequence = 0;
    callbacks.on_input = [&](const InputState& state) {
        if (state.mode != last.mode || state.buttons != last.buttons ||
            state.touch_count != last.touch_count || state.mouse_buttons != last.mouse_buttons ||
            state.left_x != last.left_x || state.wheel != last.wheel ||
            (state.mode == ModeDesktop &&
             (state.mouse_x != last.mouse_x || state.mouse_y != last.mouse_y))) {
            std::printf("[pc] input mode=%u buttons=0x%llx left_x=%d touches=%u", state.mode,
                        static_cast<unsigned long long>(state.buttons), state.left_x,
                        state.touch_count);
            if (state.touch_count > 0) {
                std::printf(" touch0=%u,%u", state.touches[0].x, state.touches[0].y);
            }
            if (state.mode == ModeDesktop) {
                std::printf(" mouse=%u,%u buttons=%u wheel=%d", state.mouse_x, state.mouse_y,
                            state.mouse_buttons, state.wheel);
            }
            std::printf("\n");
            last = state;
        }
        for (int i = 0; i < state.key_count; ++i) {
            const KeyEvent& event = state.keys[i];
            if (static_cast<int32_t>(event.sequence - last_key_sequence) > 0) {
                std::printf("[pc] key %s %u\n", event.kind == KeyText ? "text" : "special",
                            event.code);
                last_key_sequence = event.sequence;
            }
        }
    };
    int64_t session_start = 0;
    Presence* presence_ptr = nullptr;
    callbacks.on_session = [&](bool connected, const std::string& client) {
        std::printf("[pc] session %s %s\n", connected ? "started with" : "ended with",
                    client.c_str());
        if (connected) {
            session_start = NowMs();
        } else if (presence_ptr != nullptr && !presence_ptr->Token().empty()) {
            const char* game = std::getenv("CHARIS_GAME");
            account.ReportSession(presence_ptr->Token(), "0102030405060708", client, "casa",
                                  static_cast<int>((NowMs() - session_start) / 1000) + 1500,
                                  game ? game : "");
        }
    };
    callbacks.on_command = [](Type command) {
        std::printf("[pc] command %d\n", static_cast<int>(command));
    };
    callbacks.on_error = [](const std::string& message) {
        std::printf("[pc] error %s\n", message.c_str());
    };
    Host host{config, callbacks};
    if (!host.Start(error)) {
        std::printf("[pc] host failed: %s\n", error.c_str());
        return 1;
    }
    Credentials credentials;
    credentials.user = user;
    credentials.auth_hex = keys.auth_hex;
    credentials.token = token;
    credentials.create_account = create;
    if (const char* game = std::getenv("CHARIS_GAME")) {
        std::printf("[pc] playing %s\n", game);
    }
    Presence presence{http, host, credentials, "0102030405060708", "PC de teste",
                      [](const Presence::Status& status) {
                          std::printf("[pc] presence online=%d router=%d %s\n", status.online,
                                      status.router_port, status.message.c_str());
                      }};
    presence_ptr = &presence;
    presence.Start();
    if (const char* game = std::getenv("CHARIS_GAME")) {
        presence.SetGame(game);
    }
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    host.Stop();
    if (const char* game = std::getenv("CHARIS_GAME"); game && !presence.Token().empty()) {
        account.ReportGame(presence.Token(), "0102030405060708", game, "0100abcdef000000", 5400,
                           true);
    }
    presence.Stop();
    std::printf("[pc] stopped\n");
    return 0;
}
