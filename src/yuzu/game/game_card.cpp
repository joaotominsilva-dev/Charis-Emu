// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include "game_card.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "yuzu/charis_covers.h"

GameCard::GameCard(QObject* parent) : QStyledItemDelegate{parent} {
    setObjectName("GameCard");
}

void GameCard::paint(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const {
    if (!index.isValid())
        return;

    if (UISettings::values.theme == std::string("charis")) {
        PaintCharis(painter, option, index);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const bool charis = false;
    const int cardCornerRadius = charis ? 18 : 10;

    const QRect cardRect = getCardRect(option, index);

    QPalette palette = option.palette;
    QColor backgroundColor = palette.window().color();
    QColor borderColor = palette.dark().color();
    QColor textColor = palette.text().color();

    if (charis) {
        backgroundColor = QColor(0xE9, 0xEA, 0xDD);
        borderColor = backgroundColor;
        textColor = QColor(0x1D, 0x1E, 0x19);
        if (option.state & QStyle::State_Selected) {
            backgroundColor = QColor(0x20, 0x21, 0x1C);
            borderColor = backgroundColor;
            textColor = QColor(0xF3, 0xF4, 0xEA);
        } else if (option.state & QStyle::State_MouseOver) {
            backgroundColor = QColor(0xDE, 0xDF, 0xD1);
            borderColor = backgroundColor;
        }
    } else if (option.state & QStyle::State_Selected) {
        // highlight on select or hover
        backgroundColor = palette.highlight().color();
        borderColor = palette.highlight().color().lighter(150);
        textColor = palette.highlightedText().color();
    } else if (option.state & QStyle::State_MouseOver) {
        backgroundColor = backgroundColor.lighter(120);
    }

    // A hidden game only shows up when "show hidden" is on; it is drawn faded with a tag.
    const u64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
    const bool marked_hidden = program_id != 0 && UISettings::values.hidden_ids.contains(program_id);
    if (marked_hidden) {
        painter->setOpacity(0.45);
    }

    painter->setBrush(backgroundColor);
    painter->setPen(QPen(borderColor, 1));
    painter->drawRoundedRect(cardRect, cardCornerRadius, cardCornerRadius);

    const u32 icon_size = UISettings::values.game_icon_size.GetValue();
    QPixmap icon_pixmap = index.data(Qt::DecorationRole).value<QPixmap>();

    QRect iconRect;
    if (!icon_pixmap.isNull()) {
        QSize scaled = icon_pixmap.size();
        scaled.scale(icon_size, icon_size, Qt::KeepAspectRatio);

        iconRect = {cardRect.left() + (cardRect.width() - scaled.width()) / 2,
                    cardRect.top() + cardMargin - 1, scaled.width(), scaled.height()};

        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

        painter->save();
        QPainterPath clip_path;
        clip_path.addRoundedRect(iconRect, cardCornerRadius, cardCornerRadius);
        painter->setClipPath(clip_path);
        painter->drawPixmap(iconRect, icon_pixmap);
        painter->restore();
    } else {
        iconRect = {cardRect.left() + cardMargin, cardRect.top() + cardMargin,
                    static_cast<int>(icon_size), static_cast<int>(icon_size)};
    }

    if (UISettings::values.show_game_name.GetValue()) {
        QRect textRect = cardRect;
        textRect.setTop(iconRect.bottom() + cardMargin);
        textRect.adjust(cardMargin, 0, -cardMargin, -cardMargin);

        QString title = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();

        painter->setPen(textColor);
        QFont font = option.font;
        font.setBold(true);
        font.setPixelSize(std::max(11.0, std::sqrt(static_cast<double>(icon_size))));
        painter->setFont(font);

        painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, title);
    }

    if (marked_hidden) {
        painter->setOpacity(1.0);
        QFont tag_font = option.font;
        tag_font.setBold(true);
        tag_font.setPixelSize(11);
        painter->setFont(tag_font);
        const QString tag = QStringLiteral("OCULTO");
        const int tag_w = QFontMetrics(tag_font).horizontalAdvance(tag) + 20;
        const QRect tag_rect(iconRect.left() + 8, iconRect.top() + 8, tag_w, 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x20, 0x21, 0x1C));
        painter->drawRoundedRect(tag_rect, 11, 11);
        painter->setPen(QColor(0xF3, 0xF4, 0xEA));
        painter->drawText(tag_rect, Qt::AlignCenter, tag);
    }

    painter->restore();
}

namespace {

QString PlayTimeText(qulonglong seconds) {
    if (seconds < 60) {
        return QStringLiteral("Ainda não jogado");
    }
    const qulonglong hours = seconds / 3600;
    const qulonglong minutes = (seconds % 3600) / 60;
    if (hours == 0) {
        return QStringLiteral("%1 min jogados").arg(minutes);
    }
    if (minutes == 0) {
        return QStringLiteral("%1 h jogadas").arg(hours);
    }
    return QStringLiteral("%1 h %2 min jogados").arg(hours).arg(minutes);
}

} // namespace

