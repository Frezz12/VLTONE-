#pragma once
#include "model/Document.hpp"
#include "WaveformCache.hpp"
#include "WarpAnalysis.hpp"
#include <QWidget>
#include <atomic>
#include <future>
#include <map>
#include <set>

class QLabel;
class QCheckBox;
class QComboBox;
class QSpinBox;
class WarpCanvas;
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
signals:
    void edited();
    void liveEdited();
private:
    friend class WarpCanvas;
    const daw::ClipModel* clip() const;
    bool apply(const daw::ClipWarpModel& map, const char* label);
    double grid() const;
    void addMarker(double beats);
    void moveSelected(double delta, const daw::ClipWarpModel& before, bool snap);
    void analyze();
    void pollAnalysis();
    daw::EngineController* m_controller;
    QString m_trackId, m_clipId;
    WarpCanvas* m_canvas;
    QLabel *m_title, *m_status;
    QWidget* m_tools;
    QCheckBox *m_enabled, *m_snap, *m_pitch;
    QComboBox *m_grid, *m_mode;
    QSpinBox *m_strength, *m_sensitivity;
    std::set<std::string> m_selected;
    struct View { double start = 0, span = 8; } m_view;
    std::map<QString, View> m_views;
    std::shared_ptr<const daw::engine::SampleBuffer> m_audio;
    std::shared_ptr<daw::WaveformPeaks> m_peaks;
    std::vector<daw::analysis::WarpTransient> m_transients;
    double m_analysisBegin = -1, m_analysisEnd = -1;
    struct AnalysisResult {
        std::shared_ptr<daw::WaveformPeaks> peaks;
        std::vector<daw::analysis::WarpTransient> transients;
    };
    struct Job {
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::future<AnalysisResult> future;
        unsigned generation;
    };
    std::vector<Job> m_jobs;
    unsigned m_generation = 0;
    std::function<double()> m_snapProvider;
};
