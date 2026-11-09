// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QSize>
#include <QString>

#include "common/common_types.h"

class QNetworkAccessManager;

// Box art for the home screen. Covers live in <data>/charis/covers as <program id>.jpg/.png.
// Missing ones are looked up by game name in GameTDB's Switch database and downloaded; the
// user can also pick an image by hand.
class CharisCovers : public QObject {
    Q_OBJECT

public:
    static CharisCovers& Instance();

    // Cover scaled to fill (and cropped to) the given size, or a null pixmap when there is none.
    QPixmap Cover(u64 program_id, const QSize& size);
    bool HasCover(u64 program_id) const;

    // For games without box art: the game icon filling the box (cropped from the center if the
    // box is not square), scaled once and kept.
    QPixmap IconCover(const QPixmap& icon, const QSize& size);

    // Asks for a cover; when automatic covers are on and none is stored, it gets looked up.
    void Request(u64 program_id, const QString& title);
    // Looks the game up again even if an earlier lookup found nothing.
    void Retry(u64 program_id, const QString& title);
    bool SetCustom(u64 program_id, const QString& image_path);
    // Deletes the cover and stops automatic lookups for this game.
    void Remove(u64 program_id);

signals:
    void CoverChanged(u64 program_id);

private:
    explicit CharisCovers(QObject* parent = nullptr);

    QString Dir() const;
    QString PathFor(u64 program_id) const;
    QString MarkerPath(u64 program_id, const char* kind) const;
    void ClearCache(u64 program_id);

    void EnsureIndex();
    void LoadIndexFile();
    void ProcessQueue();
    void FetchArt(u64 program_id, QString game_id, QStringList regions);

    QNetworkAccessManager* net = nullptr;
    QHash<QString, QString> index; // normalized title -> "ID|REGION,REGION"
    bool index_ready = false;
    bool index_loading = false;
    QList<QPair<u64, QString>> queue;
    QSet<u64> pending;
    bool fetching = false;
    QHash<u64, QPixmap> originals;
    mutable QHash<u64, bool> has_cover;
    QHash<QString, QPixmap> scaled;
};
