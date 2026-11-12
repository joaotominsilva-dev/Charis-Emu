// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Runs a Charis Stream PC and Switch in one process, with a relay between them that loses
// packets on purpose, and checks that the picture, sound, input and commands get through.
// Build: make -C tools/charis-stream-test && tools/charis-stream-test/charis-stream-test

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "charis_stream/account.h"
#include "charis_stream/client.h"
#include "charis_stream/crypto.h"
#include "charis_stream/host.h"
#include "charis_stream/presence.h"
#include "charis_stream/upnp.h"

using namespace CharisStream;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what.c_str());
    if (!ok) {
        ++failures;
    }
}

void Sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint8_t ByteOf(uint32_t frame, size_t i) {
    uint32_t x = frame * 2654435761u + static_cast<uint32_t>(i) * 40503u;
    x ^= x >> 13;
    return static_cast<uint8_t>(x * 0x5bd1e995u >> 24);
}

// Frames whose every byte can be checked on the other side
class FakeVideo final : public VideoSource {
public:
    explicit FakeVideo(std::atomic<int>* bitrate_) : bitrate{bitrate_} {}
    bool Open(int, int, int fps, int kbps, std::string&) override {
        interval = std::chrono::microseconds(1000000 / fps);
        next = std::chrono::steady_clock::now();
        *bitrate = kbps;
        return true;
    }
    bool Next(std::vector<uint8_t>& frame, bool& keyframe, bool want_keyframe) override {
        std::this_thread::sleep_until(next);
        next += interval;
        keyframe = want_keyframe || number % 240 == 0;
        // Roughly what the bitrate asks for, keyframes much bigger
        const size_t average = static_cast<size_t>(*bitrate) * 1000 / 8 / 60;
        size_t size = keyframe ? average * 5 / 2 : average / 2 + (number * 7919u) % average;
        size = std::max<size_t>(size, 64);
        frame.resize(size);
        std::memcpy(frame.data(), &number, 4);
        const uint32_t size32 = static_cast<uint32_t>(size);
        std::memcpy(frame.data() + 4, &size32, 4);
        for (size_t i = 8; i < size; ++i) {
            frame[i] = ByteOf(number, i);
        }
        ++number;
        return true;
    }
    void SetBitrate(int kbps) override {
        *bitrate = kbps;
    }
    void Close() override {}

private:
    std::atomic<int>* bitrate;
    std::chrono::microseconds interval{16666};
    std::chrono::steady_clock::time_point next;
    uint32_t number = 0;
};

class FakeAudio final : public AudioSource {
public:
    bool Open(std::string&) override {
        next = std::chrono::steady_clock::now();
        return true;
    }
    bool Next(std::vector<uint8_t>& packet) override {
        std::this_thread::sleep_until(next);
        next += std::chrono::milliseconds(10);
        packet.assign(180, static_cast<uint8_t>(sequence++));
        return true;
    }
    void Close() override {}

private:
    std::chrono::steady_clock::time_point next;
    uint32_t sequence = 0;
};

