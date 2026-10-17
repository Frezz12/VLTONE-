#pragma once
#include "Internal/ReverbInstance.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"
#include <QAbstractButton>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QToolButton>

namespace reverbUi {
QColor background();
QColor surface();
QColor ink();
QColor muted();
QColor violet();
QColor turquoise();
QColor opacity(QColor, int alpha);
QFont font(const QFont&, int pixels, bool medium = false);
void spaceGlyph(QPainter&, const QRectF&, unsigned mode, const QColor&);
}

// Native numeric semantics and accessibility, with a painted value well.
class ReverbValue final : public QDoubleSpinBox, public ui::graphics::ScenePaintSource {
public:
    explicit ReverbValue(QWidget* parent = nullptr);
    void paintScene(QPainter&, const QRegion&) override;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;
};

class ReverbModeButton final : public QAbstractButton, public ui::graphics::ScenePaintSource {
public:
    ReverbModeButton(unsigned mode, QWidget* parent);
    QSize sizeHint() const override { return {150, 49}; }
    void paintScene(QPainter&, const QRegion&) override;
protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    unsigned m_mode;
};

class ReverbPresetButton final : public QToolButton, public ui::graphics::ScenePaintSource {
public:
    explicit ReverbPresetButton(QWidget* parent);
    void paintScene(QPainter&, const QRegion&) override;
protected:
    void paintEvent(QPaintEvent*) override;
};

// The parameter's illustration is also its drag surface. A native value field
// handles precise entry; all gestures use the same binding and one undo commit.
class ReverbSlider final : public QSlider, public ui::graphics::ScenePaintSource {
    Q_OBJECT
public:
    ReverbSlider(const daw::plugins::ParameterInfo&, QWidget* parent);
    void setPlainValue(double);
    double plainValue() const;
    void editPlainValue(double);
    void finishEditing();
    void setDuckGain(double);
    bool isEditing() const;
    ReverbValue* number() const { return m_number; }
    QRectF trackRect() const;
    QPointF handlePoint() const;
    QString displayText() const;
    const daw::plugins::ParameterInfo& parameter() const { return m_info; }
    void paintScene(QPainter&, const QRegion&) override;
signals:
    void plainValueChanged(double);
    void editFinished();
    void automationRequested(const QString&);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent*) override;
private:
    double fraction(double) const;
    double fromFraction(double) const;
    void dragTo(QPointF, Qt::KeyboardModifiers);
    void commit(double);
    daw::plugins::ParameterInfo m_info;
    ReverbValue* m_number;
    QPointF m_lastPointer;
    double m_plain = 0, m_dragFraction = 0, m_duckGain = 1;
    bool m_dragging = false, m_dirty = false, m_syncing = false, m_logarithmic = false;
};
