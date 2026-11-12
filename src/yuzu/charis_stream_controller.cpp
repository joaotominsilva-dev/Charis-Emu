// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <thread>

#include <QDateTime>

#include "charis_stream/host.h"
#include "charis_stream/sources.h"
#include "charis_stream/upnp.h"
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "yuzu/charis_account.h"
#include "yuzu/charis_stream_controller.h"

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
using VirtualStick = InputCommon::VirtualGamepad::VirtualStick;

std::string ButtonParam(int button) {
    return "engine:virtual_gamepad,port:0,pad:0,button:" + std::to_string(button);
}

std::string StickParam(int axis_x, int axis_y) {
    return "engine:virtual_gamepad,port:0,pad:0,axis_x:" + std::to_string(axis_x) +
           ",axis_y:" + std::to_string(axis_y) + ",deadzone:0.0,range:1.0";
}

bool SameTouches(uint8_t count_a, const CharisStream::TouchPoint* a, uint8_t count_b,
                 const CharisStream::TouchPoint* b) {
    if (count_a != count_b) {
        return false;
    }
    for (int i = 0; i < count_a; ++i) {
        if (a[i].id != b[i].id || a[i].x != b[i].x || a[i].y != b[i].y) {
            return false;
        }
    }
    return true;
}

} // namespace

CharisStreamController::CharisStreamController(
    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem_, Core::System& system_,
    QObject* parent)
    : QObject(parent), input_subsystem{std::move(input_subsystem_)}, system{system_} {
    // The host calls back on its network threads; UI and settings work happens here.
    connect(this, &CharisStreamController::SessionChanged, this,
            &CharisStreamController::OnSessionChanged, Qt::QueuedConnection);
}

CharisStreamController::~CharisStreamController() {
    Stop();
}

bool CharisStreamController::IsRunning() const {
    return host != nullptr;
}

bool CharisStreamController::IsStreaming() const {
    return host != nullptr && host->IsStreaming();
}

QString CharisStreamController::ClientName() const {
    std::scoped_lock lock{client_mutex};
    return client_name;
}

CharisStream::Presence::Status CharisStreamController::PresenceStatus() const {
    return presence ? presence->GetStatus() : CharisStream::Presence::Status{};
}

