// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QVersionNumber>

#include "qt_common/config/uisettings.h"
#include "yuzu/charis_updater.h"
#include "yuzu/charis_version.h"

namespace {

// The Charis Google Drive folder where new installers are published
constexpr char UpdateFolderId[] = "1P2t73x12kJYaz3Xo4rmdZ7bUWmF1SInQ";
// And the account server, which takes files of any size
constexpr char UpdateServer[] = "https://charis-conta.netlify.app/atualizacoes/";

const QRegularExpression InstallerName(
    QStringLiteral("^CharisSwitch-(\\d+\\.\\d+\\.\\d+)-instalador\\.exe$"),
    QRegularExpression::CaseInsensitiveOption);

QNetworkRequest MakeRequest(const QString& url) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 CharisSwitch"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

} // namespace

CharisUpdater& CharisUpdater::Instance() {
    static CharisUpdater* instance = new CharisUpdater();
    return *instance;
}

CharisUpdater::CharisUpdater(QObject* parent) : QObject(parent) {
    net = new QNetworkAccessManager(this);
}

QString CharisUpdater::CurrentVersion() {
    // CHARIS_TEST_VERSION: automated checks of the updater pretend to be an older version
    const QString test = qEnvironmentVariable("CHARIS_TEST_VERSION");
    return test.isEmpty() ? QStringLiteral(CHARIS_SWITCH_VERSION) : test;
}

QString CharisUpdater::FolderId(const QString& link) {
    const QString text = link.trimmed();
    static const QRegularExpression folders(QStringLiteral("folders/([A-Za-z0-9_-]{10,})"));
    static const QRegularExpression id_param(QStringLiteral("[?&]id=([A-Za-z0-9_-]{10,})"));
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]{10,}$"));
    if (const auto m = folders.match(text); m.hasMatch()) {
        return m.captured(1);
    }
    if (const auto m = id_param.match(text); m.hasMatch()) {
        return m.captured(1);
    }
    return bare.match(text).hasMatch() ? text : QString();
}

void CharisUpdater::Check() {
    if (busy) {
        return;
    }
    const QString folder_id = QString::fromLatin1(UpdateFolderId);
    busy = true;
    found_version.clear();
    found_url.clear();
    open_listings = 0;
    emit Status(QStringLiteral("Procurando atualizações..."), false);
    CheckServer();
    ListFolder(folder_id, 0);
}

void CharisUpdater::Consider(const QString& version, const QString& url) {
    const QVersionNumber best = QVersionNumber::fromString(
        found_version.isEmpty() ? CurrentVersion() : found_version);
    if (QVersionNumber::fromString(version) > best) {
        found_version = version;
        found_url = url;
    }
}

// pc.txt on the account server: "versao=1.2.3" and "arquivo=CharisSwitch-1.2.3-instalador.exe"
void CharisUpdater::CheckServer() {
    ++open_listings;
    QNetworkReply* reply =
        net->get(MakeRequest(QString::fromLatin1(UpdateServer) + QStringLiteral("pc.txt")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            QString version;
            QString file;
            for (const QString& line : QString::fromUtf8(reply->readAll()).split(QLatin1Char('\n'))) {
                const QString trimmed = line.trimmed();
                if (trimmed.startsWith(QStringLiteral("versao="))) {
                    version = trimmed.mid(7);
                } else if (trimmed.startsWith(QStringLiteral("arquivo="))) {
                    file = trimmed.mid(8);
                }
            }
            if (InstallerName.match(file).hasMatch() && !version.isEmpty()) {
                Consider(version, QString::fromLatin1(UpdateServer) + file);
            }
        }
        if (--open_listings == 0) {
            FinishListing();
        }
    });
}

// Reads the public folder page; installers are compared by version and subfolders are
// searched one level down (like the "Atualizações" folder of Charis Play).
void CharisUpdater::ListFolder(const QString& folder_id, int depth) {
    ++open_listings;
    QNetworkReply* reply = net->get(
        MakeRequest(QStringLiteral("https://drive.google.com/embeddedfolderview?id=%1").arg(folder_id)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, depth] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            const QString html = QString::fromUtf8(reply->readAll());
            static const QRegularExpression entry_title(
                QStringLiteral("<div class=\"flip-entry-title\">([^<]*)</div>"));
            const QStringList entries =
                html.split(QStringLiteral("class=\"flip-entry\" id=\"entry-"));
            for (qsizetype i = 1; i < entries.size(); ++i) {
                const QString& chunk = entries[i];
                const QString id = chunk.left(chunk.indexOf(QLatin1Char('"')));
                const auto title_match = entry_title.match(chunk);
                if (id.isEmpty() || !title_match.hasMatch()) {
                    continue;
                }
                const QString name = title_match.captured(1).trimmed();
                if (chunk.contains(QStringLiteral("/drive/folders/"))) {
                    if (depth == 0) {
                        ListFolder(id, depth + 1);
                    }
                    continue;
                }
                const auto name_match = InstallerName.match(name);
                if (!name_match.hasMatch()) {
                    continue;
                }
                Consider(name_match.captured(1),
                         QStringLiteral("https://drive.usercontent.google.com/download?id=%1"
                                        "&export=download&confirm=t")
                             .arg(id));
            }
        }
        if (--open_listings == 0) {
            FinishListing();
        }
    });
}

void CharisUpdater::FinishListing() {
    busy = false;
    if (found_version.isEmpty()) {
        emit Status(QStringLiteral("Você já está na versão mais nova (%1).")
                        .arg(CurrentVersion()),
                    false);
        emit UpToDate();
        return;
    }
    emit Status(QStringLiteral("Versão %1 disponível. Você está na %2.")
                    .arg(found_version, CurrentVersion()),
                false);
    emit UpdateFound(found_version);
}

