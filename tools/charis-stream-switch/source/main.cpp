// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// Charis Stream for Nintendo Switch (homebrew). Log in once with the Charis account; the app
// lists the PCs of that account running Charis Switch, at home or away, and plays them:
// picture and sound from the PC, controller, touch, mouse and keyboard back to it.
// Also builds on Linux for testing.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "charis_stream/account.h"
#include "charis_stream/client.h"
#include "charis_stream/crypto.h"
#include "input.h"
#include "media.h"
#include "platform.h"
#include "ui.h"

using namespace CharisStream;

namespace {

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One account call at a time off the screen thread; its result is applied on the screen thread.
class Background {
public:
    ~Background() {
        if (thread.joinable()) {
            thread.join();
        }
    }
    bool Busy() const {
        return busy;
    }
    void Run(std::function<std::function<void()>()> job) {
        if (busy) {
            return;
        }
        if (thread.joinable()) {
            thread.join();
        }
        busy = true;
        thread = std::thread([this, job] {
            std::function<void()> apply = job();
            {
                std::scoped_lock lock{mutex};
                results.push_back(std::move(apply));
            }
            busy = false;
        });
    }
    void ApplyResults() {
        std::vector<std::function<void()>> ready;
        {
            std::scoped_lock lock{mutex};
            ready.swap(results);
        }
        for (auto& apply : ready) {
            if (apply) {
                apply();
            }
        }
    }

private:
    std::thread thread;
    std::atomic<bool> busy{false};
    std::mutex mutex;
    std::vector<std::function<void()>> results;
};

enum class Screen { Login, Hosts, Connecting, Streaming, Message };

struct Entry {
    std::string id;
    std::string name;
    std::vector<Address> addresses;
    bool home = false;   // answered on this network
    bool online = false; // the account server saw it in the last minute
};

constexpr const char* QualityNames[] = {"Automática", "Máxima", "Leve"};

class App {
public:
    int Run();

private:
    // Screens
    void LoginFrame(uint64_t pressed);
    void HostsFrame(uint64_t pressed);
    void ConnectingFrame(uint64_t pressed);
    void StreamingFrame(const PadSnapshot& pad, uint64_t pressed, int dt_ms);
    void MessageFrame(uint64_t pressed);
    void DrawLogin();
    void DrawHosts();
    void DrawConnecting();
    void DrawStreaming();
    void DrawMenu();
    void DrawMessage();

    // Actions
    void SubmitLogin();
    void LoggedIn(const std::string& user, const std::string& token, const Key32& key);
    void Logout(const std::string& why);
    void RefreshHosts();
    void BuildEntries();
    void Connect(const Entry& entry);
    void SendConnectRequest();
    void StopStreaming(const std::string& title, const std::string& why);
    void MenuChoose();
    void Toast(const std::string& text, int ms = 2500);
    uint32_t BitrateFor(bool home) const;
    bool Tapped(const Rect& rect) const;

    Ui ui;
    SDL_Window* window = nullptr;
    int output_w = ScreenW;
    int output_h = ScreenH;
    Pads pads;
    VideoDecoder video;
    AudioPlayer audio;
    Platform::Saved saved;
    std::unique_ptr<Http> http;
    std::unique_ptr<Client> client;
    Key32 key{};
    Background background;
    Screen screen = Screen::Login;
    int64_t now = 0;
    bool quit = false;

    // Touch taps on the interface (not the stream)
    bool was_touching = false;
    int tap_x = -1;
    int tap_y = -1;

    // Login
    std::string form_user;
    std::string form_password;
    bool form_create = false;
    int login_focus = 0; // 0 user, 1 password, 2 submit, 3 switch mode
    std::string login_error;
    Rect user_rect, password_rect, submit_rect, mode_rect;

    // PC list
    std::vector<HostEntry> server_hosts;
    std::vector<Entry> entries;
    int selected = 0;
    int64_t last_list_ms = -100000;
    bool list_loaded = false;
    std::string list_error;
    bool auto_connect_done = false;
    int64_t list_ready_ms = 0;
    bool user_moved = false;
    bool confirm_logout = false;
    std::vector<Rect> entry_rects;

    // Connection
    Entry target;
    int64_t connect_started = 0;
    bool request_sent_with_public = false;

    // Stream
    bool desktop = false;
    bool menu_open = false;
    int menu_choice = 0;
    bool show_stats = false;
    DesktopPointer pointer;
    KeyQueue keys;
    uint64_t previous_buttons = 0;
    int home_frames = 0;
    int64_t combo_since = 0;
    bool combo_latched = false;
    std::string toast;
    int64_t toast_until = 0;
    int64_t help_until = 0;

