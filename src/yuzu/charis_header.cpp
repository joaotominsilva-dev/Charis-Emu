// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "yuzu/charis_header.h"

CharisHeader::CharisHeader(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("CharisHeader"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(40, 18, 40, 14);
    body_layout->setSpacing(6);

    auto* top_row = new QHBoxLayout();
    top_row->setSpacing(12);

    auto* icon = new QLabel(body);
    icon->setPixmap(QIcon(QStringLiteral(":charis/charis.png"))
                        .pixmap(QSize(30, 30)));
    top_row->addWidget(icon);

    auto* brand = new QLabel(QStringLiteral("Charis Switch"), body);
    brand->setObjectName(QStringLiteral("CharisBrand"));
    top_row->addWidget(brand);
    top_row->addStretch(1);

    update_button = new QPushButton(body);
    update_button->setObjectName(QStringLiteral("CharisUpdateButton"));
    update_button->setCursor(Qt::PointingHandCursor);
    update_button->setFocusPolicy(Qt::NoFocus);
    update_button->setVisible(false);
    connect(update_button, &QPushButton::clicked, this, &CharisHeader::UpdateClicked);
    top_row->addWidget(update_button);

    hidden_button = new QPushButton(body);
    hidden_button->setObjectName(QStringLiteral("CharisPillButton"));
    hidden_button->setCheckable(true);
    hidden_button->setCursor(Qt::PointingHandCursor);
    hidden_button->setFocusPolicy(Qt::NoFocus);
    hidden_button->setToolTip(QStringLiteral("Mostrar ou esconder os jogos que você ocultou"));
    connect(hidden_button, &QPushButton::toggled, this, [this](bool show) {
        SetHiddenCount(property("hidden_count").toInt());
        emit ShowHiddenToggled(show);
    });
    top_row->addWidget(hidden_button);
    SetHiddenCount(0);

    game_clock_button = new QPushButton(body);
    game_clock_button->setObjectName(QStringLiteral("CharisPillButton"));
    game_clock_button->setCursor(Qt::PointingHandCursor);
    game_clock_button->setFocusPolicy(Qt::NoFocus);
    game_clock_button->setToolTip(QStringLiteral("Mudar a data e a hora que os jogos veem"));
    connect(game_clock_button, &QPushButton::clicked, this, &CharisHeader::GameClockClicked);
    top_row->addWidget(game_clock_button);
    SetGameClockText(QStringLiteral("hora do PC"));

    clock = new QLabel(body);
    clock->setObjectName(QStringLiteral("CharisClock"));
    top_row->addSpacing(8);
    top_row->addWidget(clock);

    body_layout->addLayout(top_row);

    auto* greeting = new QLabel(QStringLiteral("Olá! O que vamos jogar?"), body);
    greeting->setObjectName(QStringLiteral("CharisGreeting"));
    body_layout->addWidget(greeting);

    outer->addWidget(body);

    // The rounded top of the cream sheet, with its small handle, like the Charis Play panels.
    auto* sheet_edge = new QWidget(this);
    sheet_edge->setObjectName(QStringLiteral("CharisSheetEdge"));
    sheet_edge->setAttribute(Qt::WA_StyledBackground, true);
    sheet_edge->setFixedHeight(22);
    auto* edge_layout = new QHBoxLayout(sheet_edge);
    edge_layout->setContentsMargins(0, 9, 0, 0);
    auto* handle = new QFrame(sheet_edge);
    handle->setObjectName(QStringLiteral("CharisSheetHandle"));
    handle->setFixedSize(48, 4);
    edge_layout->addStretch(1);
    edge_layout->addWidget(handle, 0, Qt::AlignTop);
    edge_layout->addStretch(1);
    outer->addWidget(sheet_edge);

    clock_timer = new QTimer(this);
    connect(clock_timer, &QTimer::timeout, this, &CharisHeader::UpdateClock);
    clock_timer->start(15000);
    UpdateClock();
}

void CharisHeader::Track(QWidget* page_, QList<QWidget*> watched_) {
    page = page_;
    watched = std::move(watched_);
    for (QWidget* widget : watched) {
        widget->installEventFilter(this);
    }
    UpdateVisibility();
}

void CharisHeader::SetThemeActive(bool active) {
    theme_active = active;
    UpdateVisibility();
}

void CharisHeader::SetGameClockText(const QString& text) {
    game_clock_button->setText(QStringLiteral("Hora do jogo: ") + text);
}

void CharisHeader::SetHiddenCount(int count) {
    setProperty("hidden_count", count);
    if (count == 0) {
        hidden_button->setText(QStringLiteral("Nenhum jogo oculto"));
    } else if (hidden_button->isChecked()) {
        hidden_button->setText(QStringLiteral("Esconder ocultos (%1)").arg(count));
    } else {
        hidden_button->setText(QStringLiteral("Ver ocultos (%1)").arg(count));
    }
    hidden_button->setEnabled(count > 0 || hidden_button->isChecked());
}

void CharisHeader::SetUpdateAvailable(const QString& version) {
    update_button->setText(QStringLiteral("Atualiza\u00E7\u00E3o %1 dispon\u00EDvel").arg(version));
    update_button->setVisible(!version.isEmpty());
}

void CharisHeader::SetShowHidden(bool show) {
    const QSignalBlocker blocker(hidden_button);
    hidden_button->setChecked(show);
    SetHiddenCount(property("hidden_count").toInt());
}

bool CharisHeader::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) {
        // Let the visibility change land before reading it.
        QTimer::singleShot(0, this, &CharisHeader::UpdateVisibility);
    }
    return QWidget::eventFilter(object, event);
}

void CharisHeader::UpdateClock() {
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    const QDateTime now = QDateTime::currentDateTime();
    QString day = locale.toString(now, QStringLiteral("ddd, d MMM"));
    day.remove(QLatin1Char('.'));
    if (!day.isEmpty()) {
        day[0] = day[0].toUpper();
    }
    clock->setText(day + QStringLiteral(" · ") + now.toString(QStringLiteral("HH:mm")));
}

void CharisHeader::UpdateVisibility() {
    bool any_shown = false;
    for (const QWidget* widget : watched) {
        any_shown = any_shown || !widget->isHidden();
    }
    if (page) {
        page->setVisible(any_shown);
    }
    setVisible(theme_active && any_shown);
}
