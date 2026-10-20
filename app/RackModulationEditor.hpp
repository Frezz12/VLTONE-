#pragma once
#include "Modulation/Modulation.hpp"
#include <QWidget>
#include <QCoreApplication>
#include <QPainterPath>
#include <array>
#include <functional>

// The same editor is used in the Rack and its enlarged, nonmodal window.
class RackModulationEditor final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(RackModulationEditor)
public:
    explicit RackModulationEditor(QWidget* parent = nullptr);
    ~RackModulationEditor() override;
    void setSource(const daw::modulation::Source&);
    const daw::modulation::Source& source() const { return m_source; }
    void sample(float value, float position);
    void finishGesture();
    void removeSelected();
    std::function<void()> begin, finish;
    std::function<void(const daw::modulation::Source&)> changed;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    QRectF plot() const;
    QPointF point(double x, double y) const;
    QPointF normalized(QPointF, Qt::KeyboardModifiers) const;
    void apply();
    void drawSteps(QPointF);
    void cachePath();
    daw::modulation::Source m_source;
    std::array<float, 128> m_history{};
    QPainterPath m_path;
    bool m_pathDirty = true, m_dragging = false, m_bending = false, m_snap = true, m_keyEditing = false;
    int m_active = 0, m_lastStep = -1;
    double m_lastValue = 0, m_initialBend = 0;
    QPointF m_down, m_offset;
    float m_position = 0;
};