// Sits between the Switch and the PC like a bad network: loses some packets, sometimes a few in
// a row, in both directions.
class LossyRelay {
public:
    LossyRelay(Address host_, double loss_, double burst_) : host{host_}, loss{loss_}, burst{burst_} {}
    ~LossyRelay() {
        running = false;
        if (thread.joinable()) {
            thread.join();
        }
    }
    bool Start() {
        std::string error;
        if (!client_side.Open(0, error) || !host_side.Open(0, error)) {
            return false;
        }
        running = true;
        thread = std::thread([this] { Loop(); });
        return true;
    }
    Address ClientFacing() const {
        Address out;
        out.ip = htonl_loopback();
        out.port = client_side.Port();
        return out;
    }
    void SetLoss(double value) {
        loss = value;
    }
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> passed{0};

private:
    static uint32_t htonl_loopback() {
        return Address::Parse("127.0.0.1:1").ip;
    }
    bool Drop() {
        if (burst_left > 0) {
            --burst_left;
            return true;
        }
        std::uniform_real_distribution<double> chance(0.0, 1.0);
        if (chance(random) < loss) {
            if (chance(random) < burst) {
                burst_left = 1 + static_cast<int>(random() % 3);
            }
            return true;
        }
        return false;
    }
    void Loop() {
        std::vector<uint8_t> buffer(2048);
        while (running) {
            Address from;
            long size = client_side.ReceiveFrom(buffer.data(), buffer.size(), from, 1);
            if (size > 0) {
                client = from;
                if (Drop()) {
                    ++dropped;
                } else {
                    ++passed;
                    host_side.SendTo(host, buffer.data(), static_cast<size_t>(size));
                }
            }
            while ((size = host_side.ReceiveFrom(buffer.data(), buffer.size(), from, 0)) > 0) {
                if (!client.Valid()) {
                    continue;
                }
                if (Drop()) {
                    ++dropped;
                } else {
                    ++passed;
                    client_side.SendTo(client, buffer.data(), static_cast<size_t>(size));
                }
            }
        }
    }

    Address host;
    std::atomic<double> loss;
    double burst;
    int burst_left = 0;
    std::mt19937 random{1234};
    UdpSocket client_side;
    UdpSocket host_side;
    Address client;
    std::atomic<bool> running{false};
    std::thread thread;
};

struct Received {
    std::mutex mutex;
    uint32_t frames = 0;
    uint32_t bad_frames = 0;
    uint32_t keyframes = 0;
    uint32_t audio = 0;
    uint32_t last_frame = 0;
    int64_t last_frame_ms = 0;
    int64_t worst_gap_ms = 0;
};

