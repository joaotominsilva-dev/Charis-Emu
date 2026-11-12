// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include "charis_stream/sources.h"

#ifndef _WIN32

namespace CharisStream {
std::unique_ptr<VideoSource> MakeScreenVideoSource() {
    return nullptr;
}
std::unique_ptr<AudioSource> MakeLoopbackAudioSource() {
    return nullptr;
}
} // namespace CharisStream

#else

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <opus.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace CharisStream {

namespace {

class NvencScreenSource final : public VideoSource {
public:
    ~NvencScreenSource() override {
        Close();
    }

    bool Open(int width, int height, int fps, int bitrate_kbps, std::string& error) override {
        graph = avfilter_graph_alloc();
        char args[256];
        // The pointer is drawn in: the Switch drives it in desktop mode (games hide it anyway)
        std::snprintf(args, sizeof(args), "output_idx=0:framerate=%d:draw_mouse=1:dup_frames=1",
                      fps);
        frame_rate = fps;
        AVFilterContext* capture = nullptr;
        AVFilterContext* scale = nullptr;
        if (avfilter_graph_create_filter(&capture, avfilter_get_by_name("ddagrab"), "capture",
                                         args, nullptr, graph) < 0) {
            error = "Não foi possível capturar a tela do PC.";
            return false;
        }
        std::snprintf(args, sizeof(args), "width=%d:height=%d:format=nv12", width, height);
        if (avfilter_graph_create_filter(&scale, avfilter_get_by_name("scale_d3d11"), "scale",
                                         args, nullptr, graph) < 0 ||
            avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "sink",
                                         nullptr, nullptr, graph) < 0 ||
            avfilter_link(capture, 0, scale, 0) < 0 || avfilter_link(scale, 0, sink, 0) < 0 ||
            avfilter_graph_config(graph, nullptr) < 0) {
            error = "Não foi possível preparar a imagem para o Switch.";
            return false;
        }

        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (codec == nullptr) {
            error = "O codificador NVENC não está disponível.";
            return false;
        }
        encoder = avcodec_alloc_context3(codec);
        encoder->width = width;
        encoder->height = height;
        encoder->time_base = AVRational{1, fps};
        encoder->framerate = AVRational{fps, 1};
        encoder->pix_fmt = AV_PIX_FMT_D3D11;
        encoder->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(sink));
        encoder->gop_size = fps * 10;
        encoder->max_b_frames = 0;
        encoder->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
        encoder->rc_max_rate = encoder->bit_rate;
        // About two frames of buffer keeps frame sizes even, which Wi-Fi likes
        encoder->rc_buffer_size = static_cast<int>(encoder->bit_rate / fps * 2);
        encoder->flags |= AV_CODEC_FLAG_LOW_DELAY;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "p1", 0);
        av_dict_set(&options, "tune", "ull", 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "rc", "cbr", 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        av_dict_set(&options, "profile", "high", 0);
        const int opened = avcodec_open2(encoder, codec, &options);
        av_dict_free(&options);
        if (opened < 0) {
            error = "A placa de vídeo recusou o codificador NVENC. Atualize o driver da NVIDIA.";
            return false;
        }
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        return true;
    }

    // Called by the host between frames, on the video thread
    void SetBitrate(int bitrate_kbps) override {
        if (encoder == nullptr || bitrate_kbps <= 0) {
            return;
        }
        // NVENC changes the rate in place on the next frame (and starts it as a keyframe)
        encoder->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
        encoder->rc_max_rate = encoder->bit_rate;
        encoder->rc_buffer_size = static_cast<int>(encoder->bit_rate / frame_rate * 2);
    }

    bool Next(std::vector<uint8_t>& out, bool& keyframe, bool want_keyframe) override {
        out.clear();
        while (true) {
            const int received = avcodec_receive_packet(encoder, packet);
            if (received == 0) {
                out.assign(packet->data, packet->data + packet->size);
                keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                av_packet_unref(packet);
                return true;
            }
            if (received != AVERROR(EAGAIN)) {
                return false;
            }
            if (av_buffersink_get_frame(sink, frame) < 0) {
                return false;
            }
            frame->pts = next_pts++;
            frame->pict_type = want_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            want_keyframe = false;
            const int sent = avcodec_send_frame(encoder, frame);
            av_frame_unref(frame);
            if (sent < 0) {
                return false;
            }
        }
    }

    void Close() override {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&encoder);
        avfilter_graph_free(&graph);
        sink = nullptr;
    }

