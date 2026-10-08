#pragma once

#include "PluginReadout.hpp"
#include "SlicerTools.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"

#include <QHash>
#include <QSet>
#include <QPushButton>
#include <QVector>
#include <atomic>
#include <functional>
#include <optional>

class QBoxLayout;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QDialog;
class QFormLayout;
class QLabel;
class QScrollArea;
class QSpinBox;
class QTabWidget;
class QTimer;
namespace ui { class Knob; }
namespace daw { class EngineController; }

class SlicerEffectPad final : public QWidget {
    Q_OBJECT
public:
    explicit SlicerEffectPad(QWidget* parent = nullptr);
    void setValues(double x, double y, const QString& xLabel, const QString& yLabel, bool enabled);
    bool cancelDrag();
signals:
    void positionEdited(double x, double y);
    void editFinished();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;
private:
    QRectF fieldRect() const;
    void editPosition(QPointF);
    QPointF m_value{.5, .5}, m_lastPointer, m_grabOffset;
    QString m_xLabel, m_yLabel;
    bool m_dragging = false;
};

class SlicerWaveform final : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
public:
    explicit SlicerWaveform(QWidget* parent = nullptr);
    void setSample(std::shared_ptr<const daw::engine::SampleBuffer>);
    void setTable(std::shared_ptr<const daw::plugins::slicer::SliceTable>);
    void setSelection(const QSet<quint32>& ids, quint32 primary);
    void setRange(daw::engine::FrameCount start, daw::engine::FrameCount end);
    void fitAll();
    void fitSelection();
    void zoom(double factor, double anchor = -1.0);
    int sliceAt(double x) const;
    bool cancelDrag();
    QRectF plotRect() const;
    double xForFrame(double frame) const;
    double frameForX(double x) const;
signals:
    void selectRequested(int index, Qt::KeyboardModifiers modifiers, bool audition);
    void auditionReleased();
    void boundaryBegin();
    void boundaryMoved(int rightIndex, quint32 frame);
    void boundaryCommit();
    void boundaryCancel();
    void splitRequested(quint32 frame);
    void mergeRequested(int rightIndex);
    void rangeRequested(quint32 start, quint32 end);
protected:
    void paintEvent(QPaintEvent*) override;
    void paintScene(QPainter&, const QRegion&) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    void requestPeaks();
    void setView(double start, double span);
    int boundaryAt(double x) const;
    std::shared_ptr<const daw::engine::SampleBuffer> m_sample;
    std::shared_ptr<const daw::plugins::slicer::SliceTable> m_table;
    QVector<float> m_minima, m_maxima;
    QSet<quint32> m_selected;
    quint32 m_primary = 0, m_rangeStart = 0, m_rangeEnd = 0;
    quint64 m_generation = 0;
    double m_viewStart = 0, m_viewSpan = 1, m_pressFrame = 0, m_panStart = 0;
    QPointF m_pressPosition;
    int m_dragBoundary = -1;
    bool m_panning = false, m_overview = false, m_selectingRange = false;
};

class SlicerPad final : public QPushButton {
    Q_OBJECT
public:
    explicit SlicerPad(QWidget* parent = nullptr);
    void configure(const daw::plugins::slicer::Slice* slice, int index, double rate,
                   bool selected, bool active, const QString& token);
    int index() const { return m_index; }
signals:
    void selectRequested(int index, Qt::KeyboardModifiers modifiers, bool audition);
    void auditionReleased();
    void swapRequested(quint32 fromId, quint32 toId);
protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    QPoint m_press;
    quint32 m_id = 0;
    int m_index = -1;
    QString m_token;
    bool m_muted = false, m_playing = false;
};

