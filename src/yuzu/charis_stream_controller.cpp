// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QSysInfo>

#include "charis_stream/host.h"
#include "charis_stream/sources.h"
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "yuzu/charis_stream_controller.h"

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;

std::string ButtonParam(int button) {
    return "engine:virtual_gamepad,port:0,pad:0,button:" + std::to_string(button);
}

std::string StickParam(int axis_x, int axis_y) {
    return "engine:virtual_gamepad,port:0,pad:0,axis_x:" + std::to_string(axis_x) +
           ",axis_y:" + std::to_string(axis_y) + ",deadzone:0.0,range:1.0";
}

} // namespace

CharisStreamController::CharisStreamController(
    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem_, Core::System& system_,
    QObject* parent)
    : QObject(parent), input_subsystem{std::move(input_subsystem_)}, system{system_} {
    // The host calls back on its network thread; UI and settings work happens here.
    connect(this, &CharisStreamController::SessionChanged, this,
            &CharisStreamController::OnSessionChanged, Qt::QueuedConnection);
}

CharisStreamController::~CharisStreamController() {
    Stop();
}

bool CharisStreamController::IsRunning() const {
    return host != nullptr;
}

bool CharisStreamController::Start(QString& error) {
    if (host) {
        return true;
    }
    CharisStream::HostCallbacks callbacks;
    callbacks.make_video = [] { return CharisStream::MakeScreenVideoSource(); };
    callbacks.make_audio = [] { return CharisStream::MakeLoopbackAudioSource(); };
    callbacks.on_input = [this](const CharisStream::InputState& state) { OnInput(state); };
    callbacks.on_session = [this](bool connected, const std::string& client) {
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
    auto next = std::make_unique<CharisStream::Host>(
        QSysInfo::machineHostName().toStdString(), std::move(callbacks));
    std::string start_error;
    if (!next->Start(start_error)) {
        error = QString::fromStdString(start_error);
        return false;
    }
    host = std::move(next);
    return true;
}

void CharisStreamController::Stop() {
    if (host) {
        host->Stop();
        host.reset();
    }
    RestorePlayerOne();
}

void CharisStreamController::OnSessionChanged(bool connected, const QString&) {
    if (connected) {
        BindPlayerOne();
    } else {
        RestorePlayerOne();
    }
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

void CharisStreamController::OnInput(const CharisStream::InputState& state) {
    auto* gamepad = input_subsystem->GetVirtualGamepad();
    if (gamepad == nullptr) {
        return;
    }
    const uint64_t previous = last_buttons.exchange(state.buttons);
    const uint64_t changed = previous ^ state.buttons;
    for (int bit = 0; bit < 16; ++bit) {
        if (changed & (1ull << bit)) {
            gamepad->SetButtonState(0, bit, (state.buttons >> bit) & 1);
        }
    }
    if (changed & CharisStream::ButtonHome) {
        gamepad->SetButtonState(0, VirtualButton::ButtonHome,
                                (state.buttons & CharisStream::ButtonHome) != 0);
    }
    gamepad->SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Left,
                              state.left_x / 32767.0f, state.left_y / 32767.0f);
    gamepad->SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Right,
                              state.right_x / 32767.0f, state.right_y / 32767.0f);
}