private:
    AVFilterGraph* graph = nullptr;
    AVFilterContext* sink = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int64_t next_pts = 0;
    int frame_rate = 60;
};

constexpr int OpusRate = 48000;
constexpr int OpusChannels = 2;
constexpr int OpusFrame = 480; // 10 ms

class LoopbackOpusSource final : public AudioSource {
public:
    ~LoopbackOpusSource() override {
        Close();
    }

    bool Open(std::string& error) override {
        com_ready = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator)))) {
            error = "Não foi possível acessar o som do PC.";
            return false;
        }
        const HRESULT got = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (FAILED(got) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(&client))) ||
            FAILED(client->GetMixFormat(&format))) {
            error = "Não foi possível acessar o som do PC.";
            return false;
        }
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                      200000, 0, format, nullptr)) ||
            FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(&capture))) ||
            FAILED(client->Start())) {
            error = "Não foi possível capturar o som do PC.";
            return false;
        }
        is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->wBitsPerSample == 32);
        int opus_error = 0;
        encoder = opus_encoder_create(OpusRate, OpusChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                      &opus_error);
        if (encoder == nullptr) {
            error = "Não foi possível preparar o som.";
            return false;
        }
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
        return true;
    }

    bool Next(std::vector<uint8_t>& out) override {
        out.clear();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (pending.size() < OpusFrame * OpusChannels) {
            if (!Capture()) {
                return false;
            }
            if (pending.size() >= OpusFrame * OpusChannels ||
                std::chrono::steady_clock::now() > deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (pending.size() < OpusFrame * OpusChannels) {
            return true;
        }
        out.resize(1500);
        const int bytes = opus_encode_float(encoder, pending.data(), OpusFrame, out.data(),
                                            static_cast<opus_int32>(out.size()));
        pending.erase(pending.begin(), pending.begin() + OpusFrame * OpusChannels);
        if (bytes < 0) {
            out.clear();
            return true;
        }
        out.resize(static_cast<size_t>(bytes));
        return true;
    }

    void Close() override {
        if (client != nullptr) {
            client->Stop();
        }
        if (capture != nullptr) {
            capture->Release();
            capture = nullptr;
        }
        if (client != nullptr) {
            client->Release();
            client = nullptr;
        }
        if (device != nullptr) {
            device->Release();
            device = nullptr;
        }
        if (format != nullptr) {
            CoTaskMemFree(format);
            format = nullptr;
        }
        if (encoder != nullptr) {
            opus_encoder_destroy(encoder);
            encoder = nullptr;
        }
        if (com_ready) {
            CoUninitialize();
            com_ready = false;
        }
    }

private:
    // Pulls what WASAPI has into `pending` as 48 kHz stereo float.
    bool Capture() {
        UINT32 frames_in_packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&frames_in_packet)) && frames_in_packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                return false;
            }
            const int channels = format->nChannels;
            const double step = static_cast<double>(format->nSamplesPerSec) / OpusRate;
            for (UINT32 i = 0; i < frames; ++i) {
                float left = 0.0f;
                float right = 0.0f;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    if (is_float) {
                        const float* samples = reinterpret_cast<const float*>(data);
                        left = samples[i * channels];
                        right = samples[i * channels + (channels > 1 ? 1 : 0)];
                    } else {
                        const int16_t* samples = reinterpret_cast<const int16_t*>(data);
                        left = samples[i * channels] / 32768.0f;
                        right = samples[i * channels + (channels > 1 ? 1 : 0)] / 32768.0f;
                    }
                }
                // Simple resampling when the PC does not mix at 48 kHz
                resample_position += 1.0;
                while (resample_position >= step) {
                    resample_position -= step;
                    pending.push_back(left);
                    pending.push_back(right);
                }
            }
            capture->ReleaseBuffer(frames);
        }
        return true;
    }

    bool com_ready = false;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    bool is_float = true;
    double resample_position = 0.0;
    OpusEncoder* encoder = nullptr;
    std::vector<float> pending;
};

} // namespace

std::unique_ptr<VideoSource> MakeScreenVideoSource() {
    return std::make_unique<NvencScreenSource>();
}

std::unique_ptr<AudioSource> MakeLoopbackAudioSource() {
    return std::make_unique<LoopbackOpusSource>();
}

} // namespace CharisStream

#endif
