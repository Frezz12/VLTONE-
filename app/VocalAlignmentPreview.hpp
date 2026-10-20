#pragma once

#include "SelectionModel.hpp"
#include "VocalAlignment.hpp"
#include "WaveformCache.hpp"
#include <QWidget>
#include <optional>

namespace daw { class EngineController; }

// A timing view: the source envelope follows the exact piecewise Warp map.
// It does not render or audition a second copy of the audio processing chain.
class VocalAlignmentPreview : public QWidget {
    Q_OBJECT
public:
    explicit VocalAlignmentPreview(QWidget* parent = nullptr);
    ~VocalAlignmentPreview() override;
    void setLive(daw::EngineController*, const ui::ClipSel& guide, const ui::ClipSel& target);
    void setSnapshot(const daw::alignment::AlignmentSource& guide,
                     const daw::alignment::AlignmentSource& target,
                     const std::optional<daw::alignment::AlignmentProposal>& result);
    void refreshLive();
    void fit();
    void zoom(double factor, double anchor = .5);
    void setShowChanges(bool);
    bool hasResult() const { return m_result.has_value(); }
    bool waveformsReady() const;
    double visibleStart() const { return m_start; }
    double visibleDuration() const { return m_span; }
    double projectTimeForSource(int lane, double sourceSeconds) const;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    struct Wave {
        const daw::WaveformPeaks* peaks = nullptr;
        const daw::engine::SampleBuffer* samples = nullptr;
        daw::ClipWarpModel map;
        double start = 0, duration = 0, tempo = 120;
        bool reversed = false;
    };
    struct PeakJob;
    daw::EngineController* m_controller = nullptr;
    ui::ClipSel m_guide, m_target;
    daw::alignment::AlignmentSource m_guideSource, m_targetSource;
    std::optional<daw::alignment::AlignmentProposal> m_result;
    std::shared_ptr<PeakJob> m_peakJob;
    std::shared_ptr<const daw::WaveformPeaks> m_guidePeaks, m_targetPeaks;
    double m_begin = 0, m_end = 1, m_start = 0, m_span = 1;
    double m_panStart = 0, m_panX = 0, m_hoverX = -1;
    bool m_fitted = true, m_panning = false, m_showChanges = true;
    Wave wave(int lane) const;
    QRectF plot() const;
    void updateBounds();
    void pan(double start);
};
