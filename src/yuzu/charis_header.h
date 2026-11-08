// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QWidget>

class QLabel;
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

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void UpdateClock();
    void UpdateVisibility();

    QLabel* clock = nullptr;
    QTimer* clock_timer = nullptr;
    QWidget* page = nullptr;
    QList<QWidget*> watched;
    bool theme_active = false;
};
