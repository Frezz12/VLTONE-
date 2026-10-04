#include "ScrollMotion.hpp"
#include "WarpEditorWidget.hpp"
#include "EngineController.hpp"
#include "Internal/SamplerVoice.hpp"
#include "Theme.hpp"
#include "UiFrameClock.hpp"
#include "WaveformPaint.hpp"
#include "Icons.hpp"
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QToolButton>
#include <QPainterPath>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QWheelEvent>
#include <QApplication>

class WarpCanvas : public ui::FrameWidget {
public:
    static QString tr(const char* text) { return WarpEditorWidget::tr(text); }
    explicit WarpCanvas(WarpEditorWidget* editor) : ui::FrameWidget(editor), e(editor) {
        setObjectName("WarpCanvas"); setAccessibleName(tr("Audio warp markers"));
        setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setMinimumHeight(50);
    }
    double x(double beats) const { return 12 + (beats - e->m_view.start) / e->m_view.span * std::max(1, width() - 24); }
    double beat(double x) const { return e->m_view.start + (x - 12) / std::max(1, width() - 24) * e->m_view.span; }
    void redraw() { dirty = true; update(); }
    int hit(double position) const {
        const auto* c = e->clip(); if (!c) return -1;
        int found = -1; double distance = 8;
        for (int i = 0; i < int(c->warp.markers.size()); ++i)
            if (const double d = std::abs(x(c->warp.markers[i].targetBeats) - position); d < distance) { distance = d; found = i; }
        return found;
    }
    void cancel() {
        if (dragging) { e->m_controller->cancelWarpEdit(); dragging = false; releaseMouse(); emit e->liveEdited(); }
        if (ranging) e->setRange(oldRange.first, oldRange.second);
        if (marquee) e->m_selected = oldSelection;
        ranging = marquee = false; e->m_dragLabel->hide(); e->redraw();
    }
    void finish() { if (dragging) { dragging = false; releaseMouse(); e->m_controller->commitWarpEdit(); emit e->edited(); } e->m_dragLabel->hide(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); const auto& theme = ThemeManager::instance().theme();
        const auto pixels = (size() * devicePixelRatioF()).expandedTo(QSize(1, 1));
        if (dirty || cache.size() != pixels || cache.devicePixelRatioF() != devicePixelRatioF() || cachedStyle != ui::waveformStyle()) {
            cachedStyle = ui::waveformStyle();
            cache = QPixmap(pixels); cache.setDevicePixelRatio(devicePixelRatioF());
            cache.fill(Qt::transparent); QPainter cached(&cache); paintContent(cached); dirty = false;
        }
        p.drawPixmap(0, 0, cache);
        if (const auto* c = e->clip(); c && !c->warp.empty()) {
            const double play = daw::secondsToBeats(e->m_controller->positionSeconds() - c->startSeconds, e->m_controller->tempo());
            if (play >= 0 && play <= c->warp.markers.back().targetBeats) {
                p.setPen(QPen(theme.cursor, 1.5)); p.drawLine(QPointF(x(play), 0), QPointF(x(play), height()));
            }
        }
        if (marquee) { auto fill = theme.accent; fill.setAlpha(28); p.setBrush(fill); p.setPen(theme.accent); p.drawRect(selection.normalized()); }
        if (hasFocus()) { p.setPen(theme.accent); p.setBrush(Qt::NoBrush); p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8); }
    }
    void paintContent(QPainter& p) {
        const auto& theme = ThemeManager::instance().theme();
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath shape; shape.addRoundedRect(rect(), Theme::cornerRadius, Theme::cornerRadius); p.setClipPath(shape);
        p.fillRect(rect(), theme.well());
        const auto* c = e->clip();
        if (!c || c->warp.empty()) { p.setPen(theme.textSecondary); p.drawText(rect(), Qt::AlignCenter, tr("Right-click an audio clip and choose Warp Audio")); return; }
        const auto& map = e->m_previewing && e->m_after->isChecked() ? e->m_proposal.map : c->warp;
        const double tempo = e->m_controller->tempo();
        const double origin = daw::secondsToBeats(c->startSeconds, tempo);
        const double beatUnit = 4. / std::max(1, e->m_controller->project().timeSigDenominator);
        const double step = std::max(beatUnit, std::pow(2., std::ceil(std::log2(e->m_view.span / std::max(1., width() / 65.)))));
        p.setPen(theme.gridLine);
        for (double at = std::floor((origin + e->m_view.start) / step) * step - origin; at <= e->m_view.start + e->m_view.span; at += step) {
            const double px = x(at); p.setPen(theme.gridLine); p.drawLine(QPointF(px, 28), QPointF(px, height()));
            p.setPen(theme.textSecondary);
            const int beatsPerBar = std::max(1, e->m_controller->project().timeSigNumerator);
            const double absolute = std::floor(std::max(0., origin + at) / beatUnit + 1e-7);
            p.drawText(QRectF(px + 4, 2, 65, 23), Qt::AlignLeft | Qt::AlignVCenter,
                QString("%1.%2").arg(int(absolute) / beatsPerBar + 1).arg(int(absolute) % beatsPerBar + 1));
        }
        if (e->m_rangeFinish > e->m_rangeBegin) {
            auto fill = theme.accent; fill.setAlpha(23);
            p.fillRect(QRectF(x(e->m_rangeBegin), 0, x(e->m_rangeFinish) - x(e->m_rangeBegin), height()), fill);
            fill.setAlpha(65); p.fillRect(QRectF(x(e->m_rangeBegin), 0, x(e->m_rangeFinish) - x(e->m_rangeBegin), 27), fill);
        }
        const QRectF waveArea(0, 49, width(), std::max(10, height() - 68));
        auto paintSegment = [&](double a, double b, double source, double speed) {
            const QRectF area(x(a), waveArea.top(), x(b) - x(a), waveArea.height());
            if (!area.intersects(rect())) return;
            ui::PeakPaint how; how.sourceStartSeconds = source;
            how.secondsPerPixel = speed * 60. / tempo * e->m_view.span / std::max(1, width() - 24);
            how.clipLeft = std::max(0., area.left()); how.clipRight = std::min(double(width()), area.right());
            how.color = map.enabled ? theme.waveform : theme.textSecondary;
            how.samples = e->m_audio.get();
            ui::paintPeaks(p, e->m_peaks.get(), area, how);
        };
        if (map.enabled) {
            for (std::size_t i = 0; i + 1 < map.markers.size(); ++i) {
                const auto& a = map.markers[i]; const auto& b = map.markers[i + 1];
                const double speed = daw::warpSpeedAt(map, a.targetBeats, tempo);
                paintSegment(a.targetBeats, b.targetBeats, a.sourceSeconds, speed);
                if (x(b.targetBeats) - x(a.targetBeats) > 65) {
                    p.setPen(theme.textSecondary);
                    p.drawText(QRectF(x(a.targetBeats) + 6, 28, x(b.targetBeats) - x(a.targetBeats) - 12, 19),
                        Qt::AlignCenter, QString::number(1 / speed, 'f', 2) + "x");
                }
            }
        } else {
            paintSegment(0, daw::secondsToBeats(c->durationSeconds, tempo), map.markers.front().sourceSeconds,
                (map.markers.back().sourceSeconds - map.markers.front().sourceSeconds) / c->durationSeconds);
        }
        if (map.enabled) for (const auto& t : e->m_transients) {
            if (t.strength < 1 - map.sensitivity / 100. || t.sourceSeconds <= map.markers.front().sourceSeconds ||
                t.sourceSeconds >= map.markers.back().sourceSeconds) continue;
            const double px = x(daw::warpBeatAt(map, t.sourceSeconds));
            const bool uncertain = t.confidence < .65;
            p.setPen(QPen(uncertain ? theme.textSecondary : theme.waveform, 1, Qt::DotLine));
            p.drawLine(QPointF(px, 51), QPointF(px, height() - 14));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(px, 49), uncertain ? 2.5 : 3.5, uncertain ? 2.5 : 3.5);
        }
        if (e->m_previewing) for (const auto& marker : e->m_proposal.map.markers) {
            const double original = daw::warpBeatAt(c->warp, marker.sourceSeconds);
            if (std::abs(marker.targetBeats - original) < 1e-6) continue;
            const double from = x(original), to = x(marker.targetBeats);
            p.setPen(QPen(Theme::automationAccent(), 1, Qt::DashLine));
            p.drawLine(QPointF(from, 46), QPointF(to, 46));
            p.drawLine(QPointF(from, 54), QPointF(from, height() - 16));
            p.setPen(Theme::automationAccent()); p.setBrush(Theme::automationAccent());
            const double sign = to > from ? 1 : -1;
            p.drawPolygon(QPolygonF{QPointF(to, 46), QPointF(to - sign * 4, 43), QPointF(to - sign * 4, 49)});
        }
        for (const auto& marker : map.markers) {
            const double px = x(marker.targetBeats);
            const bool selected = e->m_selected.contains(marker.id);
            p.setPen(QPen(selected ? theme.accentHighlight : theme.accent, selected ? 2 : 1));
            p.drawLine(QPointF(px, 28), QPointF(px, height() - 12));
            p.setBrush(selected ? theme.accentHighlight : theme.accent);
            if (marker.locked) { p.drawRoundedRect(QRectF(px - 4, 33, 8, 7), 1, 1); p.setBrush(Qt::NoBrush); p.drawArc(QRectF(px - 3, 28, 6, 9), 0, 180 * 16); }
            else p.drawPolygon(QPolygonF{QPointF(px - 5, 29), QPointF(px + 5, 29), QPointF(px, 39)});
        }
        if (!e->m_audio) { p.setPen(theme.textSecondary); p.drawText(waveArea, Qt::AlignCenter, tr("Source audio is unavailable.")); }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        setFocus(); const auto* c = e->clip(); if (!c || !c->warp.enabled) return;
        if (event->position().y() < 27) {
            oldRange = {e->m_rangeBegin, e->m_rangeFinish}; ranging = true;
            startBeat = beat(event->position().x()); return;
        }
        if (e->m_previewing) return;
        const int index = hit(event->position().x());
        if (index >= 0) {
            const auto marker = c->warp.markers[index];
            if (event->modifiers() & Qt::ControlModifier) {
                if (!e->m_selected.erase(marker.id)) e->m_selected.insert(marker.id);
            } else if (!e->m_selected.contains(marker.id)) { e->m_selected = {marker.id}; }
            e->updateInspector();
            if (marker.locked || !e->m_selected.contains(marker.id)) { e->redraw(); return; }
            before = c->warp; startBeat = beat(event->position().x());
            e->m_controller->beginWarpEdit(e->m_trackId.toStdString(), e->m_clipId.toStdString());
            dragging = true; grabMouse(); e->redraw(); return;
        }
        for (const auto& t : e->m_transients) if (t.strength >= 1 - c->warp.sensitivity / 100.) {
            const double b = daw::warpBeatAt(c->warp, t.sourceSeconds);
            if (std::abs(x(b) - event->position().x()) < 6) { e->addMarker(b); return; }
        }
        oldSelection = e->m_selected;
        if (!(event->modifiers() & Qt::ControlModifier)) e->m_selected.clear();
        selection = QRectF(event->position(), QSizeF()); marquee = true;
        e->updateInspector(); e->redraw();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (ranging) { e->setRange(std::min(startBeat, beat(event->position().x())), std::max(startBeat, beat(event->position().x()))); return; }
        if (marquee) {
            selection.setBottomRight(event->position());
            e->m_selected = event->modifiers() & Qt::ControlModifier ? oldSelection : std::set<std::string>{};
            if (const auto* c = e->clip()) for (const auto& m : c->warp.markers)
                if (selection.normalized().contains(QPointF(x(m.targetBeats), selection.center().y()))) e->m_selected.insert(m.id);
            e->updateInspector(); e->redraw(); return;
        }
        if (dragging) {
            e->moveSelected(beat(event->position().x()) - startBeat, before,
                            e->m_snap->isChecked() && !(event->modifiers() & Qt::AltModifier));
            emit e->liveEdited();
            e->m_dragLabel->adjustSize();
            e->m_dragLabel->move(std::clamp(int(event->position().x()) + 14, 0, std::max(0, width() - e->m_dragLabel->width())),
                std::max(50, std::min(int(event->position().y()) + 14, height() - e->m_dragLabel->height())));
            e->m_dragLabel->show(); e->m_dragLabel->raise();
        } else setCursor(hit(event->position().x()) >= 0 ? Qt::SizeHorCursor : Qt::CrossCursor);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (ranging || marquee) { ranging = marquee = false; e->redraw(); return; }
        if (dragging && event->button() == Qt::LeftButton) {
            dragging = false; releaseMouse(); e->m_controller->commitWarpEdit(); e->m_dragLabel->hide(); emit e->edited(); e->redraw();
        }
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        cancel(); if (event->position().y() < 27) { e->setRange(0, 0); return; }
        if (!e->m_previewing && event->button() == Qt::LeftButton && hit(event->position().x()) < 0) e->addMarker(beat(event->position().x()));
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) { cancel(); e->cancelPreview(); return; }
        if (e->m_previewing) { ui::FrameWidget::keyPressEvent(event); return; }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) { e->deleteMarkers(); return; }
        if (event->matches(QKeySequence::SelectAll)) {
            if (const auto* c = e->clip()) for (const auto& marker : c->warp.markers) e->m_selected.insert(marker.id);
            e->updateInspector(); e->redraw(); return;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            if (const auto* c = e->clip(); c && c->warp.enabled) {
                e->m_controller->beginWarpEdit(e->m_trackId.toStdString(), e->m_clipId.toStdString());
                e->moveSelected((event->key() == Qt::Key_Left ? -1 : 1) *
                    (event->modifiers() & Qt::ShiftModifier ? .01 : e->grid()), c->warp, false);
                e->m_controller->commitWarpEdit(); emit e->edited();
            }
            return;
        }
        ui::FrameWidget::keyPressEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        if (e->m_previewing) return;
        const auto* c = e->clip(); const int index = hit(event->pos().x());
        if (!c || !c->warp.enabled || index <= 0 || index + 1 >= int(c->warp.markers.size())) return;
        const auto marker = c->warp.markers[index];
        if (!e->m_selected.contains(marker.id)) e->m_selected = {marker.id};
        e->redraw();
        QMenu menu(this);
        auto* lock = menu.addAction(marker.locked ? tr("Unlock") : tr("Lock Position"));
        auto* reset = menu.addAction(tr("Reset Position")); reset->setEnabled(!marker.locked);
        auto* quantize = menu.addAction(tr("Quantize")); quantize->setEnabled(!marker.locked);
        auto* remove = menu.addAction(tr("Delete")); remove->setEnabled(!marker.locked);
        auto* chosen = menu.exec(event->globalPos());
        c = e->clip(); if (!c) return;
        auto map = c->warp;
        if (chosen == lock) {
            for (std::size_t i = 1; i + 1 < map.markers.size(); ++i)
                if (e->m_selected.contains(map.markers[i].id)) map.markers[i].locked = !marker.locked;
            e->apply(map, marker.locked ? "Unlock Warp Markers" : "Lock Warp Markers");
        } else if (chosen == remove) e->deleteMarkers();
        else if (chosen == quantize) e->quantize();
        else if (chosen == reset) {
            for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) {
                auto& item = map.markers[i];
                if (item.locked || !e->m_selected.contains(item.id)) continue;
                item.targetBeats = std::clamp((item.sourceSeconds - map.markers.front().sourceSeconds) /
                    (map.markers.back().sourceSeconds - map.markers.front().sourceSeconds) * map.markers.back().targetBeats,
                    map.markers[i - 1].targetBeats + 1e-5, map.markers[i + 1].targetBeats - 1e-5);
            }
            e->apply(map, "Reset Warp Markers");
        }
    }
    void wheelEvent(QWheelEvent* event) override {
        if (!e->clip()) return;
        const double amount = !event->pixelDelta().isNull() ? event->pixelDelta().y() / 60. : event->angleDelta().y() / 120.;
        if (event->modifiers() & Qt::ControlModifier) {
            ui::ScrollMotion::cancel(this);
            const double anchor = beat(event->position().x());
            const double fraction = (event->position().x() - 12) / std::max(1, width() - 24);
            e->m_view.span = std::clamp(e->m_view.span * std::pow(.8, amount), .02, 100000.);
            e->m_view.start = anchor - fraction * e->m_view.span;
        } else {
            const double dx = event->pixelDelta().x() ? event->pixelDelta().x() / 60. : amount;
            const double pixels=std::max(1,width()-24);
            ui::ScrollMotion::scroll(this,{-dx*pixels*.1,0},!event->pixelDelta().isNull(),
                [this,pixels]{return QPointF(e->m_view.start/e->m_view.span*pixels,0);},
                [this,pixels](QPointF p){e->m_view.start=std::max(0.,p.x()/pixels*e->m_view.span);e->redraw();});
        }
        e->m_view.start = std::max(0., e->m_view.start); e->redraw(); event->accept();
    }
    void hideEvent(QHideEvent* event) override { cancel(); ui::FrameWidget::hideEvent(event); }
