// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include "yuzu/game_clock_dialog.h"

namespace {

qint64 NowSeconds() {
    return QDateTime::currentSecsSinceEpoch();
}

QString FormatLong(const QDateTime& when) {
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    QString text = locale.toString(when, QStringLiteral("dddd, d 'de' MMMM 'de' yyyy '·' HH:mm"));
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

} // namespace

GameClockDialog::GameClockDialog(bool custom_enabled, qint64 offset_seconds, bool game_running,
                                 QWidget* parent)
    : QDialog(parent), offset{custom_enabled ? offset_seconds : 0} {
    setObjectName(QStringLiteral("GameClockDialog"));
    setWindowTitle(QStringLiteral("Data e hora do jogo"));
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 28, 32, 24);
    layout->setSpacing(12);

    auto* eyebrow = new QLabel(QStringLiteral("RELÓGIO DO CONSOLE"), this);
    eyebrow->setObjectName(QStringLiteral("DialogEyebrow"));
    layout->addWidget(eyebrow);

    auto* title = new QLabel(QStringLiteral("Data e hora do jogo"), this);
    title->setObjectName(QStringLiteral("DialogTitle"));
    layout->addWidget(title);

    auto* text = new QLabel(
        QStringLiteral("Alguns jogos usam o relógio do console para eventos, estações "
                       "do ano e dia e noite. Escolha a hora que o jogo vai ver."),
        this);
    text->setObjectName(QStringLiteral("DialogText"));
    text->setWordWrap(true);
    layout->addWidget(text);
    layout->addSpacing(4);

    use_pc = new QRadioButton(QStringLiteral("Usar a hora do PC"), this);
    use_custom = new QRadioButton(QStringLiteral("Escolher data e hora"), this);
    layout->addWidget(use_pc);
    layout->addWidget(use_custom);

    custom_area = new QWidget(this);
    auto* custom_layout = new QVBoxLayout(custom_area);
    custom_layout->setContentsMargins(0, 4, 0, 0);
    custom_layout->setSpacing(10);

    date_time = new QDateTimeEdit(custom_area);
    date_time->setCalendarPopup(true);
    date_time->setDisplayFormat(QStringLiteral("dd/MM/yyyy  HH:mm"));
    date_time->setMinimumDateTime(QDateTime(QDate(2000, 1, 1), QTime(0, 0)));
    date_time->setMaximumDateTime(QDateTime(QDate(2099, 12, 31), QTime(23, 59)));
    custom_layout->addWidget(date_time);

    auto* quick_row = new QHBoxLayout();
    quick_row->setSpacing(8);
    const std::pair<const char*, qint64> quick[] = {
        {"− 1 dia", -86400}, {"− 1 hora", -3600}, {"+ 1 hora", 3600}, {"+ 1 dia", 86400}};
    for (const auto& [label, seconds] : quick) {
        auto* button = new QPushButton(QString::fromUtf8(label), custom_area);
        button->setObjectName(QStringLiteral("QuickButton"));
        button->setCursor(Qt::PointingHandCursor);
        const qint64 shift = seconds;
        connect(button, &QPushButton::clicked, this, [this, shift] { ShiftBy(shift); });
        quick_row->addWidget(button);
    }
    auto* now_button = new QPushButton(QStringLiteral("Agora"), custom_area);
    now_button->setObjectName(QStringLiteral("QuickButton"));
    now_button->setCursor(Qt::PointingHandCursor);
    connect(now_button, &QPushButton::clicked, this, [this] {
        date_time->setDateTime(QDateTime::currentDateTime());
    });
    quick_row->addWidget(now_button);
    quick_row->addStretch(1);
    custom_layout->addLayout(quick_row);
    layout->addWidget(custom_area);

    preview = new QLabel(this);
    preview->setObjectName(QStringLiteral("DialogPreview"));
    preview->setWordWrap(true);
    layout->addSpacing(4);
    layout->addWidget(preview);

    if (game_running) {
        auto* note = new QLabel(
            QStringLiteral("Tem um jogo aberto. A nova hora vale na próxima vez que você "
                           "abrir o jogo."),
            this);
        note->setObjectName(QStringLiteral("DialogNote"));
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    layout->addSpacing(8);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancelar"), this);
    cancel->setObjectName(QStringLiteral("SecondaryButton"));
    auto* save = new QPushButton(QStringLiteral("Salvar"), this);
    save->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    layout->addLayout(buttons);

    (custom_enabled ? use_custom : use_pc)->setChecked(true);
    date_time->setDateTime(QDateTime::fromSecsSinceEpoch(NowSeconds() + (custom_enabled ? offset : 0)));

    connect(use_pc, &QRadioButton::toggled, this, &GameClockDialog::UpdateState);
    connect(date_time, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime& when) {
        offset = when.toSecsSinceEpoch() - NowSeconds();
        UpdateState();
    });

    // The chosen moment keeps moving forward like a real clock while the dialog is open.
    tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, &GameClockDialog::UpdateState);
    tick->start(1000);

    UpdateState();
}

bool GameClockDialog::IsCustomEnabled() const {
    return use_custom->isChecked();
}

qint64 GameClockDialog::OffsetSeconds() const {
    return use_custom->isChecked() ? offset : 0;
}

QString GameClockDialog::Describe(bool custom_enabled, qint64 offset_seconds) {
    if (!custom_enabled) {
        return QStringLiteral("hora do PC");
    }
    const QLocale locale(QLocale::Portuguese, QLocale::Brazil);
    QString text = locale.toString(QDateTime::fromSecsSinceEpoch(NowSeconds() + offset_seconds),
                                   QStringLiteral("d MMM yyyy '·' HH:mm"));
    text.remove(QLatin1Char('.'));
    return text;
}

void GameClockDialog::ShiftBy(qint64 seconds) {
    date_time->setDateTime(date_time->dateTime().addSecs(seconds));
}

void GameClockDialog::UpdateState() {
    const bool custom = use_custom->isChecked();
    custom_area->setEnabled(custom);

    const QDateTime seen = QDateTime::fromSecsSinceEpoch(NowSeconds() + (custom ? offset : 0));
    preview->setText(QStringLiteral("O jogo vai ver: ") + FormatLong(seen) +
                     (custom ? QStringLiteral("\nO relógio continua andando a partir daqui.")
                             : QString()));
}
