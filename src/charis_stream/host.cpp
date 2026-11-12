// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>

#include "charis_stream/fec.h"
#include "charis_stream/host.h"

namespace CharisStream {

struct Host::Pending {
    Address client;
    uint8_t client_nonce[16]{};
    uint8_t host_nonce[16]{};
    SessionKeys keys;
    std::string name;
    int64_t created_ms = 0;
};

struct Host::Session {
    Address client;
    SessionKeys keys;
    std::string name;
    StartRequest settings{};
    std::atomic<uint64_t> send_counter{1};
    std::atomic<bool> active{true};
    std::atomic<bool> want_keyframe{true};
    std::atomic<int> pending_bitrate{0};
    std::atomic<int> parity_percent{20};
    std::atomic<int64_t> last_seen_ms{0};
    uint64_t last_counter = 0; // highest counter accepted from the client (control thread only)
    std::thread video;
    std::thread audio;
};

namespace {

constexpr int StunRetryMs = 3000;
constexpr int StunRefreshMs = 20000; // keeps the router's mapping alive
constexpr int PunchForMs = 12000;
constexpr int PunchEveryMs = 200;
constexpr int PendingForMs = 10000;
constexpr size_t MaxPending = 8;

// Short waits that do not round up to a whole millisecond tick
void WaitUntil(std::chrono::steady_clock::time_point until) {
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
}

} // namespace

Host::Host(HostConfig config_, HostCallbacks callbacks_)
    : config{std::move(config_)}, tag{TagFromKey(config.key)}, callbacks{std::move(callbacks_)} {}

Host::~Host() {
    Stop();
}

bool Host::Start(std::string& error) {
    if (running) {
        return true;
    }
    if (!socket.Open(config.port, error)) {
        error = "A porta " + std::to_string(config.port) +
                " já está em uso. Feche outro Charis Switch aberto e tente de novo.";
        return false;
    }
    running = true;
    control_thread = std::thread([this] { ControlLoop(); });
    return true;
}

void Host::Stop() {
    if (!running) {
        return;
    }
    EndSession(true);
    running = false;
    if (control_thread.joinable()) {
        control_thread.join();
    }
    socket.Close();
}

std::vector<Address> Host::LocalAddresses() const {
    std::vector<Address> out;
    Address local;
    local.ip = PrimaryLocalIp();
    local.port = socket.Port();
    if (local.Valid()) {
        out.push_back(local);
    }
    return out;
}

Address Host::SessionPeer() const {
    std::scoped_lock lock{mutex};
    return session ? session->client : Address{};
}

Address Host::PublicAddress() const {
    std::scoped_lock lock{mutex};
    return public_address;
}

void Host::Punch(const std::vector<Address>& targets) {
    const int64_t until = NowMs() + PunchForMs;
    std::scoped_lock lock{mutex};
    for (const Address& target : targets) {
        if (!target.Valid()) {
            continue;
        }
        const auto it = std::find_if(punch_targets.begin(), punch_targets.end(),
                                     [&](const auto& entry) { return entry.first == target; });
        if (it != punch_targets.end()) {
            it->second = until;
        } else if (punch_targets.size() < 32) {
            punch_targets.emplace_back(target, until);
        }
    }
}

void Host::SendSealed(Session& target, Type type, const uint8_t* payload, size_t size,
                      uint8_t* scratch) {
    const size_t total = Seal(scratch, type, target.send_counter.fetch_add(1),
                              target.keys.host_to_client, payload, size);
    socket.SendTo(target.client, scratch, total);
}

void Host::ControlLoop() {
    std::vector<uint8_t> buffer(2048);
    std::vector<Address> stun_servers;
    bool stun_resolved = false;
    int64_t last_stun_ms = 0;
    int64_t last_punch_ms = 0;
    int64_t last_keepalive_ms = 0;

    while (running) {
        const int64_t now = NowMs();

        // Ask a STUN server which address the router gives this socket, and keep asking so the
        // router does not forget it.
        Address known_public;
        {
            std::scoped_lock lock{mutex};
            known_public = public_address;
        }
        if (now - last_stun_ms > (known_public.Valid() ? StunRefreshMs : StunRetryMs)) {
            last_stun_ms = now;
            if (!stun_resolved) {
                for (const StunServer& server : StunServers) {
                    Address address;
                    if (ResolveHost(server.host, address.ip)) {
                        address.port = server.port;
                        stun_servers.push_back(address);
                    }
                }
                stun_resolved = !stun_servers.empty();
            }
            uint8_t request[StunRequestSize];
            for (const Address& server : stun_servers) {
                BuildStunRequest(request);
                socket.SendTo(server, request, sizeof(request));
            }
        }

        if (now - last_punch_ms > PunchEveryMs) {
            last_punch_ms = now;
            uint8_t packet[sizeof(Header)];
            WriteHeader(packet, Type::Punch);
            std::scoped_lock lock{mutex};
            punch_targets.erase(std::remove_if(punch_targets.begin(), punch_targets.end(),
                                               [&](const auto& e) { return e.second < now; }),
                                punch_targets.end());
            for (const auto& entry : punch_targets) {
                socket.SendTo(entry.first, packet, sizeof(packet));
            }
            pending.erase(std::remove_if(pending.begin(), pending.end(),
                                         [&](const Pending& p) {
                                             return now - p.created_ms > PendingForMs;
                                         }),
                          pending.end());
        }

        {
            std::shared_ptr<Session> current;
            {
                std::scoped_lock lock{mutex};
                current = session;
            }
            if (current && now - current->last_seen_ms > TimeoutMs) {
                EndSession(false);
            } else if (current && now - last_keepalive_ms > KeepaliveMs) {
                // Lets the Switch know the PC is there even while the picture does not change
                last_keepalive_ms = now;
                uint8_t scratch[SealOverhead + 4];
                SendSealed(*current, Type::Keepalive, nullptr, 0, scratch);
            }
        }

        Address from;
        const long received = socket.ReceiveFrom(buffer.data(), buffer.size(), from, 50);
        if (received <= 0) {
            continue;
        }
        const size_t size = static_cast<size_t>(received);
        if (IsStunPacket(buffer.data(), size)) {
            Address mapped;
            if (ParseStunResponse(buffer.data(), size, mapped)) {
                std::scoped_lock lock{mutex};
                public_address = mapped;
            }
            continue;
        }
        Header header;
        const long payload = ReadHeader(buffer.data(), size, header);
        if (payload < 0) {
            continue;
        }
        if (IsEncrypted(header.type)) {
            HandleSealed(buffer.data(), size, header, from);
        } else {
            HandleClear(header, buffer.data() + sizeof(Header), payload, from);
        }
    }
}

void Host::HandleClear(const Header& header, const uint8_t* body, long size,
                       const Address& from) {
    if (header.type == Type::Discover && size >= static_cast<long>(sizeof(Discover))) {
        Discover discover;
        std::memcpy(&discover, body, sizeof(discover));
        if (!EqualConstantTime(discover.tag, tag.data(), tag.size())) {
            return; // another account
        }
        uint8_t reply[sizeof(Header) + sizeof(Announce)];
        const size_t offset = WriteHeader(reply, Type::Announce);
        Announce announce{};
        std::memcpy(announce.tag, tag.data(), tag.size());
        std::memcpy(announce.host_id, config.host_id.data(), config.host_id.size());
        std::strncpy(announce.name, config.name.c_str(), sizeof(announce.name) - 1);
        std::memcpy(reply + offset, &announce, sizeof(announce));
        socket.SendTo(from, reply, sizeof(reply));
        return;
    }
    if (header.type != Type::Hello || size < static_cast<long>(sizeof(Hello))) {
        return;
    }
    Hello hello;
    std::memcpy(&hello, body, sizeof(hello));
    const Tag16 expected = HelloProof(config.key, hello.client_nonce);
    if (!EqualConstantTime(hello.tag, tag.data(), tag.size()) ||
        !EqualConstantTime(hello.proof, expected.data(), expected.size())) {
        return;
    }

    Welcome welcome{};
    {
        std::scoped_lock lock{mutex};
        auto it = std::find_if(pending.begin(), pending.end(), [&](const Pending& p) {
            return p.client == from && std::memcmp(p.client_nonce, hello.client_nonce, 16) == 0;
        });
        if (it == pending.end()) {
            if (pending.size() >= MaxPending) {
                pending.erase(pending.begin());
            }
            Pending fresh;
            fresh.client = from;
            std::memcpy(fresh.client_nonce, hello.client_nonce, 16);
            RandomBytes(fresh.host_nonce, 16);
            fresh.keys = DeriveSessionKeys(config.key, fresh.client_nonce, fresh.host_nonce);
            hello.name[sizeof(hello.name) - 1] = 0;
            fresh.name = hello.name;
            fresh.created_ms = NowMs();
            pending.push_back(fresh);
            it = pending.end() - 1;
        }
        std::memcpy(welcome.client_nonce, it->client_nonce, 16);
        std::memcpy(welcome.host_nonce, it->host_nonce, 16);
    }
    const Tag16 proof = WelcomeProof(config.key, welcome.client_nonce, welcome.host_nonce);
    std::memcpy(welcome.proof, proof.data(), proof.size());
    uint8_t reply[sizeof(Header) + sizeof(Welcome)];
    const size_t offset = WriteHeader(reply, Type::Welcome);
    std::memcpy(reply + offset, &welcome, sizeof(welcome));
    socket.SendTo(from, reply, sizeof(reply));
}

void Host::HandleSealed(const uint8_t* packet, size_t size, const Header& header,
                        const Address& from) {
    uint8_t plain[2048];
    if (size > sizeof(plain)) {
        return;
    }
    std::shared_ptr<Session> current;
    {
        std::scoped_lock lock{mutex};
        current = session;
    }

    long payload = -1;
    if (current && current->client == from) {
        payload = Open(packet, size, current->keys.client_to_host, plain);
    }
    if (payload < 0) {
        // Not the running session: the first packet of a new one that finished the handshake?
        Pending match;
        bool found = false;
        {
            std::scoped_lock lock{mutex};
            for (const Pending& candidate : pending) {
                if (candidate.client == from &&
                    Open(packet, size, candidate.keys.client_to_host, plain) >=
                        static_cast<long>(sizeof(StartRequest)) &&
                    header.type == Type::Start) {
                    match = candidate;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            StartRequest request;
            std::memcpy(&request, plain, sizeof(request));
            BeginSession(match, request);
        }
        return;
    }

    // Each packet is accepted once and never an older one, so nothing can be replayed
    if (header.counter <= current->last_counter) {
        return;
    }
    current->last_counter = header.counter;
    current->last_seen_ms = NowMs();

    switch (header.type) {
    case Type::RequestKeyframe:
        current->want_keyframe = true;
        break;
    case Type::SetBitrate:
        if (payload >= static_cast<long>(sizeof(SetBitrate))) {
            SetBitrate request;
            std::memcpy(&request, plain, sizeof(request));
            current->pending_bitrate =
                static_cast<int>(std::clamp<uint32_t>(request.bitrate_kbps, 1500, 60000));
            current->parity_percent = std::clamp<int>(request.parity_percent, 10, 100);
        }
        break;
    case Type::Stop:
        EndSession(false);
        break;
    case Type::GoHome:
        if (callbacks.on_command) {
            callbacks.on_command(header.type);
        }
        break;
    case Type::Input:
        if (payload >= static_cast<long>(sizeof(InputState)) && callbacks.on_input) {
            InputState state;
            std::memcpy(&state, plain, sizeof(state));
            callbacks.on_input(state);
        }
        break;
    default: // Start again (it is repeated until video arrives) and Keepalive
        break;
    }
}

void Host::BeginSession(const Pending& accepted, const StartRequest& request) {
    EndSession(true);

    auto next = std::make_shared<Session>();
    next->client = accepted.client;
    next->keys = accepted.keys;
    next->name = accepted.name.empty() ? "Switch" : accepted.name;
    next->settings = request;
    next->settings.width = std::clamp<uint16_t>(request.width, 640, 1920);
    next->settings.height = std::clamp<uint16_t>(request.height, 360, 1080);
    next->settings.fps = std::clamp<uint16_t>(request.fps, 30, 60);
    next->settings.bitrate_kbps = std::clamp<uint32_t>(request.bitrate_kbps, 1500, 60000);
    next->last_seen_ms = NowMs();
    {
        std::scoped_lock lock{mutex};
        session = next;
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Pending& p) { return p.client == next->client; }),
                      pending.end());
        punch_targets.clear();
    }
    session_active = true;
    next->video = std::thread([this, next] { VideoLoop(next); });
    next->audio = std::thread([this, next] { AudioLoop(next); });
    if (callbacks.on_session) {
        callbacks.on_session(true, next->name);
    }
}