bool CharisStreamController::Start(const CharisAccountData& account, const QString& pc_name,
                                   const QString& host_id_text, QString& error) {
    const std::string host_id_hex = host_id_text.toStdString();
    if (host) {
        return true;
    }
    if (!account.IsLoggedIn()) {
        error = QStringLiteral("Entre na conta para jogar no Switch.");
        return false;
    }
    CharisStream::HostConfig config;
    config.name = pc_name.toStdString();
    config.key = account.stream_key;
    if (!CharisStream::FromHex(host_id_hex, config.host_id.data(), config.host_id.size())) {
        error = QStringLiteral("A identificação deste PC está corrompida.");
        return false;
    }

    CharisStream::HostCallbacks callbacks;
    callbacks.make_video = [] { return CharisStream::MakeScreenVideoSource(); };
    callbacks.make_audio = [] { return CharisStream::MakeLoopbackAudioSource(); };
    callbacks.on_input = [this](const CharisStream::InputState& state) { OnInput(state); };
    callbacks.on_session = [this](bool connected, const std::string& client) {
        // The account history: who played, for how long, at home or over the internet
        if (connected) {
            session_started_ms = QDateTime::currentMSecsSinceEpoch();
            const CharisStream::Address peer = host ? host->SessionPeer() : CharisStream::Address{};
            session_via = peer.Valid() && CharisStream::IsPrivateIp(peer.ip) ? "casa" : "internet";
            streamed_this_game = true;
        } else if (session_started_ms != 0) {
            const int seconds =
                static_cast<int>((QDateTime::currentMSecsSinceEpoch() - session_started_ms) / 1000);
            session_started_ms = 0;
            const std::string via = session_via;
            QMetaObject::invokeMethod(this, [this, client, via, seconds] {
                const std::string game = game_title.toStdString();
                Report([client, via, seconds, game, id = host_id](CharisStream::Account& account,
                                                                   const std::string& token) {
                    account.ReportSession(token, id, client, via, seconds, game);
                });
            }, Qt::QueuedConnection);
        }
        {
            std::scoped_lock lock{input_mutex};
            desktop_input.Release();
            desktop_input = {};
            last_mode = CharisStream::ModeGame;
            last_buttons = 0;
            last_touch_count = 0;
        }
        {
            std::scoped_lock lock{client_mutex};
            client_name = connected ? QString::fromStdString(client) : QString();
        }
        emit TouchesChanged({}, {});
        emit SessionChanged(connected, QString::fromStdString(client));
    };
    callbacks.on_error = [this](const std::string& message) {
        emit ErrorRaised(QString::fromStdString(message));
    };
    callbacks.on_command = [this](CharisStream::Type command) {
        if (command == CharisStream::Type::GoHome) {
            emit GoHomeRequested();
        }
    };
    host_id = host_id_hex;
    auto next = std::make_unique<CharisStream::Host>(std::move(config), std::move(callbacks));
    std::string start_error;
    if (!next->Start(start_error)) {
        error = QString::fromStdString(start_error);
        return false;
    }
    host = std::move(next);
    http = CharisStream::MakeSystemHttp();
    if (http) {
        CharisStream::Credentials credentials;
        credentials.user = account.user.toStdString();
        credentials.auth_hex = account.auth_hex.toStdString();
        credentials.token = account.token.toStdString();
        credentials.create_account = account.create_pending;
        presence = std::make_unique<CharisStream::Presence>(
            *http, *host, credentials, host_id_hex, pc_name.toStdString(),
            [this](const CharisStream::Presence::Status& status) {
                emit PresenceChanged();
                if (status.login_expired) {
                    emit LoginExpired();
                }
            },
            [this](const std::string& token) {
                QMetaObject::invokeMethod(this, [token] {
                    CharisAccountStore::SaveToken(QString::fromStdString(token));
                }, Qt::QueuedConnection);
            });
        presence->SetGame(game_title.toStdString());
        presence->Start();
    }
    return true;
}

void CharisStreamController::Stop() {
    if (presence) {
        presence->Stop();
        presence.reset();
    }
    if (host) {
        host->Stop();
        host.reset();
    }
    http.reset();
    {
        std::scoped_lock lock{input_mutex};
        desktop_input.Release();
    }
    RestorePlayerOne();
    emit PresenceChanged();
}

void CharisStreamController::OnSessionChanged(bool connected, const QString&) {
    if (connected) {
        BindPlayerOne();
    } else {
        RestorePlayerOne();
    }
}

void CharisStreamController::Report(
    std::function<void(CharisStream::Account&, const std::string& token)> call) {
    const std::string token = presence ? presence->Token() : std::string{};
    if (token.empty()) {
        return; // not on the account server (yet): nothing to keep the history in
    }
    std::thread([call = std::move(call), token] {
        if (auto report_http = CharisStream::MakeSystemHttp()) {
            CharisStream::Account account{*report_http};
            call(account, token);
        }
    }).detach();
}

void CharisStreamController::GameStarted(const QString& title, uint64_t program_id) {
    game_title = title;
    game_program = program_id;
    game_started_ms = QDateTime::currentMSecsSinceEpoch();
    streamed_this_game = IsStreaming();
    if (presence) {
        presence->SetGame(title.toStdString());
    }
}

void CharisStreamController::GameStopped() {
    if (game_title.isEmpty()) {
        return;
    }
    const int seconds =
        static_cast<int>((QDateTime::currentMSecsSinceEpoch() - game_started_ms) / 1000);
    const std::string title = game_title.toStdString();
    const std::string program =
        game_program != 0 ? QStringLiteral("%1").arg(game_program, 16, 16, QLatin1Char('0')).toStdString()
                          : std::string{};
    const bool streamed = streamed_this_game;
    game_title.clear();
    game_program = 0;
    if (presence) {
        presence->SetGame({});
    }
    if (seconds < 30) {
        return; // opened and closed: not worth a line in the history
    }
    Report([title, program, seconds, streamed, id = host_id](CharisStream::Account& account,
                                                              const std::string& token) {
        account.ReportGame(token, id, title, program, seconds, streamed);
    });
}

