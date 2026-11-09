// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <optional>

#include <QObject>
#include <QString>

#include "charis_stream/protocol.h"
#include "common/settings_input.h"

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}
namespace CharisStream {
class Host;
}

// Runs the Charis Stream host inside the emulator: the Switch app connects, gets the screen and
// sound, and its controller drives player 1 through the virtual gamepad while connected.
class CharisStreamController : public QObject {
    Q_OBJECT

public:
    CharisStreamController(std::shared_ptr<InputCommon::InputSubsystem> input_subsystem,
                           Core::System& system, QObject* parent = nullptr);
    ~CharisStreamController() override;

    bool Start(QString& error);
    void Stop();
    bool IsRunning() const;

signals:
    void SessionChanged(bool connected, const QString& client);
    void ErrorRaised(const QString& message);
    // The Switch app asked to close the game and go back to the home screen
    void GoHomeRequested();

private slots:
    void OnSessionChanged(bool connected, const QString& client);

private:
    void OnInput(const CharisStream::InputState& state);
    void BindPlayerOne();
    void RestorePlayerOne();

    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem;
    Core::System& system;
    std::unique_ptr<CharisStream::Host> host;
    std::optional<Settings::PlayerInput> saved_player;
    std::atomic<uint64_t> last_buttons{0};
};
