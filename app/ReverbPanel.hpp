#pragma once
#include "PluginReadout.hpp"
#include "UiFrameClock.hpp"
#include "graphics/ScenePaintSource.hpp"
#include <QWidget>
#include <functional>
#include <vector>

class RackParameterBinding;
class QAbstractButton;
class QToolButton;
class QTimer;
class QStackedWidget;
class QMenu;
class ReverbSlider;
class ReverbValue;
namespace daw { class EngineController; }

class ReverbCanvas final : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
public:
    explicit ReverbCanvas(bool response, QWidget* parent = nullptr);
    void setSnapshot(const daw::ReverbSnapshot&, bool reducedMotion);
    void setEarlyView(bool early);
    void paintScene(QPainter&, const QRegion&) override;
    QRectF fieldRect() const;
    QPointF point() const;
    bool animationActive() const;
signals:
    void valuesChanged(double size, double brightness);
    void gestureFinished();
    void automationRequested(const QString& id);
    void parameterChanged(const QString& id, double value);
    void parameterGestureFinished(const QString& id);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool event(QEvent*) override;
private:
    void movePoint(QPointF);
    void finishGesture();
    void dragTo(QPointF, Qt::KeyboardModifiers);
    daw::ReverbSnapshot m_snapshot;
    ui::FrameTimer* m_animation;
    QPointF m_lastPointer;
    double m_phase = 0;
    double m_responseTimeMax = 1;
    int m_dragParameter = -1;
    bool m_response = false, m_earlyView = false, m_dragging = false, m_reduced = false;
};

class ReverbPanel final : public QWidget, public ui::graphics::ScenePaintSource {
    Q_OBJECT
public:
    ReverbPanel(daw::EngineController*, QString channel, QString slot, QWidget* parent = nullptr);
    ~ReverbPanel() override;
    void refresh();
    void applyFactoryPreset(unsigned index);
    void paintScene(QPainter&, const QRegion&) override;
signals:
    void projectEdited();
    void automationRequested(const QString& parameterId);
protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void applyTheme();
    void buildPresetMenu();
    void saveUserPreset();
    void applyValues(const daw::plugins::reverb::Values&, const QString& kind, const QString& name);
    daw::EngineController* m_controller;
    RackParameterBinding* m_binding;
    ReverbCanvas *m_field, *m_response;
    QTimer* m_timer;
    QToolButton *m_preset, *m_a, *m_b, *m_copy, *m_advanced;
    QMenu* m_presetMenu;
    QAbstractButton* m_freeze;
    QStackedWidget* m_controls;
    QWidget* m_shimmerControl = nullptr;
    std::array<QAbstractButton*, 6> m_modes{};
    std::vector<ReverbSlider*> m_sliders;
    std::array<ReverbValue*,2> m_coordinates{};
};
