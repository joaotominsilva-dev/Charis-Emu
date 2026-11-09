// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QFrame>
#include <QPixmap>

#include "common/common_types.h"

class QLabel;
class QPushButton;

// "Continuar jogando" card at the top of the home screen: the last played game, one click
// (or the controller's X button) away.
class CharisContinueCard : public QFrame {
    Q_OBJECT

public:
    explicit CharisContinueCard(QWidget* parent = nullptr);

    void SetGame(const QString& title, u64 program_id, const QPixmap& icon,
                 qulonglong play_seconds);

signals:
    void PlayClicked();

private:
    QLabel* cover = nullptr;
    QLabel* title = nullptr;
    QLabel* detail = nullptr;
    QPushButton* play = nullptr;
};
