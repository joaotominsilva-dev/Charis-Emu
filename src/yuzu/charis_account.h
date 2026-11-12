// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QString>

#include "charis_stream/crypto.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class CharisStreamController;

// The Charis account on this PC: the same login the Switch app uses. Kept in the Charis data
// folder; the login token and the stream key are sealed to the Windows user (DPAPI).
struct CharisAccountData {
    QString user;
    QString token;
    CharisStream::Key32 stream_key{};

    bool IsLoggedIn() const {
        return !user.isEmpty() && !token.isEmpty();
    }
};

namespace CharisAccountStore {
CharisAccountData Load();
void Save(const CharisAccountData& account);
void Clear();
// Stable id of this PC on the account (16 hex characters), made once
QString HostId();
QString PcName();
void SetPcName(const QString& name);
// Whether the Switch may connect to this PC while Charis Switch is open
bool Available();
void SetAvailable(bool available);
} // namespace CharisAccountStore

// Log in or create the account, then see whether the Switch can reach this PC.
class CharisAccountDialog : public QDialog {
    Q_OBJECT

public:
    CharisAccountDialog(CharisStreamController* controller, QWidget* parent = nullptr);

signals:
    void LoggedIn(const CharisAccountData& account);
    void LoggedOut();
    void AvailabilityChanged(bool available);
    void PcNameChanged(const QString& name);

private:
    void ShowLogin();
    void ShowStatus();
    void SetCreateMode(bool create);
    void Submit();
    void UpdateStatus();
    void SetMessage(const QString& text, bool error);

    CharisStreamController* controller;
    QStackedWidget* pages = nullptr;

    // Login page
    QLabel* login_title = nullptr;
    QLabel* login_text = nullptr;
    QLineEdit* user_edit = nullptr;
    QLineEdit* password_edit = nullptr;
    QLineEdit* confirm_edit = nullptr;
    QLabel* login_message = nullptr;
    QPushButton* mode_button = nullptr;
    QPushButton* submit_button = nullptr;
    bool create_mode = false;
    bool busy = false;

    // Status page
    QLabel* status_title = nullptr;
    QLabel* status_preview = nullptr;
    QLabel* status_detail = nullptr;
    QCheckBox* available_box = nullptr;
    QLineEdit* name_edit = nullptr;
};

Q_DECLARE_METATYPE(CharisAccountData)
