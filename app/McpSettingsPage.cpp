#include "McpSettingsPage.hpp"
#include "McpService.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QVBoxLayout>
namespace ui {
McpSettingsPage::McpSettingsPage(QWidget *parent):QWidget(parent) {
    setObjectName("McpSettingsPage");
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(24,24,24,24); layout->setSpacing(14);
    auto *description=new QLabel(tr("Connect an external AI agent to VLTONE on this computer. It can edit projects, write music and code, install local modules and run checks. Changes appear in the shared editors and can be undone."),this);
    description->setWordWrap(true); layout->addWidget(description);
    auto *enable=new QCheckBox(tr("Enable MCP server"),this); enable->setObjectName("McpEnable"); layout->addWidget(enable);
    auto *status=new QLabel(this); status->setObjectName("McpStatus"); status->setWordWrap(true); layout->addWidget(status);
    auto *scope=new QLabel(tr("Access is limited to your OS account. Keep VLTONE running while the agent is connected. Lua does not receive permission to start system processes or use the network."),this);
    scope->setWordWrap(true); layout->addWidget(scope);
    auto *copyCodex=new QPushButton(tr("Copy configuration for Codex"),this);
    auto *copyJson=new QPushButton(tr("Copy configuration for other clients"),this);
    layout->addWidget(copyCodex); layout->addWidget(copyJson);
    auto *hint=new QLabel(tr("Configuration includes a private connection token. Paste it into your MCP client's settings."),this); hint->setWordWrap(true); layout->addWidget(hint);
    auto *stop=new QPushButton(tr("Cancel active tasks"),this); layout->addWidget(stop);
    auto *revoke=new QPushButton(tr("Revoke access"),this); layout->addWidget(revoke);
    auto *label=new QLabel(tr("Activity log"),this); layout->addWidget(label);
    auto *log=new QPlainTextEdit(this); log->setObjectName("McpActivityLog"); log->setReadOnly(true); log->setAccessibleName(tr("Activity log"));
    log->setMinimumHeight(160); label->setBuddy(log); layout->addWidget(log,1);
    auto *service=McpService::instance();
    if(!service) { setEnabled(false); status->setText(tr("MCP service is unavailable")); return; }
    const auto refresh=[=] {
        const QSignalBlocker block(enable); enable->setChecked(service->enabled()); status->setText(service->status());
        copyCodex->setEnabled(service->listening()); copyJson->setEnabled(service->listening());
        const auto text=service->log().join('\n');
        if(log->toPlainText()!=text) {
            const auto position=log->verticalScrollBar()->value();
            const bool follow=position==log->verticalScrollBar()->maximum();
            log->setPlainText(text);
            log->verticalScrollBar()->setValue(follow?log->verticalScrollBar()->maximum():position);
        }
    };
    connect(service,&McpService::changed,this,refresh);
    connect(enable,&QCheckBox::toggled,service,&McpService::setEnabled);
    connect(copyCodex,&QPushButton::clicked,this,[=] { QApplication::clipboard()->setText(service->configuration(true)); status->setText(tr("Codex configuration copied")); });
    connect(copyJson,&QPushButton::clicked,this,[=] { QApplication::clipboard()->setText(service->configuration(false)); status->setText(tr("MCP configuration copied")); });
    connect(stop,&QPushButton::clicked,service,&McpService::stopOperations);
    connect(revoke,&QPushButton::clicked,service,&McpService::revoke);
    refresh();
}
}
