// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QPainter>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>
#include <QXmlStreamReader>
#include <QtConcurrent/QtConcurrentRun>
#include <quazip.h>
#include <quazipfile.h>

#include "common/fs/path_util.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/charis_covers.h"

namespace {

constexpr int IndexMaxAgeDays = 30;
constexpr int MissRetryDays = 14;

// Lowercase ASCII words only, so "Pokémon: Let's Go" and "Pokemon - Lets Go" match.
QString Normalize(const QString& title) {
    QString text = title.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        if (ch == QLatin1Char('&')) {
            out += QStringLiteral(" and ");
        } else if (ch.isLetterOrNumber() && ch.unicode() < 128) {
            out += ch.toLower();
        } else if (ch == QLatin1Char('\'')) {
            continue;
        } else {
            out += QLatin1Char(' ');
        }
    }
    return out.simplified();
}

double WordOverlap(const QString& a, const QString& b) {
    const QStringList wa = a.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList wb = b.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (wa.isEmpty() || wb.isEmpty() || wa.first() != wb.first()) {
        return 0.0;
    }
    const QSet<QString> sa(wa.begin(), wa.end());
    const QSet<QString> sb(wb.begin(), wb.end());
    const qsizetype common = QSet<QString>(sa).intersect(sb).size();
    return double(common) / double(sa.size() + sb.size() - common);
}

QStringList ArtRegions(const QString& regions) {
    QStringList out{QStringLiteral("US"), QStringLiteral("EN")};
    for (const QString& region : regions.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (region == QStringLiteral("JPN")) {
            out << QStringLiteral("JA");
        } else if (region == QStringLiteral("KOR")) {
            out << QStringLiteral("KO");
        } else if (region == QStringLiteral("CHN") || region == QStringLiteral("TWN")) {
            out << QStringLiteral("ZH");
        }
    }
    out.removeDuplicates();
    return out;
}

// Reads switchtdb.xml from the zip into "normalized title -> ID|regions" and writes the TSV.
QHash<QString, QString> BuildIndex(const QString& zip_path, const QString& tsv_path) {
    QHash<QString, QString> result;
    QuaZip zip(zip_path);
    if (!zip.open(QuaZip::mdUnzip) || !zip.setCurrentFile(QStringLiteral("switchtdb.xml"))) {
        return result;
    }
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    QXmlStreamReader xml(&file);
    QString id, regions;
    QStringList titles;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QStringLiteral("game")) {
                id.clear();
                regions.clear();
                titles.clear();
            } else if (name == QStringLiteral("id")) {
                id = xml.readElementText();
            } else if (name == QStringLiteral("region")) {
                regions = xml.readElementText();
            } else if (name == QStringLiteral("title")) {
                titles << xml.readElementText();
            }
        } else if (xml.isEndElement() && xml.name() == QStringLiteral("game") && !id.isEmpty()) {
            for (const QString& title : titles) {
                const QString key = Normalize(title);
                if (!key.isEmpty() && !result.contains(key)) {
                    result.insert(key, id + QLatin1Char('|') + regions);
                }
            }
        }
    }
    file.close();
    zip.close();

    QSaveFile out(tsv_path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&out);
        for (auto it = result.cbegin(); it != result.cend(); ++it) {
            stream << it.key() << '\t' << it.value() << '\n';
        }
        stream.flush();
        out.commit();
    }
    return result;
}

} // namespace

CharisCovers& CharisCovers::Instance() {
    static CharisCovers* instance = new CharisCovers();
    return *instance;
}

CharisCovers::CharisCovers(QObject* parent) : QObject(parent) {
    net = new QNetworkAccessManager(this);
    QDir().mkpath(Dir());
}

QString CharisCovers::Dir() const {
    return QString::fromStdString(
               Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir)) +
           QStringLiteral("/charis/covers");
}

QString CharisCovers::PathFor(u64 program_id) const {
    const QString base = Dir() + QStringLiteral("/%1").arg(program_id, 16, 16, QLatin1Char('0'));
    if (QFileInfo::exists(base + QStringLiteral(".png"))) {
        return base + QStringLiteral(".png");
    }
    return base + QStringLiteral(".jpg");
}

QString CharisCovers::MarkerPath(u64 program_id, const char* kind) const {
    return Dir() + QStringLiteral("/%1.%2")
                       .arg(program_id, 16, 16, QLatin1Char('0'))
                       .arg(QString::fromLatin1(kind));
}

