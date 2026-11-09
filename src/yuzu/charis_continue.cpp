// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include "yuzu/charis_continue.h"
#include "yuzu/charis_covers.h"

namespace {

constexpr QSize CoverSize{72, 108};

QPixmap Rounded(const QPixmap& source, const QSize& size, int radius) {
    QPixmap out(size);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, size.width(), size.height()), radius, radius);
    painter.setClipPath(path);
    painter.fillRect(out.rect(), QColor(0xDE, 0xDF, 0xD1));
    if (!source.isNull()) {
        const QPixmap fit = source.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawPixmap((size.width() - fit.width()) / 2, (size.height() - fit.height()) / 2,
                           fit);
    }
    return out;
}

QString Detail(qulonglong seconds) {
    if (seconds < 60) {
        return QStringLiteral("Ainda sem tempo de jogo registrado");
    }
    const qulonglong hours = seconds / 3600;
    const qulonglong minutes = (seconds % 3600) / 60;
    if (hours == 0) {
        return QStringLiteral("%1 min jogados").arg(minutes);
    }
    return QStringLiteral("%1 h %2 min jogados").arg(hours).arg(minutes);
}

} // namespace

CharisContinueCard::CharisContinueCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("CharisContinue"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 12, 18, 12);
    layout->setSpacing(16);

    cover = new QLabel(this);
    cover->setFixedSize(CoverSize);
    layout->addWidget(cover);

    auto* texts = new QVBoxLayout();
    texts->setSpacing(4);
    auto* eyebrow = new QLabel(QStringLiteral("CONTINUAR JOGANDO"), this);
    eyebrow->setObjectName(QStringLiteral("ContinueEyebrow"));
    title = new QLabel(this);
    title->setObjectName(QStringLiteral("ContinueTitle"));
    detail = new QLabel(this);
    detail->setObjectName(QStringLiteral("ContinueDetail"));
    texts->addStretch(1);
    texts->addWidget(eyebrow);
    texts->addWidget(title);
    texts->addWidget(detail);
    texts->addStretch(1);
    layout->addLayout(texts, 1);

    play = new QPushButton(QStringLiteral("Jogar  ·  X"), this);
    play->setObjectName(QStringLiteral("ContinuePlay"));
    play->setCursor(Qt::PointingHandCursor);
    play->setFocusPolicy(Qt::NoFocus);
    connect(play, &QPushButton::clicked, this, &CharisContinueCard::PlayClicked);
    layout->addWidget(play, 0, Qt::AlignVCenter);
}

void CharisContinueCard::SetGame(const QString& game_title, u64 program_id, const QPixmap& icon,
                                 qulonglong play_seconds) {
    title->setText(game_title);
    detail->setText(Detail(play_seconds));
    const QPixmap box = CharisCovers::Instance().Cover(program_id, CoverSize);
    cover->setPixmap(Rounded(box.isNull() ? icon : box, CoverSize, 10));
}