class CurlHttp final : public Http {
public:
    bool Post(const std::string& url, const std::string& body, std::string& response) override {
        const std::string command = "curl -s -m 10 -X POST --data-binary '" + body + "' '" + url + "'";
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

struct Run {
    uint32_t frames = 0;
    uint32_t bad = 0;
    uint32_t lost = 0;
    uint32_t rebuilt = 0;
    uint32_t bitrate = 0;
    int64_t worst_gap = 0;
};

Run Stream(const Key32& host_key, const Key32& client_key, double loss, double burst, int seconds,
           bool expect_stream, std::atomic<int>* bitrate_seen = nullptr) {
    std::atomic<int> bitrate{0};
    std::atomic<int> sessions{0};
    std::atomic<int> inputs{0};
    std::atomic<uint64_t> last_buttons{0};
    std::atomic<int> touches{0};
    std::atomic<int> home{0};

    HostConfig config;
    config.name = "PC de teste";
    config.key = host_key;
    config.host_id = {1, 2, 3, 4, 5, 6, 7, 8};
    config.port = 48110;
    HostCallbacks callbacks;
    callbacks.make_video = [&] { return std::make_unique<FakeVideo>(&bitrate); };
    callbacks.make_audio = [] { return std::make_unique<FakeAudio>(); };
    callbacks.on_input = [&](const InputState& state) {
        ++inputs;
        last_buttons = state.buttons;
        if (state.touch_count == 1 && state.touches[0].x == 640) {
            ++touches;
        }
    };
    callbacks.on_session = [&](bool connected, const std::string&) {
        sessions += connected ? 1 : 0;
    };
    callbacks.on_command = [&](Type type) {
        if (type == Type::GoHome) {
            ++home;
        }
    };
    Host host{config, callbacks};
    std::string error;
    if (!host.Start(error)) {
        Check(false, "PC abre a porta: " + error);
        return {};
    }

    Address host_address = Address::Parse("127.0.0.1:48110");
    LossyRelay relay{host_address, loss, burst};
    relay.Start();

    Received received;
    ClientCallbacks client_callbacks;
    client_callbacks.on_video = [&](const uint8_t* frame, size_t size, bool keyframe) {
        std::scoped_lock lock{received.mutex};
        bool good = size >= 8;
        uint32_t number = 0;
        uint32_t stated = 0;
        if (good) {
            std::memcpy(&number, frame, 4);
            std::memcpy(&stated, frame + 4, 4);
            good = stated == size;
            for (size_t i = 8; good && i < size; ++i) {
                good = frame[i] == ByteOf(number, i);
            }
        }
        ++received.frames;
        received.bad_frames += good ? 0 : 1;
        received.keyframes += keyframe ? 1 : 0;
        received.last_frame = number;
        const int64_t now = NowMs();
        if (received.last_frame_ms != 0) {
            received.worst_gap_ms = std::max(received.worst_gap_ms, now - received.last_frame_ms);
        }
        received.last_frame_ms = now;
    };
    client_callbacks.on_audio = [&](const uint8_t*, size_t size) {
        std::scoped_lock lock{received.mutex};
        received.audio += size == 180 ? 1 : 0;
    };
    Client client{client_key, "Switch de teste", client_callbacks};
    client.Open(error);

    StartRequest settings{};
    settings.width = 1280;
    settings.height = 720;
    settings.fps = 60;
    settings.bitrate_kbps = 15000;
    // A wrong address first, like an old home address from the server, then the real way in
    client.Connect({Address::Parse("10.255.255.1:48010"), relay.ClientFacing()}, settings);

    const int64_t until = NowMs() + seconds * 1000;
    while (NowMs() < until && client.GetState() != Client::State::Streaming) {
        Sleep(20);
    }
    const bool streaming = client.GetState() == Client::State::Streaming;
    if (!expect_stream) {
        Check(!streaming, "um Switch com outra conta não consegue entrar");
        client.Close();
        host.Stop();
        return {};
    }
    Check(streaming, "Switch conecta no PC");

    // Input: buttons, then a touch in the middle of the screen
    for (int i = 0; i < 30 && streaming; ++i) {
        InputState input{};
        input.mode = ModeGame;
        input.buttons = ButtonA | ButtonHome;
        if (i >= 20) {
            input.touch_count = 1;
            input.touches[0].x = 640;
            input.touches[0].y = 360;
        }
        client.SendInput(input);
        Sleep(16);
    }
    client.SendCommand(Type::GoHome);

    while (NowMs() < until) {
        Sleep(100);
    }
    Sleep(200);
    const ClientStats stats = client.Stats();
    Run run;
    {
        std::scoped_lock lock{received.mutex};
        run.frames = received.frames;
        run.bad = received.bad_frames;
        run.worst_gap = received.worst_gap_ms;
        std::printf("       quadros %u (inteiros-chave %u), ruins %u, som %u, perdidos %u, "
                    "pedaços refeitos %u, taxa %u kbps, paridade %u%%, maior pausa %lld ms, "
                    "relay perdeu %llu de %llu\n",
                    received.frames, received.keyframes, received.bad_frames, received.audio,
                    stats.lost_frames, stats.rebuilt_pieces, stats.bitrate_kbps,
                    stats.parity_percent,
                    static_cast<long long>(received.worst_gap_ms),
                    static_cast<unsigned long long>(relay.dropped.load()),
                    static_cast<unsigned long long>(relay.dropped + relay.passed));
        Check(received.audio > 0, "o som chega");
    }
    run.lost = stats.lost_frames;
    run.rebuilt = stats.rebuilt_pieces;
    run.bitrate = stats.bitrate_kbps;
    Check(inputs > 0 && (last_buttons & ButtonA) != 0, "os botões chegam no PC");
    Check(touches > 0, "o toque chega no PC");
    Check(home > 0 || loss > 0.0, "o comando de voltar ao início chega no PC");
    Check(sessions == 1, "o PC abre uma sessão só");
    if (bitrate_seen) {
        *bitrate_seen = bitrate.load();
    }

    client.Disconnect();
    Sleep(300);
    Check(!host.IsStreaming(), "ao desconectar, o PC encerra a transmissão");
    client.Close();
    host.Stop();
    return run;
}

} // namespace

void KeyTests() {
    std::printf("== Chaves da conta\n");
    const auto start = NowMs();
    const AccountKeys keys = DeriveAccountKeys("  Joao.Teste ", "senha certa");
    const AccountKeys same = DeriveAccountKeys("joao.teste", "senha certa");
    const AccountKeys other = DeriveAccountKeys("joao.teste", "senha errada");
    std::printf("       3 derivações em %lld ms\n", static_cast<long long>(NowMs() - start));
    Check(keys.stream_key == same.stream_key && keys.auth_hex == same.auth_hex,
          "o mesmo usuário e senha geram as mesmas chaves");
    Check(keys.stream_key != other.stream_key && keys.auth_hex != other.auth_hex,
          "outra senha gera outras chaves");
    Check(keys.auth_hex.size() == 64, "o servidor recebe só um código, não a senha");
}

void StreamTests(const AccountKeys& keys, const AccountKeys& other, bool quick) {
    std::printf("== Pacotes protegidos\n");
    {
        uint8_t packet[256];
        uint8_t plain[256];
        const char message[] = "ola";
        const size_t size = Seal(packet, Type::Input, 7, keys.stream_key,
                                 reinterpret_cast<const uint8_t*>(message), sizeof(message));
        Check(Open(packet, size, keys.stream_key, plain) == static_cast<long>(sizeof(message)) &&
                  std::memcmp(plain, message, sizeof(message)) == 0,
              "um pacote selado abre com a chave certa");
        packet[size - 1] ^= 1;
        Check(Open(packet, size, keys.stream_key, plain) < 0, "um pacote alterado é recusado");
        packet[size - 1] ^= 1;
        Check(Open(packet, size, other.stream_key, plain) < 0, "outra chave não abre o pacote");
        packet[sizeof(Header) - 1] ^= 1;
        Check(Open(packet, size, keys.stream_key, plain) < 0, "trocar o contador invalida o pacote");
    }

    std::printf("== Rede limpa\n");
    Run clean = Stream(keys.stream_key, keys.stream_key, 0.0, 0.0, quick ? 4 : 6, true);
    Check(clean.frames > (quick ? 180u : 300u), "chegam quase todos os quadros a 60 por segundo");
    Check(clean.bad == 0, "nenhum quadro chega errado");
    Check(clean.lost == 0, "nenhum quadro se perde");

    std::printf("== Conta errada\n");
    Stream(keys.stream_key, other.stream_key, 0.0, 0.0, 3, false);

    std::printf("== Wi-Fi de casa (0,5%% de perda)\n");
    Run home = Stream(keys.stream_key, keys.stream_key, 0.005, 0.3, quick ? 6 : 10, true);
    Check(home.bad == 0, "nenhum quadro chega errado");
    Check(home.lost == 0, "nenhum quadro se perde: tudo é refeito no Switch");
    Check(home.frames > (quick ? 300u : 540u), "a imagem anda a 60 quadros por segundo");

    std::printf("== Rede ruim (3%% de perda, às vezes em sequência)\n");
    Run lossy = Stream(keys.stream_key, keys.stream_key, 0.03, 0.3, quick ? 8 : 14, true);
    Check(lossy.bad == 0, "nenhum quadro chega errado mesmo com perdas");
    Check(lossy.rebuilt > 0, "pedaços perdidos são refeitos sem pedir de novo");
    Check(lossy.frames > (quick ? 400u : 720u), "a imagem continua andando");
    Check(lossy.worst_gap < 500, "a imagem nunca congela por muito tempo");

    std::printf("== Rede muito ruim (12%% de perda)\n");
    std::atomic<int> final_bitrate{0};
    Run awful = Stream(keys.stream_key, keys.stream_key, 0.12, 0.4, quick ? 14 : 20, true,
                       &final_bitrate);
    Check(awful.bad == 0, "nenhum quadro chega errado");
    Check(awful.frames > 0, "mesmo assim a imagem aparece");
    Check(awful.bitrate < 15000 && final_bitrate < 15000,
          "a qualidade baixa sozinha para a imagem continuar fluida");
}

void AccountTests(const AccountKeys& keys, const AccountKeys& other) {
    const char* server = std::getenv("CHARIS_ACCOUNT_URL");
    if (server) {
        std::printf("== Servidor da conta (%s)\n", server);
        CurlHttp http;
        Account account{http, server};
        std::string token;
        const std::string user = "teste" + std::to_string(NowMs() % 100000);
        Check(account.Register(user, keys.auth_hex, token).empty() && !token.empty(),
              "criar conta");
        std::string again;
        Check(!account.Register(user, keys.auth_hex, again).empty(), "nome repetido é recusado");
        Check(!account.Login(user, other.auth_hex, again).empty(), "senha errada é recusada");
        Check(account.Login(user, keys.auth_hex, again).empty(), "entrar na conta");
        std::vector<ConnectRequest> requests;
        const std::vector<Address> local{Address::Parse("192.168.0.10:48010")};
        int next_poll = 0;
        Check(account.HostPoll(token, "0102030405060708", "PC da sala", local,
                               Address::Parse("200.1.2.3:40000"), requests, next_poll)
                      .empty() &&
                  next_poll == 10000,
              "o PC avisa o servidor que está ligado (e volta em 10 s)");
        std::vector<HostEntry> hosts;
        Check(account.ListHosts(again, hosts).empty() && hosts.size() == 1 &&
                  hosts[0].name == "PC da sala" && hosts[0].online &&
                  hosts[0].public_address.ToString() == "200.1.2.3:40000",
              "o Switch vê o PC na lista da conta");
        Check(account
                  .RequestConnect(again, "0102030405060708", "abcdef0123456789", "Switch",
                                  Address::Parse("177.4.5.6:50000"),
                                  {Address::Parse("192.168.0.20:50000")})
                  .empty(),
              "o Switch pede para conectar");
        Check(account.HostPoll(token, "0102030405060708", "PC da sala", local,
                               Address::Parse("200.1.2.3:40000"), requests, next_poll)
                      .empty() &&
                  requests.size() == 1 &&
                  requests[0].public_address.ToString() == "177.4.5.6:50000",
              "o PC recebe o pedido com o endereço do Switch");
        Check(next_poll == 1500, "com o Switch procurando, o PC chama a cada 1,5 s");
        Check(!account.ListHosts("token-falso", hosts).empty() && account.LoginExpired(),
              "login inválido é recusado");
    }
}


std::string Fetch(const std::string& url) {
    FILE* pipe = popen(("curl -s -m 5 '" + url + "'").c_str(), "r");
    std::string out;
    if (!pipe) {
        return out;
    }
    char chunk[512];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
        out.append(chunk, got);
    }
    pclose(pipe);
    return out;
}