class SlicerPanel final : public QWidget {
    Q_OBJECT
public:
    SlicerPanel(daw::EngineController*, QString channelId, QString slotId, QWidget* parent = nullptr);
    ~SlicerPanel() override;
    bool hasHeightForWidth() const override;
    void refresh();
    static bool checkLayoutForTest();
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    using Slice = daw::plugins::slicer::Slice;
    using Table = daw::plugins::slicer::SliceTable;
    using State = daw::plugins::slicer::ControlState;
    using Analysis = daw::plugins::slicer::AnalysisSettings;
    struct WorkResult {
        State state;
        std::shared_ptr<const Table> table;
        QStringList files;
        std::string error;
        bool ok = false;
    };
    std::optional<daw::SlicerSnapshot> slicer(bool includeActivity = false) const;
    QWidget* buildAnalysis();
    QWidget* buildSliceInspector();
    QWidget* buildPlayback();
    QWidget* buildProcessing();
    QWidget* buildExport();
    QWidget* buildSoundPanel();
    void showMenu();
    void showSettings(int tab = -1);
    void beginSliceGesture(QWidget*);
    void refreshEffect(const Slice*);
    QDoubleSpinBox* number(QFormLayout*, const QString& caption, const QString& name,
                          double lo, double hi, int decimals = 2, const QString& suffix = {});
    QComboBox* choice(QFormLayout*, const QString& caption, const QString& name, const QStringList&);
    QCheckBox* toggle(QFormLayout*, const QString& caption, const QString& name);
    ui::Knob* knob(const QString& id, const QString& caption);
    void applyTheme();
    void selectSlice(int, Qt::KeyboardModifiers, bool audition);
    void refreshSelection();
    std::vector<std::uint32_t> selectedIds() const;
    void editTable(const QString& label, const std::function<void(Table&)>&);
    void editSelected(const QString& label, const std::function<void(Slice&)>&);
    void setAnalysis(const std::function<void(Analysis&)>&, bool remap = false);
    void editBoundary(bool end, double ms);
    void swapKeys(quint32, quint32);
    void splitAt(quint32);
    void mergeAt(int);
    void copySettings();
    void pasteSettings();
    void resetSettings();
    void audition(int index);
    void releaseAudition(bool panic = false);
    void loadSample(const QString& path = {});
    void runSlice(bool newSeed = false);
    void normalizeSelection();
    void randomizeSelection(bool newSeed = false);
    void preset(bool load);
    void exportMidi(bool toFile);
    void exportWav(bool all, bool processed, bool prepareDrag = false);
    QString mediaCache() const;
    void startWork(const QString& status, std::function<WorkResult(const std::function<bool()>&)> work,
                   std::function<void(WorkResult)> finish);
    void cancelWork();
    void status(const QString&);
    void commitField();
    void beginGesture(const QString& id);
    void endGesture(const QString& id);
    void writeParameter(const QString& id, double value);
    double readParameter(const QString& id) const;
    void handleKey(QKeyEvent*);

    daw::EngineController* m_controller;
    QString m_channelId, m_slotId, m_dragToken;
    SlicerWaveform* m_waveform = nullptr;
    QWidget* m_mainArea = nullptr;
    QWidget* m_padArea = nullptr;
    QBoxLayout* m_body = nullptr;
    QTabWidget* m_tabs = nullptr;
    QDialog* m_settingsWindow = nullptr;
    QWidget* m_soundPanel = nullptr;
    QLabel* m_soundLabel = nullptr;
    QComboBox* m_effectChoice = nullptr;
    SlicerEffectPad* m_effectPad = nullptr;
    int m_effectUiType = -1;
    QHash<QString, ui::Knob*> m_sliceKnobs;
    QComboBox* m_quickMode = nullptr;
    QSpinBox* m_quickCount = nullptr;
    QPushButton* m_quickSlice = nullptr;
    QLabel *m_fileLabel = nullptr, *m_statusLabel = nullptr;
    QPushButton *m_sliceButton = nullptr, *m_dragButton = nullptr;
    QAbstractButton* m_cancelButton = nullptr;
    QComboBox *m_bank = nullptr, *m_phraseOrder = nullptr;
    std::array<SlicerPad*, 16> m_pads{};
    QHash<QString, QDoubleSpinBox*> m_numbers;
    QHash<QString, QComboBox*> m_choices;
    QHash<QString, QCheckBox*> m_toggles;
    QHash<QString, ui::Knob*> m_knobs;
    QHash<QString, double> m_gestureStart;
    QSet<quint32> m_selected;
    quint32 m_primary = 0;
    int m_selectionAnchor = -1;
    std::optional<Slice> m_copied;
    std::shared_ptr<const Table> m_seenTable;
    std::shared_ptr<const daw::engine::SampleBuffer> m_seenAudio;
    daw::PluginIdentity m_seenInstance;
    quint64 m_seenSource = 0, m_workGeneration = 0;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    QStringList m_dragFiles;
    QPoint m_dragOrigin;
    bool m_dragArmed = false, m_auditionOutstanding = false;
    QTimer* m_poll = nullptr;
    bool m_refreshing = false, m_busy = false, m_boundaryGesture = false;
    QWidget* m_fieldEditor = nullptr;
    int m_auditionKey = -1;
};
