#pragma once
#include <QWidget>

class RackParameterBinding;
class RackSamplerWaveform;
class QAbstractButton;
class QLabel;
class QMimeData;
class QStackedWidget;
class QTabBar;
class QToolButton;

/// Sampler's compact rack face. The standalone editor and DSP stay independent.
class RackSamplerView final : public QWidget {
    Q_OBJECT
public:
    explicit RackSamplerView(RackParameterBinding*, QWidget* parent = nullptr);
    void refresh();
    bool checkForTest(QString* error);

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    QString droppedFile(const QMimeData*) const;
    bool loadSample(const QString& path);
    void chooseSample();
    void applyTheme();
    RackParameterBinding* m_binding;
    RackSamplerWaveform* m_waveform;
    QToolButton* m_file;
    QLabel* m_info;
    QAbstractButton* m_envelope;
    QTabBar* m_tabs;
    QStackedWidget* m_pages;
    QString m_loadError;
    bool m_loading = false;
};