// Needs fake_igd.py running (CHARIS_UPNP_TEST=1)
void UpnpTests() {
    std::printf("== Roteador (UPnP)\n");
    PortMapping mapping;
    std::string error;
    const int64_t started = NowMs();
    Check(mapping.Map(48010, error), "o roteador abre uma porta para o PC (" + error + ")");
    std::printf("       levou %lld ms\n", static_cast<long long>(NowMs() - started));
    Check(mapping.External().ToString() == "203.0.113.7:48011",
          "porta ocupada: usa a próxima e sabe o endereço externo (" +
              mapping.External().ToString() + ")");
    Check(mapping.IsReachable(), "o endereço externo serve para a internet");
    Check(Fetch("http://127.0.0.1:5431/mappings").find("\"48011\"") != std::string::npos,
          "o roteador guardou o redirecionamento");
    Check(mapping.Refresh(), "renovar o redirecionamento");
    mapping.Remove();
    Check(Fetch("http://127.0.0.1:5431/mappings") == "{}", "fechar a porta ao sair");
    Address cgnat;
    cgnat.ip = Address::Parse("100.72.1.2:1").ip;
    Check(IsPrivateIp(cgnat.ip) && !IsPrivateIp(Address::Parse("203.0.113.7:1").ip),
          "reconhece endereço de operadora (CGNAT)");
}

