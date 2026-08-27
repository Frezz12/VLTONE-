#pragma once

#include <QWidget>
#include <QScrollBar>

#include <memory>
#include <unordered_map>

#include "daw/graph/PeakFile.h"
#include "daw/model/Session.h"

class TimelineView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineView(QWidget* parent = nullptr);

    void setSession(std::shared_ptr<daw::model::Session> session);
    void setPeakFile(const std::string& audioPath,
                     std::shared_ptr<daw::graph::PeakFile> peakFile);
    void setPlayheadPosition(std::int64_t samples);
    void setZoom(double samplesPerPixel);
    void setScrollOffset(double samples);
    void setSampleRate(double rate);
    void setPlaying(bool playing);

    double samplesPerPixel() const noexcept { return samplesPerPixel_; }
    double scrollOffset() const noexcept { return scrollOffset_; }
    double sampleRate() const noexcept { return sampleRate_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void locateRequested(std::int64_t sample);
    void zoomChanged(double samplesPerPixel);
    void scrollChanged(double samples);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void paintTrackRow(QPainter& p, int y, int h, const daw::model::Track& track);
    void paintClip(QPainter& p, const daw::model::Clip& clip,
                   const daw::graph::PeakFile* peakFile,
                   int trackY, int trackH);
    int viewWidth() const;
    int viewHeight() const;
    int chooseLevel(const daw::graph::PeakFile& pf) const;
    int totalContentWidth() const;
    int totalContentHeight() const;
    void updateScrollbars();

    std::shared_ptr<daw::model::Session> session_;
    std::unordered_map<std::string, std::shared_ptr<daw::graph::PeakFile>> peakFiles_;

    std::int64_t playheadSamples_ = 0;
    double samplesPerPixel_ = 1000.0;
    double scrollOffset_ = 0.0;
    double sampleRate_ = 48000.0;
    bool playing_ = false;

    QScrollBar* hScroll_ = nullptr;
    QScrollBar* vScroll_ = nullptr;

    int dragStartX_ = -1;
    double dragStartScroll_ = 0.0;

    static constexpr int kLabelWidth = 120;
    static constexpr int kTrackHeight = 72;
    static constexpr int kTrackGap = 2;
};