void Host::EndSession(bool notify_client) {
    std::shared_ptr<Session> ending;
    {
        std::scoped_lock lock{mutex};
        ending = std::move(session);
        session.reset();
    }
    if (!ending) {
        return;
    }
    ending->active = false;
    if (notify_client) {
        uint8_t scratch[SealOverhead + 4];
        for (int i = 0; i < 3; ++i) {
            SendSealed(*ending, Type::Stop, nullptr, 0, scratch);
        }
    }
    if (ending->video.joinable()) {
        ending->video.join();
    }
    if (ending->audio.joinable()) {
        ending->audio.join();
    }
    session_active = false;
    if (callbacks.on_session) {
        callbacks.on_session(false, ending->name);
    }
}

void Host::VideoLoop(std::shared_ptr<Session> current) {
    std::unique_ptr<VideoSource> source = callbacks.make_video ? callbacks.make_video() : nullptr;
    std::string error;
    if (!source || !source->Open(current->settings.width, current->settings.height,
                                 current->settings.fps,
                                 static_cast<int>(current->settings.bitrate_kbps), error)) {
        if (callbacks.on_error) {
            callbacks.on_error(error.empty() ? "Não foi possível capturar a tela." : error);
        }
        return;
    }
    std::vector<uint8_t> frame;
    std::vector<uint8_t> plain(sizeof(VideoChunk) + ChunkBytes);
    std::vector<uint8_t> sealed(plain.size() + SealOverhead);
    std::vector<uint8_t> parity(size_t{Fec::MaxParity} * ChunkBytes);
    uint32_t frame_number = 0;
    int64_t last_keyframe_ms = 0;
    const int64_t frame_interval_us = 1000000 / current->settings.fps;

    int applied_bitrate = static_cast<int>(current->settings.bitrate_kbps);
    while (current->active) {
        // A full frame is big: at most four a second, however often the Switch asks
        bool want_keyframe = false;
        const int64_t now = NowMs();
        if (current->want_keyframe && now - last_keyframe_ms > 250) {
            current->want_keyframe = false;
            want_keyframe = true;
            last_keyframe_ms = now;
        }
        bool keyframe = false;
        if (!source->Next(frame, keyframe, want_keyframe)) {
            if (callbacks.on_error) {
                callbacks.on_error("A captura de vídeo parou.");
            }
            break;
        }
        if (frame.empty() || frame.size() > MaxFrameBytes) {
            continue;
        }
        SendFrame(*current, frame, frame_number++, keyframe, frame_interval_us, plain, sealed,
                  parity);
        if (const int bitrate = current->pending_bitrate.exchange(0); bitrate > 0 &&
                                                                      bitrate != applied_bitrate) {
            applied_bitrate = bitrate;
            source->SetBitrate(bitrate);
        }
    }
    source->Close();
}