// Charis card: portrait box art (or the game icon on a soft panel), the name and play time.
void GameCard::PaintCharis(QPainter* painter, const QStyleOptionViewItem& option,
                           const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    QColor card(0xE9, 0xEA, 0xDD);
    QColor panel(0xDE, 0xDF, 0xD1);
    QColor text(0x1D, 0x1E, 0x19);
    QColor soft(0x55, 0x56, 0x4B);
    if (selected) {
        card = QColor(0x20, 0x21, 0x1C);
        panel = QColor(0x2C, 0x2D, 0x27);
        text = QColor(0xF3, 0xF4, 0xEA);
        soft = QColor(0xA9, 0xAA, 0x98);
    } else if (hovered) {
        card = QColor(0xDE, 0xDF, 0xD1);
        panel = QColor(0xD3, 0xD4, 0xC5);
    }

    const u64 program_id = index.data(GameListItemPath::ProgramIdRole).toULongLong();
    const bool marked_hidden =
        program_id != 0 && UISettings::values.hidden_ids.contains(program_id);
    if (marked_hidden) {
        painter->setOpacity(0.45);
    }

    const QRect card_rect = getCardRect(option, index);
    painter->setPen(Qt::NoPen);
    painter->setBrush(card);
    painter->drawRoundedRect(card_rect, 20, 20);

    // Cover area: 2:3 portrait, as wide as the card allows
    const int pad = 12;
    const int cover_w = card_rect.width() - pad * 2;
    const int cover_h = std::min(cover_w * 3 / 2, card_rect.height() - pad - 66);
    const QRect cover_rect(card_rect.left() + pad, card_rect.top() + pad, cover_w, cover_h);

    QPainterPath clip;
    clip.addRoundedRect(cover_rect, 14, 14);
    painter->save();
    painter->setClipPath(clip);
    const QPixmap cover = CharisCovers::Instance().Cover(program_id, cover_rect.size());
    if (!cover.isNull()) {
        painter->drawPixmap(cover_rect, cover);
    } else {
        painter->fillRect(cover_rect, panel);
        const QPixmap icon = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!icon.isNull()) {
            const int side = std::min(cover_w - 32, 200);
            const QRect icon_rect(cover_rect.center().x() - side / 2,
                                  cover_rect.center().y() - side / 2, side, side);
            QPainterPath icon_clip;
            icon_clip.addRoundedRect(icon_rect, 12, 12);
            painter->setClipPath(icon_clip);
            painter->drawPixmap(icon_rect, icon);
        }
    }
    painter->restore();

    // Name (two lines at most) and play time
    QFont title_font = option.font;
    title_font.setBold(true);
    title_font.setPixelSize(15);
    painter->setFont(title_font);
    painter->setPen(text);
    const QString title = index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();
    const QRect title_rect(card_rect.left() + pad, cover_rect.bottom() + 10, cover_w, 40);
    const QFontMetrics metrics(title_font);
    QString shown = title;
    if (metrics.boundingRect(title_rect, Qt::TextWordWrap, title).height() > title_rect.height()) {
        shown = metrics.elidedText(title, Qt::ElideRight, cover_w * 2 - 8);
    }
    QRect used_rect;
    painter->drawText(title_rect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, shown,
                      &used_rect);

    const QModelIndex time_index =
        index.sibling(index.row(), GameListModel::COLUMN_PLAY_TIME);
    const qulonglong seconds =
        time_index.data(GameListItemPlayTime::PlayTimeRole).toULongLong();
    QFont small_font = option.font;
    small_font.setPixelSize(12);
    painter->setFont(small_font);
    painter->setPen(soft);
    painter->drawText(QRect(card_rect.left() + pad, used_rect.bottom() + 4, cover_w, 18),
                      Qt::AlignLeft | Qt::AlignVCenter, PlayTimeText(seconds));

    if (marked_hidden) {
        painter->setOpacity(1.0);
        QFont tag_font = option.font;
        tag_font.setBold(true);
        tag_font.setPixelSize(11);
        painter->setFont(tag_font);
        const QString tag = QStringLiteral("OCULTO");
        const int tag_w = QFontMetrics(tag_font).horizontalAdvance(tag) + 20;
        const QRect tag_rect(cover_rect.left() + 8, cover_rect.top() + 8, tag_w, 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x20, 0x21, 0x1C));
        painter->drawRoundedRect(tag_rect, 11, 11);
        painter->setPen(QColor(0xF3, 0xF4, 0xEA));
        painter->drawText(tag_rect, Qt::AlignCenter, tag);
    }

    painter->restore();
}

QRect GameCard::getCardRect(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int cell_width = option.rect.width();

    const int card_width = cell_width - m_padding;

    int card_left, card_top, card_height;

    if (m_columns >= 1) {
        // grid mode

        // center everything in-line, such that the leftmost and rightmost cards
        // have ~ equal padding to the edge of the viewport
        // spacing between each card is larger, but equal to each other
        const int column = index.row() % m_columns;

        const int row_width = m_columns * cell_width;
        const int total_gap = row_width - cardMargin * 2 - m_columns * card_width;
        const int gap = (m_columns > 1) ? (total_gap / (m_columns - 1)) : 0;
        card_left =
            option.rect.left() - column * cell_width + cardMargin + column * (card_width + gap) + 4;

        // fill cell vertically
        card_top = option.rect.top() + cardMargin;
        card_height = option.rect.height() - cardMargin - 1;
    } else {
        // carousel mode
        card_left = option.rect.left() + cardMargin + 4;

        // the delegate itself takes up the full height, but the card itself
        // gets centered
        const int content_height = m_contentSize.height() - cardMargin;
        const int cell_height = option.rect.height();

        card_height = std::min(content_height, cell_height - cardMargin * 2) - 1;
        card_top = option.rect.top() + (cell_height - card_height) / 2;
    }

    return QRect(card_left, card_top, card_width - cardMargin, card_height);
}

bool GameCard::hitTest(const QPoint& point, const QModelIndex& index, const QWidget* widget,
                       const QRect& cellRect) const {
    QStyleOptionViewItem option;
    option.initFrom(widget);
    option.rect = cellRect;
    return getCardRect(option, index).contains(point);
}

QSize GameCard::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return m_size;
}

void GameCard::setSize(const QSize& newSize, const QSize& contentSize, const int padding,
                       const int columns) {
    m_size = newSize;
    m_contentSize = contentSize;
    m_padding = padding;
    m_columns = columns;
}
