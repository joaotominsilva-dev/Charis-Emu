// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <fmt/ranges.h>
#include "common/scm_rev.h"
#include "ui_aboutdialog.h"
#include "yuzu/about_dialog.h"
#include "yuzu/charis_version.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent), ui{std::make_unique<Ui::AboutDialog>()} {
    const std::string yuzu_build_version = fmt::format(
        "Versão {} | {}", CHARIS_SWITCH_VERSION, std::string{Common::g_compiler_id});

    ui->setupUi(this);
    ui->labelBuildInfo->setText(
        ui->labelBuildInfo->text().arg(QString::fromStdString(yuzu_build_version),
                                       QString::fromUtf8(Common::g_build_date).left(10)));
}

AboutDialog::~AboutDialog() = default;