private:
    WarpEditorWidget* e;
    bool dragging = false;
    bool dirty = true, marquee = false, ranging = false;
    QPixmap cache;
    ui::WaveformStyle cachedStyle = ui::waveformStyle();
    QRectF selection;
    std::set<std::string> oldSelection;
    std::pair<double, double> oldRange;
    double startBeat = 0;
    daw::ClipWarpModel before;
};

class WarpOverview : public ui::FrameWidget {
public:
    static QString tr(const char* text) { return WarpEditorWidget::tr(text); }
    explicit WarpOverview(WarpEditorWidget* editor) : ui::FrameWidget(editor), e(editor) {
        setObjectName("WarpOverview"); setAccessibleName(tr("Warp overview navigation"));
        setToolTip(tr("Drag to navigate · Drag the edges to zoom · Arrow keys to pan"));
        setFocusPolicy(Qt::StrongFocus); setFixedHeight(32); setCursor(Qt::OpenHandCursor);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); const auto& t = ThemeManager::instance().theme();
        p.setRenderHint(QPainter::Antialiasing); p.setPen(t.separator()); p.setBrush(t.well());
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 8, 8);
        const auto* c = e->clip(); if (!c || c->warp.empty()) return;
        auto map = e->m_previewing && e->m_after->isChecked() ? e->m_proposal.map : c->warp;
        if (!map.enabled) {
            map.markers = {map.markers.front(), map.markers.back()};
            map.markers.back().targetBeats = daw::secondsToBeats(c->durationSeconds, e->m_controller->tempo());
        }
        const double length = map.markers.back().targetBeats;
        for (std::size_t i = 0; i + 1 < map.markers.size(); ++i) {
            const auto& a = map.markers[i]; const auto& b = map.markers[i + 1];
            QRectF area(a.targetBeats / length * width(), 3, (b.targetBeats - a.targetBeats) / length * width(), height() - 6);
            ui::PeakPaint how; how.sourceStartSeconds = a.sourceSeconds;
            how.secondsPerPixel = (b.sourceSeconds - a.sourceSeconds) / std::max(1., area.width());
            how.clipLeft = area.left(); how.clipRight = area.right(); how.color = t.textSecondary;
            ui::paintPeaks(p, e->m_peaks.get(), area, how);
        }
        auto fill = t.accent; fill.setAlpha(35); p.setBrush(fill); p.setPen(t.accent);
        const double left = e->m_view.start / length * width(), right = (e->m_view.start + e->m_view.span) / length * width();
        p.drawRoundedRect(QRectF(left, 1, right - left, height() - 2), 6, 6);
        p.setPen(QPen(t.accent, 2));
        p.drawLine(QPointF(left + 3, 10), QPointF(left + 3, height() - 10));
        p.drawLine(QPointF(right - 3, 10), QPointF(right - 3, height() - 10));
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !e->clip() || e->clip()->warp.empty()) return;
        setFocus(); dragging = true;
        const double length = e->clip()->warp.markers.back().targetBeats;
        const double left = e->m_view.start / length * width(), right = (e->m_view.start + e->m_view.span) / length * width();
        edge = std::abs(event->position().x() - left) < 7 ? -1 : std::abs(event->position().x() - right) < 7 ? 1 : 0;
        move(event->position().x());
    }
    void mouseMoveEvent(QMouseEvent* event) override { if (dragging) move(event->position().x()); }
    void mouseReleaseEvent(QMouseEvent*) override { dragging = false; }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            e->m_view.start = std::max(0., e->m_view.start + (event->key() == Qt::Key_Left ? -1 : 1) * e->m_view.span * .1);
            e->redraw(); return;
        }
        ui::FrameWidget::keyPressEvent(event);
    }
