#include "WarpEditorWidget.hpp"
#include "EngineController.hpp"
#include "Internal/SamplerVoice.hpp"
#include "Theme.hpp"
#include "UiFrameClock.hpp"
#include "WaveformPaint.hpp"
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
    explicit WarpCanvas(WarpEditorWidget* editor) : ui::FrameWidget(editor), e(editor) {
        setObjectName("WarpCanvas"); setAccessibleName(tr("Audio warp markers"));
        setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setMinimumHeight(90);
    }
    double x(double beats) const { return 12 + (beats - e->m_view.start) / e->m_view.span * std::max(1, width() - 24); }
    double beat(double x) const { return e->m_view.start + (x - 12) / std::max(1, width() - 24) * e->m_view.span; }
    int hit(double position) const {
        const auto* c = e->clip(); if (!c) return -1;
        int found = -1; double distance = 8;
        for (int i = 0; i < int(c->warp.markers.size()); ++i)
            if (const double d = std::abs(x(c->warp.markers[i].targetBeats) - position); d < distance) { distance = d; found = i; }
        return found;
    }
    void cancel() { if (dragging) { e->m_controller->cancelWarpEdit(); dragging = false; releaseMouse(); emit e->liveEdited(); } update(); }
    void finish() { if (dragging) { dragging = false; releaseMouse(); e->m_controller->commitWarpEdit(); emit e->edited(); } }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); const auto& theme = ThemeManager::instance().theme();
        p.fillRect(rect(), theme.well());
        const auto* c = e->clip();
        if (!c || c->warp.empty()) { p.setPen(theme.textSecondary); p.drawText(rect(), Qt::AlignCenter, tr("Right-click an audio clip and choose Warp Audio")); return; }
        const auto& map = c->warp;
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
            p.setPen(QPen(theme.textSecondary, 1, Qt::DotLine)); p.drawLine(QPointF(px, 51), QPointF(px, height() - 14));
            p.drawLine(QPointF(px - 3, 49), QPointF(px + 3, 49));
        }
        for (const auto& marker : map.markers) {
            const double px = x(marker.targetBeats);
            const bool selected = e->m_selected.contains(marker.id);
            p.setPen(QPen(selected ? theme.accentHighlight : theme.accent, selected ? 2 : 1));
            p.drawLine(QPointF(px, 28), QPointF(px, height() - 12));
            p.setBrush(selected ? theme.accentHighlight : theme.accent);
            if (marker.locked) p.drawRect(QRectF(px - 4, 30, 8, 8));
            else p.drawPolygon(QPolygonF{QPointF(px - 5, 29), QPointF(px + 5, 29), QPointF(px, 39)});
        }
        const double play = daw::secondsToBeats(e->m_controller->positionSeconds() - c->startSeconds, tempo);
        if (play >= 0 && play <= map.markers.back().targetBeats) {
            p.setPen(QPen(theme.cursor, 1.5)); p.drawLine(QPointF(x(play), 0), QPointF(x(play), height()));
        }
        if (hasFocus()) { p.setPen(theme.accent); p.setBrush(Qt::NoBrush); p.drawRect(rect().adjusted(0, 0, -1, -1)); }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        setFocus(); const auto* c = e->clip(); if (!c || !c->warp.enabled) return;
        const int index = hit(event->position().x());
        if (index >= 0) {
            const auto marker = c->warp.markers[index];
            if (event->modifiers() & Qt::ControlModifier) {
                if (!e->m_selected.erase(marker.id)) e->m_selected.insert(marker.id);
            } else if (!e->m_selected.contains(marker.id)) { e->m_selected = {marker.id}; }
            if (marker.locked || !e->m_selected.contains(marker.id)) { update(); return; }
            before = c->warp; startBeat = beat(event->position().x());
            e->m_controller->beginWarpEdit(e->m_trackId.toStdString(), e->m_clipId.toStdString());
            dragging = true; grabMouse(); update(); return;
        }
        for (const auto& t : e->m_transients) if (t.strength >= 1 - c->warp.sensitivity / 100.) {
            const double b = daw::warpBeatAt(c->warp, t.sourceSeconds);
            if (std::abs(x(b) - event->position().x()) < 6) { e->addMarker(b); return; }
        }
        if (!(event->modifiers() & Qt::ControlModifier)) e->m_selected.clear();
        update();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging) {
            e->moveSelected(beat(event->position().x()) - startBeat, before,
                            e->m_snap->isChecked() && !(event->modifiers() & Qt::AltModifier));
            emit e->liveEdited();
        } else setCursor(hit(event->position().x()) >= 0 ? Qt::SizeHorCursor : Qt::CrossCursor);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (dragging && event->button() == Qt::LeftButton) {
            dragging = false; releaseMouse(); e->m_controller->commitWarpEdit(); emit e->edited(); update();
        }
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        cancel(); if (event->button() == Qt::LeftButton && hit(event->position().x()) < 0) e->addMarker(beat(event->position().x()));
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) { cancel(); return; }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) { e->deleteMarkers(); return; }
        if (event->matches(QKeySequence::SelectAll)) {
            if (const auto* c = e->clip()) for (const auto& marker : c->warp.markers) e->m_selected.insert(marker.id);
            update(); return;
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
        const auto* c = e->clip(); const int index = hit(event->pos().x());
        if (!c || !c->warp.enabled || index <= 0 || index + 1 >= int(c->warp.markers.size())) return;
        const auto marker = c->warp.markers[index];
        if (!e->m_selected.contains(marker.id)) e->m_selected = {marker.id};
        update();
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
            const double anchor = beat(event->position().x());
            const double fraction = (event->position().x() - 12) / std::max(1, width() - 24);
            e->m_view.span = std::clamp(e->m_view.span * std::pow(.8, amount), .02, 100000.);
            e->m_view.start = anchor - fraction * e->m_view.span;
        } else {
            const double dx = event->pixelDelta().x() ? event->pixelDelta().x() / 60. : amount;
            e->m_view.start -= dx * e->m_view.span * .1;
        }
        e->m_view.start = std::max(0., e->m_view.start); update(); event->accept();
    }
    void hideEvent(QHideEvent* event) override { cancel(); ui::FrameWidget::hideEvent(event); }
