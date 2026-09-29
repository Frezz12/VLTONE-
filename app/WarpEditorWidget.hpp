#pragma once
#include "model/Document.hpp"
#include "WaveformCache.hpp"
#include "WarpAnalysis.hpp"
#include "WarpTools.hpp"
#include "AudioMusicalAnalysis.hpp"
#include <QWidget>
#include <atomic>
#include <future>
#include <map>
#include <set>

class QLabel;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QToolButton;
class QScrollArea;
class QHBoxLayout;
class QTimer;
class WarpCanvas;
class WarpOverview;
namespace ui { class FrameTimer; }
namespace daw { class EngineController; }

class WarpEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit WarpEditorWidget(daw::EngineController* controller, QWidget* parent = nullptr);
    ~WarpEditorWidget() override;
    bool setClip(const QString& trackId, const QString& clipId);
    void setSnapProvider(std::function<double()> provider) { m_snapProvider = std::move(provider); }
    void refresh();
    void clearClip();
    void finishPendingEdit();
    void deleteMarkers();
    void quantize();
    void resetMap();
    bool ownsEditingFocus() const;
    const QString& clipId() const { return m_clipId; }
    void previewAlignment();
    void applyPreview();
    void cancelPreview();
signals:
    void edited();
    void liveEdited();
    void closeRequested();
protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
private:
    friend class WarpCanvas;
    friend class WarpOverview;
    const daw::ClipModel* clip() const;
    bool apply(const daw::ClipWarpModel& map, const char* label);
    double grid() const;
    void addMarker(double beats);
    void moveSelected(double delta, const daw::ClipWarpModel& before, bool snap);
    void analyze();
    void pollAnalysis();
    void showTempo(const daw::analysis::TempoEstimate& tempo);
    void rebuildPreview();
    void updateInspector();
    void updateStyle();
    void arrangeInspector();
    void setRange(double begin, double end);
    void extractGroove();
    void loadGrooves();
    void redraw();
    daw::warptools::AlignParams alignParams(bool attacks) const;
    daw::EngineController* m_controller;
    QString m_trackId, m_clipId;
    WarpCanvas* m_canvas;
    QLabel *m_title, *m_status;
    QLabel *m_analysisLabel, *m_previewSummary, *m_tempoLabel, *m_dragLabel;
    QWidget* m_tools;
    QWidget *m_body, *m_previewBar, *m_waveHost;
    QHBoxLayout* m_bodyLayout;
    QScrollArea* m_inspector;
    QToolButton *m_options, *m_before, *m_after;
    QPushButton *m_alignButton;
    WarpOverview* m_overview;
    QCheckBox *m_enabled, *m_snap, *m_pitch;
    QComboBox *m_grid, *m_mode;
    QSpinBox *m_strength, *m_sensitivity;
    QSpinBox *m_swing, *m_tolerance;
    QDoubleSpinBox *m_markerSource, *m_markerTarget, *m_rangeStart, *m_rangeEnd;
    QCheckBox *m_markerLock, *m_uncertain;
    QComboBox *m_groove, *m_swingUnit, *m_extractSource, *m_extractBars;
    std::vector<daw::miditools::Groove> m_grooves;
    QTimer* m_analysisTimer;
    ui::FrameTimer* m_frameTimer;
    daw::ClipWarpModel m_previewBase, m_lastMap;
    daw::warptools::Proposal m_proposal;
    bool m_previewing = false;
    bool m_inspectorOpen = true;
    double m_rangeBegin = 0, m_rangeFinish = 0;
    double m_lastPosition = -1, m_lastStart = -1, m_lastTempo = -1;
    std::uint64_t m_projectGeneration = 0;
    std::set<std::string> m_selected;
    struct View { double start = 0, span = 8; } m_view;
    std::map<QString, View> m_views;
    std::shared_ptr<const daw::engine::SampleBuffer> m_audio;
    std::shared_ptr<daw::WaveformPeaks> m_peaks;
    std::vector<daw::analysis::WarpTransient> m_transients;
    std::vector<daw::analysis::WarpTransient> m_previewAttacks;
    double m_analysisBegin = -1, m_analysisEnd = -1;
    struct AnalysisResult {
        std::shared_ptr<daw::WaveformPeaks> peaks;
        std::vector<daw::analysis::WarpTransient> transients;
        daw::analysis::TempoEstimate tempo;
    };
    struct CachedAnalysis {
        std::shared_ptr<const daw::engine::SampleBuffer> audio;
        double begin = 0, end = 0;
        int version = daw::analysis::kWarpAnalysisVersion;
        AnalysisResult result;
    };
    std::vector<CachedAnalysis> m_analysisCache;
    struct Job {
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::future<AnalysisResult> future;
        unsigned generation;
    };
    std::vector<Job> m_jobs;
    unsigned m_generation = 0;
    std::function<double()> m_snapProvider;
};