// The PC side of the account: listed, found by the Switch, opens the way when asked
void PresenceTests(const AccountKeys& keys, const char* server) {
    std::printf("== PC na conta (%s)\n", server);
    CurlHttp http;
    Account account{http, server};
    std::string token;
    const std::string user = "pc" + std::to_string(NowMs() % 100000);
    Check(account.Register(user, keys.auth_hex, token).empty(), "criar conta");

    HostConfig config;
    config.name = "PC teste";
    config.key = keys.stream_key;
    config.port = 0;
    HostCallbacks callbacks;
    callbacks.make_video = [] { return std::unique_ptr<VideoSource>{}; };
    callbacks.make_audio = [] { return std::unique_ptr<AudioSource>{}; };
    Host host{config, callbacks};
    std::string error;
    Check(host.Start(error), "PC escutando");
    const bool upnp = std::getenv("CHARIS_UPNP_TEST") != nullptr;
    std::atomic<int> changes{0};
    Credentials credentials;
    credentials.user = user;
    credentials.auth_hex = keys.auth_hex;
    credentials.token = token;
    Presence presence{http, host, credentials, "1122334455667788", "PC teste",
                      [&](const Presence::Status&) { ++changes; }, {}, server};
    presence.Start();
    int64_t until = NowMs() + 8000;
    while (NowMs() < until && !presence.GetStatus().online) {
        Sleep(50);
    }
    Check(presence.GetStatus().online,
          "o PC aparece como ligado na conta " + presence.GetStatus().message);
    if (upnp) {
        Check(presence.GetStatus().router_port, "o PC avisa que o roteador abriu a porta");
    }

    std::vector<HostEntry> hosts;
    Check(account.ListHosts(token, hosts).empty() && hosts.size() == 1 && hosts[0].online,
          "o Switch vê o PC");
    bool has_lan = false;
    bool has_router = false;
    if (!hosts.empty()) {
        for (const Address& address : hosts[0].local) {
            has_lan |= address.port == host.Port() && address.ip == PrimaryLocalIp();
            has_router |= address.ToString().rfind("203.0.113.7:", 0) == 0;
        }
    }
    Check(has_lan, "com o endereço da rede de casa");
    if (upnp) {
        Check(has_router, "e com o endereço da porta aberta no roteador");
    }

    UdpSocket switch_socket;
    Check(switch_socket.Open(0, error), "Switch escutando");
    Address switch_address;
    switch_address.ip = Address::Parse("127.0.0.1:1").ip;
    switch_address.port = switch_socket.Port();
    const int64_t asked = NowMs();
    Check(account
              .RequestConnect(token, "1122334455667788", "aabbccddeeff0011", "Switch teste",
                              switch_address, {switch_address})
              .empty(),
          "o Switch pede para conectar");
    bool punched = false;
    std::vector<uint8_t> buffer(2048);
    until = NowMs() + 14000;
    while (!punched && NowMs() < until) {
        Address from;
        const long size = switch_socket.ReceiveFrom(buffer.data(), buffer.size(), from, 200);
        Header header;
        if (size > 0 && ReadHeader(buffer.data(), static_cast<size_t>(size), header) >= 0 &&
            header.type == Type::Punch) {
            punched = true;
        }
    }
    Check(punched, "o PC abre o caminho para o Switch em " + std::to_string(NowMs() - asked) +
                       " ms (no máximo uma espera de 10 s)");
    // Let the first round of punching end, then ask again: now the account knows a Switch
    // is looking, so the PC picks the request up within a couple of seconds
    const auto next_punch = [&](int64_t wait_ms) {
        const int64_t stop = NowMs() + wait_ms;
        while (NowMs() < stop) {
            Address from;
            const long size = switch_socket.ReceiveFrom(buffer.data(), buffer.size(), from, 100);
            Header header;
            if (size > 0 && ReadHeader(buffer.data(), static_cast<size_t>(size), header) >= 0 &&
                header.type == Type::Punch) {
                return true;
            }
        }
        return false;
    };
    until = NowMs() + 16000;
    while (NowMs() < until && next_punch(800)) {
    }
    const int64_t asked_again = NowMs();
    account.RequestConnect(token, "1122334455667788", "aabbccddeeff0011", "Switch teste",
                           switch_address, {switch_address});
    const bool fast = next_punch(5000);
    Check(fast && NowMs() - asked_again < 2500,
          "com o Switch procurando, o caminho abre em " + std::to_string(NowMs() - asked_again) +
              " ms");
    presence.Stop();
    host.Stop();
    if (upnp) {
        Check(Fetch("http://127.0.0.1:5431/mappings") == "{}", "a porta do roteador fecha ao sair");
    }
}


