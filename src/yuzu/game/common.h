// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include <QDir>
#include <QStandardItem>
#include <QStringList>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"

namespace Yuzu {

inline QString HiddenKey(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toLower();
}

// Hidden by file (one entry), or, from older versions, by game (every entry of that game)
inline bool IsEntryMarkedHidden(u64 program_id, const QString& path) {
    if (program_id != 0 && UISettings::values.hidden_ids.contains(program_id)) {
        return true;
    }
    const QString key = HiddenKey(path);
    return std::any_of(UISettings::values.hidden_paths.cbegin(),
                       UISettings::values.hidden_paths.cend(),
                       [&](const QString& hidden) { return HiddenKey(hidden) == key; });
}

inline bool IsGameMarkedHidden(const QStandardItem* item) {
    return IsEntryMarkedHidden(item->data(GameListItemPath::ProgramIdRole).toULongLong(),
                               item->data(GameListItemPath::FullPathRole).toString());
}

// Hidden games stay out of the home screen unless the user asked to see them.
inline bool IsGameHidden(const QStandardItem* item) {
    return !UISettings::values.show_hidden_games.GetValue() && IsGameMarkedHidden(item);
}

inline bool ContainsAllWords(const QString& haystack, const QString& userinput) {
    const QStringList userinput_split = userinput.split(QLatin1Char{' '}, Qt::SkipEmptyParts);
    return std::all_of(userinput_split.begin(), userinput_split.end(), [&haystack](const QString& s) {
        return haystack.contains(s);
    });
}

inline bool FilterMatches(const QString& filter, const QStandardItem* item) {
    if (filter.isEmpty())
        return true;

    auto const sluggify = [](const QString& s) {
        QString o(s.normalized(QString::NormalizationForm_D).toLower());
        return o.replace(QRegularExpression(QStringLiteral("[^a-z\\s]")), QStringLiteral(""));
    };

    const auto norm_filter = sluggify(filter);
    const auto program_id = item->data(GameListItemPath::ProgramIdRole).toULongLong();
    const QString file_path = sluggify(item->data(GameListItemPath::FullPathRole).toString());
    const QString file_title = sluggify(item->data(GameListItemPath::TitleRole).toString());
    const QString file_program_id = QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char{'0'});
    const QString file_name = sluggify(file_path.mid(file_path.lastIndexOf(QLatin1Char{'/'}) + 1) + QLatin1Char{' '} + file_title);
    return (ContainsAllWords(file_name, norm_filter)
        || ContainsAllWords(file_title, norm_filter))
        || (file_program_id.size() == 16 && file_program_id.contains(norm_filter));
}

} // namespace Yuzu