// Player 1 becomes a Pro Controller read from the virtual gamepad; the old mapping is kept
// and put back when the Switch disconnects.
void CharisStreamController::BindPlayerOne() {
    auto& players = Settings::values.players.GetValue();
    if (!saved_player) {
        saved_player = players[0];
    }
    auto& player = players[0];
    player.connected = true;
    player.controller_type = Settings::ControllerType::ProController;
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        player.buttons[i].clear();
    }
    for (int i = Settings::NativeButton::A; i <= Settings::NativeButton::DDown; ++i) {
        player.buttons[i] = ButtonParam(i); // same order as the virtual gamepad
    }
    player.buttons[Settings::NativeButton::Home] =
        ButtonParam(static_cast<int>(VirtualButton::ButtonHome));
    player.buttons[Settings::NativeButton::Screenshot] =
        ButtonParam(static_cast<int>(VirtualButton::ButtonCapture));
    player.analogs[Settings::NativeAnalog::LStick] = StickParam(0, 1);
    player.analogs[Settings::NativeAnalog::RStick] = StickParam(2, 3);
    system.HIDCore().ReloadInputDevices();
}

void CharisStreamController::RestorePlayerOne() {
    if (!saved_player) {
        return;
    }
    Settings::values.players.GetValue()[0] = *saved_player;
    saved_player.reset();
    if (auto* gamepad = input_subsystem->GetVirtualGamepad()) {
        gamepad->ResetControllers();
    }
    system.HIDCore().ReloadInputDevices();
}

// Lets go of everything held in game mode (the Switch moved to desktop mode).
void CharisStreamController::ReleaseGame() {
    if (auto* gamepad = input_subsystem->GetVirtualGamepad()) {
        gamepad->ResetControllers();
    }
    last_buttons = 0;
    if (last_touch_count != 0) {
        last_touch_count = 0;
        emit TouchesChanged({}, {});
    }
}

void CharisStreamController::OnInput(const CharisStream::InputState& state) {
    std::scoped_lock lock{input_mutex};
    const uint8_t mode = state.mode == CharisStream::ModeDesktop ? CharisStream::ModeDesktop
                                                                 : CharisStream::ModeGame;
    if (mode != last_mode) {
        if (mode == CharisStream::ModeDesktop) {
            ReleaseGame();
        } else {
            desktop_input.Release();
        }
        last_mode = mode;
    }

    // Typed text and special keys work in both modes (the emulator's own text boxes too)
    desktop_input.Apply(state, mode == CharisStream::ModeDesktop);
    if (mode == CharisStream::ModeDesktop) {
        return;
    }

    if (auto* gamepad = input_subsystem->GetVirtualGamepad()) {
        const uint64_t changed = last_buttons ^ state.buttons;
        for (int bit = 0; bit < 16; ++bit) {
            if (changed & (1ull << bit)) {
                gamepad->SetButtonState(0, bit, (state.buttons >> bit) & 1);
            }
        }
        if (changed & CharisStream::ButtonHome) {
            gamepad->SetButtonState(0, VirtualButton::ButtonHome,
                                    (state.buttons & CharisStream::ButtonHome) != 0);
        }
        last_buttons = state.buttons;
        gamepad->SetStickPosition(0, VirtualStick::Left, state.left_x / 32767.0f,
                                  state.left_y / 32767.0f);
        gamepad->SetStickPosition(0, VirtualStick::Right, state.right_x / 32767.0f,
                                  state.right_y / 32767.0f);
    }

    const uint8_t touch_count = std::min<uint8_t>(state.touch_count, CharisStream::MaxTouches);
    if (!SameTouches(touch_count, state.touches, last_touch_count, last_touches)) {
        QList<QPointF> points;
        QList<int> ids;
        for (int i = 0; i < touch_count; ++i) {
            points.append(QPointF(state.touches[i].x / 65535.0, state.touches[i].y / 65535.0));
            ids.append(state.touches[i].id);
        }
        std::memcpy(last_touches, state.touches, sizeof(last_touches));
        last_touch_count = touch_count;
        emit TouchesChanged(points, ids);
    }
}