    // Message
    std::string message_title;
    std::string message_text;
};

bool App::Tapped(const Rect& rect) const {
    return tap_x >= 0 && rect.Contains(tap_x, tap_y);
}

void App::Toast(const std::string& text, int ms) {
    toast = text;
    toast_until = now + ms;
}

uint32_t App::BitrateFor(bool home) const {
    const bool big = output_h >= 1080;
    uint32_t kbps = home ? (big ? 25000 : 15000) : (big ? 14000 : 10000);
    if (saved.quality == 1) {
        kbps = kbps * 8 / 5;
    } else if (saved.quality == 2) {
        kbps = big ? 9000 : 6000;
    }
    return kbps;
}

// ---------- Login ----------

void App::SubmitLogin() {
    if (background.Busy()) {
        return;
    }
    std::string user;
    for (const char c : form_user) {
        if (c != ' ') {
            user += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (user.size() < 3) {
        login_error = "Digite seu usuário (o mesmo do Charis Switch no PC).";
        login_focus = 0;
        return;
    }
    if (form_password.size() < 6) {
        login_error = "A senha tem pelo menos 6 caracteres.";
        login_focus = 1;
        return;
    }
    login_error.clear();
    const std::string password = form_password;
    const bool create = form_create;
    background.Run([this, user, password, create]() -> std::function<void()> {
        const AccountKeys keys_made = DeriveAccountKeys(user, password);
        Account account{*http};
        std::string token;
        const std::string device = Platform::DeviceName();
        const std::string error =
            create ? account.Register(user, keys_made.auth_hex, token, "switch", device)
                   : account.Login(user, keys_made.auth_hex, token, "switch", device);
        const bool offline = !error.empty() && account.Unavailable();
        return [this, error, offline, create, user, token, keys_made] {
            if (!error.empty() && !offline) {
                login_error = error;
                return;
            }
            // Without the account server the login still works at home: the PC answers on the
            // home network to the same user and password; the server part is done later
            saved.auth_hex = keys_made.auth_hex;
            saved.create_pending = offline && create;
            LoggedIn(user, token, keys_made.stream_key);
        };
    });
}

void App::LoggedIn(const std::string& user, const std::string& token, const Key32& stream_key) {
    saved.user = user;
    saved.token = token;
    saved.key_hex = ToHex(stream_key.data(), stream_key.size());
    Platform::Store(saved);
    key = stream_key;
    form_password.clear();
    client = std::make_unique<Client>(
        key, Platform::DeviceName(),
        ClientCallbacks{
            [this](const uint8_t* frame, size_t size, bool keyframe) {
                video.Submit(frame, size, keyframe);
            },
            [this](const uint8_t* opus, size_t size) { audio.Play(opus, size); }});
    std::string error;
    if (!client->Open(error)) {
        message_title = "Sem rede";
        message_text = error;
        screen = Screen::Message;
        return;
    }
    client->SetDiscovery(true);
    server_hosts.clear();
    entries.clear();
    list_loaded = false;
    last_list_ms = -100000;
    screen = Screen::Hosts;
}

void App::Logout(const std::string& why) {
    if (client) {
        client->Disconnect();
        client->Close();
        client.reset();
    }
    saved.token.clear();
    saved.key_hex.clear();
    saved.auth_hex.clear();
    saved.create_pending = false;
    Platform::Store(saved);
    form_user = saved.user;
    form_password.clear();
    login_error = why;
    login_focus = form_user.empty() ? 0 : 1;
    screen = Screen::Login;
}

void App::LoginFrame(uint64_t pressed) {
    if (background.Busy()) {
        return;
    }
    if (pressed & ButtonDown) {
        login_focus = std::min(login_focus + 1, 3);
    }
    if (pressed & ButtonUp) {
        login_focus = std::max(login_focus - 1, 0);
    }
    int activate = -1;
    if (pressed & ButtonA) {
        activate = login_focus;
    }
    if (pressed & ButtonPlus) {
        activate = 2;
    }
    if (pressed & ButtonY) {
        activate = 3;
    }
    if (Tapped(user_rect)) {
        activate = 0;
    } else if (Tapped(password_rect)) {
        activate = 1;
    } else if (Tapped(submit_rect)) {
        activate = 2;
    } else if (Tapped(mode_rect)) {
        activate = 3;
    }
    if (activate >= 0) {
        login_focus = activate;
    }
    switch (activate) {
    case 0: {
        std::string text;
        if (Platform::Keyboard("Usuário da conta Charis", "Seu usuário", form_user, false, 24,
                               text)) {
            form_user = text;
            login_focus = form_password.empty() ? 1 : 2;
        }
        break;
    }
    case 1: {
        std::string text;
        if (Platform::Keyboard("Senha da conta Charis", "Sua senha", "", true, 64, text)) {
            form_password = text;
            login_focus = 2;
        }
        break;
    }
    case 2:
        SubmitLogin();
        break;
    case 3:
        form_create = !form_create;
        login_error.clear();
        break;
    default:
        break;
    }
    if (pressed & ButtonMinus) {
        quit = true;
    }
}

void App::DrawLogin() {
    ui.Page("CHARIS STREAM", form_create ? "Criar conta" : "Entre na sua conta", 196);
    ui.Paragraph(ui.body,
                 form_create ? "Escolha um usuário e uma senha. Depois entre com eles no Charis "
                               "Switch do PC e pronto: sem configurar IP."
                             : "Use a mesma conta do Charis Switch no PC. O Switch acha o PC "
                               "sozinho, em casa ou fora dela.",
                 72, 228, 1136, InkSoft);

    const auto field = [&](Rect& rect, int y, const std::string& label, const std::string& value,
                           bool focus) {
        rect = {72, y, 760, 76};
        ui.Box(rect, 20, focus ? Cream3 : Cream2);
        if (focus) {
            ui.Outline(rect, 20, Charcoal, 2);
        }
        ui.Text(ui.small, label, 96, y + 10, InkSoft);
        ui.Text(ui.bold, value, 96, y + 36, Ink);
    };
    field(user_rect, 316, "USUÁRIO", form_user.empty() ? "Toque para digitar" : form_user,
          login_focus == 0);
    field(password_rect, 408, "SENHA",
          form_password.empty() ? "Toque para digitar" : std::string(form_password.size(), '*'),
          login_focus == 1);

    submit_rect = {72, 508, 360, 70};
    ui.Box(submit_rect, 22, Charcoal);
    if (login_focus == 2) {
        ui.Outline({submit_rect.x - 4, submit_rect.y - 4, submit_rect.w + 8, submit_rect.h + 8},
                   24, InkSoft, 2);
    }
    if (background.Busy()) {
        ui.Dots(submit_rect.x + submit_rect.w / 2 - 22, submit_rect.y + 35, Cream, Now());
    } else {
        ui.Text(ui.bold, form_create ? "Criar e entrar" : "Entrar", submit_rect.x + submit_rect.w / 2,
                submit_rect.y + 20, Cream, 1);
    }
    mode_rect = {452, 508, 380, 70};
    ui.Box(mode_rect, 22, login_focus == 3 ? Cream3 : Cream2);
    if (login_focus == 3) {
        ui.Outline(mode_rect, 22, Charcoal, 2);
    }
    ui.Text(ui.bold, form_create ? "Já tenho conta" : "Criar uma conta", mode_rect.x + mode_rect.w / 2,
            mode_rect.y + 20, Ink, 1);

    if (!login_error.empty()) {
        ui.Box({872, 316, 336, 262}, 22, Danger);
        ui.Paragraph(ui.small, login_error, 896, 336, 292, Cream, 4);
    } else {
        ui.Box({872, 316, 336, 262}, 22, Cream2);
        ui.Text(ui.bold, "Como funciona", 896, 336, Ink);
        ui.Paragraph(ui.small,
                     "1. No PC, abra o Charis Switch e entre na conta. 2. Aqui, entre com a "
                     "mesma conta. 3. Escolha o PC e jogue.",
                     896, 372, 292, InkSoft, 4);
    }
    int x = 72;
    x += ui.Hint(x, ScreenH - 62, "A", "Escolher", false);
    x += ui.Hint(x, ScreenH - 62, "+", form_create ? "Criar" : "Entrar", false);
    ui.Hint(x, ScreenH - 62, "-", "Sair do app", false);
}

// ---------- PC list ----------

void App::RefreshHosts() {
    if (background.Busy() || !client) {
        return;
    }
    last_list_ms = now;
    const Platform::Saved current = saved;
    const std::string device = Platform::DeviceName();
    background.Run([this, current, device]() -> std::function<void()> {
        Account account{*http};
        std::string token = current.token;
        std::string error;
        bool expired = false;
        bool new_token = false;
        if (token.empty() && !current.auth_hex.empty()) {
            // The server was down at login: finish the login now
            error = current.create_pending
                        ? account.Register(current.user, current.auth_hex, token, "switch", device)
                        : account.Login(current.user, current.auth_hex, token, "switch", device);
            if (!error.empty() && current.create_pending && !account.Unavailable()) {
                error = account.Login(current.user, current.auth_hex, token, "switch", device);
            }
            expired = !error.empty() && !account.Unavailable();
            new_token = error.empty();
        }
        std::vector<HostEntry> hosts;
        if (error.empty()) {
            error = account.ListHosts(token, hosts, current.client_id, device);
            expired = account.LoginExpired();
            if (expired && !current.auth_hex.empty()) {
                // Old token: log in again with the saved code, once
                token.clear();
                error = account.Login(current.user, current.auth_hex, token, "switch", device);
                expired = !error.empty() && !account.Unavailable();
                new_token = error.empty();
                if (new_token) {
                    error = account.ListHosts(token, hosts, current.client_id, device);
                }
            }
        }
        return [this, error, hosts, expired, token, new_token] {
            if (expired) {
                Logout(error);
                return;
            }
            if (new_token) {
                saved.token = token;
                saved.create_pending = false;
                Platform::Store(saved);
            }
            list_error = error;
            if (error.empty()) {
                server_hosts = hosts;
            }
            list_loaded = true;
        };
    });
}

void App::BuildEntries() {
    std::vector<Entry> next;
    const std::vector<DiscoveredHost> found = client ? client->Discovered() : std::vector<DiscoveredHost>{};
    for (const DiscoveredHost& lan : found) {
        Entry entry;
        entry.id = ToHex(lan.id.data(), lan.id.size());
        entry.name = lan.name;
        entry.addresses.push_back(lan.address);
        entry.home = true;
        entry.online = true;
        next.push_back(entry);
    }
    for (const HostEntry& host : server_hosts) {
        auto it = std::find_if(next.begin(), next.end(),
                               [&](const Entry& e) { return e.id == host.id; });
        if (it == next.end()) {
            Entry entry;
            entry.id = host.id;
            entry.name = host.name;
            entry.online = host.online;
            next.push_back(entry);
            it = next.end() - 1;
        }
        for (const Address& address : host.local) {
            if (std::find(it->addresses.begin(), it->addresses.end(), address) ==
                it->addresses.end()) {
                it->addresses.push_back(address);
            }
        }
        if (host.public_address.Valid() &&
            std::find(it->addresses.begin(), it->addresses.end(), host.public_address) ==
                it->addresses.end()) {
            it->addresses.push_back(host.public_address);
        }
    }
    std::stable_sort(next.begin(), next.end(), [](const Entry& a, const Entry& b) {
        const int ra = a.home ? 0 : (a.online ? 1 : 2);
        const int rb = b.home ? 0 : (b.online ? 1 : 2);
        return ra < rb;
    });
    // Keep the same PC selected while the list changes
    const std::string selected_id =
        selected < static_cast<int>(entries.size()) ? entries[selected].id : saved.last_host;
    entries = std::move(next);
    selected = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == selected_id) {
            selected = static_cast<int>(i);
        }
    }
}

void App::HostsFrame(uint64_t pressed) {
    if (now - last_list_ms > 15000) {
        RefreshHosts(); // also tells the PCs of the account that a Switch is looking
    }
    BuildEntries();
    if (confirm_logout) {
        if (pressed & ButtonA) {
            confirm_logout = false;
            Logout("");
        } else if (pressed & (ButtonB | ButtonX)) {
            confirm_logout = false;
        }
        return;
    }
    if (pressed) {
        user_moved = true;
    }
    if (pressed & ButtonDown) {
        selected = std::min(selected + 1, std::max(0, static_cast<int>(entries.size()) - 1));
    }
    if (pressed & ButtonUp) {
        selected = std::max(selected - 1, 0);
    }
    for (size_t i = 0; i < entry_rects.size() && i < entries.size(); ++i) {
        if (Tapped(entry_rects[i])) {
            selected = static_cast<int>(i);
            user_moved = true;
            pressed |= ButtonA;
        }
    }
    if (pressed & ButtonY) {
        last_list_ms = -100000;
    }
    if (pressed & ButtonX) {
        confirm_logout = true;
    }
    if (pressed & ButtonPlus) {
        quit = true;
    }
    const bool can_connect =
        selected < static_cast<int>(entries.size()) &&
        (entries[selected].home || entries[selected].online) &&
        !entries[selected].addresses.empty();
    if ((pressed & ButtonA) && can_connect) {
        Connect(entries[selected]);
        return;
    }
    // Automated runs on a PC: connect to the first PC that can be reached
    if (std::getenv("CHARIS_AUTOCONNECT") != nullptr && !auto_connect_done && can_connect) {
        auto_connect_done = true;
        Connect(entries[selected]);
        return;
    }
    // Opening the app goes straight back to the PC used last time, when it is there
    if (!auto_connect_done && list_loaded) {
        if (list_ready_ms == 0) {
            list_ready_ms = now;
        }
        if (user_moved || now - list_ready_ms > 6000) {
            auto_connect_done = true;
        } else if (can_connect && entries[selected].id == saved.last_host &&
                   now - list_ready_ms > 1200) {
            auto_connect_done = true;
            Connect(entries[selected]);
        }
    }
}

void App::DrawHosts() {
    ui.Page("CHARIS STREAM · " + saved.user, "Seus PCs", 196);
    entry_rects.clear();
    if (entries.empty()) {
        ui.Text(ui.title, list_loaded ? "Nenhum PC ligado ainda" : "Procurando seus PCs", 72, 240,
                Ink);
        if (!list_loaded) {
            ui.Dots(72 + ui.Measure(ui.title, "Procurando seus PCs") + 24, 262, Ink, Now());
        }
        ui.Paragraph(ui.body,
                     list_error.empty()
                         ? "No PC, abra o Charis Switch e entre na conta " + saved.user +
                               ". Ele aparece aqui sozinho, em casa ou fora dela."
                         : list_error + " Em casa, o PC ainda aparece pela rede.",
                     72, 300, 1136, InkSoft);
    }
    for (size_t i = 0; i < entries.size() && i < 4; ++i) {
        const Entry& entry = entries[i];
        const bool on = static_cast<int>(i) == selected;
        const Rect rect{64, 236 + static_cast<int>(i) * 96, ScreenW - 128, 82};
        entry_rects.push_back(rect);
        ui.Box(rect, 22, on ? Charcoal : Cream2);
        ui.Text(ui.bold, entry.name, rect.x + 32, rect.y + 14, on ? Cream : Ink);
        std::string where;
        Color dot = InkSoft;
        if (entry.home) {
            where = "Em casa · pronto para jogar";
            dot = Good;
        } else if (entry.online) {
            where = "Pela internet · pronto para jogar";
            dot = Good;
        } else {
            where = "Desligado ou com o Charis Switch fechado";
            dot = SageLight;
        }
        ui.Circle(rect.x + 40, rect.y + 58, 6, dot);
        ui.Text(ui.small, where, rect.x + 56, rect.y + 47, on ? OnDarkSoft : InkSoft);
        if (entry.id == saved.last_host) {
            ui.Text(ui.small, "último usado", rect.x + rect.w - 32, rect.y + 30,
                    on ? OnDarkSoft : InkSoft, 2);
        }
    }
    if (!entries.empty() && !list_error.empty()) {
        ui.Box({64, ScreenH - 124, ScreenW - 128, 48}, 18, Cream2);
        ui.Text(ui.small, "Sem o servidor da conta agora: aparecem só os PCs da sua casa.", 92,
                ScreenH - 111, InkSoft);
    }
    int x = 72;
    x += ui.Hint(x, ScreenH - 62, "A", "Jogar", false);
    x += ui.Hint(x, ScreenH - 62, "Y", "Atualizar", false);
    x += ui.Hint(x, ScreenH - 62, "X", "Sair da conta", false);
    ui.Hint(x, ScreenH - 62, "+", "Fechar o app", false);

    if (confirm_logout) {
        ui.Shade(150);
        ui.Box({340, 220, 600, 280}, 28, Cream);
        ui.Text(ui.title, "Sair da conta?", 380, 256, Ink);
        ui.Paragraph(ui.body, "Para jogar de novo, você vai precisar entrar com a senha.", 380,
                     312, 520, InkSoft);
        ui.Hint(380, 440, "A", "Sair", false);
        ui.Hint(540, 440, "B", "Cancelar", false);
    }
}

// ---------- Connecting ----------

void App::SendConnectRequest() {
    if (!client) {
        return;
    }
    const Address public_address = client->PublicAddress();
    const Address local = client->LocalAddress();
    request_sent_with_public = public_address.Valid();
    const std::string token = saved.token;
    if (token.empty()) {
        return; // no account server right now: home network only
    }
    const std::string host_id = target.id;
    const std::string client_id = saved.client_id;
    const std::string name = Platform::DeviceName();
    std::thread([this, token, host_id, client_id, name, public_address, local] {
        Account account{*http};
        std::vector<Address> locals;
        if (local.Valid()) {
            locals.push_back(local);
        }
        account.RequestConnect(token, host_id, client_id, name, public_address, locals);
    }).detach();
}

void App::Connect(const Entry& entry) {
    target = entry;
    saved.last_host = entry.id;
    Platform::Store(saved);
    video.Reset();
    audio.Reset();
    StartRequest settings{};
    settings.width = static_cast<uint16_t>(output_w >= 1920 ? 1920 : 1280);
    settings.height = static_cast<uint16_t>(output_w >= 1920 ? 1080 : 720);
    settings.fps = 60;
    settings.bitrate_kbps = BitrateFor(entry.home);
    client->SetDiscovery(false);
    client->Connect(entry.addresses, settings);
    connect_started = now;
    // The PC opens its way to this Switch when it hears about it from the account server
    SendConnectRequest();
    screen = Screen::Connecting;
}

void App::ConnectingFrame(uint64_t pressed) {
    if (!client) {
        return;
    }
    if (!request_sent_with_public && client->PublicAddress().Valid()) {
        SendConnectRequest();
    }
    const Client::State state = client->GetState();
    if (state == Client::State::Streaming) {
        screen = Screen::Streaming;
        desktop = false;
        menu_open = false;
        pointer.Reset();
        keys.Clear();
        previous_buttons = ~0ull; // nothing counts as pressed until it is let go
        Platform::KeepAwake(true);
        Toast("Segure + e − juntos para abrir o menu", 4000);
        if (std::getenv("CHARIS_TEST_DESKTOP") != nullptr) { // automated runs on a PC
            desktop = true;
            help_until = now + 6000;
            if (const char* text = std::getenv("CHARIS_KEYBOARD")) {
                keys.Text(text);
                keys.Special(KeyEnter);
            }
        }
        return;
    }
    if (state == Client::State::Failed) {
        StopStreaming("Não deu para conectar", client->FailureReason());
        return;
    }
    if (pressed & ButtonB) {
        client->Disconnect();
        client->SetDiscovery(true);
        screen = Screen::Hosts;
        auto_connect_done = true;
    }
}

void App::DrawConnecting() {
    ui.Page("CHARIS STREAM", "Conectando", 196);
    ui.Text(ui.title, target.name, 72, 240, Ink);
    ui.Dots(72 + ui.Measure(ui.title, target.name) + 26, 262, Ink, Now());
    const int64_t elapsed = now - connect_started;
    std::string step;
    if (target.home) {
        step = "Pela rede de casa.";
    } else if (elapsed < 4000) {
        step = "Pela internet: avisando o PC pela sua conta.";
    } else {
        step = "Pela internet: abrindo o caminho entre o Switch e o PC. Pode levar até 15 "
               "segundos na primeira vez.";
    }
    ui.Paragraph(ui.body, step, 72, 300, 1136, InkSoft);
    ui.Hint(72, ScreenH - 62, "B", "Cancelar", false);
}

// ---------- Streaming ----------

void App::StopStreaming(const std::string& title, const std::string& why) {
    if (client) {
        client->Disconnect();
        client->SetDiscovery(true);
    }
    Platform::KeepAwake(false);
    video.Reset();
    audio.Reset();
    menu_open = false;
    auto_connect_done = true;
    if (title.empty()) {
        screen = Screen::Hosts;
        last_list_ms = -100000;
        return;
    }
    message_title = title;
    message_text = why;
    screen = Screen::Message;
}

void App::MenuChoose() {
    switch (menu_choice) {
    case 0:
        menu_open = false;
        break;
    case 1:
        desktop = !desktop;
        pointer.Reset();
        menu_open = false;
        if (desktop) {
            help_until = now + 6000;
            Toast("Modo PC: mouse e teclado");
        } else {
            Toast("Modo jogo: controle e toque");
        }
        break;
    case 2: {
        std::string text;
        if (Platform::Keyboard("Digitar no PC", "O texto vai para onde está o cursor no PC", "",
                               false, 400, text)) {
            keys.Text(text);
        }
        menu_open = false;
        break;
    }
    case 3:
        client->SendCommand(Type::GoHome);
        menu_open = false;
        desktop = false;
        Toast("Voltando para a tela inicial do Charis Switch");
        break;
    case 4:
        home_frames = 8;
        menu_open = false;
        break;
    case 5:
        saved.quality = (saved.quality + 1) % 3;
        Platform::Store(saved);
        client->RequestBitrate(BitrateFor(target.home));
        break;
    case 6:
        show_stats = !show_stats;
        break;
    case 7:
        StopStreaming("", "");
        break;
    default:
        break;
    }
}

void App::StreamingFrame(const PadSnapshot& pad, uint64_t pressed, int dt_ms) {
    if (!client) {
        return;
    }
    const Client::State state = client->GetState();
    if (state == Client::State::Failed) {
        StopStreaming("A conexão caiu", client->FailureReason());
        return;
    }
    if (state != Client::State::Streaming) {
        StopStreaming("", "");
        return;
    }
    if (video.TakeKeyframeRequest()) {
        client->SendCommand(Type::RequestKeyframe);
    }

    // + and − together for a moment: the Charis menu (the game does not see it)
    const bool combo = (pad.buttons & ButtonPlus) && (pad.buttons & ButtonMinus);
    if (!combo) {
        combo_latched = false; // let go: the combo can work again
    }
    combo_since = combo && !combo_latched ? (combo_since == 0 ? now : combo_since) : 0;
    if (combo_since != 0 && now - combo_since > 700) {
        menu_open = !menu_open;
        menu_choice = 0;
        combo_since = 0;
        combo_latched = true;
        previous_buttons = ~0ull;
        pressed = 0;
    }

    InputState input{};
    if (menu_open) {
        if (pressed & ButtonDown) {
            menu_choice = std::min(menu_choice + 1, 7);
        }
        if (pressed & ButtonUp) {
            menu_choice = std::max(menu_choice - 1, 0);
        }
        if (pressed & ButtonB) {
            menu_open = false;
        }
        if (pressed & ButtonA) {
            MenuChoose();
        }
        if (screen != Screen::Streaming) {
            return;
        }
        input.mode = desktop ? ModeDesktop : ModeGame;
        if (desktop) {
            pointer.Fill(input);
            input.mouse_buttons = 0;
        }
    } else if (desktop) {
        pointer.Update(pad, pressed, dt_ms);
        if (pressed & ButtonY) {
            std::string text;
            if (Platform::Keyboard("Digitar no PC", "O texto vai para onde está o cursor", "",
                                   false, 400, text)) {
                keys.Text(text);
            }
        }
        const std::pair<uint64_t, Key> shortcuts[] = {
            {ButtonB, KeyEscape},     {ButtonZR, KeyEnter},     {ButtonZL, KeyBackspace},
            {ButtonLeft, KeyLeft},    {ButtonRight, KeyRight},  {ButtonUp, KeyUp},
            {ButtonDown, KeyDown},    {ButtonL, KeyAltTab},     {ButtonR, KeyWindows},
        };
        for (const auto& [button, special] : shortcuts) {
            if (pressed & button) {
                keys.Special(special);
            }
        }
        pointer.Fill(input);
    } else {
        input.mode = ModeGame;
        input.buttons = pad.buttons;
        if (home_frames > 0) {
            input.buttons |= ButtonHome;
            --home_frames;
        }
        input.left_x = static_cast<int16_t>(pad.left_x);
        input.left_y = static_cast<int16_t>(pad.left_y);
        input.right_x = static_cast<int16_t>(pad.right_x);
        input.right_y = static_cast<int16_t>(pad.right_y);
        input.touch_count = static_cast<uint8_t>(pad.touch_count);
        for (int i = 0; i < pad.touch_count; ++i) {
            input.touches[i].id = static_cast<uint8_t>(pad.touches[i].id);
            input.touches[i].x = static_cast<uint16_t>(pad.touches[i].x * 65535 / 1279);
            input.touches[i].y = static_cast<uint16_t>(pad.touches[i].y * 65535 / 719);
        }
    }
    keys.Fill(input);
    client->SendInput(input);
}

void App::DrawStreaming() {
    SDL_SetRenderDrawColor(ui.renderer, 0, 0, 0, 255);
    SDL_RenderClear(ui.renderer);
    if (video.Present(ui.renderer) && video.Texture() != nullptr) {
        SDL_Rect all{0, 0, ScreenW, ScreenH};
        SDL_RenderCopy(ui.renderer, video.Texture(), nullptr, &all);
    } else {
        ui.Page("CHARIS STREAM", "Abrindo a tela", 196);
        ui.Text(ui.title, target.name, 72, 240, Ink);
        ui.Dots(72 + ui.Measure(ui.title, target.name) + 26, 262, Ink, Now());
    }
    if (desktop && !menu_open) {
        // Where the PC pointer is, while a finger is not on it
        const int px = static_cast<int>(pointer.X() * ScreenW);
        const int py = static_cast<int>(pointer.Y() * ScreenH);
        aacircleRGBA(ui.renderer, px, py, 13, Charcoal.r, Charcoal.g, Charcoal.b, 160);
        aacircleRGBA(ui.renderer, px, py, 12, Cream.r, Cream.g, Cream.b, 200);
    }
    if (desktop && now < help_until && !menu_open) {
        ui.Box({40, ScreenH - 98, ScreenW - 80, 66}, 22, Color{0x20, 0x21, 0x1C, 225});
        int x = 64;
        x += ui.Hint(x, ScreenH - 80, "A", "Clique", true);
        x += ui.Hint(x, ScreenH - 80, "X", "Botão direito", true);
        x += ui.Hint(x, ScreenH - 80, "Y", "Teclado", true);
        x += ui.Hint(x, ScreenH - 80, "B", "Esc", true);
        x += ui.Hint(x, ScreenH - 80, "ZR", "Enter", true);
        ui.Hint(x, ScreenH - 80, "R", "Iniciar", true);
    }
    if (show_stats && client) {
        const ClientStats stats = client->Stats();
        char line[160];
        std::snprintf(line, sizeof(line), "%.1f Mbps · perda %.1f%% · decodificação %.0f ms · %s",
                      stats.bitrate_kbps / 1000.0, stats.loss_percent, video.DecodeMs(),
                      target.home ? "em casa" : "internet");
        ui.Box({24, 20, ui.Measure(ui.small, line) + 36, 38}, 14, Color{0x20, 0x21, 0x1C, 210});
        ui.Text(ui.small, line, 42, 28, Cream);
    }
    if (now < toast_until && !menu_open) {
        const int w = ui.Measure(ui.bold, toast) + 56;
        ui.Box({(ScreenW - w) / 2, 28, w, 52}, 26, Color{0x20, 0x21, 0x1C, 230});
        ui.Text(ui.bold, toast, ScreenW / 2, 41, Cream, 1);
    }
}

void App::DrawMenu() {
    ui.Shade(140);
    ui.Box({ScreenW - 540, 0, 580, ScreenH}, 32, Charcoal);
    ui.Text(ui.small, "CHARIS STREAM · " + target.name, ScreenW - 500, 34, OnDarkSoft);
    ui.Text(ui.title, "Menu", ScreenW - 500, 58, Cream);
    const std::string items[] = {
        "Continuar",
        desktop ? "Voltar ao modo jogo" : "Usar como PC (mouse e teclado)",
        "Digitar texto",
        "Voltar ao início",
        "Botão HOME do jogo",
        std::string("Qualidade: ") + QualityNames[saved.quality],
        show_stats ? "Esconder informações" : "Mostrar informações",
        "Desconectar",
    };
    const std::string details[] = {
        "Volta para onde estava",
        desktop ? "Controle e toque vão para o jogo" : "Analógico move, A clica, Y abre o teclado",
        "Escreve no PC com o teclado do Switch",
        "Fecha o jogo e mostra seus jogos no PC",
        "Abre o menu HOME dentro do jogo",
        "A muda: Automática, Máxima ou Leve",
        "Velocidade e perda da conexão",
        "Encerra e volta para a lista de PCs",
    };
    for (int i = 0; i < 8; ++i) {
        const bool on = i == menu_choice;
        const Rect rect{ScreenW - 516, 118 + i * 66, 476, 60};
        ui.Box(rect, 18, on ? Cream : Charcoal2);
        ui.Text(ui.bold, items[i], rect.x + 22, rect.y + 6, on ? Ink : Cream);
        ui.Text(ui.small, details[i], rect.x + 22, rect.y + 33, on ? InkSoft : OnDarkSoft);
    }
    int x = ScreenW - 500;
    x += ui.Hint(x, ScreenH - 54, "A", "Escolher", true);
    ui.Hint(x, ScreenH - 54, "B", "Voltar ao jogo", true);
}

// ---------- Message ----------

void App::MessageFrame(uint64_t pressed) {
    if (pressed & (ButtonA | ButtonB)) {
        screen = client ? Screen::Hosts : Screen::Login;
        last_list_ms = -100000;
    }
}

void App::DrawMessage() {
    ui.Page("CHARIS STREAM", message_title, 196);
    ui.Paragraph(ui.body, message_text, 72, 240, 1136, InkSoft);
    ui.Hint(72, ScreenH - 62, "A", "Voltar", false);
}

// ---------- Main loop ----------

int App::Run() {
    Platform::Init();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    TTF_Init();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_DisplayMode mode{};
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w >= 1920 && Platform::Docked()) {
        output_w = 1920;
        output_h = 1080;
    }
    window = SDL_CreateWindow("Charis Stream", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              output_w, output_h, 0);
    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RenderSetLogicalSize(renderer, ScreenW, ScreenH);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    ui.Open(renderer, Platform::FontPath(false).c_str(), Platform::FontPath(true).c_str());
    video.Open();
    audio.Open();
    pads.Open();
    http = Platform::MakeHttp();
    saved = Platform::Load();
    form_user = saved.user;

