// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "charis_stream/crypto.h"
#include "charis_stream/net.h"
#include "charis_stream/protocol.h"

namespace CharisStream {

// Encoded H.264 frames (Annex B, SPS/PPS repeated on keyframes).
class VideoSource {
public:
    virtual ~VideoSource() = default;
    virtual bool Open(int width, int height, int fps, int bitrate_kbps, std::string& error) = 0;
    // Waits for the next frame. Returns false when the source failed.
    virtual bool Next(std::vector<uint8_t>& frame, bool& keyframe, bool want_keyframe) = 0;
    virtual void SetBitrate(int /*bitrate_kbps*/) {}
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

struct HostConfig {
    std::string name;                 // shown on the Switch
    Key32 key{};                      // the account's stream key
    std::array<uint8_t, 8> host_id{}; // stable id of this PC
    uint16_t port = HostPort;
};

struct HostCallbacks {
    std::function<std::unique_ptr<VideoSource>()> make_video;
    std::function<std::unique_ptr<AudioSource>()> make_audio;
    // All called on the host's own threads
    std::function<void(const InputState&)> on_input;
    std::function<void(bool connected, const std::string& client_name)> on_session;
    std::function<void(const std::string& message)> on_error;
    std::function<void(Type command)> on_command; // Type::GoHome
};

class Host {
public:
    Host(HostConfig config, HostCallbacks callbacks);
    ~Host();

    bool Start(std::string& error);
    void Stop();
    bool IsStreaming() const {
        return session_active;
    }
    uint16_t Port() const {
        return socket.Port();
    }

    // Where a Switch can reach this PC: on the home network, and from the internet (the latter
    // is known a moment after Start, once a STUN server answered; not valid until then).
    std::vector<Address> LocalAddresses() const;
    Address PublicAddress() const;
    // A Switch asked to connect from these addresses: send to them for a while so this PC's
    // router lets its packets in.
    void Punch(const std::vector<Address>& targets);

private:
    struct Session;
    struct Pending;

    void ControlLoop();
    void HandleClear(const Header& header, const uint8_t* body, long size, const Address& from);
    void HandleSealed(const uint8_t* packet, size_t size, const Header& header,
                      const Address& from);
    void BeginSession(const Pending& pending, const StartRequest& request);
    void EndSession(bool notify_client);
    void VideoLoop(std::shared_ptr<Session> session);
    void AudioLoop(std::shared_ptr<Session> session);
    void SendFrame(Session& session, std::vector<uint8_t>& frame, uint32_t number, bool keyframe,
                   int64_t frame_interval_us, std::vector<uint8_t>& plain,
                   std::vector<uint8_t>& sealed, std::vector<uint8_t>& parity);
    void SendSealed(Session& session, Type type, const uint8_t* payload, size_t size,
                    uint8_t* scratch);

    HostConfig config;
    Tag16 tag{};
    HostCallbacks callbacks;
    UdpSocket socket;
    std::thread control_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> session_active{false};

    mutable std::mutex mutex; // session, pending, public_address, punch_targets
    std::shared_ptr<Session> session;
    std::vector<Pending> pending;
    Address public_address;
    std::vector<std::pair<Address, int64_t>> punch_targets; // address, stop sending at
};

} // namespace CharisStream
