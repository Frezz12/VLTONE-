#include "RecoverySettingsPage.hpp"

#include "Controls.hpp"
#include "RecoveryPrefs.hpp"
#include "RecoverySupport.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

RecoverySettingsPage::RecoverySettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(16, 16, 16, 16);
    column->setSpacing(8);

    column->addWidget(ui::sectionLabel(tr("AFTER A CRASH"), this));

    m_enabled = new QCheckBox(tr("Keep a copy of my work while I edit"), this);
    m_enabled->setChecked(ui::recoveryprefs::enabled());
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        ui::recoveryprefs::setEnabled(on);
        m_watchdog->setEnabled(on);
        refreshStatus();
        emit changed();
    });
    column->addWidget(m_enabled);

    auto* what = new QLabel(
        tr("Every couple of seconds the project is written to a separate "
           "recovery file. If VLT Studio Pro closes unexpectedly, the next launch offers "
           "that work back."),
        this);
    what->setWordWrap(true);
    column->addWidget(what);

    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("WHAT COMES BACK"), this));

    // Written out rather than implied. Each line is a real limit of the design,
    // and a user who reads them after a crash rather than before has already
    // been let down.
    auto* limits = new QLabel(
        tr("• Notes, clips, tracks and the mix — as of the last automatic "
           "recovery save.\n"
           "• Plugin settings and presets — from that same recovery save. A "
           "plugin that refuses to provide state falls back to its host "
           "parameters or last manual project state.\n"
           "• Audio is read from your original files, not from the copies "
           "inside a saved project. A file you have since moved or deleted "
           "cannot come back.\n"
           "• Your project file is never overwritten automatically. Recovered "
           "work arrives unsaved, and it is yours to keep or discard."),
        this);
    limits->setWordWrap(true);
    column->addWidget(limits);

    column->addWidget(ui::separatorLine(Qt::Horizontal, 0, this));
    column->addWidget(ui::sectionLabel(tr("WATCHDOG"), this));

    m_watchdog = new QCheckBox(
        tr("Run a watchdog process alongside VLT Studio Pro"), this);
    m_watchdog->setChecked(ui::recoveryprefs::watchdog());
    m_watchdog->setEnabled(ui::recoveryprefs::enabled());
    connect(m_watchdog, &QCheckBox::toggled, this, [this](bool on) {
        ui::recoveryprefs::setWatchdog(on);
        refreshStatus();
        emit changed();
    });
    column->addWidget(m_watchdog);

    auto* watchdogNote = new QLabel(
        tr("A small separate program that notices if VLT Studio Pro freezes — "
           "something VLT Studio Pro itself cannot detect — and keeps a log of "
           "how it was running. "
           "It cannot save anything on its own; that is what the copy above "
           "is for. Turning it off loses only the log and freeze detection."),
        this);
    watchdogNote->setWordWrap(true);
    column->addWidget(watchdogNote);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    column->addWidget(m_status);

    auto* buttons = new QHBoxLayout();
    auto* reveal = new QPushButton(tr("Open Recovery Folder"), this);
    connect(reveal, &QPushButton::clicked, this, [] {
        const QString root = ui::recovery::rootDir();
        QDir().mkpath(root);
        QDesktopServices::openUrl(QUrl::fromLocalFile(root));
    });
    buttons->addWidget(reveal);
    buttons->addStretch(1);
    column->addLayout(buttons);

    column->addStretch(1);
    refreshStatus();
}

void RecoverySettingsPage::refreshStatus() {
    if (!ui::recoveryprefs::enabled()) {
        m_status->setText(
            tr("Recovery is off. Unsaved work will be lost if VLT Studio Pro closes "
               "unexpectedly. Takes effect on the next launch."));
        return;
    }
    if (ui::recoveryprefs::watchdog() && ui::recovery::guardPath().isEmpty()) {
        m_status->setText(
            tr("The watchdog program was not found next to VLT Studio Pro, so freezes "
               "will go unnoticed. Your work is still being copied."));
        return;
    }
    m_status->setText(
        tr("Changes take effect the next time VLT Studio Pro starts."));
}