// The account server is down when the PC logs in: home play works anyway, and the PC creates
// the account by itself once the server is back. Needs CHARIS_DEV_SERVER (dev-server.mjs path).
void OfflineTests(const AccountKeys& keys) {
    const char* script = std::getenv("CHARIS_DEV_SERVER");
    if (script == nullptr) {
        return;
    }
    std::printf("== Servidor fora do ar no login\n");
    const std::string url = "http://127.0.0.1:8799/api/";
    CurlHttp http;
    Account account{http, url};
    std::string token;
    Check(!account.Login("semservidor", keys.auth_hex, token).empty() && account.Unavailable(),
          "sem servidor: o erro diz que o servidor não respondeu (não que a senha está errada)");

    HostConfig config;
    config.name = "PC offline";
    config.key = keys.stream_key;
    config.port = 0;
    HostCallbacks callbacks;
    callbacks.make_video = [] { return std::unique_ptr<VideoSource>{}; };
    callbacks.make_audio = [] { return std::unique_ptr<AudioSource>{}; };
    Host host{config, callbacks};
    std::string error;
    Check(host.Start(error), "PC escutando em casa mesmo sem servidor");
    Credentials credentials;
    credentials.user = "semservidor" + std::to_string(NowMs() % 10000);
    credentials.auth_hex = keys.auth_hex;
    credentials.create_account = true;
    std::string saved_token;
    std::mutex saved_mutex;
    Presence presence{http, host, credentials, "99aa99aa99aa99aa", "PC offline", {},
                      [&](const std::string& t) {
                          std::scoped_lock lock{saved_mutex};
                          saved_token = t;
                      },
                      url};
    presence.Start();
    Sleep(2500);
    Check(!presence.GetStatus().online && !presence.GetStatus().login_expired,
          "sem servidor: o PC espera, sem pedir a senha de novo (" +
              presence.GetStatus().message + ")");
    std::system(("node " + std::string(script) + " 8799 >/dev/null 2>&1 &").c_str());
    const int64_t until = NowMs() + 40000;
    while (NowMs() < until && !presence.GetStatus().online) {
        Sleep(200);
    }
    Check(presence.GetStatus().online, "o servidor voltou: o PC criou a conta e apareceu sozinho");
    {
        std::scoped_lock lock{saved_mutex};
        Check(!saved_token.empty(), "o PC guardou o login novo");
    }
    presence.Stop();
    host.Stop();
    std::system("pkill -f 'dev-server.mjs 8799'");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    const bool account_only = argc > 1 && std::strcmp(argv[1], "--account") == 0;
    KeyTests();
    const AccountKeys keys = DeriveAccountKeys("joao.teste", "senha certa");
    const AccountKeys other = DeriveAccountKeys("joao.teste", "senha errada");
    if (!account_only) {
        StreamTests(keys, other, quick);
    }
    AccountTests(keys, other);
    if (std::getenv("CHARIS_UPNP_TEST")) {
        UpnpTests();
    }
    if (const char* server = std::getenv("CHARIS_ACCOUNT_URL")) {
        PresenceTests(keys, server);
    }
    OfflineTests(keys);
    std::printf(failures == 0 ? "\nTUDO CERTO\n" : "\n%d FALHA(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
