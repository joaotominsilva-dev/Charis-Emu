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
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
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
#include <libswscale/swscale.h>
}

namespace CharisStream {

namespace {

// ---------- FFmpeg's own messages, so a failure can say what went wrong ----------

std::mutex ffmpeg_log_mutex;
std::deque<std::string> ffmpeg_log_lines;

void FfmpegLog(void* context, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING) {
        return;
    }
    char line[512];
    int prefix = 0; // no "[name @ 0x...]": the name alone reads better on the Switch
    av_log_format_line(context, level, format, args, line, sizeof(line), &prefix);
    std::string text = line;
    if (context != nullptr) {
        if (const AVClass* av_class = *static_cast<AVClass**>(context)) {
            text = std::string(av_class->item_name(context)) + ": " + text;
        }
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    std::scoped_lock lock{ffmpeg_log_mutex};
    ffmpeg_log_lines.push_back(text);
    while (ffmpeg_log_lines.size() > 6) {
        ffmpeg_log_lines.pop_front();
    }
}

void ClearFfmpegLog() {
    std::scoped_lock lock{ffmpeg_log_mutex};
    ffmpeg_log_lines.clear();
}

std::string FfmpegDetails() {
    std::scoped_lock lock{ffmpeg_log_mutex};
    std::string out;
    const size_t first = ffmpeg_log_lines.size() > 3 ? ffmpeg_log_lines.size() - 3 : 0;
    for (size_t i = first; i < ffmpeg_log_lines.size(); ++i) {
        out += (out.empty() ? "" : " | ") + ffmpeg_log_lines[i];
    }
    return out;
}

std::string WithDetails(const std::string& message, int code = 0) {
    std::string out = message;
    if (code < 0) {
        char text[128] = {};
        av_strerror(code, text, sizeof(text));
        out += std::string(" (") + text + ")";
    }
    const std::string details = FfmpegDetails();
    if (!details.empty()) {
        out += " [" + details + "]";
    }
    return out;
}

// ---------- NVENC ----------

class NvencEncoder {
public:
    ~NvencEncoder() {
        Close();
    }

    // hw_frames: D3D11 frames straight from the GPU, or null for NV12 frames in memory
    bool Open(int width, int height, int fps_, int bitrate_kbps, AVBufferRef* hw_frames,
              std::string& error) {
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (codec == nullptr) {
            error = "O codificador NVENC não está neste programa.";
            return false;
        }
        fps = fps_;
        context = avcodec_alloc_context3(codec);
        context->width = width;
        context->height = height;
        context->time_base = AVRational{1, fps};
        context->framerate = AVRational{fps, 1};
        if (hw_frames != nullptr) {
            context->pix_fmt = AV_PIX_FMT_D3D11;
            context->hw_frames_ctx = av_buffer_ref(hw_frames);
        } else {
            context->pix_fmt = AV_PIX_FMT_NV12;
        }
        context->gop_size = fps * 10;
        context->max_b_frames = 0;
        context->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
        context->rc_max_rate = context->bit_rate;
        // About two frames of buffer keeps frame sizes even, which Wi-Fi likes
        context->rc_buffer_size = static_cast<int>(context->bit_rate / fps * 2);
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "preset", "p1", 0);
        av_dict_set(&options, "tune", "ull", 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "rc", "cbr", 0);
        av_dict_set(&options, "delay", "0", 0);
        av_dict_set(&options, "forced-idr", "1", 0);
        av_dict_set(&options, "profile", "high", 0);
        const int opened = avcodec_open2(context, codec, &options);
        av_dict_free(&options);
        if (opened < 0) {
            error = WithDetails("A placa de vídeo recusou o NVENC", opened);
            avcodec_free_context(&context);
            return false;
        }
        packet = av_packet_alloc();
        return true;
    }

    bool Encode(AVFrame* frame, bool want_keyframe, std::vector<uint8_t>& out, bool& keyframe,
                std::string& error) {
        frame->pts = next_pts++;
        frame->pict_type = want_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        const int sent = avcodec_send_frame(context, frame);
        if (sent < 0) {
            error = WithDetails("O NVENC parou de codificar", sent);
            return false;
        }
        out.clear();
        while (true) {
            const int received = avcodec_receive_packet(context, packet);
            if (received == AVERROR(EAGAIN)) {
                return true;
            }
            if (received < 0) {
                error = WithDetails("O NVENC parou de codificar", received);
                return false;
            }
            out.insert(out.end(), packet->data, packet->data + packet->size);
            keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            av_packet_unref(packet);
        }
    }

