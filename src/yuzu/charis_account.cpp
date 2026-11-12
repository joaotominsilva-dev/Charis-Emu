// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include <QCheckBox>
#include <QDir>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

#include "charis_stream/account.h"
#include "charis_stream/crypto.h"
#include "charis_stream/sources.h"
#include "common/fs/path_util.h"
#include "yuzu/charis_account.h"
#include "yuzu/charis_stream_controller.h"

namespace {

QString AccountFile() {
    const QString dir =
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir)) +
        QStringLiteral("/charis");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/account.ini");
}

// Secrets are sealed to this Windows user, so copying the file to another PC is useless.
QString Seal(const QByteArray& plain) {
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (CryptProtectData(&in, L"Charis", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                         &out)) {
        const QByteArray sealed(reinterpret_cast<const char*>(out.pbData),
                                static_cast<int>(out.cbData));
        LocalFree(out.pbData);
        return QStringLiteral("dpapi:") + QString::fromLatin1(sealed.toBase64());
    }
#endif
    return QStringLiteral("plain:") + QString::fromLatin1(plain.toBase64());
}

QByteArray Unseal(const QString& text) {
    const int colon = text.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return {};
    }
    const QString kind = text.left(colon);
    const QByteArray data = QByteArray::fromBase64(text.mid(colon + 1).toLatin1());
    if (kind == QStringLiteral("plain")) {
        return data;
    }
#ifdef _WIN32
    if (kind == QStringLiteral("dpapi")) {
        DATA_BLOB in{static_cast<DWORD>(data.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(data.data()))};
        DATA_BLOB out{};
        if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                               CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            const QByteArray plain(reinterpret_cast<const char*>(out.pbData),
                                   static_cast<int>(out.cbData));
            LocalFree(out.pbData);
            return plain;
        }
    }
#endif
    return {};
}

QString CanonicalUser(const QString& text) {
    QString user = text.trimmed().toLower();
    user.remove(QLatin1Char(' '));
    return user;
}

struct LoginResult {
    QString error;
    CharisAccountData account;
};

LoginResult RunLogin(const QString& user, const QString& password, bool create) {
    LoginResult result;
    const auto http = CharisStream::MakeSystemHttp();
    if (!http) {
        result.error = QStringLiteral("Este sistema não consegue acessar a internet.");
        return result;
    }
    const CharisStream::AccountKeys keys =
        CharisStream::DeriveAccountKeys(user.toStdString(), password.toStdString());
    CharisStream::Account account{*http};
    std::string token;
    const std::string error = create ? account.Register(user.toStdString(), keys.auth_hex, token)
                                     : account.Login(user.toStdString(), keys.auth_hex, token);
    if (!error.empty()) {
        result.error = QString::fromStdString(error);
        return result;
    }
    result.account.user = user;
    result.account.token = QString::fromStdString(token);
    result.account.stream_key = keys.stream_key;
    return result;
}

void Repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

namespace CharisAccountStore {

CharisAccountData Load() {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    CharisAccountData account;
    account.user = settings.value(QStringLiteral("account/user")).toString();
    account.token =
        QString::fromLatin1(Unseal(settings.value(QStringLiteral("account/token")).toString()));
    const QByteArray key = Unseal(settings.value(QStringLiteral("account/key")).toString());
    if (key.size() == static_cast<int>(account.stream_key.size())) {
        std::memcpy(account.stream_key.data(), key.constData(), account.stream_key.size());
    } else {
        account.token.clear();
    }
    return account;
}

void Save(const CharisAccountData& account) {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("account/user"), account.user);
    settings.setValue(QStringLiteral("account/token"), Seal(account.token.toLatin1()));
    settings.setValue(
        QStringLiteral("account/key"),
        Seal(QByteArray(reinterpret_cast<const char*>(account.stream_key.data()),
                        static_cast<int>(account.stream_key.size()))));
}

void Clear() {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    settings.remove(QStringLiteral("account"));
}

QString HostId() {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    QString id = settings.value(QStringLiteral("pc/id")).toString();
    uint8_t bytes[8];
    if (id.size() != 16 || !CharisStream::FromHex(id.toStdString(), bytes, sizeof(bytes))) {
        CharisStream::RandomBytes(bytes, sizeof(bytes));
        id = QString::fromStdString(CharisStream::ToHex(bytes, sizeof(bytes)));
        settings.setValue(QStringLiteral("pc/id"), id);
    }
    return id;
}

QString PcName() {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    const QString name = settings.value(QStringLiteral("pc/name")).toString().trimmed();
    return name.isEmpty() ? QSysInfo::machineHostName() : name;
}