private:
    void move(double x) {
        const double length = e->clip()->warp.markers.back().targetBeats;
        const double at = std::clamp(x / std::max(1, width()) * length, 0., length);
        if (edge < 0) {
            const double right = e->m_view.start + e->m_view.span;
            e->m_view.start = std::clamp(at, 0., std::max(0., right - .02)); e->m_view.span = right - e->m_view.start;
        } else if (edge > 0) e->m_view.span = std::max(.02, at - e->m_view.start);
        else e->m_view.start = std::clamp(at - e->m_view.span * .5, 0., std::max(0., length - e->m_view.span));
        e->redraw();
    }
    WarpEditorWidget* e;
    bool dragging = false;
    int edge = 0;
};

WarpEditorWidget::WarpEditorWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("WarpEditor"); setAccessibleName(tr("Warp audio editor"));
    m_projectGeneration = controller->projectGeneration();
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(12, 8, 12, 8); layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto* header = new QHBoxLayout; header->setSpacing(10);
    auto* brand = new QLabel("Warp", this); brand->setObjectName("WarpBrand");
    auto titleFont = font(); titleFont.setBold(true); brand->setFont(titleFont); header->addWidget(brand);
    m_title = new QLabel(tr("Select an audio clip"), this); m_title->setMinimumWidth(0);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); header->addWidget(m_title, 1);
    m_analysisLabel = new QLabel(this); m_analysisLabel->setObjectName("WarpAnalysisState"); header->addWidget(m_analysisLabel);
    m_options = new QToolButton(this); m_options->setObjectName("WarpInspectorButton"); m_options->setCheckable(true); m_options->setChecked(true);
    m_options->setAccessibleName(tr("Warp settings")); m_options->setToolTip(tr("Warp settings")); m_options->setFixedSize(28, 28); header->addWidget(m_options);
    auto* close = new QToolButton(this); close->setObjectName("WarpCloseButton"); close->setAccessibleName(tr("Close Warp")); close->setToolTip(tr("Close Warp")); close->setFixedSize(28, 28);
    header->addWidget(close); connect(close, &QToolButton::clicked, this, &WarpEditorWidget::closeRequested); layout->addLayout(header);

    m_tools = new QWidget(this); auto* toolGroups = new QBoxLayout(QBoxLayout::LeftToRight, m_tools);
    toolGroups->setContentsMargins(0, 0, 0, 0); toolGroups->setSpacing(6);
    auto* editGroup = new QWidget(m_tools); auto* editTools = new QHBoxLayout(editGroup); editTools->setContentsMargins(0, 0, 0, 0); editTools->setSpacing(6);
    auto* timingGroup = new QWidget(m_tools); auto* tools = new QHBoxLayout(timingGroup); tools->setContentsMargins(0, 0, 0, 0); tools->setSpacing(6);
    toolGroups->addWidget(editGroup); toolGroups->addWidget(timingGroup, 1);
    auto check = [&](const QString& text) { auto* w = new QCheckBox(text, editGroup); w->setAccessibleName(text); editTools->addWidget(w); return w; };
    m_enabled = check(tr("Warp")); m_enabled->setObjectName("WarpEnabled"); m_snap = check(tr("Snap")); m_snap->setChecked(true);
    m_grid = new QComboBox(m_tools); m_grid->setAccessibleName(tr("Warp grid")); m_grid->setObjectName("WarpGrid");
    m_grid->addItem(tr("Project grid"), 0.);
    for (const auto& item : {std::pair{"1/4", 1.}, {"1/8", .5}, {"1/16", .25}, {"1/32", .125}, {"1/8 triplet", 1./3}}) m_grid->addItem(item.first, item.second);
    m_grid->setMinimumWidth(110); m_grid->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_grid->setMinimumContentsLength(11); editTools->addWidget(m_grid);
    auto* fit = new QPushButton(tr("Fit"), editGroup); fit->setObjectName("WarpFit"); fit->setAccessibleName(tr("Fit")); editTools->addWidget(fit);
    connect(fit, &QPushButton::clicked, this, [this] { if (const auto* c = clip(); c && !c->warp.empty()) m_view = {0, c->warp.markers.back().targetBeats}; redraw(); });
    editTools->addStretch();
    auto button = [&](const QString& text, const char* name, auto callback) {
        auto* b = new QPushButton(text, m_tools); b->setObjectName(name); b->setAccessibleName(text); tools->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback); return b;
    };
    button(tr("Quantize"), "WarpQuantize", [this] { quantize(); });
    m_strength = new QSpinBox(m_tools); m_strength->setObjectName("WarpStrength"); m_strength->setRange(0, 100); m_strength->setValue(100); m_strength->setSuffix("%");
    m_strength->setAccessibleName(tr("Timing strength")); m_strength->setToolTip(tr("Timing strength")); tools->addWidget(m_strength);
    m_alignButton = button(tr("Suggest alignment"), "WarpSuggest", [this] { previewAlignment(); });
    tools->addStretch();
    layout->addWidget(m_tools);

    m_previewBar = new QWidget(this); m_previewBar->setObjectName("WarpPreviewBar"); auto* previewRow = new QHBoxLayout(m_previewBar);
    previewRow->setContentsMargins(8, 4, 8, 4); previewRow->setSpacing(4);
    auto* compare = new QButtonGroup(this); compare->setExclusive(true);
    m_before = new QToolButton(m_previewBar); m_after = new QToolButton(m_previewBar);
    m_before->setText(tr("Before")); m_after->setText(tr("After")); m_before->setObjectName("WarpBefore"); m_after->setObjectName("WarpAfter");
    for (auto* b : {m_before, m_after}) { b->setCheckable(true); b->setAccessibleName(b->text()); compare->addButton(b); previewRow->addWidget(b); }
    m_after->setChecked(true);
    m_previewSummary = new QLabel(m_previewBar); m_previewSummary->setMinimumWidth(0); m_previewSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); previewRow->addWidget(m_previewSummary, 1);
    auto* apply = new QPushButton(tr("Apply"), m_previewBar); apply->setObjectName("WarpApply"); previewRow->addWidget(apply);
    auto* cancel = new QPushButton(tr("Cancel"), m_previewBar); cancel->setObjectName("WarpCancel"); previewRow->addWidget(cancel);
    connect(apply, &QPushButton::clicked, this, &WarpEditorWidget::applyPreview); connect(cancel, &QPushButton::clicked, this, &WarpEditorWidget::cancelPreview);
    connect(m_after, &QToolButton::toggled, this, [this](bool after) { if (m_previewing) { m_controller->auditionWarpPreview(after); redraw(); } });
    m_previewBar->hide(); layout->addWidget(m_previewBar);

    m_body = new QWidget(this); m_bodyLayout = new QHBoxLayout(m_body); m_bodyLayout->setContentsMargins(0, 0, 0, 0); m_bodyLayout->setSpacing(8);
    m_waveHost = new QWidget(m_body); auto* waveLayout = new QVBoxLayout(m_waveHost); waveLayout->setContentsMargins(0, 0, 0, 0); waveLayout->setSpacing(5);
    m_canvas = new WarpCanvas(this); m_canvas->setMinimumWidth(80); waveLayout->addWidget(m_canvas, 1);
    m_overview = new WarpOverview(this); waveLayout->addWidget(m_overview); m_bodyLayout->addWidget(m_waveHost, 1);
    m_dragLabel = new QLabel(m_canvas); m_dragLabel->setObjectName("WarpDragReadout"); m_dragLabel->setAttribute(Qt::WA_TransparentForMouseEvents); m_dragLabel->hide();
    m_inspector = new QScrollArea(m_body); m_inspector->setObjectName("WarpInspector"); m_inspector->setWidgetResizable(true); m_inspector->setFrameShape(QFrame::NoFrame);
    m_inspector->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_inspector->setFixedWidth(248);
    auto* options = new QWidget; auto* form = new QFormLayout(options); form->setContentsMargins(10, 8, 10, 8); form->setSpacing(8); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    const auto section = [&](const QString& text) { auto* label = new QLabel(text, options); label->setFont(titleFont); form->addRow(label); };
    const auto spin = [&](const QString& name, int from, int to, int value, const QString& suffix) {
        auto* w = new QSpinBox(options); w->setRange(from, to); w->setValue(value); w->setSuffix(suffix); w->setAccessibleName(name); return w;
    };
    const auto precise = [&](const QString& name) {
        auto* w = new QDoubleSpinBox(options); w->setRange(0, 1e9); w->setDecimals(4); w->setSingleStep(.01); w->setAccessibleName(name); w->setKeyboardTracking(false); return w;
    };
    section(tr("Timing"));
    m_tempoLabel = new QLabel(tr("Detecting tempo…"), options); m_tempoLabel->setWordWrap(true); form->addRow(m_tempoLabel);
    m_groove = new QComboBox(options); m_groove->setObjectName("WarpGroove"); m_groove->setAccessibleName(tr("Groove")); form->addRow(tr("Groove"), m_groove); loadGrooves();
    m_swing = spin(tr("Swing"), 50, 75, 50, "%"); m_swing->setObjectName("WarpSwing"); form->addRow(tr("Swing"), m_swing);
    m_swingUnit = new QComboBox(options); m_swingUnit->addItem("1/8", .5); m_swingUnit->addItem("1/16", .25); m_swingUnit->setAccessibleName(tr("Swing division")); form->addRow(tr("Division"), m_swingUnit);
    m_tolerance = spin(tr("Keep timing within"), 0, 100, 10, tr(" ms")); m_tolerance->setObjectName("WarpTolerance"); form->addRow(tr("Tolerance"), m_tolerance);
    m_uncertain = new QCheckBox(tr("Include uncertain attacks"), options); m_uncertain->setObjectName("WarpIncludeUncertain"); form->addRow(m_uncertain);
    auto* groovePreview = new QPushButton(tr("Preview groove"), options); groovePreview->setObjectName("WarpPreviewGroove"); form->addRow(groovePreview);
    connect(groovePreview, &QPushButton::clicked, this, &WarpEditorWidget::previewAlignment);
    section(tr("Range"));
    m_rangeStart = precise(tr("Range start, beat")); m_rangeEnd = precise(tr("Range end, beat")); m_rangeStart->setObjectName("WarpRangeStart"); m_rangeEnd->setObjectName("WarpRangeEnd");
    form->addRow(tr("From beat"), m_rangeStart); form->addRow(tr("To beat"), m_rangeEnd);
    auto* whole = new QPushButton(tr("Whole clip"), options); form->addRow(whole); connect(whole, &QPushButton::clicked, this, [this] { setRange(0, 0); });
    section(tr("Selected marker"));
    m_markerSource = precise(tr("Source position, milliseconds")); m_markerSource->setReadOnly(true); m_markerSource->setButtonSymbols(QAbstractSpinBox::NoButtons); m_markerSource->setSuffix(tr(" ms"));
    m_markerTarget = precise(tr("Target position, beat")); m_markerTarget->setObjectName("WarpMarkerTarget");
    m_markerLock = new QCheckBox(tr("Lock position"), options); m_markerLock->setObjectName("WarpMarkerLock");
    form->addRow(tr("Source"), m_markerSource); form->addRow(tr("Beat"), m_markerTarget); form->addRow(m_markerLock);
    section(tr("Sound"));
    m_pitch = new QCheckBox(tr("Preserve Pitch"), options); form->addRow(m_pitch);
    m_mode = new QComboBox(options); m_mode->addItems({tr("Drums"), tr("Loop"), tr("Vocal"), tr("Complex")}); m_mode->setAccessibleName(tr("Stretch algorithm")); form->addRow(tr("Algorithm"), m_mode);
    m_sensitivity = spin(tr("Transient sensitivity"), 0, 100, 50, "%"); form->addRow(tr("Sensitivity"), m_sensitivity);
    auto* reset = new QPushButton(tr("Reset map"), options); form->addRow(reset); connect(reset, &QPushButton::clicked, this, &WarpEditorWidget::resetMap);
    section(tr("Extract groove"));
    m_extractSource = new QComboBox(options); m_extractSource->setObjectName("WarpGrooveSource"); m_extractSource->setAccessibleName(tr("Groove source")); form->addRow(tr("Source"), m_extractSource);
    m_extractBars = new QComboBox(options); for (int bars : {1, 2, 4}) m_extractBars->addItem(tr("%1 bars").arg(bars), bars); m_extractBars->setAccessibleName(tr("Groove length")); form->addRow(tr("Length"), m_extractBars);
    auto* extract = new QPushButton(tr("Save groove…"), options); extract->setObjectName("WarpSaveGroove"); form->addRow(extract); connect(extract, &QPushButton::clicked, this, &WarpEditorWidget::extractGroove);
    for (auto* combo : options->findChildren<QComboBox*>()) { combo->setMinimumContentsLength(4); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); }
    m_inspector->setWidget(options); m_bodyLayout->addWidget(m_inspector); layout->addWidget(m_body, 1);
    m_status = new QLabel(tr("Double-click: marker · Drag ruler: range · Ctrl+wheel: zoom"), this); m_status->setMinimumWidth(0); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(m_status);

    connect(m_options, &QToolButton::toggled, this, [this](bool open) { m_inspectorOpen = open; arrangeInspector(); });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool value) { if (const auto* c = clip()) { auto w = c->warp; w.enabled = value; this->apply(w, "Toggle Warp"); } });
    connect(m_pitch, &QCheckBox::toggled, this, [this](bool value) { if (const auto* c = clip()) { auto w = c->warp; w.preservePitch = value; this->apply(w, "Warp Preserve Pitch"); } });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int value) { if (const auto* c = clip()) { auto w = c->warp; w.mode = value + 1; this->apply(w, "Warp Algorithm"); } });
    connect(m_sensitivity, &QSpinBox::editingFinished, this, [this] {
        if (const auto* c = clip()) { auto w = c->warp; w.sensitivity = m_sensitivity->value(); this->apply(w, "Warp Sensitivity"); }
    });
    for (auto* w : {m_strength, m_swing, m_tolerance}) connect(w, &QSpinBox::valueChanged, this, [this] { if (m_previewing) rebuildPreview(); });
    for (auto* w : {m_grid, m_groove, m_swingUnit}) connect(w, &QComboBox::currentIndexChanged, this, [this] { if (m_previewing) rebuildPreview(); });
    connect(m_uncertain, &QCheckBox::toggled, this, [this] { if (m_previewing) rebuildPreview(); });
    connect(m_rangeStart, &QDoubleSpinBox::editingFinished, this, [this] { setRange(m_rangeStart->value() - 1, m_rangeEnd->value() - 1); });
    connect(m_rangeEnd, &QDoubleSpinBox::editingFinished, this, [this] { setRange(m_rangeStart->value() - 1, m_rangeEnd->value() - 1); });
    connect(m_markerTarget, &QDoubleSpinBox::editingFinished, this, [this] {
        const auto* c = clip(); if (!c || m_previewing || m_selected.size() != 1) return;
        for (const auto& marker : c->warp.markers) if (m_selected.contains(marker.id) && !marker.locked) {
            const auto before = c->warp; m_controller->beginWarpEdit(m_trackId.toStdString(), m_clipId.toStdString());
            moveSelected(m_markerTarget->value() - 1 - marker.targetBeats, before, false); m_controller->commitWarpEdit(); emit edited(); break;
        }
        updateInspector();
    });
    connect(m_markerLock, &QCheckBox::toggled, this, [this](bool locked) {
        const auto* c = clip(); if (!c || m_previewing) return; auto map = c->warp;
        for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) if (m_selected.contains(map.markers[i].id)) map.markers[i].locked = locked;
        this->apply(map, locked ? "Lock Warp Markers" : "Unlock Warp Markers");
    });
    m_analysisTimer = new QTimer(this); m_analysisTimer->setInterval(60);
    connect(m_analysisTimer, &QTimer::timeout, this, &WarpEditorWidget::pollAnalysis);
    m_frameTimer = new ui::FrameTimer(this);
    connect(m_frameTimer, &ui::FrameTimer::timeout, this, [this] {
        const auto* c = clip(); if (!c) return;
        const double position = m_controller->positionSeconds();
        if (position == m_lastPosition) return;
        const double old = m_canvas->x(daw::secondsToBeats(m_lastPosition - c->startSeconds, m_controller->tempo()));
        const double now = m_canvas->x(daw::secondsToBeats(position - c->startSeconds, m_controller->tempo())); m_lastPosition = position;
        m_canvas->update(QRegion(QRect(int(old) - 3, 0, 7, m_canvas->height())) | QRegion(QRect(int(now) - 3, 0, 7, m_canvas->height())));
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { updateStyle(); redraw(); });
    connect(&ThemeManager::instance(), &ThemeManager::fontChanged, this, [this] { redraw(); });
    updateStyle(); refresh();
}