    void SetBitrate(int bitrate_kbps) {
        if (context == nullptr || bitrate_kbps <= 0) {
            return;
        }
        // NVENC changes the rate in place on the next frame (and starts it as a keyframe)
        context->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
        context->rc_max_rate = context->bit_rate;
        context->rc_buffer_size = static_cast<int>(context->bit_rate / fps * 2);
    }

    void Close() {
        av_packet_free(&packet);
        avcodec_free_context(&context);
    }

private:
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    int fps = 60;
    int64_t next_pts = 0;
};

// ---------- Ways to get the screen ----------

enum class Capture { GpuScale, Download, Gdi };

const char* CaptureName(Capture capture) {
    switch (capture) {
    case Capture::GpuScale:
        return "captura pela placa de vídeo";
    case Capture::Download:
        return "captura com cópia para a memória";
    default:
        return "captura pelo Windows (GDI)";
    }
}

// One way of capturing plus NVENC. GpuScale keeps everything on the GPU (fastest); Download
// copies each picture to memory and scales it on the processor; Gdi asks Windows for the screen
// the old way, which works almost everywhere.
class CaptureSession {
public:
    ~CaptureSession() {
        Close();
    }

    bool Open(Capture mode_, int width_, int height_, int fps_, int bitrate_kbps,
              std::string& error) {
        mode = mode_;
        width = width_;
        height = height_;
        fps = fps_;
        ClearFfmpegLog();
        frame = av_frame_alloc();
        if (mode == Capture::Gdi) {
            if (!OpenGdi(error)) {
                return false;
            }
            return OpenScaled(bitrate_kbps, error);
        }

        graph = avfilter_graph_alloc();
        char args[256];
        // The pointer is drawn in: the Switch drives it in desktop mode (games hide it anyway)
        std::snprintf(args, sizeof(args), "output_idx=0:framerate=%d:draw_mouse=1:dup_frames=1",
                      fps);
        AVFilterContext* capture = nullptr;
        int result = avfilter_graph_create_filter(&capture, avfilter_get_by_name("ddagrab"),
                                                  "capture", args, nullptr, graph);
        if (result < 0) {
            error = WithDetails("A captura da tela (DXGI) não abriu", result);
            return false;
        }
        AVFilterContext* last = capture;
        const auto add = [&](const char* filter, const char* name, const char* filter_args) {
            AVFilterContext* next = nullptr;
            result = avfilter_graph_create_filter(&next, avfilter_get_by_name(filter), name,
                                                  filter_args, nullptr, graph);
            if (result >= 0) {
                result = avfilter_link(last, 0, next, 0);
                last = next;
            }
            return result >= 0;
        };
        bool built = false;
        if (mode == Capture::GpuScale) {
            std::snprintf(args, sizeof(args), "width=%d:height=%d:format=nv12", width, height);
            built = add("scale_d3d11", "scale", args);
        } else {
            built = add("hwdownload", "download", nullptr) && add("format", "bgra", "pix_fmts=bgra");
        }
        if (!built || avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"),
                                                   "sink", nullptr, nullptr, graph) < 0 ||
            avfilter_link(last, 0, sink, 0) < 0) {
            error = WithDetails("Não foi possível montar a captura", result);
            return false;
        }
        result = avfilter_graph_config(graph, nullptr);
        if (result < 0) {
            error = WithDetails("A captura da tela não começou", result);
            return false;
        }
        if (mode == Capture::GpuScale) {
            AVBufferRef* hw_frames = av_buffersink_get_hw_frames_ctx(sink);
            if (hw_frames == nullptr) {
                error = "A captura não entregou imagens da placa de vídeo.";
                return false;
            }
            return encoder.Open(width, height, fps, bitrate_kbps, hw_frames, error);
        }
        return OpenScaled(bitrate_kbps, error);
    }