void SetPcName(const QString& name) {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("pc/name"), name.trimmed().left(40));
}

bool Available() {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    return settings.value(QStringLiteral("pc/available"), true).toBool();
}

void SetAvailable(bool available) {
    QSettings settings(AccountFile(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("pc/available"), available);
}

} // namespace CharisAccountStore

CharisAccountDialog::CharisAccountDialog(CharisStreamController* controller_, QWidget* parent)
    : QDialog(parent), controller{controller_} {
    setObjectName(QStringLiteral("CharisAccountDialog"));
    setWindowTitle(QStringLiteral("Jogar no Switch"));
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(32, 28, 32, 24);
    pages = new QStackedWidget(this);
    layout->addWidget(pages);

    // Login page
    auto* login = new QWidget(pages);
    auto* login_layout = new QVBoxLayout(login);
    login_layout->setContentsMargins(0, 0, 0, 0);
    login_layout->setSpacing(12);
    auto* eyebrow = new QLabel(QStringLiteral("CHARIS STREAM"), login);
    eyebrow->setObjectName(QStringLiteral("DialogEyebrow"));
    login_layout->addWidget(eyebrow);
    login_title = new QLabel(login);
    login_title->setObjectName(QStringLiteral("DialogTitle"));
    login_layout->addWidget(login_title);
    login_text = new QLabel(login);
    login_text->setObjectName(QStringLiteral("DialogText"));
    login_text->setWordWrap(true);
    login_layout->addWidget(login_text);
    login_layout->addSpacing(4);

    user_edit = new QLineEdit(login);
    user_edit->setPlaceholderText(QStringLiteral("Usuário"));
    user_edit->setMaxLength(24);
    password_edit = new QLineEdit(login);
    password_edit->setPlaceholderText(QStringLiteral("Senha"));
    password_edit->setEchoMode(QLineEdit::Password);
    confirm_edit = new QLineEdit(login);
    confirm_edit->setPlaceholderText(QStringLiteral("Repita a senha"));
    confirm_edit->setEchoMode(QLineEdit::Password);
    login_layout->addWidget(user_edit);
    login_layout->addWidget(password_edit);
    login_layout->addWidget(confirm_edit);

    login_message = new QLabel(login);
    login_message->setObjectName(QStringLiteral("DialogPreview"));
    login_message->setWordWrap(true);
    login_layout->addWidget(login_message);
    login_layout->addSpacing(8);

    auto* login_buttons = new QHBoxLayout();
    mode_button = new QPushButton(login);
    mode_button->setObjectName(QStringLiteral("SecondaryButton"));
    mode_button->setCursor(Qt::PointingHandCursor);
    auto* close_login = new QPushButton(QStringLiteral("Agora não"), login);
    close_login->setObjectName(QStringLiteral("SecondaryButton"));
    submit_button = new QPushButton(login);
    submit_button->setDefault(true);
    login_buttons->addWidget(mode_button);
    login_buttons->addStretch(1);
    login_buttons->addWidget(close_login);
    login_buttons->addWidget(submit_button);
    login_layout->addLayout(login_buttons);
    pages->addWidget(login);

    connect(mode_button, &QPushButton::clicked, this, [this] { SetCreateMode(!create_mode); });
    connect(close_login, &QPushButton::clicked, this, &QDialog::reject);
    connect(submit_button, &QPushButton::clicked, this, &CharisAccountDialog::Submit);
    connect(password_edit, &QLineEdit::returnPressed, this, &CharisAccountDialog::Submit);
    connect(confirm_edit, &QLineEdit::returnPressed, this, &CharisAccountDialog::Submit);

    // Status page
    auto* status = new QWidget(pages);
    auto* status_layout = new QVBoxLayout(status);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(12);
    auto* status_eyebrow = new QLabel(QStringLiteral("CHARIS STREAM"), status);
    status_eyebrow->setObjectName(QStringLiteral("DialogEyebrow"));
    status_layout->addWidget(status_eyebrow);
    status_title = new QLabel(status);
    status_title->setObjectName(QStringLiteral("DialogTitle"));
    status_layout->addWidget(status_title);
    status_preview = new QLabel(status);
    status_preview->setObjectName(QStringLiteral("DialogPreview"));
    status_preview->setWordWrap(true);
    status_layout->addWidget(status_preview);
    status_detail = new QLabel(status);
    status_detail->setObjectName(QStringLiteral("DialogText"));
    status_detail->setWordWrap(true);
    status_layout->addWidget(status_detail);

    auto* name_label = new QLabel(QStringLiteral("Nome deste PC no Switch"), status);
    name_label->setObjectName(QStringLiteral("DialogText"));
    status_layout->addWidget(name_label);
    name_edit = new QLineEdit(status);
    name_edit->setMaxLength(40);
    status_layout->addWidget(name_edit);
    available_box = new QCheckBox(QStringLiteral("Deixar este PC disponível para o Switch"), status);
    status_layout->addWidget(available_box);
    status_layout->addSpacing(8);

    auto* status_buttons = new QHBoxLayout();
    auto* logout = new QPushButton(QStringLiteral("Sair da conta"), status);
    logout->setObjectName(QStringLiteral("SecondaryButton"));
    logout->setCursor(Qt::PointingHandCursor);
    auto* done = new QPushButton(QStringLiteral("Pronto"), status);
    done->setDefault(true);
    status_buttons->addWidget(logout);
    status_buttons->addStretch(1);
    status_buttons->addWidget(done);
    status_layout->addLayout(status_buttons);
    pages->addWidget(status);

    connect(logout, &QPushButton::clicked, this, [this] {
        emit LoggedOut();
        ShowLogin();
    });
    connect(done, &QPushButton::clicked, this, [this] {
        const QString name = name_edit->text().trimmed();
        if (!name.isEmpty() && name != CharisAccountStore::PcName()) {
            CharisAccountStore::SetPcName(name);
            emit PcNameChanged(name);
        }
        accept();
    });
    connect(available_box, &QCheckBox::toggled, this, [this](bool on) {
        CharisAccountStore::SetAvailable(on);
        emit AvailabilityChanged(on);
        UpdateStatus();
    });
    if (controller) {
        connect(controller, &CharisStreamController::PresenceChanged, this,
                &CharisAccountDialog::UpdateStatus);
        connect(controller, &CharisStreamController::SessionChanged, this,
                &CharisAccountDialog::UpdateStatus);
    }

    if (CharisAccountStore::Load().IsLoggedIn()) {
        ShowStatus();
    } else {
        ShowLogin();
    }

    // Automated checks only: CHARIS_TEST_LOGIN="user:password[:create]" fills the form and
    // submits it; CHARIS_SNAPSHOT=prefix saves pictures of the dialog along the way.
    const QString test_login = qEnvironmentVariable("CHARIS_TEST_LOGIN");
    const QString snapshot = qEnvironmentVariable("CHARIS_SNAPSHOT");
    if (!snapshot.isEmpty()) {
        for (const int ms : {1500, 9000}) {
            QTimer::singleShot(ms, this, [this, snapshot, ms] {
                grab().save(snapshot + QStringLiteral("-%1.png").arg(ms));
            });
        }
    }
    if (!test_login.isEmpty() && pages->currentIndex() == 0) {
        QTimer::singleShot(2500, this, [this, test_login] {
            const QStringList parts = test_login.split(QLatin1Char(':'));
            SetCreateMode(parts.size() > 2 && parts[2] == QStringLiteral("create"));
            user_edit->setText(parts.value(0));
            password_edit->setText(parts.value(1));
            confirm_edit->setText(parts.value(1));
            Submit();
        });
    }
}

