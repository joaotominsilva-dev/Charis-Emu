// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "charis_stream/protocol.h"

namespace CharisStream {

// Encoded H.264 frames (Annex B, SPS/PPS repeated on keyframes).
class VideoSource {
public:
    virtual ~VideoSource() = default;
    virtual bool Open(int width, int height, int fps, int bitrate_kbps, std::string& error) = 0;
    // Waits for the next frame. Returns false when the source failed.
    virtual bool Next(std::vector<uint8_t>& frame, bool& keyframe, bool want_keyframe) = 0;
    virtual void Close() = 0;
};

// Opus packets, 48 kHz stereo.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual bool Open(std::string& error) = 0;
    // Waits up to about 50 ms for the next packet; an empty packet means nothing to send yet.
    virtual bool Next(std::vector<uint8_t>& packet) = 0;
    virtual void Close() = 0;
};

struct HostCallbacks {
    std::function<std::unique_ptr<VideoSource>()> make_video;
    std::function<std::unique_ptr<AudioSource>()> make_audio;
    // Called on the network thread
    std::function<void(const InputState&)> on_input;
    std::function<void(bool connected, const std::string& client)> on_session;
    std::function<void(const std::string& message)> on_error;
    // Requests from the Switch app such as Type::GoHome, on the network thread
    std::function<void(Type command)> on_command;
};

class Host {
public:
    Host(std::string name, HostCallbacks callbacks);
    ~Host();

    bool Start(std::string& error);
    void Stop();
    bool IsStreaming() const {
        return session_active;
    }

private:
    struct Session;

    void ControlLoop();
    void BeginSession(const void* address, int address_size, const StartRequest& request);
    void EndSession(bool notify_client);
    void VideoLoop(std::shared_ptr<Session> session);
    void AudioLoop(std::shared_ptr<Session> session);
    void SendTo(const std::shared_ptr<Session>& session, const uint8_t* data, size_t size);

    std::string name;
    HostCallbacks callbacks;
    intptr_t socket_fd = -1;
    std::thread control_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> session_active{false};
    std::mutex session_mutex;
    std::shared_ptr<Session> session;
};

} // namespace CharisStream