    bool Next(std::vector<uint8_t>& out, bool& keyframe, bool want_keyframe, std::string& error) {
        if (mode == Capture::Gdi) {
            if (!GrabGdi(error)) {
                return false;
            }
            return ScaleAndEncode(gdi_frame, out, keyframe, want_keyframe, error);
        }
        const int got = av_buffersink_get_frame(sink, frame);
        if (got < 0) {
            error = WithDetails("A captura da tela parou", got);
            return false;
        }
        bool ok = false;
        if (mode == Capture::GpuScale) {
            ok = encoder.Encode(frame, want_keyframe, out, keyframe, error);
        } else {
            ok = ScaleAndEncode(frame, out, keyframe, want_keyframe, error);
        }
        av_frame_unref(frame);
        return ok;
    }

    void SetBitrate(int bitrate_kbps) {
        encoder.SetBitrate(bitrate_kbps);
    }

    void Close() {
        encoder.Close();
        av_frame_free(&frame);
        av_frame_free(&scaled);
        av_frame_free(&gdi_frame);
        sws_freeContext(scaler);
        scaler = nullptr;
        avfilter_graph_free(&graph);
        sink = nullptr;
        if (gdi_memory != nullptr) {
            DeleteDC(gdi_memory);
            gdi_memory = nullptr;
        }
        if (gdi_bitmap != nullptr) {
            DeleteObject(gdi_bitmap);
            gdi_bitmap = nullptr;
        }
        if (gdi_screen != nullptr) {
            ReleaseDC(nullptr, gdi_screen);
            gdi_screen = nullptr;
        }
    }

private:
    bool OpenScaled(int bitrate_kbps, std::string& error) {
        scaled = av_frame_alloc();
        scaled->format = AV_PIX_FMT_NV12;
        scaled->width = width;
        scaled->height = height;
        if (av_frame_get_buffer(scaled, 0) < 0) {
            error = "Sem memória para a imagem.";
            return false;
        }
        return encoder.Open(width, height, fps, bitrate_kbps, nullptr, error);
    }

    bool ScaleAndEncode(AVFrame* source, std::vector<uint8_t>& out, bool& keyframe,
                        bool want_keyframe, std::string& error) {
        scaler = sws_getCachedContext(scaler, source->width, source->height,
                                      static_cast<AVPixelFormat>(source->format), width, height,
                                      AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr,
                                      nullptr);
        if (scaler == nullptr || av_frame_make_writable(scaled) < 0) {
            error = "Não foi possível ajustar o tamanho da imagem.";
            return false;
        }
        sws_scale(scaler, source->data, source->linesize, 0, source->height, scaled->data,
                  scaled->linesize);
        return encoder.Encode(scaled, want_keyframe, out, keyframe, error);
    }

    bool OpenGdi(std::string& error) {
        gdi_screen = GetDC(nullptr);
        gdi_w = GetSystemMetrics(SM_CXSCREEN);
        gdi_h = GetSystemMetrics(SM_CYSCREEN);
        if (gdi_screen == nullptr || gdi_w <= 0 || gdi_h <= 0) {
            error = "O Windows não entregou a tela.";
            return false;
        }
        gdi_memory = CreateCompatibleDC(gdi_screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = gdi_w;
        info.bmiHeader.biHeight = -gdi_h; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        gdi_bitmap = CreateDIBSection(gdi_screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (gdi_memory == nullptr || gdi_bitmap == nullptr || bits == nullptr) {
            error = "Sem memória para copiar a tela.";
            return false;
        }
        SelectObject(gdi_memory, gdi_bitmap);
        gdi_frame = av_frame_alloc();
        gdi_frame->format = AV_PIX_FMT_BGRA;
        gdi_frame->width = gdi_w;
        gdi_frame->height = gdi_h;
        gdi_frame->data[0] = static_cast<uint8_t*>(bits);
        gdi_frame->linesize[0] = gdi_w * 4;
        gdi_next = std::chrono::steady_clock::now();
        return true;
    }

    bool GrabGdi(std::string& error) {
        // GDI has no "new frame" signal: keep the frame rate by the clock
        gdi_next += std::chrono::microseconds(1000000 / fps);
        const auto now = std::chrono::steady_clock::now();
        if (gdi_next < now) {
            gdi_next = now;
        } else {
            std::this_thread::sleep_until(gdi_next);
        }
        if (!BitBlt(gdi_memory, 0, 0, gdi_w, gdi_h, gdi_screen, 0, 0, SRCCOPY | CAPTUREBLT)) {
            error = "O Windows parou de entregar a tela.";
            return false;
        }
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING)) {
            ICONINFO icon{};
            if (GetIconInfo(cursor.hCursor, &icon)) {
                DrawIconEx(gdi_memory, cursor.ptScreenPos.x - static_cast<int>(icon.xHotspot),
                           cursor.ptScreenPos.y - static_cast<int>(icon.yHotspot), cursor.hCursor,
                           0, 0, 0, nullptr, DI_NORMAL);
                if (icon.hbmMask != nullptr) {
                    DeleteObject(icon.hbmMask);
                }
                if (icon.hbmColor != nullptr) {
                    DeleteObject(icon.hbmColor);
                }
            }
        }
        GdiFlush();
        return true;
    }