void CharisAccountDialog::ShowLogin() {
    pages->setCurrentIndex(0);
    const QString last_user = CharisAccountStore::Load().user;
    user_edit->setText(last_user);
    password_edit->clear();
    confirm_edit->clear();
    SetCreateMode(false);
    SetMessage({}, false);
    (last_user.isEmpty() ? user_edit : password_edit)->setFocus();
}

void CharisAccountDialog::ShowStatus() {
    pages->setCurrentIndex(1);
    const QSignalBlocker blocker(available_box);
    available_box->setChecked(CharisAccountStore::Available());
    name_edit->setText(CharisAccountStore::PcName());
    UpdateStatus();
}

void CharisAccountDialog::SetCreateMode(bool create) {
    create_mode = create;
    confirm_edit->setVisible(create);
    login_title->setText(create ? QStringLiteral("Criar conta") : QStringLiteral("Entrar na conta"));
    login_text->setText(
        create ? QStringLiteral("Escolha um usuário e uma senha. Depois entre com eles no app "
                                "Charis Stream do Switch e pronto: ele acha este PC sozinho, em "
                                "casa ou fora dela, sem configurar IP.")
               : QStringLiteral("Use a mesma conta aqui e no app Charis Stream do Switch. O "
                                "Switch acha este PC sozinho, em casa ou fora dela."));
    mode_button->setText(create ? QStringLiteral("Já tenho conta")
                                : QStringLiteral("Criar uma conta"));
    submit_button->setText(create ? QStringLiteral("Criar e entrar") : QStringLiteral("Entrar"));
    SetMessage({}, false);
}

