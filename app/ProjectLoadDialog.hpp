#pragma once

#include "EngineController.hpp"
#include "Theme.hpp"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QLabel>
#include <QProgressBar>
#include <QProgressDialog>

namespace ui {

// Native plugins are prepared on the control/UI thread. During that phase
// update only this window's paint, never spin a nested event loop over a
// partially replaced graph (QProgressDialog::setValue would do that).
class ProjectLoadDialog final : public QProgressDialog {
    Q_DECLARE_TR_FUNCTIONS(ProjectLoadDialog)
public:
    ProjectLoadDialog(QWidget* parent, const QString& name)
        : QProgressDialog(parent), m_name(name) {
        setObjectName("ProjectLoadDialog");
        setWindowTitle(tr("Opening project"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setWindowFlag(Qt::WindowCloseButtonHint, false);
        setWindowModality(Qt::ApplicationModal);
        setMinimumWidth(420);
        setMinimumDuration(0);
        setAutoClose(false); setAutoReset(false);
        m_label = new QLabel(this);
        m_label->setTextFormat(Qt::PlainText);
        m_label->setMinimumHeight(72);
        m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setLabel(m_label);
        m_bar = new QProgressBar(this);
        m_bar->setTextVisible(false); m_bar->setFixedHeight(6);
        m_bar->setStyleSheet(QString("QProgressBar { border: none; border-radius: 3px; background: %1; }"
            "QProgressBar::chunk { border-radius: 3px; background: %2; }")
            .arg(th().surfaceElevated.name(), th().accent.name()));
        setBar(m_bar);
        setCancelButtonText(tr("Cancel"));
        stage(tr("Reading project and audio…"));
    }

    QString message(const QString& stage) const {
        return m_label->fontMetrics().elidedText(m_name, Qt::ElideMiddle, 380) + "\n\n" + stage;
    }
    void stage(const QString& status, bool cancellable = true) {
        m_cancellable = cancellable;
        if (!cancellable) setCancelButton(nullptr);
        m_label->setText(message(status));
        m_label->setAccessibleName(m_name + ": " + status);
        m_bar->setRange(0, 0);
        repaint(); m_label->repaint(); m_bar->repaint();
    }
    void plugin(std::string_view name, std::size_t completed, std::size_t total) {
        if (name.empty()) { stage(tr("Preparing audio engine…"), false); return; }
        const auto status = tr("Loading plugin %1 of %2: %3")
            .arg(qulonglong(completed + 1)).arg(qulonglong(total))
            .arg(QString::fromUtf8(name.data(), qsizetype(name.size())));
        stage(m_label->fontMetrics().elidedText(status, Qt::ElideMiddle, 380), false);
        m_label->setAccessibleName(m_name + ": " + status);
        m_bar->setRange(0, int(total)); m_bar->setValue(int(completed));
        m_bar->repaint();
    }

    // The callback cannot outlive either the document or this stack dialog.
    class Binding {
    public:
        Binding(ProjectLoadDialog& dialog, daw::EngineController& controller)
            : m_controller(controller), m_previous(controller.setPluginLoadProgress(
                [&dialog](auto name, auto done, auto total) { dialog.plugin(name, done, total); })) {}
        ~Binding() { m_controller.setPluginLoadProgress(std::move(m_previous)); }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;
    private:
        daw::EngineController& m_controller;
        daw::AudioSessionSpec::PluginLoadProgress m_previous;
    };
protected:
    void reject() override { if (m_cancellable) QProgressDialog::reject(); }
    void closeEvent(QCloseEvent* event) override {
        if (m_cancellable) QProgressDialog::closeEvent(event); else event->ignore();
    }
private:
    QString m_name;
    QLabel* m_label = nullptr;
    QProgressBar* m_bar = nullptr;
    bool m_cancellable = true;
};
} // namespace ui
