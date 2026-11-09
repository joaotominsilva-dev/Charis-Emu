// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

// Graphite top area of the Charis layout: brand, clock and greeting, ending in the
// rounded edge of the cream sheet that holds the game list.
class CharisHeader : public QWidget {
    Q_OBJECT

public:
    explicit CharisHeader(QWidget* parent = nullptr);

    // Shows the header only while one of the watched widgets is shown, and hides the
    // page that holds them when none are, so the render window gets the full area.
    void Track(QWidget* page, QList<QWidget*> watched);
    void SetThemeActive(bool active);

    // Text of the "game clock" button: the PC time, or the custom date the games see.
    void SetGameClockText(const QString& text);
    void SetHiddenCount(int count);
    void SetShowHidden(bool show);
    void SetUpdateAvailable(const QString& version);
    // 0 off, 1 waiting for the Switch, 2 streaming
    void SetStreamState(int state);

signals:
    void GameClockClicked();
    void UpdateClicked();
    void StreamToggled(bool on);
    void ShowHiddenToggled(bool show);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void UpdateClock();
    void UpdateVisibility();

    QLabel* clock = nullptr;
    QPushButton* game_clock_button = nullptr;
    QPushButton* hidden_button = nullptr;
    QPushButton* update_button = nullptr;
    QPushButton* stream_button = nullptr;
    QTimer* clock_timer = nullptr;
    QWidget* page = nullptr;
    QList<QWidget*> watched;
    bool theme_active = false;
};
