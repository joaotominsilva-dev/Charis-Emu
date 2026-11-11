// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "charis_stream/crypto.h"
#include "charis_stream/net.h"
#include "charis_stream/protocol.h"

namespace CharisStream {

struct DiscoveredHost {
    Address address;
    std::array<uint8_t, 8> id{};
    std::string name;
    int64_t seen_ms = 0;
};

struct ClientCallbacks {
    // Both are called on the client's network thread, in order
    std::function<void(const uint8_t* frame, size_t size, bool keyframe)> on_video;
    std::function<void(const uint8_t* opus, size_t size)> on_audio;
};

struct ClientStats {
    uint32_t frames = 0;         // delivered
    uint32_t lost_frames = 0;    // could not be rebuilt
    uint32_t rebuilt_pieces = 0; // pieces recovered from parity
    uint32_t bitrate_kbps = 0;   // what the PC is asked to send now
    uint32_t parity_percent = 0;
    float loss_percent = 0;      // packets lost on the way, last second
};

// The Switch side of Charis Stream, without the screen: finds PCs of the account on the home
// network, connects (trying every address it is given at once), receives and reassembles the
// video and audio, and sends the input back.
class Client {
public:
    enum class State { Idle, Connecting, Streaming, Failed };

    Client(const Key32& key, std::string device_name, ClientCallbacks callbacks);
    ~Client();

    bool Open(std::string& error);
    void Close();

    void SetDiscovery(bool enabled);
    std::vector<DiscoveredHost> Discovered() const;
    Address LocalAddress() const;
    Address PublicAddress() const; // valid once a STUN server answered

    void Connect(const std::vector<Address>& candidates, const StartRequest& settings);
    void AddCandidates(const std::vector<Address>& candidates);
    void Disconnect();
    State GetState() const {
        return state;
    }
    std::string FailureReason() const;
    ClientStats Stats() const;

    void SendInput(InputState input); // the sequence number is filled in here
    void SendCommand(Type type);
    void RequestBitrate(uint32_t bitrate_kbps); // new ceiling chosen by the user

private:
    struct Block {
        uint32_t first = 0;      // first piece of the frame in this block
        uint8_t data_count = 0;
        uint8_t parity_count = 0;
        uint8_t missing = 0;     // data pieces not here yet
        uint8_t parity_have = 0;
        size_t length = 0;
        bool done = false;
        std::vector<uint8_t> parity;
        std::vector<uint8_t> have; // data_count + parity_count flags
    };
    struct Assembly {
        uint32_t frame_size = 0;
        bool keyframe = false;
        bool complete = false;
        uint32_t blocks_left = 0;
        std::vector<uint8_t> data;
        std::vector<Block> blocks;
    };

    void Loop();
    void HandleClear(const Header& header, const uint8_t* body, long size, const Address& from);
    void HandleSealed(const uint8_t* packet, size_t size, const Header& header);
    void HandleVideo(const uint8_t* body, size_t size);
    void Recover(Assembly& assembly, Block& block);
    void Adapt(int64_t now);
    void Deliver();
    bool AcceptCounter(uint64_t counter);
    void NoteLoss();
    void Fail(const std::string& reason);
    void SendSealed(Type type, const uint8_t* payload, size_t size);
    void ResetStream();

    Key32 key;
    Tag16 tag;
    std::string device_name;
    ClientCallbacks callbacks;
    UdpSocket socket;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<State> state{State::Idle};
    std::atomic<bool> discovery{false};
    std::atomic<uint32_t> generation{0}; // bumped on connect/disconnect: the loop starts clean

    mutable std::mutex mutex; // everything below up to send_mutex
    std::vector<DiscoveredHost> discovered;
    std::vector<Address> candidates;
    Address public_address;
    std::string failure;
    ClientStats stats;
    StartRequest settings{};
    uint8_t client_nonce[16]{};
    int64_t connect_started_ms = 0;
    bool have_keys = false;

    mutable std::mutex send_mutex; // peer, keys and counter for sending
    Address peer;
    SessionKeys keys;
    uint64_t send_counter = 1;
    uint32_t input_sequence = 0;

    // Network thread only
    std::map<uint32_t, Assembly> assemblies;
    uint32_t next_frame = 0;
    bool waiting_keyframe = true;
    bool got_first_frame = false;
    int64_t last_keyframe_request_ms = 0;
    int64_t last_packet_ms = 0;
    uint32_t last_audio_sequence = 0;
    bool have_audio = false;
    std::deque<int64_t> loss_events; // frames that could not be rebuilt
    uint32_t current_bitrate = 0;
    uint32_t applied_target = 0;
    std::atomic<uint32_t> target_bitrate{0};
    uint32_t parity_percent = 20;
    int64_t last_decrease_ms = 0;
    int64_t last_increase_ms = 0;
    int64_t last_bitrate_sent_ms = 0;
    int clean_seconds = 0;
    uint64_t window_start_counter = 0;
    uint64_t window_received = 0;
    uint64_t highest_counter = 0;
    uint64_t seen_window = 0;
};

} // namespace CharisStream
