#pragma once

#include "UiFrameClock.hpp"
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QScrollBar>
#include <deque>

// Arrangement-only controls. The QScrollBar contract remains available to
// accessibility, keyboard input and the existing viewport synchronisation.
class TimelineNavigationRail final : public QScrollBar {
    Q_OBJECT
public:
    TimelineNavigationRail(Qt::Orientation, QWidget* parent);
    void setPlayheadPosition(double x);
    void setLeadingConnected(bool connected);
    void cancelInteraction();
signals:
    void interactionStarted();
protected:
    void paintEvent(QPaintEvent*) override;
    void sliderChange(SliderChange) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    void dragTo(const QPointF&);
    void applyValue(double);
    void finishDrag();
    struct Sample { qint64 ns; double value; };
    std::deque<Sample> m_samples;
    QElapsedTimer m_dragClock, m_coastClock;
    ui::FrameTimer m_coast;
    QEasingCurve m_coastEase;
    double m_lastPointer = 0, m_dragValue = 0, m_unitsPerPixel = 1;
    double m_lastDelta = 0, m_coastFrom = 0, m_coastTo = 0, m_playheadX = -1;
    bool m_applyingValue = false;
    bool m_leadingConnected = false;
};

class TimelineNavigationCorner final : public ui::FrameWidget {
public:
    explicit TimelineNavigationCorner(QWidget* parent);
protected:
    void paintEvent(QPaintEvent*) override;
};
