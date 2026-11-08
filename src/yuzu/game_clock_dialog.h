// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QDateTimeEdit;
class QLabel;
class QRadioButton;
class QTimer;

// Lets the user pick the date and time games see. The choice is kept as an offset from the
// PC clock (custom_rtc_offset), so the game clock keeps running from the chosen moment.
class GameClockDialog : public QDialog {
    Q_OBJECT

public:
    GameClockDialog(bool custom_enabled, qint64 offset_seconds, bool game_running,
                    QWidget* parent = nullptr);

    bool IsCustomEnabled() const;
    qint64 OffsetSeconds() const;

    // Short text for the header button: "hora do PC" or the custom date the games see.
    static QString Describe(bool custom_enabled, qint64 offset_seconds);

private:
    void ShiftBy(qint64 seconds);
    void UpdateState();

    QRadioButton* use_pc = nullptr;
    QRadioButton* use_custom = nullptr;
    QDateTimeEdit* date_time = nullptr;
    QWidget* custom_area = nullptr;
    QLabel* preview = nullptr;
    QTimer* tick = nullptr;
    qint64 offset = 0;
};
