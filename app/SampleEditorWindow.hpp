#pragma once

#include <QString>

#include <QWidget>

#include <functional>

namespace daw { class EngineController; }

/// Top-level host for the Clip context of the shared Sample/Clip Editor.
/// Instrument context remains hosted by PluginEditorWindow because it belongs
/// to a plugin slot; both windows embed the same SamplerPanel implementation.
class SampleEditorWindow : public QWidget {
    Q_OBJECT
public:
    SampleEditorWindow(daw::EngineController* controller, QString trackId,
                       QString clipId, QWidget* parent = nullptr);

    const QString& trackId() const { return m_trackId; }
    const QString& clipId() const { return m_clipId; }

    /// Forwarded to the panel: the arrangement's grid, so stretching a clip
    /// can land its end on a bar line.
    void setSnapProvider(std::function<double()> provider);

signals:
    void closing(const QString& trackId, const QString& clipId);
    void pluginEditorRequested(const QString& channelId,
                               const QString& insertId);
    void projectEdited();
    /// A control is being dragged — repaint, but do not touch undo.
    void liveEdited();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    class SamplerPanel* m_panel = nullptr;
    QString m_trackId;
    QString m_clipId;
};
