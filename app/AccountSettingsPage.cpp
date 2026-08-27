#include "AccountSettingsPage.hpp"

#include "AccountService.hpp"

#include <QDateTime>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

AccountSettingsPage::AccountSettingsPage(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    auto* title = new QLabel(tr("VLT Studio account"), this);
    QFont font = title->font(); font.setBold(true); font.setPointSize(font.pointSize() + 2);
    title->setFont(font);
    column->addWidget(title);
    auto* copy = new QLabel(tr("Your Demo entitlement and AI allowance are verified by the VLT account service."), this);
    copy->setWordWrap(true);
    column->addWidget(copy);
    auto* form = new QFormLayout;
    m_identity = new QLabel(this); m_plan = new QLabel(this); m_quota = new QLabel(this);
    m_sync = new QLabel(this); m_device = new QLabel(this);
    m_device->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Account"), m_identity);
    form->addRow(tr("Subscription"), m_plan);
    form->addRow(tr("AI tokens"), m_quota);
    form->addRow(tr("Last verification"), m_sync);
    form->addRow(tr("Device"), m_device);
    column->addLayout(form);
    auto* note = new QLabel(tr("When the monthly AI allowance is exhausted, only AI is paused. Editing, playback, recording, plugins and export remain available."), this);
    note->setWordWrap(true);
    column->addWidget(note);
    auto* logout = new QPushButton(tr("Sign out…"), this);
    connect(logout, &QPushButton::clicked, this, &AccountSettingsPage::logoutRequested);
    column->addWidget(logout, 0, Qt::AlignLeft);
    column->addStretch(1);
    if (auto* service = account::Service::instance()) {
        connect(service, &account::Service::snapshotChanged, this, &AccountSettingsPage::refresh);
    }
    refresh();
}

void AccountSettingsPage::refresh() {
    auto* service = account::Service::instance();
    if (!service || !service->authenticated()) {
        m_identity->setText(tr("Not signed in"));
        return;
    }
    const auto& state = service->snapshot();
    m_identity->setText(QStringLiteral("%1  ·  %2").arg(state.nickname, state.email));
    m_plan->setText(state.offline ? tr("Demo · offline allowance") : tr("Demo · all features"));
    m_quota->setText(tr("%1 remaining of %2 · resets %3 UTC")
        .arg(QLocale().toString(state.tokensRemaining), QLocale().toString(state.tokenLimit),
             QLocale().toString(state.cycleEndsAt.date(), QLocale::ShortFormat)));
    m_sync->setText(QLocale().toString(state.lastSyncAt.toLocalTime(), QLocale::ShortFormat));
    m_device->setText(state.deviceId);
}