private:
    WarpEditorWidget* e;
    bool dragging = false;
    double startBeat = 0;
    daw::ClipWarpModel before;
};

WarpEditorWidget::WarpEditorWidget(daw::EngineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setObjectName("WarpEditor");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(8, 5, 8, 5); layout->setSpacing(5);
    m_title = new QLabel(tr("Warp Audio"), this); layout->addWidget(m_title);
    m_tools = new QWidget(this); auto* tools = new QHBoxLayout(m_tools); tools->setContentsMargins(0, 0, 0, 0);
    auto check = [&](const QString& text) { auto* w = new QCheckBox(text, m_tools); w->setAccessibleName(text); tools->addWidget(w); return w; };
    m_enabled = check(tr("Warp")); m_snap = check(tr("Snap")); m_snap->setChecked(true);
    m_grid = new QComboBox(m_tools); m_grid->setAccessibleName(tr("Warp grid"));
    m_grid->addItem(tr("Project grid"), 0.);
    for (const auto& item : {std::pair{"1/4", 1.}, {"1/8", .5}, {"1/16", .25}, {"1/32", .125}, {"1/8 triplet", 1./3}})
        m_grid->addItem(item.first, item.second);
    m_grid->setCurrentIndex(0); tools->addWidget(m_grid);
    auto button = [&](const QString& text, auto callback) { auto* b = new QPushButton(text, m_tools); tools->addWidget(b); connect(b, &QPushButton::clicked, this, callback); return b; };
    button(tr("Quantize"), [this] { quantize(); });
    m_strength = new QSpinBox(m_tools); m_strength->setRange(0, 100); m_strength->setValue(100); m_strength->setSuffix("%");
    m_strength->setAccessibleName(tr("Quantize strength")); m_strength->setToolTip(tr("Quantize strength")); tools->addWidget(m_strength);
    button(tr("Reset"), [this] { resetMap(); });
    button(tr("Fit"), [this] { if (const auto* c = clip(); c && !c->warp.empty()) m_view = {0, c->warp.markers.back().targetBeats}; m_canvas->update(); });
    tools->addStretch();
    auto* advanced = new QPushButton(tr("Options"), m_tools); advanced->setCheckable(true); tools->addWidget(advanced);
    layout->addWidget(m_tools);
    auto* options = new QWidget(this); auto* row = new QHBoxLayout(options); row->setContentsMargins(0, 0, 0, 0);
    m_pitch = new QCheckBox(tr("Preserve Pitch"), options); row->addWidget(m_pitch);
    m_mode = new QComboBox(options); m_mode->addItems({tr("Drums"), tr("Loop"), tr("Vocal"), tr("Complex")});
    m_mode->setAccessibleName(tr("Stretch algorithm")); row->addWidget(m_mode);
    row->addWidget(new QLabel(tr("Transient sensitivity"), options));
    m_sensitivity = new QSpinBox(options); m_sensitivity->setRange(0, 100); m_sensitivity->setSuffix("%");
    m_sensitivity->setAccessibleName(tr("Transient sensitivity")); row->addWidget(m_sensitivity); row->addStretch();
    options->hide(); layout->addWidget(options); connect(advanced, &QPushButton::toggled, options, &QWidget::setVisible);
    m_canvas = new WarpCanvas(this); layout->addWidget(m_canvas, 1);
    m_status = new QLabel(tr("Double-click to add a marker · Ctrl+wheel to zoom · Alt to bypass Snap"), this);
    layout->addWidget(m_status);
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool value) { if (const auto* c = clip()) { auto w = c->warp; w.enabled = value; apply(w, "Toggle Warp"); } });
    connect(m_pitch, &QCheckBox::toggled, this, [this](bool value) { if (const auto* c = clip()) { auto w = c->warp; w.preservePitch = value; apply(w, "Warp Preserve Pitch"); } });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int value) { if (const auto* c = clip()) { auto w = c->warp; w.mode = value + 1; apply(w, "Warp Algorithm"); } });
    connect(m_sensitivity, &QSpinBox::editingFinished, this, [this] { if (const auto* c = clip()) { auto w = c->warp; w.sensitivity = m_sensitivity->value(); apply(w, "Warp Sensitivity"); } });
    auto* timer = new QTimer(this); timer->setInterval(40);
    connect(timer, &QTimer::timeout, this, [this] { pollAnalysis(); if (isVisible()) refresh(); }); timer->start();
    refresh();
}