void CharisUpdater::Download() {
    if (busy || found_url.isEmpty()) {
        return;
    }
    busy = true;
    emit Status(QStringLiteral("Baixando a versão %1...").arg(found_version), false);
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/CharisSwitch");
    QDir().mkpath(dir);
    installer_path = dir + QStringLiteral("/CharisSwitch-%1-instalador.exe").arg(found_version);

    QNetworkReply* reply = net->get(MakeRequest(found_url));
    connect(reply, &QNetworkReply::downloadProgress, this, &CharisUpdater::Progress);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        busy = false;
        const QByteArray data = reply->readAll();
        // A real installer is a Windows program (starts with "MZ"); anything else is an
        // error page from Drive.
        if (reply->error() != QNetworkReply::NoError || data.size() < 1024 * 1024 ||
            !data.startsWith("MZ")) {
            emit Status(QStringLiteral("O download falhou. Confira se o arquivo está "
                                       "compartilhado e tente de novo."),
                        true);
            return;
        }
        QSaveFile file(installer_path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            emit Status(QStringLiteral("Não consegui salvar o instalador."), true);
            return;
        }
        emit Status(QStringLiteral("Pronto para instalar a versão %1.").arg(found_version),
                    false);
        emit Downloaded();
    });
}

bool CharisUpdater::Install() {
    if (installer_path.isEmpty() || !QFile::exists(installer_path)) {
        return false;
    }
    // Silent install into this same folder; the installer waits for the program to close
    // and opens it again when done. NSIS wants /D= last and without quotes.
    QProcess process;
    process.setProgram(installer_path);
    const QString install_dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
#ifdef _WIN32
    process.setNativeArguments(QStringLiteral("/S /RELAUNCH /D=%1").arg(install_dir));
#else
    process.setArguments({QStringLiteral("/S"), QStringLiteral("/RELAUNCH"),
                          QStringLiteral("/D=%1").arg(install_dir)});
#endif
    if (!process.startDetached()) {
        emit Status(QStringLiteral("Não consegui abrir o instalador."), true);
        return false;
    }
    return true;
}

CharisUpdateDialog::CharisUpdateDialog(bool game_running_, QWidget* parent)
    : QDialog(parent), game_running{game_running_} {
    setObjectName(QStringLiteral("CharisUpdateDialog"));
    setWindowTitle(QStringLiteral("Atualizações"));
    setMinimumWidth(600);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 28, 32, 24);
    layout->setSpacing(12);

    auto* eyebrow = new QLabel(QStringLiteral("ATUALIZAÇÕES"), this);
    eyebrow->setObjectName(QStringLiteral("DialogEyebrow"));
    layout->addWidget(eyebrow);
    auto* title = new QLabel(QStringLiteral("Atualizar o Charis Switch"), this);
    title->setObjectName(QStringLiteral("DialogTitle"));
    layout->addWidget(title);
    auto* text = new QLabel(
        QStringLiteral("Você está na versão %1. As versões novas vêm da pasta do Charis "
                       "Switch no Google Drive. Seus jogos, saves e configurações ficam como "
                       "estão.")
            .arg(CharisUpdater::CurrentVersion()),
        this);
    text->setObjectName(QStringLiteral("DialogText"));
    text->setWordWrap(true);
    layout->addWidget(text);


    status = new QLabel(this);
    status->setObjectName(QStringLiteral("DialogPreview"));
    status->setWordWrap(true);
    status->setVisible(false);
    layout->addWidget(status);

    progress = new QProgressBar(this);
    progress->setVisible(false);
    progress->setTextVisible(false);
    layout->addWidget(progress);

    layout->addSpacing(8);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("Fechar"), this);
    close->setObjectName(QStringLiteral("SecondaryButton"));
    check = new QPushButton(QStringLiteral("Procurar atualização"), this);
    check->setObjectName(QStringLiteral("SecondaryButton"));
    update = new QPushButton(QStringLiteral("Atualizar agora"), this);
    update->setEnabled(false);
    buttons->addWidget(close);
    buttons->addWidget(check);
    buttons->addWidget(update);
    layout->addLayout(buttons);

    auto& updater = CharisUpdater::Instance();
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(check, &QPushButton::clicked, this, [this, &updater] {
        update->setEnabled(false);
        updater.Check();
    });
    connect(update, &QPushButton::clicked, this, [this, &updater] {
        if (game_running) {
            SetStatus(QStringLiteral("Feche o jogo antes de atualizar."), true);
            return;
        }
        update->setEnabled(false);
        check->setEnabled(false);
        progress->setVisible(true);
        progress->setRange(0, 0);
        updater.Download();
    });
    connect(&updater, &CharisUpdater::Status, this, &CharisUpdateDialog::SetStatus);
    connect(&updater, &CharisUpdater::UpdateFound, this, [this](const QString&) {
        update->setEnabled(true);
    });
    connect(&updater, &CharisUpdater::Progress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(received * 1000 / total));
        }
    });
    connect(&updater, &CharisUpdater::Downloaded, this, [this, &updater] {
        if (updater.Install()) {
            emit InstallStarted();
            accept();
        } else {
            check->setEnabled(true);
        }
    });

    if (!updater.FoundVersion().isEmpty()) {
        SetStatus(QStringLiteral("Versão %1 disponível.").arg(updater.FoundVersion()), false);
        update->setEnabled(true);
    }
}

void CharisUpdateDialog::SetStatus(const QString& text, bool error) {
    status->setVisible(true);
    status->setText(text);
    status->setProperty("error", error);
    if (error) {
        progress->setVisible(false);
        check->setEnabled(true);
    }
}