void CharisAccountDialog::SetMessage(const QString& text, bool error) {
    login_message->setVisible(!text.isEmpty());
    login_message->setText(text);
    login_message->setProperty("error", error);
    Repolish(login_message);
}

void CharisAccountDialog::Submit() {
    if (busy) {
        return;
    }
    const QString user = CanonicalUser(user_edit->text());
    const QString password = password_edit->text();
    static const QRegularExpression valid_user(QStringLiteral("^[a-z0-9][a-z0-9_.-]{2,23}$"));
    if (!valid_user.match(user).hasMatch()) {
        SetMessage(QStringLiteral("Use de 3 a 24 letras minúsculas, números, ponto, traço ou "
                                  "sublinhado no usuário."),
                   true);
        user_edit->setFocus();
        return;
    }
    if (password.size() < 6) {
        SetMessage(QStringLiteral("A senha precisa ter pelo menos 6 caracteres."), true);
        password_edit->setFocus();
        return;
    }
    if (create_mode && confirm_edit->text() != password) {
        SetMessage(QStringLiteral("As duas senhas não são iguais."), true);
        confirm_edit->setFocus();
        return;
    }

    busy = true;
    for (QWidget* widget : {static_cast<QWidget*>(user_edit), static_cast<QWidget*>(password_edit),
                            static_cast<QWidget*>(confirm_edit),
                            static_cast<QWidget*>(submit_button),
                            static_cast<QWidget*>(mode_button)}) {
        widget->setEnabled(false);
    }
    SetMessage(create_mode ? QStringLiteral("Criando a conta...") : QStringLiteral("Entrando..."),
               false);

    auto* watcher = new QFutureWatcher<LoginResult>(this);
    connect(watcher, &QFutureWatcher<LoginResult>::finished, this, [this, watcher] {
        const LoginResult result = watcher->result();
        watcher->deleteLater();
        busy = false;
        for (QWidget* widget :
             {static_cast<QWidget*>(user_edit), static_cast<QWidget*>(password_edit),
              static_cast<QWidget*>(confirm_edit), static_cast<QWidget*>(submit_button),
              static_cast<QWidget*>(mode_button)}) {
            widget->setEnabled(true);
        }
        if (!result.error.isEmpty()) {
            SetMessage(result.error, true);
            return;
        }
        CharisAccountStore::Save(result.account);
        emit LoggedIn(result.account);
        ShowStatus();
    });
    watcher->setFuture(QtConcurrent::run(RunLogin, user, password, create_mode));
}

void CharisAccountDialog::UpdateStatus() {
    if (pages->currentIndex() != 1) {
        return;
    }
    const CharisAccountData account = CharisAccountStore::Load();
    status_title->setText(QStringLiteral("Conta %1").arg(account.user));

    bool error = false;
    QString headline;
    QString detail;
    if (!available_box->isChecked()) {
        headline = QStringLiteral("Este PC está escondido do Switch.");
        detail = QStringLiteral("Marque a opção abaixo para o Switch poder conectar.");
    } else if (controller && controller->IsStreaming()) {
        headline = QStringLiteral("Jogando no %1 agora.").arg(controller->ClientName());
        detail = QStringLiteral("Para parar, use o menu do app no Switch (botão + e −).");
    } else {
        const CharisStream::Presence::Status status =
            controller ? controller->PresenceStatus() : CharisStream::Presence::Status{};
        if (status.login_expired) {
            error = true;
            headline = QStringLiteral("O login expirou. Saia e entre na conta de novo.");
        } else if (controller && controller->IsRunning() && status.online) {
            headline = QStringLiteral("Pronto. Abra o Charis Stream no Switch e entre com a "
                                      "conta %1.")
                           .arg(account.user);
            detail = status.router_port
                         ? QStringLiteral("Em casa e fora dela: o roteador abriu o caminho para "
                                          "este PC sozinho.")
                         : QStringLiteral("Em casa funciona direto. Fora de casa o Switch tenta "
                                          "pela internet; se não conseguir, ative o UPnP no "
                                          "roteador.");
        } else if (controller && controller->IsRunning() && !status.message.empty()) {
            error = true;
            headline = QStringLiteral("Sem contato com o servidor da conta.");
            detail = QString::fromStdString(status.message) +
                     QStringLiteral(" Em casa o Switch ainda acha este PC pela rede.");
        } else {
            headline = QStringLiteral("Preparando...");
        }
    }
    status_preview->setText(headline);
    status_preview->setProperty("error", error);
    Repolish(status_preview);
    status_detail->setText(detail);
    status_detail->setVisible(!detail.isEmpty());
}
