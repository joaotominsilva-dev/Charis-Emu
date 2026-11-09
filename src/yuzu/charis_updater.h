// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QObject>
#include <QString>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;

// Updates from a shared Google Drive folder: the newest "CharisSwitch-X.Y.Z-instalador.exe"
// in it (or in one of its subfolders) is downloaded and installed silently, then the program
// reopens.
class CharisUpdater : public QObject {
    Q_OBJECT

public:
    static CharisUpdater& Instance();

    static QString CurrentVersion();
    static QString FolderId(const QString& link);

    void Check();
    void Download();
    // Runs the downloaded installer and returns true when the program should now close.
    bool Install();

    QString FoundVersion() const {
        return found_version;
    }
    bool IsBusy() const {
        return busy;
    }

signals:
    void Status(const QString& text, bool error);
    void UpdateFound(const QString& version);
    void UpToDate();
    void Progress(qint64 received, qint64 total);
    void Downloaded();

private:
    explicit CharisUpdater(QObject* parent = nullptr);
    void ListFolder(const QString& folder_id, int depth);
    void FinishListing();

    QNetworkAccessManager* net = nullptr;
    int open_listings = 0;
    bool busy = false;
    QString found_version;
    QString found_file_id;
    QString installer_path;
};

class CharisUpdateDialog : public QDialog {
    Q_OBJECT

public:
    explicit CharisUpdateDialog(bool game_running, QWidget* parent = nullptr);

signals:
    void InstallStarted();

private:
    void SetStatus(const QString& text, bool error);

    bool game_running = false;
    QLineEdit* folder = nullptr;
    QLabel* status = nullptr;
    QProgressBar* progress = nullptr;
    QPushButton* check = nullptr;
    QPushButton* update = nullptr;
};