    Capture mode = Capture::GpuScale;
    int width = 1280;
    int height = 720;
    int fps = 60;
    NvencEncoder encoder;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* sink = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* scaled = nullptr;
    SwsContext* scaler = nullptr;
    // GDI
    HDC gdi_screen = nullptr;
    HDC gdi_memory = nullptr;
    HBITMAP gdi_bitmap = nullptr;
    AVFrame* gdi_frame = nullptr;
    int gdi_w = 0;
    int gdi_h = 0;
    std::chrono::steady_clock::time_point gdi_next;
};

// Tries the ways to capture in order until one works, and moves on (or starts again) when the
// one in use stops, so a driver hiccup or a screen change does not end the stream.
class ScreenSource final : public VideoSource {
public:
    ~ScreenSource() override {
        Close();
    }

    bool Open(int width_, int height_, int fps_, int bitrate_kbps_, std::string& error) override {
        width = width_;
        height = height_;
        fps = fps_;
        bitrate_kbps = bitrate_kbps_;
        std::string reasons;
        for (int index = 0; index < 3; ++index) {
            std::string reason;
            if (OpenMode(static_cast<Capture>(index), reason)) {
                return true;
            }
            reasons += std::string(reasons.empty() ? "" : " / ") +
                       CaptureName(static_cast<Capture>(index)) + ": " + reason;
        }
        error = "O PC não conseguiu mandar a imagem. " + reasons;
        return false;
    }

    bool Next(std::vector<uint8_t>& out, bool& keyframe, bool want_keyframe) override {
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::string error;
            if (session && session->Next(out, keyframe, want_keyframe, error)) {
                failures_in_a_row = 0;
                return true;
            }
            // Stopped: a resolution change or a fullscreen game can do that. Open it again,
            // and after repeated failures try the next way of capturing.
            ++failures_in_a_row;
            const int start = failures_in_a_row >= 3 ? static_cast<int>(current) + 1
                                                     : static_cast<int>(current);
            failures_in_a_row = failures_in_a_row >= 3 ? 0 : failures_in_a_row;
            bool reopened = false;
            for (int index = start; index < 3 && !reopened; ++index) {
                std::string reason;
                reopened = OpenMode(static_cast<Capture>(index), reason);
            }
            if (!reopened) {
                return false;
            }
            want_keyframe = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    void SetBitrate(int kbps) override {
        bitrate_kbps = kbps;
        if (session) {
            session->SetBitrate(kbps);
        }
    }

    void Close() override {
        session.reset();
    }

private:
    bool OpenMode(Capture mode, std::string& reason) {
        session.reset();
        auto next = std::make_unique<CaptureSession>();
        if (!next->Open(mode, width, height, fps, bitrate_kbps, reason)) {
            return false;
        }
        session = std::move(next);
        current = mode;
        return true;
    }

    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrate_kbps = 15000;
    Capture current = Capture::GpuScale;
    int failures_in_a_row = 0;
    std::unique_ptr<CaptureSession> session;
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
    static const bool logging = [] {
        av_log_set_callback(FfmpegLog);
        return true;
    }();
    (void)logging;
    return std::make_unique<ScreenSource>();
}

std::unique_ptr<AudioSource> MakeLoopbackAudioSource() {
    return std::make_unique<LoopbackOpusSource>();
}

} // namespace CharisStream

#endif