WarpEditorWidget::~WarpEditorWidget() {
    if (m_previewing) m_controller->cancelWarpPreview();
    for (auto& job : m_jobs) job.cancelled->store(true);
}
void WarpEditorWidget::finishPendingEdit() { m_canvas->finish(); cancelPreview(); }
const daw::ClipModel* WarpEditorWidget::clip() const { return m_controller->audioClip(m_trackId.toStdString(), m_clipId.toStdString()); }
bool WarpEditorWidget::ownsEditingFocus() const {
    auto* focus = QApplication::focusWidget();
    return isVisible() && focus && (focus == this || isAncestorOf(focus));
}
double WarpEditorWidget::grid() const {
    const double step = m_grid->currentData().toDouble();
    if (step > 0) return step;
    const double seconds = m_snapProvider ? m_snapProvider() : 0;
    return seconds > 0 ? daw::secondsToBeats(seconds, m_controller->tempo()) : .25;
}
bool WarpEditorWidget::setClip(const QString& trackId, const QString& clipId) {
    m_canvas->cancel();
    cancelPreview();
    if (!m_controller->initializeClipWarp(trackId.toStdString(), clipId.toStdString())) return false;
    if (m_clipId != clipId) {
        if (!m_clipId.isEmpty()) m_views[m_clipId] = m_view;
        m_trackId = trackId; m_clipId = clipId; m_selected.clear(); m_rangeBegin = m_rangeFinish = 0;
        const auto* c = clip();
        m_view = m_views.contains(clipId) ? m_views[clipId] : View{0, c->warp.markers.back().targetBeats};
        m_audio.reset(); m_analysisBegin = -1;
    }
    m_trackId = trackId;
    refresh(); m_canvas->setFocus(); return true;
}
void WarpEditorWidget::clearClip() {
    cancelPreview();
    m_canvas->cancel(); m_trackId.clear(); m_clipId.clear(); m_selected.clear(); m_views.clear();
    for (auto& job : m_jobs) job.cancelled->store(true);
    ++m_generation; m_audio.reset(); m_peaks.reset(); m_transients.clear(); m_analysisCache.clear(); m_rangeBegin = m_rangeFinish = 0;
    m_lastMap = {}; m_projectGeneration = m_controller->projectGeneration(); refresh();
}
void WarpEditorWidget::refresh() {
    if (m_projectGeneration != m_controller->projectGeneration()) { clearClip(); return; }
    if (m_previewing && !m_controller->warpPreviewActive()) {
        m_previewing = false; m_previewBar->hide(); m_previewBase = {}; redraw();
        m_status->setText(tr("Preview cancelled because the clip or project changed."));
    }
    if (!clip() && !m_clipId.isEmpty()) {
        for (const auto& track : m_controller->project().tracks)
            for (const auto& item : track.clips) if (item.id == m_clipId.toStdString()) {
                m_trackId = QString::fromStdString(track.id); break;
            }
    }
    const auto* c = clip();
    if (!c || c->warp.empty()) {
        m_tools->setEnabled(false); m_pitch->setEnabled(false); m_mode->setEnabled(false); m_sensitivity->setEnabled(false);
        m_title->setText(tr("Select an audio clip")); m_analysisLabel->clear();
        if (m_audio) {
            for (auto& job : m_jobs) job.cancelled->store(true);
            ++m_generation; m_audio.reset(); m_peaks.reset(); m_transients.clear(); m_analysisBegin = -1;
        }
        m_inspector->setEnabled(false); m_lastMap = {}; redraw(); return;
    }
    const auto reason = m_controller->warpUnavailableReason(m_trackId.toStdString(), m_clipId.toStdString());
    m_tools->setEnabled(reason.empty());
    m_inspector->setEnabled(reason.empty());
    m_pitch->setEnabled(reason.empty()); m_mode->setEnabled(reason.empty()); m_sensitivity->setEnabled(reason.empty());
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    m_title->setText(QString::fromStdString(c->name) + "  /  " + QString::fromStdString(track ? track->name : ""));
    m_title->setToolTip(m_title->text());
    const QSignalBlocker a(m_enabled), b(m_pitch), d(m_mode), f(m_sensitivity);
    m_enabled->setChecked(c->warp.enabled); m_pitch->setChecked(c->warp.preservePitch); m_mode->setCurrentIndex(c->warp.mode - 1);
    if (!m_sensitivity->hasFocus()) m_sensitivity->setValue(int(c->warp.sensitivity));
    if (reason.empty()) analyze(); else m_status->setText(QString::fromStdString(reason));
    m_alignButton->setEnabled(c->warp.enabled && bool(m_audio));
    updateInspector();
    if (m_lastMap != c->warp || m_lastTempo != m_controller->tempo() || m_lastStart != c->startSeconds) {
        m_lastMap = c->warp; m_lastTempo = m_controller->tempo(); m_lastStart = c->startSeconds; redraw();
    }
}
bool WarpEditorWidget::apply(const daw::ClipWarpModel& map, const char* label) {
    cancelPreview();
    if (!m_controller->setClipWarp(m_trackId.toStdString(), m_clipId.toStdString(), map, label)) return false;
    emit edited(); refresh(); return true;
}
void WarpEditorWidget::addMarker(double beats) {
    const auto* c = clip(); if (!c || !c->warp.enabled) return;
    auto map = c->warp;
    if (beats <= 1e-5 || beats >= map.markers.back().targetBeats - 1e-5) return;
    if (std::any_of(map.markers.begin(), map.markers.end(), [&](const auto& p) { return std::abs(p.targetBeats - beats) < 1e-5; })) return;
    daw::WarpMarker marker{daw::newUuid(), daw::warpSourceAt(map, beats), beats, false};
    map.markers.insert(map.markers.begin() + std::ptrdiff_t(daw::warpSegment(map, beats) + 1), marker);
    m_selected = {marker.id}; apply(map, "Add Warp Marker");
}
void WarpEditorWidget::deleteMarkers() {
    if (m_previewing) return;
    const auto* c = clip(); if (!c || !c->warp.enabled) return;
    auto map = c->warp;
    std::erase_if(map.markers, [&](const auto& marker) { return !marker.locked && m_selected.contains(marker.id); });
    if (apply(map, "Delete Warp Markers")) {
        std::erase_if(m_selected, [&](const auto& id) {
            return std::none_of(map.markers.begin(), map.markers.end(), [&](const auto& marker) { return marker.id == id; });
        });
    }
}
void WarpEditorWidget::moveSelected(double delta, const daw::ClipWarpModel& before, bool snap) {
    auto map = before; double low = -1e20, high = 1e20; bool any = false;
    const auto selected = [&](std::size_t i) { return !map.markers[i].locked && m_selected.contains(map.markers[i].id); };
    if (snap) for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) if (selected(i)) {
        const double origin = daw::secondsToBeats(clip()->startSeconds, m_controller->tempo());
        delta = std::round((origin + map.markers[i].targetBeats + delta) / grid()) * grid() - origin - map.markers[i].targetBeats; break;
    }
    for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) if (selected(i)) {
        any = true;
        if (!selected(i - 1)) low = std::max(low, map.markers[i - 1].targetBeats + 1e-5 - map.markers[i].targetBeats);
        if (!selected(i + 1)) high = std::min(high, map.markers[i + 1].targetBeats - 1e-5 - map.markers[i].targetBeats);
    }
    if (!any) return;
    delta = std::clamp(delta, low, high);
    for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) if (selected(i)) map.markers[i].targetBeats += delta;
    if (m_controller->setClipWarp(m_trackId.toStdString(), m_clipId.toStdString(), map)) {
        for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) if (selected(i)) {
            const auto readout = tr("Source %1 ms → Beat %2\nΔ %5 ms · Left %3x · Right %4x")
                .arg(map.markers[i].sourceSeconds * 1000, 0, 'f', 1).arg(map.markers[i].targetBeats + 1, 0, 'f', 3)
                .arg(1 / daw::warpSpeedAt(map, map.markers[i - 1].targetBeats, m_controller->tempo()), 0, 'f', 3)
                .arg(1 / daw::warpSpeedAt(map, map.markers[i].targetBeats, m_controller->tempo()), 0, 'f', 3)
                .arg(daw::beatsToSeconds(delta, m_controller->tempo()) * 1000, 0, 'f', 1);
            m_dragLabel->setText(readout); m_status->setText(QString(readout).replace('\n', "  ·  ")); break;
        }
    }
    updateInspector(); redraw();
}
void WarpEditorWidget::quantize() {
    const auto* c = clip(); if (!c || !c->warp.enabled) return;
    if (m_previewing) { rebuildPreview(); return; }
    apply(daw::warptools::align(c->warp, {}, alignParams(false)).map, "Quantize Audio");
}
void WarpEditorWidget::resetMap() {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    auto map = c->warp; map.markers = {map.markers.front(), map.markers.back()};
    m_selected.clear(); apply(map, "Reset Warp");
}
void WarpEditorWidget::redraw() {
    m_canvas->redraw(); m_overview->update();
}
void WarpEditorWidget::updateStyle() {
    const auto& t = ThemeManager::instance().theme();
    setStyleSheet(QString(
        "#WarpEditor { background: %1; }"
        "#WarpInspector, #WarpInspector > QWidget > QWidget { background: %2; border-radius: 8px; }"
        "#WarpPreviewBar { background: %2; border: 1px solid %3; border-radius: 8px; }"
        "#WarpAnalysisState { color: %4; }"
        "#WarpDragReadout { background: %2; color: %5; border: 1px solid %3; border-radius: 8px; padding: 8px; }"
        "#WarpApply { background: %6; color: %7; }"
        "#WarpEditor QToolButton:checked { background: %6; color: %7; }")
        .arg(t.surface.name(), t.surfaceElevated.name(), t.separator().name(), t.textSecondary.name(),
             t.textPrimary.name(), t.accent.name(), t.accentText().name()));
    m_options->setIcon(icons::icon(icons::Glyph::Inspector, t.textPrimary));
    findChild<QToolButton*>("WarpCloseButton")->setIcon(icons::icon(icons::Glyph::Close, t.textPrimary));
    m_alignButton->setIcon(icons::icon(icons::Glyph::Grid, t.textPrimary));
}
void WarpEditorWidget::arrangeInspector() {
    const bool compact = width() < 900;
    static_cast<QBoxLayout*>(m_tools->layout())->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    if (compact) {
        m_bodyLayout->removeWidget(m_inspector);
        m_inspector->setGeometry(std::max(0, m_body->width() - 248), 0, 248, m_body->height());
    } else if (m_bodyLayout->indexOf(m_inspector) < 0) m_bodyLayout->addWidget(m_inspector);
    m_inspector->setVisible(m_inspectorOpen); if (compact && m_inspectorOpen) m_inspector->raise();
    m_alignButton->setText(compact ? tr("Suggest") : tr("Suggest alignment"));
    m_analysisLabel->setVisible(width() >= 600);
    m_overview->setVisible(height() >= 240); m_status->setVisible(height() >= 220);
}
void WarpEditorWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event); arrangeInspector();
}
void WarpEditorWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event); refresh(); m_frameTimer->start(); arrangeInspector();
}
void WarpEditorWidget::hideEvent(QHideEvent* event) {
    m_frameTimer->stop(); cancelPreview(); QWidget::hideEvent(event);
}
void WarpEditorWidget::setRange(double begin, double end) {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    const double length = c->warp.markers.back().targetBeats;
    m_rangeBegin = std::clamp(begin, 0., length); m_rangeFinish = std::clamp(end, m_rangeBegin, length);
    if (m_rangeFinish - m_rangeBegin < 1e-4) m_rangeBegin = m_rangeFinish = 0;
    updateInspector(); if (m_previewing) rebuildPreview(); redraw();
}
void WarpEditorWidget::updateInspector() {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    const QSignalBlocker a(m_rangeStart), b(m_rangeEnd), d(m_markerTarget), f(m_markerLock);
    const double length = c->warp.markers.back().targetBeats;
    m_rangeStart->setRange(1, length + 1); m_rangeEnd->setRange(1, length + 1);
    if (!m_rangeStart->hasFocus()) m_rangeStart->setValue(m_rangeFinish > m_rangeBegin ? m_rangeBegin + 1 : 1);
    if (!m_rangeEnd->hasFocus()) m_rangeEnd->setValue(m_rangeFinish > m_rangeBegin ? m_rangeFinish + 1 : length + 1);
    const auto marker = std::find_if(c->warp.markers.begin(), c->warp.markers.end(), [this](const auto& m) { return m_selected.contains(m.id); });
    const bool single = m_selected.size() == 1 && marker != c->warp.markers.end();
    m_markerSource->setEnabled(single); m_markerTarget->setEnabled(single && !marker->locked && !m_previewing && c->warp.enabled);
    const bool interior = single && marker != c->warp.markers.begin() && marker + 1 != c->warp.markers.end();
    m_markerLock->setEnabled(interior && !m_previewing && c->warp.enabled);
    if (single) {
        m_markerSource->setValue(marker->sourceSeconds * 1000);
        if (!m_markerTarget->hasFocus()) m_markerTarget->setValue(marker->targetBeats + 1);
        m_markerLock->setChecked(marker->locked);
    }
    // Keep the source picker in step with the project without losing its selection.
    QStringList ids, names; ids << m_clipId; names << tr("This audio clip");
    for (const auto& track : m_controller->project().tracks) for (const auto& item : track.clips)
        if (item.kind == daw::ClipKind::Midi) { ids << QString::fromStdString(item.id); names << QString::fromStdString(track.name + " / " + item.name); }
    bool changed = m_extractSource->count() != ids.size();
    for (int i = 0; !changed && i < ids.size(); ++i) changed = m_extractSource->itemData(i).toString() != ids[i] || m_extractSource->itemText(i) != names[i];
    if (changed) {
        const auto selected = m_extractSource->currentData(); m_extractSource->clear();
        for (int i = 0; i < ids.size(); ++i) m_extractSource->addItem(names[i], ids[i]);
        m_extractSource->setCurrentIndex(std::max(0, m_extractSource->findData(selected)));
    }
}

