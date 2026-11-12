// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#include "charis_stream/client.h"
#include "charis_stream/fec.h"

namespace CharisStream {

namespace {

constexpr int ConnectForMs = 20000;
constexpr int HelloEveryMs = 250;
constexpr int StartEveryMs = 300;
constexpr int KeyframeRequestEveryMs = 300;
constexpr int StunRetryMs = 3000;
constexpr int StunRefreshMs = 20000;
constexpr int StunFailedRetryMs = 30000;
constexpr uint32_t MinBitrate = 2500;
constexpr size_t MaxAssemblies = 4;

} // namespace

Client::Client(const Key32& key_, std::string device_name_, ClientCallbacks callbacks_)
    : key{key_}, tag{TagFromKey(key_)}, device_name{std::move(device_name_)},
      callbacks{std::move(callbacks_)} {}

Client::~Client() {
    Close();
}

bool Client::Open(std::string& error) {
    if (running) {
        return true;
    }
    if (!socket.Open(0, error)) {
        return false;
    }
    running = true;
    thread = std::thread([this] { Loop(); });
    return true;
}

void Client::Close() {
    if (!running) {
        return;
    }
    Disconnect();
    running = false;
    if (thread.joinable()) {
        thread.join();
    }
    socket.Close();
}

void Client::SetDiscovery(bool enabled) {
    discovery = enabled;
}

std::vector<DiscoveredHost> Client::Discovered() const {
    const int64_t now = NowMs();
    std::scoped_lock lock{mutex};
    std::vector<DiscoveredHost> out;
    for (const DiscoveredHost& host : discovered) {
        if (now - host.seen_ms < 5000) {
            out.push_back(host);
        }
    }
    return out;
}

Address Client::LocalAddress() const {
    Address local;
    local.ip = PrimaryLocalIp();
    local.port = socket.Port();
    return local;
}

Address Client::PublicAddress() const {
    std::scoped_lock lock{mutex};
    return public_address;
}

std::string Client::FailureReason() const {
    std::scoped_lock lock{mutex};
    return failure;
}

ClientStats Client::Stats() const {
    std::scoped_lock lock{mutex};
    return stats;
}

void Client::Connect(const std::vector<Address>& list, const StartRequest& wanted) {
    {
        std::scoped_lock lock{mutex};
        candidates = list;
        settings = wanted;
        RandomBytes(client_nonce, sizeof(client_nonce));
        connect_started_ms = NowMs();
        have_keys = false;
        failure.clear();
        stats = {};
        stats.bitrate_kbps = wanted.bitrate_kbps;
    }
    {
        std::scoped_lock lock{send_mutex};
        peer = {};
        send_counter = 1;
        input_sequence = 0;
    }
    target_bitrate = wanted.bitrate_kbps;
    generation.fetch_add(1);
    state = State::Connecting;
}

void Client::AddCandidates(const std::vector<Address>& list) {
    std::scoped_lock lock{mutex};
    for (const Address& address : list) {
        if (address.Valid() &&
            std::find(candidates.begin(), candidates.end(), address) == candidates.end() &&
            candidates.size() < 16) {
            candidates.push_back(address);
        }
    }
}

void Client::Disconnect() {
    const State before = state.exchange(State::Idle);
    if (before == State::Streaming || before == State::Connecting) {
        for (int i = 0; i < 3; ++i) {
            SendSealed(Type::Stop, nullptr, 0);
        }
    }
    {
        std::scoped_lock lock{mutex};
        have_keys = false;
    }
    generation.fetch_add(1);
}

void Client::Fail(const std::string& reason) {
    {
        std::scoped_lock lock{mutex};
        failure = reason;
        have_keys = false;
    }
    state = State::Failed;
}

void Client::SendSealed(Type type, const uint8_t* payload, size_t size) {
    uint8_t packet[SealOverhead + sizeof(InputState) + 64];
    if (size > sizeof(packet) - SealOverhead) {
        return;
    }
    std::scoped_lock lock{send_mutex};
    if (!peer.Valid()) {
        return;
    }
    const size_t total = Seal(packet, type, send_counter++, keys.client_to_host, payload, size);
    socket.SendTo(peer, packet, total);
}

void Client::SendInput(InputState input) {
    if (state != State::Streaming) {
        return;
    }
    {
        std::scoped_lock lock{send_mutex};
        input.sequence = ++input_sequence;
    }
    SendSealed(Type::Input, reinterpret_cast<const uint8_t*>(&input), sizeof(input));
}

void Client::SendCommand(Type type) {
    if (state == State::Streaming) {
        SendSealed(type, nullptr, 0);
    }
}

void Client::RequestBitrate(uint32_t bitrate_kbps) {
    target_bitrate = std::max(MinBitrate, bitrate_kbps);
}

void Client::ResetStream() {
    assemblies.clear();
    next_frame = 0;
    waiting_keyframe = true;
    got_first_frame = false;
    last_keyframe_request_ms = NowMs();
    last_packet_ms = NowMs();
    have_audio = false;
    loss_events.clear();
    current_bitrate = target_bitrate;
    applied_target = target_bitrate;
    parity_percent = 20;
    highest_counter = 0;
    seen_window = 0;
    window_start_counter = 0;
    window_received = 0;
    clean_seconds = 0;
    last_decrease_ms = NowMs();
    last_increase_ms = NowMs();
    last_bitrate_sent_ms = 0;
}

void Client::Loop() {
    std::vector<uint8_t> buffer(2048);
    std::vector<Address> stun_servers;
    int64_t last_stun_ms = 0;
    int64_t next_resolve_ms = 0;
    int64_t last_discover_ms = 0;
    int64_t last_hello_ms = 0;
    int64_t last_start_ms = 0;
    int64_t last_keepalive_ms = 0;
    int64_t last_adapt_ms = 0;
    uint32_t seen_generation = 0;

    while (running) {
        const int64_t now = NowMs();
        const State current = state;
        if (const uint32_t g = generation; g != seen_generation) {
            seen_generation = g;
            ResetStream();
        }

        // Which address does the router give this socket? Name lookups can block, so they
        // only happen while nothing is streaming.
        Address known_public;
        {
            std::scoped_lock lock{mutex};
            known_public = public_address;
        }
        if (stun_servers.empty() && current != State::Streaming && now >= next_resolve_ms) {
            for (const StunServer& server : StunServers) {
                Address address;
                if (ResolveHost(server.host, address.ip)) {
                    address.port = server.port;
                    stun_servers.push_back(address);
                }
            }
            next_resolve_ms = NowMs() + StunFailedRetryMs;
        }
        if (!stun_servers.empty() &&
            now - last_stun_ms > (known_public.Valid() ? StunRefreshMs : StunRetryMs)) {
            last_stun_ms = now;
            uint8_t request[StunRequestSize];
            for (const Address& server : stun_servers) {
                BuildStunRequest(request);
                socket.SendTo(server, request, sizeof(request));
            }
        }

        if (current == State::Idle && discovery && now - last_discover_ms > 1000) {
            last_discover_ms = now;
            uint8_t packet[sizeof(Header) + sizeof(Discover)];
            const size_t offset = WriteHeader(packet, Type::Discover);
            std::memcpy(packet + offset, tag.data(), tag.size());
            Address everyone;
            everyone.ip = BroadcastIp();
            everyone.port = HostPort;
            socket.SendTo(everyone, packet, sizeof(packet));
        }

        if (current == State::Connecting) {
            bool keys_ready;
            std::vector<Address> targets;
            StartRequest wanted;
            Hello hello{};
            int64_t started;
            {
                std::scoped_lock lock{mutex};
                keys_ready = have_keys;
                targets = candidates;
                wanted = settings;
                started = connect_started_ms;
                std::memcpy(hello.client_nonce, client_nonce, 16);
            }
            if (!keys_ready && now - last_hello_ms > HelloEveryMs) {
                last_hello_ms = now;
                std::memcpy(hello.tag, tag.data(), tag.size());
                const Tag16 proof = HelloProof(key, hello.client_nonce);
                std::memcpy(hello.proof, proof.data(), proof.size());
                std::strncpy(hello.name, device_name.c_str(), sizeof(hello.name) - 1);
                uint8_t packet[sizeof(Header) + sizeof(Hello)];
                const size_t offset = WriteHeader(packet, Type::Hello);
                std::memcpy(packet + offset, &hello, sizeof(hello));
                for (const Address& target : targets) {
                    socket.SendTo(target, packet, sizeof(packet));
                }
            }
            if (keys_ready && now - last_start_ms > StartEveryMs) {
                last_start_ms = now;
                SendSealed(Type::Start, reinterpret_cast<const uint8_t*>(&wanted),
                           sizeof(wanted));
            }
            if (now - started > ConnectForMs) {
                Fail("O PC não respondeu. Confira: o Charis Switch está aberto no PC, com a "
                     "mesma conta e senha daqui; e o Windows deixou o Charis Switch usar a rede "
                     "(reinstale a versão nova e aceite o pedido de permissão). Fora de casa, "
                     "algumas redes (como 4G) não deixam a conexão direta passar.");
            }
        }

        if (current == State::Streaming) {
            if (now - last_keepalive_ms > KeepaliveMs) {
                last_keepalive_ms = now;
                SendSealed(Type::Keepalive, nullptr, 0);
            }
            if (now - last_packet_ms > TimeoutMs) {
                Fail("A conexão com o PC caiu.");
            }
            if (waiting_keyframe && now - last_keyframe_request_ms > KeyframeRequestEveryMs) {
                last_keyframe_request_ms = now;
                SendSealed(Type::RequestKeyframe, nullptr, 0);
            }
            if (now - last_adapt_ms >= 1000) {
                last_adapt_ms = now;
                Adapt(now);
            }
        }

        Address from;
        const long received =
            socket.ReceiveFrom(buffer.data(), buffer.size(), from,
                               current == State::Streaming || current == State::Connecting ? 5
                                                                                           : 50);
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
        if (!IsEncrypted(header.type)) {
            HandleClear(header, buffer.data() + sizeof(Header), payload, from);
            continue;
        }
        Address expected;
        {
            std::scoped_lock lock{send_mutex};
            expected = peer;
        }
        if (expected.Valid() && from == expected) {
            HandleSealed(buffer.data(), size, header);
        }
    }
}

void Client::HandleClear(const Header& header, const uint8_t* body, long size,
                         const Address& from) {
    if (header.type == Type::Announce && size >= static_cast<long>(sizeof(Announce))) {
        Announce announce;
        std::memcpy(&announce, body, sizeof(announce));
        if (!EqualConstantTime(announce.tag, tag.data(), tag.size())) {
            return;
        }
        announce.name[sizeof(announce.name) - 1] = 0;
        std::scoped_lock lock{mutex};
        auto it = std::find_if(discovered.begin(), discovered.end(), [&](const auto& host) {
            return std::memcmp(host.id.data(), announce.host_id, 8) == 0;
        });
        if (it == discovered.end()) {
            discovered.emplace_back();
            it = discovered.end() - 1;
            std::memcpy(it->id.data(), announce.host_id, 8);
        }
        it->address = from;
        it->name = announce.name;
        it->seen_ms = NowMs();
        return;
    }
    if (header.type != Type::Welcome || size < static_cast<long>(sizeof(Welcome)) ||
        state != State::Connecting) {
        return;
    }
    Welcome welcome;
    std::memcpy(&welcome, body, sizeof(welcome));
    std::scoped_lock lock{mutex};
    if (have_keys || std::memcmp(welcome.client_nonce, client_nonce, 16) != 0) {
        return;
    }
    const Tag16 expected = WelcomeProof(key, welcome.client_nonce, welcome.host_nonce);
    if (!EqualConstantTime(welcome.proof, expected.data(), expected.size())) {
        return; // not a PC of this account
    }
    {
        std::scoped_lock send_lock{send_mutex};
        keys = DeriveSessionKeys(key, welcome.client_nonce, welcome.host_nonce);
        peer = from;
        send_counter = 1;
    }
    have_keys = true;
}

void Client::HandleSealed(const uint8_t* packet, size_t size, const Header& header) {
    uint8_t plain[2048];
    if (size > sizeof(plain)) {
        return;
    }
    Key32 open_key;
    {
        std::scoped_lock lock{send_mutex};
        open_key = keys.host_to_client;
    }
    const long payload = CharisStream::Open(packet, size, open_key, plain);
    if (payload < 0 || !AcceptCounter(header.counter)) {
        return;
    }
    last_packet_ms = NowMs();
    State expected = State::Connecting;
    state.compare_exchange_strong(expected, State::Streaming);
    if (state != State::Streaming) {
        return;
    }

    switch (header.type) {
    case Type::Video:
        HandleVideo(plain, static_cast<size_t>(payload));
        break;
    case Type::Audio:
        if (payload > static_cast<long>(sizeof(AudioChunk)) && callbacks.on_audio) {
            AudioChunk chunk;
            std::memcpy(&chunk, plain, sizeof(chunk));
            if (!have_audio || static_cast<int32_t>(chunk.sequence - last_audio_sequence) > 0) {
                have_audio = true;
                last_audio_sequence = chunk.sequence;
                callbacks.on_audio(plain + sizeof(chunk),
                                   static_cast<size_t>(payload) - sizeof(chunk));
            }
        }
        break;
    case Type::Stop:
        Fail("O PC encerrou a transmissão.");
        break;
    default:
        break;
    }
}

// Packets come from two threads on the PC, so they can arrive a little out of order: remember
// which of the last 64 counters were seen and refuse anything older or repeated.
bool Client::AcceptCounter(uint64_t counter) {
    if (counter > highest_counter) {
        const uint64_t shift = counter - highest_counter;
        seen_window = shift >= 64 ? 1 : (seen_window << shift) | 1;
        if (highest_counter == 0) {
            window_start_counter = counter - 1;
        }
        highest_counter = counter;
        ++window_received;
        return true;
    }
    const uint64_t age = highest_counter - counter;
    if (age >= 64 || (seen_window & (uint64_t{1} << age)) != 0) {
        return false;
    }
    seen_window |= uint64_t{1} << age;
    ++window_received;
    return true;
}

// Once a second: how many packets from the PC went missing? A few: more parity. Many, or
// frames that could not be rebuilt: a lower rate too. A clean network for a while: back up.
void Client::Adapt(int64_t now) {
    const uint64_t expected = highest_counter - window_start_counter;
    const float loss =
        expected == 0 ? 0.0f
                      : 1.0f - static_cast<float>(std::min(window_received, expected)) /
                                   static_cast<float>(expected);
    window_start_counter = highest_counter;
    window_received = 0;
    while (!loss_events.empty() && now - loss_events.front() > 5000) {
        loss_events.pop_front();
    }

    const uint32_t target = target_bitrate;
    uint32_t bitrate = current_bitrate;
    uint32_t parity = parity_percent;
    if (target != applied_target) {
        bitrate = target; // the user picked another quality
        applied_target = target;
    }
    if (loss >= 0.08f) {
        parity = 60;
    } else if (loss >= 0.03f) {
        parity = std::max<uint32_t>(parity, 40);
    } else if (loss >= 0.01f) {
        parity = std::max<uint32_t>(parity, 30);
    }
    if (loss < 0.005f && loss_events.empty()) {
        ++clean_seconds;
    } else {
        clean_seconds = 0;
    }
    if ((loss >= 0.05f || loss_events.size() >= 2) && now - last_decrease_ms >= 2000) {
        bitrate = std::max(MinBitrate, bitrate * 3 / 4);
        last_decrease_ms = now;
        loss_events.clear();
    } else if (clean_seconds >= 8 && now - last_increase_ms >= 8000) {
        parity = std::max<uint32_t>(20, parity - 10);
        if (bitrate < target) {
            bitrate = std::min(target, bitrate * 115 / 100);
        }
        last_increase_ms = now;
    }

    const bool changed = bitrate != current_bitrate || parity != parity_percent;
    current_bitrate = bitrate;
    parity_percent = parity;
    // Sent again now and then: a lost request must not leave the PC on old settings
    if (changed || now - last_bitrate_sent_ms > 3000) {
        last_bitrate_sent_ms = now;
        SetBitrate request{};
        request.bitrate_kbps = bitrate;
        request.parity_percent = static_cast<uint8_t>(parity);
        SendSealed(Type::SetBitrate, reinterpret_cast<const uint8_t*>(&request), sizeof(request));
    }
    std::scoped_lock lock{mutex};
    stats.bitrate_kbps = bitrate;
    stats.parity_percent = parity;
    stats.loss_percent = loss * 100.0f;
}

void Client::NoteLoss() {
    {
        std::scoped_lock lock{mutex};
        ++stats.lost_frames;
    }
    loss_events.push_back(NowMs());
}

void Client::HandleVideo(const uint8_t* body, size_t size) {
    if (size <= sizeof(VideoChunk)) {
        return;
    }
    VideoChunk chunk;
    std::memcpy(&chunk, body, sizeof(chunk));
    const uint8_t* piece = body + sizeof(chunk);
    const size_t bytes = size - sizeof(chunk);
    if (chunk.frame_size == 0 || chunk.frame_size > MaxFrameBytes) {
        return;
    }
    const uint32_t pieces = PieceCount(chunk.frame_size);
    const uint32_t block_count = BlockCount(pieces);
    if (chunk.block_count != block_count || chunk.block >= block_count) {
        return;
    }
    uint32_t first, count;
    BlockRange(pieces, block_count, chunk.block, first, count);
    const size_t length = BlockLength(chunk.frame_size, first, count);
    if (chunk.data_count != count || chunk.parity_count == 0 ||
        chunk.parity_count > Fec::MaxParity || chunk.shard >= count + chunk.parity_count) {
        return;
    }
    const bool is_data = chunk.shard < count;
    const size_t expected_bytes =
        is_data ? std::min(length, size_t{chunk.frame_size} - size_t{first + chunk.shard} * ChunkBytes)
                : length;
    if (bytes != expected_bytes) {
        return;
    }

    if (got_first_frame && static_cast<int32_t>(chunk.frame - next_frame) < 0) {
        return; // a late piece of a frame already shown or given up on
    }
    if (!got_first_frame && assemblies.empty()) {
        next_frame = chunk.frame;
    }

    // Two frames ahead of one that is still incomplete: stop waiting for it
    if (static_cast<int32_t>(chunk.frame - next_frame) >= 2) {
        const auto stuck = assemblies.find(next_frame);
        if (stuck == assemblies.end() || !stuck->second.complete) {
            if (got_first_frame && !waiting_keyframe) {
                NoteLoss();
            }
            waiting_keyframe = true;
            for (auto it = assemblies.begin(); it != assemblies.end();) {
                it = static_cast<int32_t>(it->first - chunk.frame) < -1 ? assemblies.erase(it)
                                                                         : std::next(it);
            }
            next_frame = chunk.frame - 1;
        }
    }

    if (assemblies.find(chunk.frame) == assemblies.end() && assemblies.size() >= MaxAssemblies) {
        assemblies.erase(assemblies.begin());
    }
    Assembly& assembly = assemblies[chunk.frame];
    if (assembly.frame_size == 0) {
        assembly.frame_size = chunk.frame_size;
        assembly.keyframe = chunk.keyframe != 0;
        assembly.data.assign(size_t{pieces} * ChunkBytes, 0);
        assembly.blocks.resize(block_count);
        assembly.blocks_left = block_count;
    } else if (assembly.frame_size != chunk.frame_size) {
        return;
    }
    if (assembly.complete) {
        return;
    }
    Block& block = assembly.blocks[chunk.block];
    if (block.have.empty()) {
        block.first = first;
        block.data_count = static_cast<uint8_t>(count);
        block.parity_count = chunk.parity_count;
        block.missing = static_cast<uint8_t>(count);
        block.length = length;
        block.have.assign(count + chunk.parity_count, 0);
    } else if (block.parity_count != chunk.parity_count) {
        return;
    }
    if (block.done || block.have[chunk.shard]) {
        return;
    }
    block.have[chunk.shard] = 1;
    if (is_data) {
        std::memcpy(assembly.data.data() + size_t{first + chunk.shard} * ChunkBytes, piece, bytes);
        --block.missing;
    } else {
        if (block.parity.empty()) {
            block.parity.assign(size_t{block.parity_count} * length, 0);
        }
        std::memcpy(block.parity.data() + size_t(chunk.shard - count) * length, piece, bytes);
        ++block.parity_have;
    }

    if (block.missing > 0 && block.parity_have >= block.missing) {
        Recover(assembly, block);
    }
    if (block.missing == 0) {
        block.done = true;
        block.parity.clear();
        block.parity.shrink_to_fit();
        if (--assembly.blocks_left == 0) {
            assembly.complete = true;
            Deliver();
        }
    }
}

void Client::Recover(Assembly& assembly, Block& block) {
    uint8_t* data[Fec::MaxData];
    bool data_present[Fec::MaxData];
    const uint8_t* parity[Fec::MaxParity];
    bool parity_present[Fec::MaxParity];
    for (int i = 0; i < block.data_count; ++i) {
        data[i] = assembly.data.data() + size_t{block.first + static_cast<uint32_t>(i)} * ChunkBytes;
        data_present[i] = block.have[i] != 0;
    }
    for (int p = 0; p < block.parity_count; ++p) {
        parity[p] = block.parity.data() + size_t(p) * block.length;
        parity_present[p] = block.have[block.data_count + p] != 0;
    }
    if (!Fec::Decode(data, data_present, block.data_count, parity, parity_present,
                     block.parity_count, block.length)) {
        return;
    }
    // The last piece of the frame may be shorter: what decoding wrote past the frame was the
    // zero padding, and the buffer has room for whole pieces
    {
        std::scoped_lock lock{mutex};
        stats.rebuilt_pieces += block.missing;
    }
    for (int i = 0; i < block.data_count; ++i) {
        block.have[i] = 1;
    }
    block.missing = 0;
}

void Client::Deliver() {
    while (true) {
        auto it = assemblies.end();
        if (waiting_keyframe) {
            // Only a full frame can restart the picture
            it = std::find_if(assemblies.begin(), assemblies.end(), [](const auto& entry) {
                return entry.second.complete && entry.second.keyframe;
            });
        } else {
            it = assemblies.find(next_frame);
            if (it != assemblies.end() && !it->second.complete) {
                it = assemblies.end();
            }
        }
        if (it == assemblies.end()) {
            return;
        }
        const Assembly& frame = it->second;
        if (callbacks.on_video) {
            callbacks.on_video(frame.data.data(), frame.frame_size, frame.keyframe);
        }
        {
            std::scoped_lock lock{mutex};
            ++stats.frames;
        }
        waiting_keyframe = false;
        got_first_frame = true;
        next_frame = it->first + 1;
        assemblies.erase(assemblies.begin(), std::next(it));
    }
}

} // namespace CharisStream