void Host::SendFrame(Session& current, std::vector<uint8_t>& frame, uint32_t number, bool keyframe,
                     int64_t frame_interval_us, std::vector<uint8_t>& plain,
                     std::vector<uint8_t>& sealed, std::vector<uint8_t>& parity) {
    const uint32_t frame_size = static_cast<uint32_t>(frame.size());
    const uint32_t pieces = PieceCount(frame_size);
    const uint32_t blocks = BlockCount(pieces);
    const int percent = current.parity_percent;
    // The last piece is zero padded so parity can be computed over whole pieces
    frame.resize(size_t{pieces} * ChunkBytes, 0);

    uint32_t total = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
        uint32_t first, count;
        BlockRange(pieces, blocks, b, first, count);
        total += count + ParityFor(count, static_cast<uint32_t>(percent));
    }
    // Spread the frame over part of the time until the next one, so the router and the
    // Switch's Wi-Fi get a steady flow instead of a burst they would drop pieces of
    const int64_t spread_us = frame_interval_us * (keyframe ? 5 : 3) / 10;
    const auto gap = std::chrono::microseconds(std::min<int64_t>(150, spread_us / total));
    auto next_send = std::chrono::steady_clock::now();

    VideoChunk chunk{};
    chunk.frame = number;
    chunk.frame_size = frame_size;
    chunk.block_count = static_cast<uint8_t>(blocks);
    chunk.keyframe = keyframe ? 1 : 0;
    const auto send = [&](const uint8_t* piece, size_t bytes) {
        WaitUntil(next_send);
        std::memcpy(plain.data(), &chunk, sizeof(chunk));
        std::memcpy(plain.data() + sizeof(chunk), piece, bytes);
        SendSealed(current, Type::Video, plain.data(), sizeof(chunk) + bytes, sealed.data());
        next_send += gap;
    };

    for (uint32_t b = 0; b < blocks && current.active; ++b) {
        uint32_t first, count;
        BlockRange(pieces, blocks, b, first, count);
        const size_t length = BlockLength(frame_size, first, count);
        chunk.block = static_cast<uint8_t>(b);
        chunk.data_count = static_cast<uint8_t>(count);
        chunk.parity_count = ParityFor(count, static_cast<uint32_t>(percent));
        const uint8_t* data[Fec::MaxData];
        for (uint32_t i = 0; i < count; ++i) {
            data[i] = frame.data() + size_t{first + i} * ChunkBytes;
            chunk.shard = static_cast<uint8_t>(i);
            const size_t bytes =
                std::min(length, size_t{frame_size} - size_t{first + i} * ChunkBytes);
            send(data[i], bytes);
        }
        uint8_t* out[Fec::MaxParity];
        for (int p = 0; p < chunk.parity_count; ++p) {
            out[p] = parity.data() + size_t(p) * ChunkBytes;
        }
        Fec::Encode(data, static_cast<int>(count), out, chunk.parity_count, length);
        for (int p = 0; p < chunk.parity_count && current.active; ++p) {
            chunk.shard = static_cast<uint8_t>(count + static_cast<uint32_t>(p));
            send(out[p], length);
        }
    }
}

void Host::AudioLoop(std::shared_ptr<Session> current) {
    std::unique_ptr<AudioSource> source = callbacks.make_audio ? callbacks.make_audio() : nullptr;
    std::string error;
    if (!source || !source->Open(error)) {
        if (callbacks.on_error && !error.empty()) {
            callbacks.on_error(error);
        }
        return;
    }
    std::vector<uint8_t> opus;
    std::vector<uint8_t> plain(sizeof(AudioChunk) + 1300);
    std::vector<uint8_t> sealed(plain.size() + SealOverhead);
    uint32_t sequence = 0;
    while (current->active) {
        if (!source->Next(opus)) {
            break;
        }
        if (opus.empty() || opus.size() > 1300) {
            continue;
        }
        const AudioChunk chunk{sequence++};
        std::memcpy(plain.data(), &chunk, sizeof(chunk));
        std::memcpy(plain.data() + sizeof(chunk), opus.data(), opus.size());
        SendSealed(*current, Type::Audio, plain.data(), sizeof(chunk) + opus.size(),
                   sealed.data());
    }
    source->Close();
}

} // namespace CharisStream
