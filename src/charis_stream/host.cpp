// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
#define CLOSE_SOCKET close
#endif

#include "charis_stream/host.h"

namespace CharisStream {

struct Host::Session {
    sockaddr_in client{};
    StartRequest settings{};
    std::atomic<bool> active{true};
    std::atomic<bool> want_keyframe{true};
    std::atomic<int64_t> last_seen_ms{0};
    std::thread video;
    std::thread audio;
};

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string AddressText(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return text;
}

bool WaitReadable(intptr_t fd, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD pfd{static_cast<SOCKET>(fd), POLLRDNORM, 0};
    return WSAPoll(&pfd, 1, timeout_ms) > 0;
#else
    pollfd pfd{static_cast<int>(fd), POLLIN, 0};
    return poll(&pfd, 1, timeout_ms) > 0;
#endif
}

} // namespace

Host::Host(std::string name_, HostCallbacks callbacks_)
    : name{std::move(name_)}, callbacks{std::move(callbacks_)} {}

Host::~Host() {
    Stop();
}

bool Host::Start(std::string& error) {
    if (running) {
        return true;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        error = "Não foi possível abrir a conexão de rede.";
        return false;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    const int buffer = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer), sizeof(buffer));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(HostPort);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CLOSE_SOCKET(fd);
        error = "A porta 48010 já está em uso. Feche outro programa de transmissão e tente de novo.";
        return false;
    }
    socket_fd = static_cast<intptr_t>(fd);
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
    CLOSE_SOCKET(socket_fd);
    socket_fd = -1;
}

void Host::SendTo(const std::shared_ptr<Session>& target, const uint8_t* data, size_t size) {
    sendto(socket_fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&target->client), sizeof(target->client));
}

void Host::ControlLoop() {
    std::vector<uint8_t> buffer(2048);
    while (running) {
        {
            std::shared_ptr<Session> current;
            {
                std::scoped_lock lock{session_mutex};
                current = session;
            }
            if (current && NowMs() - current->last_seen_ms > TimeoutMs) {
                EndSession(false);
            }
        }
        if (!WaitReadable(socket_fd, 200)) {
            continue;
        }
        sockaddr_in from{};
        socklen_type from_size = sizeof(from);
        const auto received =
            recvfrom(socket_fd, reinterpret_cast<char*>(buffer.data()),
                     static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                     &from_size);
        if (received <= 0) {
            continue;
        }
        Type type;
        const long payload = ReadHeader(buffer.data(), static_cast<size_t>(received), type);
        if (payload < 0) {
            continue;
        }
        const uint8_t* body = buffer.data() + sizeof(Header);

        std::shared_ptr<Session> current;
        {
            std::scoped_lock lock{session_mutex};
            current = session;
        }
        const bool from_client = current && current->client.sin_addr.s_addr == from.sin_addr.s_addr;

        switch (type) {
        case Type::Discover: {
            uint8_t reply[sizeof(Header) + sizeof(Announce)] = {};
            const size_t offset = WriteHeader(reply, Type::Announce);
            Announce announce{};
            std::strncpy(announce.name, name.c_str(), sizeof(announce.name) - 1);
            announce.width = 1920;
            announce.height = 1080;
            std::memcpy(reply + offset, &announce, sizeof(announce));
            sendto(socket_fd, reinterpret_cast<const char*>(reply), sizeof(reply), 0,
                   reinterpret_cast<const sockaddr*>(&from), from_size);
            break;
        }
        case Type::Start:
            if (payload >= static_cast<long>(sizeof(StartRequest))) {
                StartRequest request;
                std::memcpy(&request, body, sizeof(request));
                BeginSession(&from, static_cast<int>(from_size), request);
            }
            break;
        case Type::Keepalive:
            if (from_client) {
                current->last_seen_ms = NowMs();
            }
            break;
        case Type::RequestKeyframe:
            if (from_client) {
                current->want_keyframe = true;
            }
            break;
        case Type::Stop:
            if (from_client) {
                EndSession(false);
            }
            break;
        case Type::GoHome:
            if (from_client && callbacks.on_command) {
                current->last_seen_ms = NowMs();
                callbacks.on_command(type);
            }
            break;
        case Type::Input:
            if (from_client && payload >= static_cast<long>(sizeof(InputState))) {
                current->last_seen_ms = NowMs();
                InputState state;
                std::memcpy(&state, body, sizeof(state));
                if (callbacks.on_input) {
                    callbacks.on_input(state);
                }
            }
            break;
        default:
            break;
        }
    }
}