WarpEditorWidget::~WarpEditorWidget() { for (auto& job : m_jobs) job.cancelled->store(true); }
void WarpEditorWidget::finishPendingEdit() { m_canvas->finish(); }
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
    if (!m_controller->initializeClipWarp(trackId.toStdString(), clipId.toStdString())) return false;
    if (m_clipId != clipId) {
        if (!m_clipId.isEmpty()) m_views[m_clipId] = m_view;
        m_trackId = trackId; m_clipId = clipId; m_selected.clear();
        const auto* c = clip();
        m_view = m_views.contains(clipId) ? m_views[clipId] : View{0, c->warp.markers.back().targetBeats};
        m_audio.reset(); m_analysisBegin = -1;
    }
    m_trackId = trackId;
    refresh(); m_canvas->setFocus(); return true;
}
void WarpEditorWidget::clearClip() {
    m_canvas->cancel(); m_trackId.clear(); m_clipId.clear(); m_selected.clear(); m_views.clear();
    for (auto& job : m_jobs) job.cancelled->store(true);
    ++m_generation; m_audio.reset(); m_peaks.reset(); m_transients.clear(); refresh();
}
void WarpEditorWidget::refresh() {
    if (!clip() && !m_clipId.isEmpty()) {
        for (const auto& track : m_controller->project().tracks)
            for (const auto& item : track.clips) if (item.id == m_clipId.toStdString()) {
                m_trackId = QString::fromStdString(track.id); break;
            }
    }
    const auto* c = clip();
    if (!c || c->warp.empty()) {
        m_tools->setEnabled(false); m_pitch->setEnabled(false); m_mode->setEnabled(false); m_sensitivity->setEnabled(false);
        m_title->setText(tr("Warp Audio"));
        if (m_audio) {
            for (auto& job : m_jobs) job.cancelled->store(true);
            ++m_generation; m_audio.reset(); m_peaks.reset(); m_transients.clear(); m_analysisBegin = -1;
        }
        m_canvas->update(); return;
    }
    const auto reason = m_controller->warpUnavailableReason(m_trackId.toStdString(), m_clipId.toStdString());
    m_tools->setEnabled(reason.empty());
    m_pitch->setEnabled(reason.empty()); m_mode->setEnabled(reason.empty()); m_sensitivity->setEnabled(reason.empty());
    const auto* track = m_controller->project().findTrack(m_trackId.toStdString());
    m_title->setText(QString::fromStdString(c->name) + "  /  " + QString::fromStdString(track ? track->name : ""));
    const QSignalBlocker a(m_enabled), b(m_pitch), d(m_mode), f(m_sensitivity);
    m_enabled->setChecked(c->warp.enabled); m_pitch->setChecked(c->warp.preservePitch); m_mode->setCurrentIndex(c->warp.mode - 1);
    if (!m_sensitivity->hasFocus()) m_sensitivity->setValue(int(c->warp.sensitivity));
    if (reason.empty()) analyze(); else m_status->setText(QString::fromStdString(reason));
    m_canvas->update();
}
bool WarpEditorWidget::apply(const daw::ClipWarpModel& map, const char* label) {
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
            m_status->setText(tr("Source %1 ms  →  Beat %2   |   Left %3x   Right %4x")
                .arg(map.markers[i].sourceSeconds * 1000, 0, 'f', 1).arg(map.markers[i].targetBeats + 1, 0, 'f', 3)
                .arg(1 / daw::warpSpeedAt(map, map.markers[i - 1].targetBeats, m_controller->tempo()), 0, 'f', 3)
                .arg(1 / daw::warpSpeedAt(map, map.markers[i].targetBeats, m_controller->tempo()), 0, 'f', 3)); break;
        }
    }
    m_canvas->update();
}
void WarpEditorWidget::quantize() {
    const auto* c = clip(); if (!c || !c->warp.enabled) return;
    auto map = c->warp; const double origin = daw::secondsToBeats(c->startSeconds, m_controller->tempo());
    for (std::size_t i = 1; i + 1 < map.markers.size(); ++i) {
        auto& marker = map.markers[i];
        if (marker.locked || (!m_selected.empty() && !m_selected.contains(marker.id))) continue;
        const double target = std::round((origin + marker.targetBeats) / grid()) * grid() - origin;
        marker.targetBeats = std::clamp(marker.targetBeats + (target - marker.targetBeats) * m_strength->value() / 100.,
            map.markers[i - 1].targetBeats + 1e-5, map.markers[i + 1].targetBeats - 1e-5);
    }
    apply(map, "Quantize Audio");
}
void WarpEditorWidget::resetMap() {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    auto map = c->warp; map.markers = {map.markers.front(), map.markers.back()};
    m_selected.clear(); apply(map, "Reset Warp");
}
void WarpEditorWidget::analyze() {
    const auto* c = clip(); if (!c || c->warp.empty()) return;
    auto data = m_controller->clipSampleData(m_trackId.toStdString(), m_clipId.toStdString());
    if (!data || !data->audio) { m_status->setText(tr("Source audio is unavailable.")); return; }
    const double begin = c->warp.markers.front().sourceSeconds, end = c->warp.markers.back().sourceSeconds;
    if (data->audio == m_audio && begin == m_analysisBegin && end == m_analysisEnd) return;
    for (auto& job : m_jobs) job.cancelled->store(true);
    ++m_generation; m_audio = data->audio; m_analysisBegin = begin; m_analysisEnd = end;
    m_peaks.reset(); m_transients.clear(); m_status->setText(tr("Analyzing transients… You can already add markers."));
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_jobs.push_back({cancelled, std::async(std::launch::async, [audio = m_audio, begin, end, cancelled] {
        AnalysisResult result; result.peaks = std::make_shared<daw::WaveformPeaks>();
        const auto keep = [&] { return !cancelled->load(); };
        daw::buildPeaks(*audio, *result.peaks, keep);
        if (keep()) result.transients = daw::analysis::detectWarpTransients(*audio, begin, end, keep);
        return result;
    }), m_generation});
}
void WarpEditorWidget::pollAnalysis() {
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        try {
            auto result = it->future.get();
            if (it->generation == m_generation && !it->cancelled->load()) {
                m_peaks = std::move(result.peaks); m_transients = std::move(result.transients);
                m_status->setText(tr("%1 transient suggestions · Double-click to add · Ctrl+wheel to zoom").arg(m_transients.size()));
                m_canvas->update();
            }
        } catch (...) { if (it->generation == m_generation) m_status->setText(tr("Analysis failed. Manual Warp editing is available.")); }
        it = m_jobs.erase(it);
    }
}