daw::warptools::AlignParams WarpEditorWidget::alignParams(bool attacks) const {
    daw::warptools::AlignParams p;
    if (const auto* c = clip()) {
        p.originBeats = daw::secondsToBeats(c->startSeconds, m_controller->tempo());
        p.tempo = m_controller->tempo();
        p.minimumStrength = 1 - c->warp.sensitivity / 100.;
    }
    p.timing.gridBeats = grid(); p.timing.strength = m_strength->value() / 100.;
    p.timing.swing = m_swing->value() / 100.; p.timing.swingUnitBeats = m_swingUnit->currentData().toDouble();
    p.timing.toleranceBeats = daw::secondsToBeats(m_tolerance->value() / 1000., m_controller->tempo());
    const auto groove = m_groove->currentIndex();
    if (groove >= 0 && groove < int(m_grooves.size())) p.timing.groove = m_grooves[groove];
    p.timing.grooveVelocity = 0;
    if (m_rangeFinish > m_rangeBegin) { p.beginBeats = m_rangeBegin; p.endBeats = m_rangeFinish; }
    p.addTransients = attacks; p.includeUncertain = m_uncertain->isChecked();
    // Explicit selection is for the quick marker command; the assistant works
    // on the chosen ruler range and retains every existing manual marker.
    if (!attacks) p.selected = m_selected;
    return p;
}
void WarpEditorWidget::previewAlignment() {
    m_canvas->finish();
    m_previewAttacks = m_transients;
    if (!m_previewing) {
        if (!m_controller->beginWarpPreview(m_trackId.toStdString(), m_clipId.toStdString())) return;
        m_previewBase = clip()->warp; m_previewing = true;
        m_after->setChecked(true); m_previewBar->show();
    }
    rebuildPreview(); m_canvas->setFocus();
}
void WarpEditorWidget::rebuildPreview() {
    if (!m_previewing) return;
    if (!m_controller->warpPreviewActive()) { cancelPreview(); return; }
    m_proposal = daw::warptools::align(m_previewBase, m_previewAttacks, alignParams(true));
    if (!m_controller->updateWarpPreview(m_proposal.map)) {
        cancelPreview(); m_status->setText(tr("This proposal exceeds the supported stretch range.")); return;
    }
    m_previewSummary->setText(tr("%1 moves · %2 ms max · %3 to review")
        .arg(m_proposal.moved).arg(daw::beatsToSeconds(m_proposal.maximumShiftBeats, m_controller->tempo()) * 1000, 0, 'f', 1)
        .arg(m_proposal.uncertain + m_proposal.constrained));
    m_previewSummary->setToolTip(tr("%1 uncertain attacks · %2 moves limited by neighbouring markers")
        .arg(m_proposal.uncertain).arg(m_proposal.constrained));
    m_status->setText(tr("Preview only · Compare Before / After, then Apply or Cancel"));
    updateInspector(); redraw();
}
void WarpEditorWidget::applyPreview() {
    if (!m_previewing) return;
    const bool applied = m_controller->commitWarpPreview();
    m_previewing = false; m_previewBase = {}; m_previewBar->hide();
    m_status->setText(applied ? tr("Timing applied · Undo restores the previous map") : tr("No timing changes"));
    if (applied) emit edited(); refresh(); redraw();
}
void WarpEditorWidget::cancelPreview() {
    if (!m_previewing) return;
    m_controller->cancelWarpPreview(); m_previewing = false; m_previewBase = {}; m_previewBar->hide();
    m_status->setText(tr("Preview cancelled · Original timing restored")); updateInspector(); redraw();
}
void WarpEditorWidget::loadGrooves() {
    m_grooves = daw::miditools::groovePresets();
    const auto saved = QJsonDocument::fromJson(QSettings().value("warp/userGrooves").toByteArray()).array();
    for (const auto& value : saved) {
        if (m_grooves.size() >= 71) break;
        const auto object = value.toObject(); const auto array = object["offsets"].toArray();
        daw::miditools::Groove groove; groove.name = object["name"].toString().left(80).toStdString(); groove.lengthBeats = object["length"].toDouble();
        if (groove.name.empty() || groove.lengthBeats <= 0 || groove.lengthBeats > 512 || array.isEmpty() || array.size() > 4096) continue;
        bool valid = true;
        for (const auto& offset : array) { const double d = offset.toDouble(1e9); valid &= std::isfinite(d) && std::abs(d) <= groove.lengthBeats / array.size() * .5; groove.offsets.push_back(d); }
        if (valid) m_grooves.push_back(std::move(groove));
    }
    const QSignalBlocker blocker(m_groove); m_groove->clear();
    for (const auto& groove : m_grooves) m_groove->addItem(QString::fromStdString(groove.name));
}
void WarpEditorWidget::extractGroove() {
    const double length = m_extractBars->currentData().toInt() * m_controller->project().timeSigNumerator *
        4. / std::max(1, m_controller->project().timeSigDenominator);
    const auto* source = clip();
    const auto sourceId = m_extractSource->currentData().toString().toStdString();
    for (const auto& track : m_controller->project().tracks) for (const auto& c : track.clips) if (c.id == sourceId) source = &c;
    if (!source) return;
    const double start = m_rangeBegin;
    const double available = daw::secondsToBeats(source->durationSeconds, m_controller->tempo());
    if (start + length > available + 1e-5) { m_status->setText(tr("Choose a source with at least %1 full bars from the range start.").arg(m_extractBars->currentData().toInt())); return; }
    std::vector<double> onsets;
    if (source->kind == daw::ClipKind::Midi) {
        for (const auto& note : daw::midiNotes(*source)) if (note.startBeats >= start && note.startBeats < start + length) onsets.push_back(note.startBeats - start);
    } else {
        for (const auto& attack : m_transients) if (attack.strength >= 1 - source->warp.sensitivity / 100. && (attack.confidence >= .65 || m_uncertain->isChecked())) {
            const auto& map = source->warp;
            const double beat = (map.enabled ? daw::warpBeatAt(map, attack.sourceSeconds) :
                daw::secondsToBeats((attack.sourceSeconds - map.markers.front().sourceSeconds) /
                    (map.markers.back().sourceSeconds - map.markers.front().sourceSeconds) * map.baselineDurationSeconds,
                    m_controller->tempo())) - start;
            if (beat >= 0 && beat < length) onsets.push_back(beat);
        }
    }
    if (onsets.empty()) { m_status->setText(tr("No reliable attacks in this range. Adjust sensitivity or choose a MIDI clip.")); return; }
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("Save groove"), tr("Groove name"), QLineEdit::Normal, QString::fromStdString(source->name), &ok).trimmed().left(80);
    if (!ok || name.isEmpty()) return;
    auto groove = daw::warptools::extractGroove(onsets, length, grid(), name.toStdString());
    if (groove.empty()) return;
    const auto firstUser = daw::miditools::groovePresets().size();
    auto found = std::find_if(m_grooves.begin() + firstUser, m_grooves.end(), [&](const auto& g) { return g.name == groove.name; });
    if (found != m_grooves.end()) *found = groove;
    else { if (m_grooves.size() >= firstUser + 64) { m_status->setText(tr("The user groove library is full (64 presets).")); return; } m_grooves.push_back(groove); }
    QJsonArray saved;
    for (std::size_t i = firstUser; i < m_grooves.size(); ++i) {
        QJsonArray offsets; for (double d : m_grooves[i].offsets) offsets.push_back(d);
        saved.push_back(QJsonObject{{"name", QString::fromStdString(m_grooves[i].name)}, {"length", m_grooves[i].lengthBeats}, {"offsets", offsets}});
    }
    QSettings settings; settings.setValue("warp/userGrooves", QJsonDocument(saved).toJson(QJsonDocument::Compact)); settings.sync();
    if (settings.status() != QSettings::NoError) { m_status->setText(tr("Could not save the groove preset.")); return; }
    loadGrooves(); m_groove->setCurrentText(name); m_status->setText(tr("Groove saved · Use Preview groove to audition it"));
}
void WarpEditorWidget::showTempo(const daw::analysis::TempoEstimate& tempo) {
    m_tempoLabel->setText(tempo.bpm > 0 ? tr("Source %1 BPM · confidence %2%")
        .arg(tempo.bpm, 0, 'f', 1).arg(tempo.confidence * 100, 0, 'f', 0) : tr("Tempo unavailable"));
    QString detail = QString::fromStdString(tempo.reason);
    if (tempo.status == daw::analysis::DetectionStatus::Ambiguous) m_tempoLabel->setText(m_tempoLabel->text() + tr(" · ambiguous"));
    if (!tempo.alternatives.empty()) {
        QStringList choices; for (double bpm : tempo.alternatives) choices << QString::number(bpm, 'f', 1);
        detail += "\n" + tr("Alternatives: %1 BPM").arg(choices.join(" / "));
    }
    m_tempoLabel->setToolTip(detail);
}
void WarpEditorWidget::analyze() {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    auto data = m_controller->clipSampleData(m_trackId.toStdString(), m_clipId.toStdString());
    if (!data || !data->audio) {
        if (m_audio) { for (auto& job : m_jobs) job.cancelled->store(true); ++m_generation; m_audio.reset(); m_peaks.reset(); m_transients.clear(); redraw(); }
        m_status->setText(tr("Source audio is unavailable.")); m_analysisLabel->setText(tr("Missing audio")); return;
    }
    const double begin = c->warp.markers.front().sourceSeconds, end = c->warp.markers.back().sourceSeconds;
    if (data->audio == m_audio && begin == m_analysisBegin && end == m_analysisEnd) return;
    for (auto& job : m_jobs) job.cancelled->store(true);
    ++m_generation; m_audio = data->audio; m_analysisBegin = begin; m_analysisEnd = end;
    m_peaks.reset(); m_transients.clear();
    for (const auto& cached : m_analysisCache) if (cached.audio == m_audio && cached.begin == begin && cached.end == end && cached.version == daw::analysis::kWarpAnalysisVersion) {
        m_peaks = cached.result.peaks; m_transients = cached.result.transients;
        showTempo(cached.result.tempo);
        m_analysisLabel->setText(tr("%1 attacks").arg(m_transients.size())); redraw(); return;
    }
    m_analysisLabel->setText(tr("Analyzing…")); m_tempoLabel->setText(tr("Detecting tempo…"));
    m_status->setText(tr("Analyzing transients… You can already add markers."));
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_jobs.push_back({cancelled, std::async(std::launch::async, [audio = m_audio, begin, end, cancelled] {
        AnalysisResult result; result.peaks = std::make_shared<daw::WaveformPeaks>();
        const auto keep = [&] { return !cancelled->load(); };
        daw::buildPeaks(*audio, *result.peaks, keep);
        if (keep()) result.transients = daw::analysis::detectWarpTransients(*audio, begin, end, keep);
        // The existing tempo detector consumes interleaved samples. Bound its
        // window to 90 seconds; all conversion and analysis stay on this worker.
        const auto first = std::size_t(std::clamp(begin * audio->sampleRate(), 0., double(audio->frames())));
        const auto last = std::size_t(std::clamp(std::min(end, begin + 90.) * audio->sampleRate(), double(first), double(audio->frames())));
        const int channels = int(audio->channels());
        if (keep() && last > first && channels > 0) {
            std::vector<float> interleaved((last - first) * channels);
            for (std::size_t i = first; i < last; ++i) {
                if ((i & 4095) == 0 && !keep()) return result;
                for (int ch = 0; ch < channels; ++ch) interleaved[(i - first) * channels + ch] = audio->channel(ch)[i];
            }
            daw::analysis::MusicalAnalysisRequest request; request.detectKey = false; request.useNeuralModels = false;
            daw::analysis::MusicalAnalysisResult musical;
            daw::analysis::analyzeAudioSamples(interleaved.data(), last - first, channels, audio->sampleRate(), request, musical,
                [&](double, std::string_view) { return keep(); });
            result.tempo = std::move(musical.tempo);
        }
        return result;
    }), m_generation});
    m_analysisTimer->start(); redraw();
}
void WarpEditorWidget::pollAnalysis() {
    // Invalidate finished workers before publishing when the document changed
    // between the last UI refresh and this completion notification.
    refresh();
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        try {
            auto result = it->future.get();
            if (it->generation == m_generation && !it->cancelled->load()) {
                m_peaks = result.peaks; m_transients = result.transients;
                showTempo(result.tempo);
                m_analysisLabel->setText(tr("%1 attacks").arg(m_transients.size()));
                // Bounded cache of immutable source/range/version results. A
                // sensitivity edit filters these candidates without reanalysis.
                if (m_audio->frames() * double(m_audio->channels()) * sizeof(float) < 128 * 1024 * 1024) {
                    if (m_analysisCache.size() >= 3) m_analysisCache.erase(m_analysisCache.begin());
                    m_analysisCache.push_back({m_audio, m_analysisBegin, m_analysisEnd, daw::analysis::kWarpAnalysisVersion, std::move(result)});
                }
                if (!m_previewing) m_status->setText(tr("%1 transient suggestions · Double-click to add · Ctrl+wheel to zoom").arg(m_transients.size()));
                // An already auditioned proposal stays frozen until the user
                // asks again; a late analysis result never changes the sound.
                redraw();
            }
        } catch (...) { if (it->generation == m_generation) { m_analysisLabel->setText(tr("Analysis unavailable")); m_status->setText(tr("Analysis failed. Manual Warp editing is available.")); } }
        it = m_jobs.erase(it);
    }
    if (m_jobs.empty()) m_analysisTimer->stop();
}