void Host::BeginSession(const void* address, int address_size, const StartRequest& request) {
    sockaddr_in client{};
    std::memcpy(&client, address, std::min<size_t>(sizeof(client), static_cast<size_t>(address_size)));
    {
        std::shared_ptr<Session> current;
        {
            std::scoped_lock lock{session_mutex};
            current = session;
        }
        // The same Switch asking again (it restarted): keep the running session
        if (current && current->client.sin_addr.s_addr == client.sin_addr.s_addr &&
            current->client.sin_port == client.sin_port) {
            current->last_seen_ms = NowMs();
            current->want_keyframe = true;
            return;
        }
    }
    EndSession(true);

    auto next = std::make_shared<Session>();
    next->client = client;
    next->settings = request;
    next->settings.width = std::clamp<uint16_t>(request.width, 640, 1920);
    next->settings.height = std::clamp<uint16_t>(request.height, 360, 1080);
    next->settings.fps = std::clamp<uint16_t>(request.fps, 30, 60);
    next->settings.bitrate_kbps = std::clamp<uint32_t>(request.bitrate_kbps, 2000, 60000);
    next->last_seen_ms = NowMs();
    {
        std::scoped_lock lock{session_mutex};
        session = next;
    }
    session_active = true;
    next->video = std::thread([this, next] { VideoLoop(next); });
    next->audio = std::thread([this, next] { AudioLoop(next); });
    if (callbacks.on_session) {
        callbacks.on_session(true, AddressText(client));
    }
}

void Host::EndSession(bool notify_client) {
    std::shared_ptr<Session> ending;
    {
        std::scoped_lock lock{session_mutex};
        ending = std::move(session);
        session.reset();
    }
    if (!ending) {
        return;
    }
    ending->active = false;
    if (notify_client) {
        uint8_t packet[sizeof(Header)];
        WriteHeader(packet, Type::Stop);
        SendTo(ending, packet, sizeof(packet));
    }
    if (ending->video.joinable()) {
        ending->video.join();
    }
    if (ending->audio.joinable()) {
        ending->audio.join();
    }
    session_active = false;
    if (callbacks.on_session) {
        callbacks.on_session(false, AddressText(ending->client));
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
    std::vector<uint8_t> packet(sizeof(Header) + sizeof(VideoChunk) + MaxVideoBytes);
    uint32_t frame_number = 0;
    while (current->active) {
        bool keyframe = false;
        if (!source->Next(frame, keyframe, current->want_keyframe.exchange(false))) {
            if (callbacks.on_error) {
                callbacks.on_error("A captura de vídeo parou.");
            }
            break;
        }
        if (frame.empty()) {
            continue;
        }
        const uint16_t count =
            static_cast<uint16_t>((frame.size() + MaxVideoBytes - 1) / MaxVideoBytes);
        for (uint16_t index = 0; index < count && current->active; ++index) {
            const size_t offset = WriteHeader(packet.data(), Type::Video);
            VideoChunk chunk{};
            chunk.frame = frame_number;
            chunk.index = index;
            chunk.count = count;
            chunk.frame_size = static_cast<uint32_t>(frame.size());
            chunk.keyframe = keyframe ? 1 : 0;
            std::memcpy(packet.data() + offset, &chunk, sizeof(chunk));
            const size_t start = static_cast<size_t>(index) * MaxVideoBytes;
            const size_t bytes = std::min(MaxVideoBytes, frame.size() - start);
            std::memcpy(packet.data() + offset + sizeof(chunk), frame.data() + start, bytes);
            SendTo(current, packet.data(), offset + sizeof(chunk) + bytes);
            // Spread big keyframes a little so the Switch's Wi-Fi buffer keeps up
            if (count > 48 && index % 24 == 23) {
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        }
        ++frame_number;
    }
    source->Close();
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
    std::vector<uint8_t> packet(sizeof(Header) + sizeof(AudioChunk) + 1500);
    uint32_t sequence = 0;
    while (current->active) {
        if (!source->Next(opus)) {
            break;
        }
        if (opus.empty() || opus.size() > 1500) {
            continue;
        }
        const size_t offset = WriteHeader(packet.data(), Type::Audio);
        const AudioChunk chunk{sequence++};
        std::memcpy(packet.data() + offset, &chunk, sizeof(chunk));
        std::memcpy(packet.data() + offset + sizeof(chunk), opus.data(), opus.size());
        SendTo(current, packet.data(), offset + sizeof(chunk) + opus.size());
    }
    source->Close();
}

} // namespace CharisStream
