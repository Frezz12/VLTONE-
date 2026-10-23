#include "RackModulationEditor.hpp"
#include "Theme.hpp"
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

using namespace daw::modulation;
RackModulationEditor::RackModulationEditor(QWidget* parent) : QWidget(parent) {
    setObjectName("ModulationGraph");
    setMinimumSize(120, 72);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { update(); });
}
RackModulationEditor::~RackModulationEditor() { finishGesture(); }
void RackModulationEditor::setSource(const Source& source) {
    if (m_source == source) return;
    m_source = source;
    m_active = std::clamp(m_active, 0, int(source.kind == Kind::Steps ? source.steps.size() : source.points.size()) - 1);
    m_pathDirty = true;
    setAccessibleName(QString::fromStdString(source.name));
    setToolTip(source.kind == Kind::Shaper
        ? tr("Click to add a point. Drag to move. Shift-drag a segment to bend it. Alt bypasses the grid. Delete removes the selected point. Right-click for segment shapes.")
        : source.kind == Kind::Steps ? tr("Drag to paint steps. Left/Right selects a step; Up/Down changes its value.") : QString{});
    update();
}
QRectF RackModulationEditor::plot() const { return QRectF(rect()).adjusted(7, 8, -7, -8); }
QPointF RackModulationEditor::point(double x, double y) const {
    const auto r = plot(); return {r.left() + x * r.width(), r.bottom() - y * r.height()};
}
QPointF RackModulationEditor::normalized(QPointF p, Qt::KeyboardModifiers mods) const {
    const auto r = plot();
    double x = (p.x() - r.left()) / r.width();
    if (m_snap && !(mods & Qt::AltModifier)) x = std::round(x * 16) / 16;
    return {std::clamp(x, 0., 1.), std::clamp((r.bottom() - p.y()) / r.height(), 0., 1.)};
}
void RackModulationEditor::sample(float value, float position) {
    if (visibleRegion().isEmpty()) return;
    if (m_source.kind == Kind::Follower || m_source.kind == Kind::Random) {
        std::move(m_history.begin() + 1, m_history.end(), m_history.begin());
        m_history.back() = std::clamp(value, 0.f, 1.f);
        m_pathDirty = true;
    } else if (m_position == position) return;
    m_position = position; update();
}
void RackModulationEditor::cachePath() {
    m_path = {};
    if (m_source.kind == Kind::Steps) return;
    bool first = true;
    const auto append = [&](double x, double y) {
        if (first) { m_path.moveTo(point(x, y)); first = false; } else m_path.lineTo(point(x, y));
    };
    if (m_source.kind == Kind::Follower || m_source.kind == Kind::Random) {
        for (unsigned i = 0; i < m_history.size(); ++i) append(double(i) / (m_history.size() - 1), m_history[i]);
    } else if (m_source.kind == Kind::Shaper) {
        for (std::size_t i = 1; i < m_source.points.size(); ++i) {
            const auto& a = m_source.points[i - 1]; const auto& b = m_source.points[i];
            for (int j = 0; j < 32; ++j) {
                const double t = j / 32.;
                append(std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, daw::engine::curve::shapeT(t, a.shape, a.bend)));
            }
            if (a.shape == daw::engine::curve::Shape::Hold) append(b.x, a.y);
            append(b.x, b.y);
        }
    } else {
        const int count = std::max(64, width());
        for (int i = 0; i <= count; ++i) append(double(i) / count, waveValue(m_source.wave, double(i) / count));
    }
    m_pathDirty = false;
}
void RackModulationEditor::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen); p.setBrush(th().surface); p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 4, 4);
    const auto r = plot(); p.setPen(QPen(th().separator(), 1));
    for (int i = 0; i <= 4; ++i) p.drawLine(point(i / 4., 0), point(i / 4., 1));
    p.drawLine(point(0, .5), point(1, .5));
    p.drawLine(point(0, 0), point(1, 0));
    if (m_source.kind == Kind::Steps) {
        const auto count = m_source.steps.size(); const auto w = r.width() / count;
        for (std::size_t i = 0; i < count; ++i) {
            QColor fill = th().accent; fill.setAlphaF(m_source.enabled ? .65 : .25);
            p.setBrush(fill); p.setPen(Qt::NoPen);
            const auto top = point(double(i) / count, m_source.steps[i]);
            p.drawRoundedRect(QRectF(top.x() + 1, top.y(), std::max(1., w - 2), std::max(2., r.bottom() - top.y())), 1.5, 1.5);
            if (hasFocus() && int(i) == m_active) {
                p.setBrush(Qt::NoBrush); p.setPen(QPen(th().textPrimary, 1));
                p.drawRect(QRectF(r.left() + i * w, r.top(), w, r.height()));
            }
        }
    } else {
        if (m_pathDirty) cachePath();
        p.setPen(QPen(m_source.enabled ? th().accent : th().textSecondary, 1.7)); p.setBrush(Qt::NoBrush); p.drawPath(m_path);
    }
    if (hasRate(m_source.kind) && m_source.kind != Kind::Random) {
        auto color = th().textPrimary; color.setAlpha(110); p.setPen(QPen(color, 1));
        p.drawLine(point(m_position, 0), point(m_position, 1));
    }
    if (m_source.kind == Kind::Shaper) for (std::size_t i = 0; i < m_source.points.size(); ++i) {
        const auto& pt = m_source.points[i];
        p.setPen(QPen(th().accent, 1)); p.setBrush(int(i) == m_active && hasFocus() ? th().textPrimary : th().surface);
        p.drawEllipse(point(pt.x, pt.y), 3.5, 3.5);
    }
}
void RackModulationEditor::resizeEvent(QResizeEvent* e) { m_pathDirty = true; QWidget::resizeEvent(e); }
void RackModulationEditor::apply() {
    m_pathDirty = true;
    if (changed) changed(m_source);
    update();
}
void RackModulationEditor::drawSteps(QPointF p) {
    const auto count = int(m_source.steps.size()); const auto r = plot();
    const int step = std::clamp(int((p.x() - r.left()) / r.width() * count), 0, count - 1);
    const double value = std::clamp((r.bottom() - p.y()) / r.height(), 0., 1.);
    if (m_lastStep < 0 || m_lastStep == step) m_source.steps[step] = value;
    else for (int i = std::min(step, m_lastStep); i <= std::max(step, m_lastStep); ++i)
        m_source.steps[i] = std::lerp(m_lastValue, value, double(i - m_lastStep) / (step - m_lastStep));
    m_active = m_lastStep = step; m_lastValue = value; apply();
}
void RackModulationEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    setFocus(Qt::MouseFocusReason);
    // Read-only wave previews are still selectable sources. Consuming the click
    // prevents the rack background from taking focus and starting a marquee.
    if (m_source.kind != Kind::Shaper && m_source.kind != Kind::Steps) { e->accept(); return; }
    finishGesture();
    setFocus(); m_dragging = true; m_down = e->position(); m_offset = {}; m_lastStep = -1;
    if (begin) begin();
    if (m_source.kind == Kind::Steps) { drawSteps(e->position()); return; }
    double best = 12; int nearest = -1;
    for (std::size_t i = 0; i < m_source.points.size(); ++i) {
        const auto& pt = m_source.points[i]; const auto distance = QLineF(point(pt.x, pt.y), e->position()).length();
        if (distance < best) { best = distance; nearest = int(i); }
    }
    const auto at = normalized(e->position(), e->modifiers());
    m_bending = e->modifiers() & Qt::ShiftModifier;
    if (m_bending) {
        m_active = 0;
        for (std::size_t i = 1; i < m_source.points.size(); ++i) if (m_source.points[i].x <= at.x()) m_active = int(i);
        m_active = std::min(m_active, int(m_source.points.size()) - 2);
        m_initialBend = m_source.points[m_active].bend;
    } else if (nearest >= 0) {
        m_active = nearest;
        const auto& pt = m_source.points[m_active]; m_offset = e->position() - point(pt.x, pt.y);
    } else {
        auto it = std::lower_bound(m_source.points.begin(), m_source.points.end(), at.x(), [](const auto& p, double x) { return p.x < x; });
        m_active = int(it - m_source.points.begin());
        if (it != m_source.points.end() && std::abs(it->x - at.x()) < 1e-5) it->y = at.y();
        else if (it != m_source.points.begin() && std::abs((it - 1)->x - at.x()) < 1e-5) { --m_active; (it - 1)->y = at.y(); }
        else if (m_source.points.size() < 64 && at.x() > 0 && at.x() < 1) m_source.points.insert(it, ShapePoint{at.x(), at.y()});
        else { finishGesture(); return; }
        m_active = std::min(m_active, int(m_source.points.size()) - 1);
        apply();
    }
    update();
}
void RackModulationEditor::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    if (!(e->buttons() & Qt::LeftButton)) { finishGesture(); return; }
    if (m_source.kind == Kind::Steps) { drawSteps(e->position()); return; }
    auto& pt = m_source.points[m_active];
    if (m_bending) {
        pt.shape = daw::engine::curve::Shape::Linear;
        pt.bend = std::clamp(m_initialBend + (m_down.y() - e->position().y()) / 80, -1., 1.);
    } else {
        const auto at = normalized(e->position() - m_offset, e->modifiers());
        if (m_active > 0 && m_active + 1 < int(m_source.points.size()))
            pt.x = std::clamp(at.x(), std::nextafter(m_source.points[m_active - 1].x, 1.), std::nextafter(m_source.points[m_active + 1].x, 0.));
        pt.y = at.y();
    }
    apply();
}
void RackModulationEditor::finishGesture() {
    if (!m_dragging && !m_keyEditing) return;
    m_dragging = m_bending = m_keyEditing = false;
    if (finish) finish();
}
void RackModulationEditor::mouseReleaseEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) finishGesture(); }
void RackModulationEditor::focusOutEvent(QFocusEvent* e) { finishGesture(); QWidget::focusOutEvent(e); update(); }
void RackModulationEditor::removeSelected() {
    finishGesture();
    if (m_source.kind != Kind::Steps && (m_source.kind != Kind::Shaper || m_active == 0 || m_active + 1 == int(m_source.points.size()))) return;
    if (begin) begin();
    if (m_source.kind == Kind::Steps) m_source.steps[m_active] = 0;
    else { m_source.points.erase(m_source.points.begin() + m_active); m_active = std::min(m_active, int(m_source.points.size()) - 1); }
    apply(); if (finish) finish();
}
void RackModulationEditor::keyPressEvent(QKeyEvent* e) {
    if (m_source.kind != Kind::Steps && m_source.kind != Kind::Shaper) { QWidget::keyPressEvent(e); return; }
    const int count = int(m_source.kind == Kind::Steps ? m_source.steps.size() : m_source.points.size());
    if ((e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) && (m_source.kind == Kind::Steps || e->modifiers() & Qt::ControlModifier)) {
        m_active = std::clamp(m_active + (e->key() == Qt::Key_Left ? -1 : 1), 0, count - 1); update(); return;
    }
    const bool arrow = e->key() >= Qt::Key_Left && e->key() <= Qt::Key_Down;
    if (!arrow && e->key() != Qt::Key_Delete && e->key() != Qt::Key_Backspace) { QWidget::keyPressEvent(e); return; }
    if (!m_keyEditing) { if (begin) begin(); m_keyEditing = true; }
    const double amount = e->modifiers() & Qt::ShiftModifier ? .1 : .01;
    if (m_source.kind == Kind::Steps) {
        auto& v = m_source.steps[m_active];
        if (arrow) v = std::clamp(v + (e->key() == Qt::Key_Up ? amount : -amount), 0., 1.); else v = 0;
    } else {
        auto& pt = m_source.points[m_active];
        if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) pt.y = std::clamp(pt.y + (e->key() == Qt::Key_Up ? amount : -amount), 0., 1.);
        else if (m_active > 0 && m_active + 1 < count) {
            if (arrow) pt.x = std::clamp(pt.x + (e->key() == Qt::Key_Right ? 1./16 : -1./16), std::nextafter(m_source.points[m_active - 1].x, 1.), std::nextafter(m_source.points[m_active + 1].x, 0.));
            else { m_source.points.erase(m_source.points.begin() + m_active); m_active = std::min(m_active, count - 2); }
        }
    }
    apply();
}
void RackModulationEditor::keyReleaseEvent(QKeyEvent* e) {
    if (m_keyEditing && !e->isAutoRepeat()) finishGesture();
    QWidget::keyReleaseEvent(e);
}
void RackModulationEditor::contextMenuEvent(QContextMenuEvent* e) {
    if (m_source.kind != Kind::Shaper) return;
    finishGesture();
    if (e->reason() == QContextMenuEvent::Mouse) {
        double best = 12; int nearest = -1;
        for (std::size_t i = 0; i < m_source.points.size(); ++i) {
            const auto& pt = m_source.points[i]; const auto distance = QLineF(point(pt.x, pt.y), e->pos()).length();
            if (distance < best) { best = distance; nearest = int(i); }
        }
        if (nearest >= 0) m_active = nearest;
        else {
            const auto at = normalized(e->pos(), Qt::AltModifier);
            m_active = 0;
            for (std::size_t i = 1; i + 1 < m_source.points.size(); ++i)
                if (m_source.points[i].x <= at.x()) m_active = int(i);
        }
        update();
    }
    QMenu menu(this);
    auto* snap = menu.addAction(tr("Snap to 1/16 grid")); snap->setCheckable(true); snap->setChecked(m_snap);
    menu.addSeparator();
    auto* linear = menu.addAction(tr("Linear segment")); auto* hold = menu.addAction(tr("Hold segment")); auto* smooth = menu.addAction(tr("S-curve segment"));
    auto* bend = menu.addAction(tr("Bend…")); auto* remove = menu.addAction(tr("Delete point"));
    remove->setEnabled(m_active > 0 && m_active + 1 < int(m_source.points.size()));
    const auto action = menu.exec(e->globalPos());
    if (!action) return;
    if (action == snap) { m_snap = snap->isChecked(); return; }
    double value = m_source.points[std::min(m_active, int(m_source.points.size()) - 2)].bend;
    if (action == bend) {
        bool ok; value = QInputDialog::getDouble(this, tr("Bend"), tr("Bend · −1…1"), value, -1, 1, 2, &ok);
        if (!ok) return;
    }
    if (begin) begin();
    if (action == remove) { m_source.points.erase(m_source.points.begin() + m_active); m_active = std::min(m_active, int(m_source.points.size()) - 1); }
    else {
        auto& pt = m_source.points[std::min(m_active, int(m_source.points.size()) - 2)];
        pt.shape = action == hold ? daw::engine::curve::Shape::Hold : action == smooth ? daw::engine::curve::Shape::SCurve : daw::engine::curve::Shape::Linear;
        pt.bend = action == bend ? value : 0;
    }
    apply(); if (finish) finish();
}
