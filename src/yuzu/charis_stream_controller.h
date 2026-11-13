// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <QList>
#include <QObject>
#include <QPointF>
#include <QString>

#include "charis_stream/presence.h"
#include "charis_stream/protocol.h"
#include "charis_stream/sources.h"
#include "common/settings_input.h"

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}
struct CharisAccountData;

// Runs Charis Stream inside the emulator while the account is logged in: the PC stays listed
// on the account, the Switch app connects to it, gets the screen and sound, and sends back
// its controller (player 1), touch (the game's touch screen), mouse and keyboard (the PC).
class CharisStreamController : public QObject {
    Q_OBJECT

public:
    CharisStreamController(std::shared_ptr<InputCommon::InputSubsystem> input_subsystem,
                           Core::System& system, QObject* parent = nullptr);
    ~CharisStreamController() override;

    bool Start(const CharisAccountData& account, const QString& pc_name, const QString& host_id,
               QString& error);
    void Stop();
    bool IsRunning() const;
    bool IsStreaming() const;
    QString ClientName() const;
    CharisStream::Presence::Status PresenceStatus() const;

    // For the account history and the web panel: the game running on this PC
    void GameStarted(const QString& title, uint64_t program_id);
    void GameStopped();

signals:
    void SessionChanged(bool connected, const QString& client);
    void PresenceChanged();
    void LoginExpired();
    void ErrorRaised(const QString& message);
    // The Switch app asked to close the game and go back to the home screen
    void GoHomeRequested();
    // Fingers on the Switch screen in game mode: 0..1 across the PC's main monitor
    void TouchesChanged(const QList<QPointF>& points, const QList<int>& ids);

private slots:
    void OnSessionChanged(bool connected, const QString& client);

private:
    void OnInput(const CharisStream::InputState& state);
    void ReleaseGame();
    void BindPlayerOne();
    void RestorePlayerOne();

    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem;
    Core::System& system;
    std::unique_ptr<CharisStream::Http> http;
    std::unique_ptr<CharisStream::Host> host;
    std::unique_ptr<CharisStream::Presence> presence;
    std::optional<Settings::PlayerInput> saved_player;

    // Input from the Switch (host threads)
    std::mutex input_mutex;
    CharisStream::DesktopInput desktop_input;
    uint8_t last_mode = CharisStream::ModeGame;
    uint64_t last_buttons = 0;
    uint8_t last_touch_count = 0;
    CharisStream::TouchPoint last_touches[CharisStream::MaxTouches]{};

    void Report(std::function<void(CharisStream::Account&, const std::string& token)> call);

    mutable std::mutex client_mutex;
    QString client_name;
    std::string host_id;

    // History (UI thread, except where noted)
    QString game_title;
    uint64_t game_program = 0;
    qint64 game_started_ms = 0;
    std::atomic<bool> streamed_this_game{false}; // set from the host thread
    std::atomic<bool> game_running{false};
    qint64 session_started_ms = 0;               // host thread
    std::string session_via;                     // host thread
};