    // Automated runs on a PC: CHARIS_USER and CHARIS_PASSWORD log in by themselves
    if (const char* user = std::getenv("CHARIS_USER")) {
        form_user = user;
        form_password = std::getenv("CHARIS_PASSWORD") ? std::getenv("CHARIS_PASSWORD") : "";
        form_create = std::getenv("CHARIS_CREATE") != nullptr;
        SubmitLogin();
    } else if (!saved.user.empty() && saved.key_hex.size() == 64 &&
               FromHex(saved.key_hex, key.data(), key.size())) {
        LoggedIn(saved.user, saved.token, key);
    } else {
        login_focus = form_user.empty() ? 0 : 1;
    }
    const char* screenshot = std::getenv("CHARIS_SCREENSHOT");
    const int screenshot_after = std::getenv("CHARIS_SCREENSHOT_MS")
                                     ? std::atoi(std::getenv("CHARIS_SCREENSHOT_MS"))
                                     : 6000;
    const int64_t started = Now();
    int64_t last_frame = Now();

    while (!quit && Platform::Running()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            }
        }
        now = Now();
        const int dt_ms = static_cast<int>(now - last_frame);
        last_frame = now;
        background.ApplyResults();

        const PadSnapshot pad = pads.Read();
        const uint64_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        // A finger lifted from the interface counts as a tap where it was
        tap_x = tap_y = -1;
        if (screen != Screen::Streaming) {
            static int last_x = -1;
            static int last_y = -1;
            if (pad.touch_count > 0) {
                last_x = pad.touches[0].x;
                last_y = pad.touches[0].y;
                was_touching = true;
            } else if (was_touching) {
                was_touching = false;
                tap_x = last_x;
                tap_y = last_y;
            }
        } else {
            was_touching = false;
        }

        switch (screen) {
        case Screen::Login:
            LoginFrame(pressed);
            break;
        case Screen::Hosts:
            HostsFrame(pressed);
            break;
        case Screen::Connecting:
            ConnectingFrame(pressed);
            break;
        case Screen::Streaming:
            StreamingFrame(pad, pressed, dt_ms);
            break;
        case Screen::Message:
            MessageFrame(pressed);
            break;
        }

        switch (screen) {
        case Screen::Login:
            DrawLogin();
            break;
        case Screen::Hosts:
            DrawHosts();
            break;
        case Screen::Connecting:
            DrawConnecting();
            break;
        case Screen::Streaming:
            DrawStreaming();
            if (menu_open) {
                DrawMenu();
            }
            break;
        case Screen::Message:
            DrawMessage();
            break;
        }
        SDL_RenderPresent(renderer);

        if (screenshot != nullptr && now - started > screenshot_after) {
            SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, ScreenW, ScreenH, 32,
                                                               SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels,
                                 shot->pitch);
            SDL_SaveBMP(shot, screenshot);
            SDL_FreeSurface(shot);
            std::printf("screen=%d decoded=%u entries=%zu error=%s\n", static_cast<int>(screen),
                        video.Decoded(), entries.size(),
                        (login_error + list_error + message_text).c_str());
            quit = true;
        }
#ifndef __SWITCH__
        SDL_Delay(8);
#endif
    }

    if (client) {
        client->Disconnect();
        client->Close();
        client.reset();
    }
    video.Close();
    audio.Close();
    ui.Close();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    Platform::Exit();
    return 0;
}

} // namespace

int main(int, char**) {
    App app;
    return app.Run();
}