bool CharisCovers::HasCover(u64 program_id) const {
    return program_id != 0 && QFileInfo::exists(PathFor(program_id));
}

QPixmap CharisCovers::Cover(u64 program_id, const QSize& size) {
    if (program_id == 0 || size.isEmpty()) {
        return {};
    }
    const QString key = QStringLiteral("%1:%2x%3").arg(program_id).arg(size.width()).arg(size.height());
    if (const auto it = scaled.constFind(key); it != scaled.cend()) {
        return *it;
    }
    if (!originals.contains(program_id)) {
        QPixmap pixmap;
        if (HasCover(program_id)) {
            pixmap.load(PathFor(program_id));
        }
        originals.insert(program_id, pixmap);
    }
    const QPixmap original = originals.value(program_id);
    if (original.isNull()) {
        return {};
    }
    // Fill the box and crop the overflow from the center, scaled once and kept.
    const QPixmap filled =
        original.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QPixmap cropped = filled.copy((filled.width() - size.width()) / 2,
                                        (filled.height() - size.height()) / 2, size.width(),
                                        size.height());
    scaled.insert(key, cropped);
    return cropped;
}

QPixmap CharisCovers::IconCover(const QPixmap& icon, const QSize& size) {
    if (icon.isNull() || size.isEmpty()) {
        return {};
    }
    const QString key = QStringLiteral("icon:%1:%2x%3")
                            .arg(icon.cacheKey())
                            .arg(size.width())
                            .arg(size.height());
    if (const auto it = scaled.constFind(key); it != scaled.cend()) {
        return *it;
    }
    QPixmap out(size);
    out.fill(QColor(0x20, 0x21, 0x1C));
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    // Shrinking to a few pixels and back gives a soft blur of the icon's colors
    const QPixmap tiny = icon.scaled(QSize(8, 8), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const QPixmap blurred = tiny.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    painter.drawPixmap((size.width() - blurred.width()) / 2, (size.height() - blurred.height()) / 2,
                       blurred);
    painter.fillRect(out.rect(), QColor(0x20, 0x21, 0x1C, 70));
    const int side = std::min(size.width(), size.height());
    const QPixmap fit = icon.scaled(QSize(side, side), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    painter.drawPixmap((size.width() - fit.width()) / 2, (size.height() - fit.height()) / 2, fit);
    painter.end();
    scaled.insert(key, out);
    return out;
}

void CharisCovers::ClearCache(u64 program_id) {
    originals.remove(program_id);
    const QString prefix = QStringLiteral("%1:").arg(program_id);
    for (auto it = scaled.begin(); it != scaled.end();) {
        it = it.key().startsWith(prefix) ? scaled.erase(it) : std::next(it);
    }
}

void CharisCovers::Request(u64 program_id, const QString& title) {
    if (program_id == 0 || !UISettings::values.charis_auto_covers.GetValue() ||
        HasCover(program_id) || pending.contains(program_id) ||
        QFileInfo::exists(MarkerPath(program_id, "off"))) {
        return;
    }
    const QFileInfo miss(MarkerPath(program_id, "none"));
    if (miss.exists() && miss.lastModified().daysTo(QDateTime::currentDateTime()) < MissRetryDays) {
        return;
    }
    pending.insert(program_id);
    queue.append({program_id, title});
    EnsureIndex();
    ProcessQueue();
}

void CharisCovers::Retry(u64 program_id, const QString& title) {
    QFile::remove(MarkerPath(program_id, "none"));
    QFile::remove(MarkerPath(program_id, "off"));
    Request(program_id, title);
}

bool CharisCovers::SetCustom(u64 program_id, const QString& image_path) {
    QImage image(image_path);
    if (program_id == 0 || image.isNull()) {
        return false;
    }
    if (image.height() > 1200) {
        image = image.scaledToHeight(1200, Qt::SmoothTransformation);
    }
    const QString base = Dir() + QStringLiteral("/%1").arg(program_id, 16, 16, QLatin1Char('0'));
    QFile::remove(base + QStringLiteral(".jpg"));
    if (!image.save(base + QStringLiteral(".png"))) {
        return false;
    }
    QFile::remove(MarkerPath(program_id, "none"));
    QFile::remove(MarkerPath(program_id, "off"));
    ClearCache(program_id);
    emit CoverChanged(program_id);
    return true;
}

void CharisCovers::Remove(u64 program_id) {
    const QString base = Dir() + QStringLiteral("/%1").arg(program_id, 16, 16, QLatin1Char('0'));
    QFile::remove(base + QStringLiteral(".jpg"));
    QFile::remove(base + QStringLiteral(".png"));
    QFile off(MarkerPath(program_id, "off"));
    if (off.open(QIODevice::WriteOnly)) {
        off.close();
    }
    ClearCache(program_id);
    emit CoverChanged(program_id);
}

void CharisCovers::LoadIndexFile() {
    QFile file(Dir() + QStringLiteral("/switchtdb.tsv"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const qsizetype tab = line.indexOf(QLatin1Char('\t'));
        if (tab > 0) {
            index.insert(line.left(tab), line.mid(tab + 1));
        }
    }
    index_ready = !index.isEmpty();
}

void CharisCovers::EnsureIndex() {
    if (index_ready || index_loading) {
        return;
    }
    const QFileInfo tsv(Dir() + QStringLiteral("/switchtdb.tsv"));
    if (tsv.exists() && tsv.lastModified().daysTo(QDateTime::currentDateTime()) < IndexMaxAgeDays) {
        LoadIndexFile();
        if (index_ready) {
            return;
        }
    }

    index_loading = true;
    QNetworkRequest request(QUrl(QStringLiteral("https://www.gametdb.com/switchtdb.zip?LANG=EN")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CharisSwitch"));
    QNetworkReply* reply = net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QString zip_path = Dir() + QStringLiteral("/switchtdb.zip");
        if (reply->error() != QNetworkReply::NoError) {
            index_loading = false;
            LoadIndexFile(); // an old index is better than none
            ProcessQueue();
            return;
        }
        QSaveFile zip(zip_path);
        if (zip.open(QIODevice::WriteOnly)) {
            zip.write(reply->readAll());
            zip.commit();
        }
        const QString tsv_path = Dir() + QStringLiteral("/switchtdb.tsv");
        auto* watcher = new QFutureWatcher<QHash<QString, QString>>(this);
        connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished, this,
                [this, watcher, zip_path] {
                    index = watcher->result();
                    watcher->deleteLater();
                    QFile::remove(zip_path);
                    index_ready = !index.isEmpty();
                    index_loading = false;
                    ProcessQueue();
                });
        watcher->setFuture(QtConcurrent::run(BuildIndex, zip_path, tsv_path));
    });
}

void CharisCovers::ProcessQueue() {
    if (fetching || index_loading || queue.isEmpty()) {
        return;
    }
    const auto [program_id, title] = queue.takeFirst();

    QString found;
    if (index_ready) {
        const QString key = Normalize(title);
        found = index.value(key);
        if (found.isEmpty() && !key.isEmpty()) {
            double best = 0.75;
            for (auto it = index.cbegin(); it != index.cend(); ++it) {
                const double score = WordOverlap(key, it.key());
                if (score > best) {
                    best = score;
                    found = it.value();
                }
            }
        }
    }
    if (found.isEmpty()) {
        QFile miss(MarkerPath(program_id, "none"));
        if (index_ready && miss.open(QIODevice::WriteOnly)) {
            miss.close();
        }
        pending.remove(program_id);
        ProcessQueue();
        return;
    }
    const qsizetype bar = found.indexOf(QLatin1Char('|'));
    FetchArt(program_id, found.left(bar), ArtRegions(found.mid(bar + 1)));
}

void CharisCovers::FetchArt(u64 program_id, QString game_id, QStringList regions) {
    if (regions.isEmpty()) {
        QFile miss(MarkerPath(program_id, "none"));
        if (miss.open(QIODevice::WriteOnly)) {
            miss.close();
        }
        pending.remove(program_id);
        fetching = false;
        ProcessQueue();
        return;
    }
    fetching = true;
    const QString region = regions.takeFirst();
    QNetworkRequest request(QUrl(QStringLiteral("https://art.gametdb.com/switch/coverM/%1/%2.jpg")
                                     .arg(region, game_id)));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("CharisSwitch"));
    QNetworkReply* reply = net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, program_id, game_id, regions] {
        reply->deleteLater();
        const QByteArray data = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QImage image;
        if (reply->error() != QNetworkReply::NoError || status != 200 ||
            !image.loadFromData(data)) {
            FetchArt(program_id, game_id, regions);
            return;
        }
        QSaveFile file(Dir() + QStringLiteral("/%1.jpg").arg(program_id, 16, 16, QLatin1Char('0')));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.commit();
        }
        ClearCache(program_id);
        pending.remove(program_id);
        fetching = false;
        emit CoverChanged(program_id);
        ProcessQueue();
    });
}
