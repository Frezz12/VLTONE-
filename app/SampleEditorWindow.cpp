#include "SampleEditorWindow.hpp"

#include "EngineController.hpp"
#include "SamplerPanel.hpp"

#include <QCloseEvent>
#include <QVBoxLayout>

SampleEditorWindow::SampleEditorWindow(daw::EngineController* controller,
                                       QString trackId, QString clipId,
                                       QWidget* parent)
    : QWidget(parent), m_trackId(std::move(trackId)),
      m_clipId(std::move(clipId)) {
    setAttribute(Qt::WA_DeleteOnClose);
    QString title = tr("Sampler");
    if (controller) {
        if (const daw::ClipModel* clip = controller->audioClip(
                m_trackId.toStdString(), m_clipId.toStdString())) {
            if (!clip->name.empty())
                title = tr("Sampler — %1").arg(QString::fromStdString(clip->name));
        }
    }
    setWindowTitle(title);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* panel = new SamplerPanel(controller, SamplerPanel::Context::Clip,
                                   m_trackId, m_clipId, this);
    m_panel = panel;
    connect(panel, &SamplerPanel::projectEdited, this,
            &SampleEditorWindow::projectEdited);
    connect(panel, &SamplerPanel::liveEdited, this,
            &SampleEditorWindow::liveEdited);
    connect(panel, &SamplerPanel::pluginEditorRequested, this,
            &SampleEditorWindow::pluginEditorRequested);
    layout->addWidget(panel);

    setMinimumSize(860, 520);
    resize(1080, 650);
}

void SampleEditorWindow::setSnapProvider(std::function<double()> provider) {
    if (m_panel) m_panel->setSnapProvider(std::move(provider));
}

void SampleEditorWindow::refresh() {
    if (m_panel) m_panel->refresh();
}

QWidget* SampleEditorWindow::collaborationPresenceSurface() const {
    return m_panel;
}

void SampleEditorWindow::closeEvent(QCloseEvent* event) {
    emit closing(m_trackId, m_clipId);
    QWidget::closeEvent(event);
}
