#include "RackSamplerView.hpp"
#include "AudioImportPreparation.hpp"
#include "Controls.hpp"
#include "FileTypes.hpp"
#include "Internal/SamplerVoice.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace {
constexpr std::array<const char*, 15> displayIds{
    "startoffset", "endoffset", "amp.on", "amp.delay", "amp.att", "amp.hold",
    "amp.dec", "amp.sus", "amp.rel", "amp.atttens", "amp.dectens", "amp.reltens",
    "loop.mode", "loop.start", "loop.end"};
QString text(const char* source) { return QCoreApplication::translate("RackSamplerView", source); }
QString timeText(double seconds) {
    return seconds < 1. ? text("%1 ms").arg(seconds * 1000., 0, 'f', seconds < .01 ? 1 : 0)
                        : text("%1 s").arg(seconds, 0, 'f', 2);
}
}

class RackSamplerWaveform final : public QWidget {
public:
    explicit RackSamplerWaveform(RackParameterBinding* binding, QWidget* parent)
        : QWidget(parent), binding(binding) {
        setObjectName("RackSamplerWaveform");
        setMinimumSize(180, 60);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(text("Sample waveform and volume envelope"));
        setToolTip(text("Drag the start and end flags to trim. Enable Envelope to edit its points.\n"
                        "Shift: fine adjustment · Double-click a point: reset · Escape: cancel\n"
                        "Sustain lasts until note-off; the dashed release is a preview."));
    }
    struct Handle { QString id; QPointF point; QString label; };
    RackParameterBinding* binding;
    std::shared_ptr<const daw::plugins::sampler::SampleData> sample;
    QVector<float> peaks;
    std::array<double, displayIds.size()> values{};
    std::function<void()> openFile;
    bool dropActive = false;
    QString selected, dragging;
    QPointF lastPoint;
    double dragInitial = 0., dragSpan = 0.;

    double value(const char* id) const { return binding->value(id); }
    bool envelopeOn() const { return value("amp.on") >= .5; }
    double duration() const {
        if (!sample || !sample->audio || sample->audio->sampleRate() <= 0) return 1.;
        return std::max(.001, double(sample->baseFrames ? sample->baseFrames : sample->audio->frames()) /
                                 sample->audio->sampleRate());
    }
    double start() const { return value("startoffset") * duration(); }
    double end() const { return value("endoffset") * duration(); }
    double decayEnd() const { return start() + value("amp.delay") + value("amp.att") +
                                     value("amp.hold") + value("amp.dec"); }
    double noteOff() const {
        // A note can be held indefinitely. This guide is explicitly a preview,
        // independent of release time so its handle follows the pointer 1:1.
        return std::max(start() + (end() - start()) * .75,
                        decayEnd() + std::max(.05, (end() - start()) * .12));
    }
    double span() const {
        if (!dragging.isEmpty()) return dragSpan;
        const double audioLength = sample && sample->audio
            ? double(sample->audio->frames()) / sample->audio->sampleRate() : duration();
        return std::max(audioLength, envelopeOn() ? noteOff() + value("amp.rel") : 0.);
    }
    QRectF area() const { return QRectF(rect()).adjusted(14, height() < 90 ? 14 : 20, -14, -24); }
    QPointF point(double seconds, double level) const {
        const auto r = area();
        return {r.left() + r.width() * seconds / span(), r.bottom() - r.height() * level};
    }
    QVector<Handle> handles() const {
        if (!sample || !sample->audio) return {};
        QVector<Handle> result{
            {"startoffset", point(start(), 0.) + QPointF(0, 12), text("Start")},
            {"endoffset", point(end(), 0.) + QPointF(0, 12), text("End")}};
        if (!envelopeOn()) return result;
        const double delay = start() + value("amp.delay"), attack = delay + value("amp.att"),
                     hold = attack + value("amp.hold"), sustain = value("amp.sus");
        // Stagger the zero-length attack/hold and start/delay targets vertically.
        result += QVector<Handle>{
            {"amp.delay", point(delay, 0.) + QPointF(0, -12), text("Dly")},
            {"amp.att", point(attack, 1.) + QPointF(0, -10), "A"},
            {"amp.hold", point(hold, 1.) + QPointF(0, 10), "H"},
            {"amp.dec", point(decayEnd(), sustain), "D"},
            {"amp.sus", point(noteOff(), sustain), "S"},
            {"amp.rel", point(noteOff() + value("amp.rel"), 0.), "R"}};
        return result;
    }
    Handle hit(QPointF at) const {
        Handle found;
        double distance = 11.;
        for (const auto& handle : handles()) {
            const double next = QLineF(at, handle.point).length();
            if (next < distance) { distance = next; found = handle; }
        }
        return found;
    }
    void setDropActive(bool active) {
        if (dropActive == active) return;
        dropActive = active;
        update();
    }
    void refresh() {
        const auto snapshot = binding->controller()->samplerSnapshot(binding->channel(), binding->slot());
        const auto audio = snapshot.sample ? snapshot.sample->audio : nullptr;
        bool changed = audio != (sample ? sample->audio : nullptr) ||
                       (snapshot.sample && sample && snapshot.sample->baseFrames != sample->baseFrames);
        if (audio != (sample ? sample->audio : nullptr)) {
            finish();
            peaks.clear();
            if (audio && audio->frames()) {
                // Bounded overview work on buffer replacement, never in paint or pointer events.
                constexpr int bins = 512;
                peaks.resize(bins);
                for (int i = 0; i < bins; ++i) {
                    const auto first = audio->frames() * i / bins;
                    const auto last = std::min(audio->frames(), std::max(first + 1, audio->frames() * (i + 1) / bins));
                    const auto step = std::max<daw::engine::FrameCount>(1, (last - first) / 64);
                    float peak = 0.f;
                    for (auto frame = first; frame < last; frame += step)
                        for (daw::engine::ChannelCount ch = 0; ch < audio->channels(); ++ch)
                            peak = std::max(peak, std::abs(audio->readSample(ch, frame)));
                    peaks[i] = peak;
                }
            }
        }
        sample = snapshot.sample;
        for (std::size_t i = 0; i < displayIds.size(); ++i) {
            const double next = value(displayIds[i]);
            changed |= values[i] != next;
            values[i] = next;
        }
        if (!envelopeOn()) {
            if (dragging.startsWith("amp.")) finish();
            if (selected.startsWith("amp.")) { selected.clear(); changed = true; }
        }
        if (changed) update();
    }
    void finish(bool cancel = false) {
        if (dragging.isEmpty()) return;
        const auto id = dragging;
        if (cancel) binding->write(id, dragInitial);
        dragging.clear();
        binding->finish(id);
        binding->refresh();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = th();
        p.setBrush(t.well());
        p.setPen(QPen(dropActive ? t.accent : t.separator(), dropActive ? 2. : 1.));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 5, 5);
        auto font = p.font(); font.setPixelSize(10); font.setFeature(QFont::Tag("tnum"), 1); p.setFont(font);
        if (dropActive || peaks.isEmpty()) {
            p.setPen(dropActive ? t.accent : t.textPrimary);
            p.drawText(rect().adjusted(12, 8, -12, -8), Qt::AlignCenter,
                dropActive ? text("Drop to replace sample") : text("Drop audio here\nor choose a file above"));
            return;
        }
        const auto r = area();
        if (r.width() <= 0 || r.height() <= 0) return;
        p.setPen(QPen(t.separator(), 1));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        for (int i = 1; i < 4; ++i) {
            const double x = r.left() + r.width() * i / 4.;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        const double audioLength = double(sample->audio->frames()) / sample->audio->sampleRate();
        p.save(); p.setClipRect(r);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (int i = 0; i < peaks.size(); ++i) {
            const double seconds = audioLength * i / peaks.size();
            const double x = point(seconds, 0).x(), h = std::min(1.f, peaks[i]) * r.height() * .46;
            const bool trimmed = seconds < start() || seconds > end();
            p.setPen(mixColors(t.well(), t.accent, trimmed ? .20 : envelopeOn() ? .42 : .85));
            p.drawLine(QPointF(x, r.center().y() - h), QPointF(x, r.center().y() + h));
        }
        p.restore();
        p.setPen(QPen(t.textSecondary, 1));
        for (double time : {start(), end()}) p.drawLine(point(time, 0), point(time, 1));
        if (value("loop.mode") >= .5) {
            p.setPen(QPen(t.textSecondary, 1, Qt::DashLine));
            for (double fraction : {value("loop.start"), value("loop.end")})
                p.drawLine(point(fraction * duration(), 0), point(fraction * duration(), 1));
        }
        if (envelopeOn()) {
            const double d = start() + value("amp.delay"), a = d + value("amp.att"),
                         h = a + value("amp.hold"), s = value("amp.sus");
            QPainterPath curve;
            curve.moveTo(point(start(), 0)); curve.lineTo(point(d, 0));
            const auto segment = [&](double from, double to, double low, double high, double tension) {
                for (int i = 1; i <= 40; ++i) {
                    const double u = i / 40.;
                    curve.lineTo(point(from + (to - from) * u,
                        low + (high - low) * daw::plugins::sampler::applyTension(u, tension)));
                }
            };
            segment(d, a, 0, 1, value("amp.atttens")); curve.lineTo(point(h, 1));
            segment(h, decayEnd(), 1, s, value("amp.dectens"));
            curve.lineTo(point(noteOff(), s));
            auto fill = curve; fill.lineTo(point(noteOff(), 0)); fill.lineTo(point(start(), 0)); fill.closeSubpath();
            auto tint = t.accent; tint.setAlpha(18); p.fillPath(fill, tint);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(t.accentHighlight, 1.8)); p.drawPath(curve);
            curve = QPainterPath(point(noteOff(), s));
            segment(noteOff(), noteOff() + value("amp.rel"), s, 0, value("amp.reltens"));
            p.setPen(QPen(t.accentHighlight, 1.5, Qt::DashLine)); p.drawPath(curve);
            p.setPen(QPen(t.textSecondary, 1, Qt::DotLine));
            p.drawLine(point(noteOff(), 0), point(noteOff(), 1));
        }
        for (const auto& handle : handles()) {
            const bool trim = !handle.id.startsWith("amp.");
            const bool active = handle.id == selected;
            const double leader = trim ? -12. : handle.id == "amp.att" ? 10. :
                handle.id == "amp.hold" ? -10. : handle.id == "amp.delay" ? 12. : 0.;
            if (leader != 0.) {
                p.setPen(QPen(trim ? t.textSecondary : t.accentHighlight, .7));
                p.drawLine(handle.point, handle.point + QPointF(0, leader));
            }
            p.setPen(QPen(trim ? t.textSecondary : t.accentHighlight, active ? 2 : 1));
            p.setBrush(active ? t.accent : t.well());
            if (trim) p.drawRect(QRectF(handle.point - QPointF(4, 4), QSizeF(8, 8)));
            else p.drawEllipse(handle.point, 4., 4.);
            p.setPen(t.textPrimary);
            const double labelWidth = p.fontMetrics().horizontalAdvance(handle.label) + 2;
            const bool rightEdge = handle.point.x() + 7 + labelWidth > r.right();
            p.drawText(QRectF(handle.point.x() + (rightEdge ? -labelWidth - 7 : 7),
                              handle.point.y() - 7, labelWidth, 14),
                       Qt::AlignVCenter | (rightEdge ? Qt::AlignRight : Qt::AlignLeft), handle.label);
        }
        // The readout belongs to the selected handle; it never follows the pointer or covers the wave.
        if (!selected.isEmpty()) {
            const auto* info = binding->info(selected);
            const auto readout = binding->controller()->insertParameterText(binding->channel(), binding->slot(),
                selected.toStdString(), binding->value(selected), info ? info->index : 0);
            p.setPen(t.textPrimary);
            p.drawText(rect().adjusted(70, 0, -12, -3), Qt::AlignBottom | Qt::AlignRight,
                       QString::fromStdString(readout));
        } else {
            p.setPen(t.textSecondary);
            p.drawText(rect().adjusted(70, 0, -12, -3), Qt::AlignBottom | Qt::AlignHCenter,
                       envelopeOn() ? text("Release after note-off") : timeText(end() - start()));
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
        const auto handle = hit(event->position());
        if (handle.id.isEmpty()) { event->accept(); return; }
        dragSpan = span(); dragging = selected = handle.id;
        dragInitial = binding->value(dragging); lastPoint = event->position();
        setFocus(Qt::MouseFocusReason); update(); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging.isEmpty()) {
            const auto handle = hit(event->position());
            setCursor(handle.id.isEmpty() ? Qt::ArrowCursor : handle.id == "amp.sus" ? Qt::SizeVerCursor : Qt::SizeHorCursor);
            return;
        }
        const auto delta = event->position() - lastPoint; lastPoint = event->position();
        const double fine = event->modifiers().testFlag(Qt::ShiftModifier) ? .1 : 1.;
        const double change = dragging == "amp.sus" ? -delta.y() / area().height() :
            delta.x() / area().width() * dragSpan / (dragging.startsWith("amp.") ? 1. : duration());
        edit(dragging, binding->value(dragging) + change * fine);
    }
    void edit(const QString& id, double next) {
        if (id == "startoffset") next = std::clamp(next, 0., std::max(0., value("endoffset") - .0001));
        if (id == "endoffset") next = std::clamp(next, std::min(1., value("startoffset") + .0001), 1.);
        binding->write(id, next); binding->refresh(); update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) finish();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        const auto handle = hit(event->position());
        if (const auto* info = binding->info(handle.id)) {
            finish(); selected = handle.id; edit(handle.id, info->defaultValue); binding->finish(handle.id);
        } else if (peaks.isEmpty() && openFile) openFile();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && !dragging.isEmpty()) { finish(true); event->accept(); return; }
        if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_8) {
            const auto hs = handles(); const int index = event->key() - Qt::Key_1;
            if (index < hs.size()) { selected = hs[index].id; update(); event->accept(); return; }
        }
        if (!selected.isEmpty() && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
                                   event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
            const double sign = event->key() == Qt::Key_Right || event->key() == Qt::Key_Up ? 1. : -1.;
            edit(selected, binding->value(selected) + sign * (event->modifiers().testFlag(Qt::ShiftModifier) ? .001 : .01));
            binding->finish(selected); event->accept(); return;
        }
        QWidget::keyPressEvent(event);
    }
    void hideEvent(QHideEvent* event) override { finish(); setDropActive(false); QWidget::hideEvent(event); }
};

RackSamplerView::RackSamplerView(RackParameterBinding* binding, QWidget* parent)
    : QWidget(parent), m_binding(binding) {
    setObjectName("RackSampler");
    setAcceptDrops(true);
    auto* row = new QHBoxLayout(this); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(8);
    auto* sample = new QVBoxLayout; sample->setSpacing(4);
    auto* header = new QHBoxLayout; header->setSpacing(5);
    m_file = new QToolButton(this); m_file->setObjectName("RackSamplerLoad");
    m_file->setMinimumSize(0, 24); m_file->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_file->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_file->setAccessibleName(tr("Load or replace sample"));
    m_file->setFocusPolicy(Qt::StrongFocus);
    connect(m_file, &QToolButton::clicked, this, &RackSamplerView::chooseSample);
    header->addWidget(m_file, 1);
    m_envelope = binding->toggle("amp.on", tr("Envelope"), this);
    qobject_cast<QToolButton*>(m_envelope)->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_envelope->setObjectName("RackSamplerEnvelope");
    m_envelope->setToolTip(tr("Enable the volume envelope and edit its points on the sample"));
    m_envelope->setFocusPolicy(Qt::StrongFocus);
    header->addWidget(m_envelope); sample->addLayout(header);
    m_waveform = new RackSamplerWaveform(binding, this);
    m_waveform->openFile = [this] { chooseSample(); };
    sample->addWidget(m_waveform, 1);
    m_info = new QLabel(this); m_info->setObjectName("RackSamplerInfo");
    m_info->setMinimumWidth(0); m_info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_info->setFixedHeight(14);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(6);
    footer->addWidget(m_info, 1);
    sample->addLayout(footer);
    row->addLayout(sample, 1);
    m_tabs = new QTabBar(this); m_tabs->setObjectName("RackSamplerTabs");
    m_tabs->addTab(tr("Playback")); m_tabs->addTab(tr("Envelope"));
    m_tabs->setExpanding(false); m_tabs->setFixedHeight(24); footer->addWidget(m_tabs);
    m_pages = new QStackedWidget(this);
    m_pages->setFixedWidth(180);
    m_pages->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    row->addWidget(m_pages, 0, Qt::AlignVCenter);
    const auto page = [&](const QStringList& ids, const QStringList& labels) {
        auto* host = new QWidget(m_pages); auto* grid = new QGridLayout(host);
        grid->setContentsMargins(0, 0, 0, 0); grid->setSpacing(0);
        for (int i = 0; i < ids.size(); ++i) {
            auto* knob = binding->knob(ids[i], host, 60);
            knob->setVisualStyle(ui::Knob::VisualStyle::RackDigital); knob->setFixedSize(60, 58);
            knob->setCaption(labels[i]);
            connect(knob, &ui::Knob::valueChanged, m_waveform, [this] { m_waveform->update(); });
            grid->addWidget(knob, i / 3, i % 3, Qt::AlignCenter);
        }
        m_pages->addWidget(host);
    };
    page({"startoffset", "endoffset", "loop.mode", "stretch.pitch", "vol", "pan"},
         {tr("Start"), tr("End"), tr("Loop"), tr("Pitch"), tr("Volume"), tr("Pan")});
    page({"amp.delay", "amp.att", "amp.hold", "amp.dec", "amp.sus", "amp.rel"},
         {tr("Delay"), tr("Attack"), tr("Hold"), tr("Decay"), tr("Sustain"), tr("Release")});
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        m_waveform->finish(); m_binding->finishAll(); m_pages->setCurrentIndex(index);
    });
    connect(m_envelope, &QAbstractButton::clicked, this, [this](bool on) {
        m_tabs->setCurrentIndex(on ? 1 : 0); refresh();
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { applyTheme(); });
    applyTheme(); refresh();
}

void RackSamplerView::applyTheme() {
    const auto& t = th();
    setStyleSheet(QStringLiteral(
        "#RackSampler QToolButton { color:%1; background:%2; border:1px solid %3; border-radius:4px; padding:1px 6px; font-size:11px; }"
        "#RackSampler QToolButton:hover { background:%3; }"
        "#RackSampler QToolButton:checked { color:%1; background:%4; }"
        "#RackSamplerInfo { color:%5; font-size:10px; }"
        "#RackSamplerTabs::tab { color:%5; background:transparent; border:0; padding:3px 5px; font-size:10px; }"
        "#RackSamplerTabs::tab:selected { color:%1; background:%2; border-radius:4px; }")
        .arg(t.textPrimary.name(), t.well().name(), t.separator().name(),
             mixColors(t.well(), t.accent, .22).name(), t.textSecondary.name()));
    m_file->setIcon(icons::icon(icons::Glyph::Folder, t.textPrimary, 14));
    m_envelope->setIcon(icons::icon(icons::Glyph::Power, t.textPrimary, 12));
    m_waveform->update();
}
void RackSamplerView::refresh() {
    if (m_loading || !m_binding->available()) return;
    m_waveform->refresh();
    const auto state = m_binding->controller()->samplerSnapshot(m_binding->channel(), m_binding->slot());
    const QString name = state.name.empty() ? tr("Load sample…") : QString::fromStdString(state.name);
    m_file->setText(m_file->fontMetrics().elidedText(name, Qt::ElideMiddle, std::max(40, m_file->width() - 32)));
    m_file->setToolTip(state.path.empty() ? tr("Choose an audio file or drop it anywhere on this module")
        : QString::fromStdString(state.path) + "\n" + tr("Click to replace sample"));
    if (state.sample && state.sample->audio) {
        const auto& audio = *state.sample->audio;
        m_info->setText(tr("%1 · %2 kHz · %3").arg(timeText(double(audio.frames()) / audio.sampleRate()))
            .arg(audio.sampleRate() / 1000., 0, 'g', 3).arg(audio.channels() == 1 ? tr("Mono") : tr("Stereo")));
    } else m_info->setText(tr("Audio files from the browser or your computer"));
    if (!m_loadError.isEmpty()) m_info->setText(m_loadError);
}
QString RackSamplerView::droppedFile(const QMimeData* mime) const {
    if (!mime || m_loading || !m_binding->available()) return {};
    for (const auto& url : mime->urls())
        if (url.isLocalFile() && ui::isAudioFile(url.toLocalFile()) && QFileInfo(url.toLocalFile()).isFile())
            return url.toLocalFile();
    return {};
}
bool RackSamplerView::loadSample(const QString& path) {
    if (path.isEmpty() || m_loading || !m_binding->available()) return false;
    m_waveform->finish(); m_binding->finishAll();
    const QPointer<RackSamplerView> guard(this);
    m_loadError.clear(); m_loading = true; m_info->setText(tr("Loading sample…"));
    const bool prepared = ui::prepareAudioImport(this, *m_binding->controller(), path);
    if (!guard) return false;
    const bool loaded = prepared && m_binding->available() &&
        m_binding->controller()->loadSamplerSample(m_binding->channel(), m_binding->slot(), path.toStdString());
    m_loading = false;
    if (prepared && !loaded) m_loadError = tr("Could not load sample. Choose another audio file.");
    refresh();
    if (loaded) emit m_binding->edited();
    return loaded;
}
void RackSamplerView::chooseSample() {
    const QPointer<RackSamplerView> guard(this);
    const auto file = QFileDialog::getOpenFileName(this, tr("Load sample"), {}, ui::audioNameFilter());
    if (guard && !file.isEmpty()) loadSample(file);
}
void RackSamplerView::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->possibleActions().testFlag(Qt::CopyAction) || droppedFile(event->mimeData()).isEmpty()) {
        m_waveform->setDropActive(false); event->ignore(); return;
    }
    event->setDropAction(Qt::CopyAction); event->accept(); m_waveform->setDropActive(true);
}
void RackSamplerView::dragMoveEvent(QDragMoveEvent* event) {
    if (!event->possibleActions().testFlag(Qt::CopyAction) || droppedFile(event->mimeData()).isEmpty()) {
        event->ignore(); m_waveform->setDropActive(false); return;
    }
    event->setDropAction(Qt::CopyAction); event->accept(); m_waveform->setDropActive(true);
}
void RackSamplerView::dragLeaveEvent(QDragLeaveEvent* event) {
    m_waveform->setDropActive(false); event->accept();
}
void RackSamplerView::dropEvent(QDropEvent* event) {
    m_waveform->setDropActive(false);
    const QString path = droppedFile(event->mimeData());
    if (!event->possibleActions().testFlag(Qt::CopyAction) || path.isEmpty()) { event->ignore(); return; }
    if (loadSample(path)) { event->setDropAction(Qt::CopyAction); event->accept(); }
    else event->ignore();
}
void RackSamplerView::hideEvent(QHideEvent* event) {
    m_waveform->finish(); m_waveform->setDropActive(false); QWidget::hideEvent(event);
}

bool RackSamplerView::checkForTest(QString* error) {
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    auto* controller = m_binding->controller();
    const auto settle = [&] {
        controller->pumpPreviewPluginEvents(); m_binding->refresh(); refresh(); QApplication::processEvents();
    };
    const auto pointer = [&](QEvent::Type type, QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, at, m_waveform->mapToGlobal(at),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(m_waveform, &event); settle();
    };
    const auto handle = [&](const QString& id) {
        for (const auto& h : m_waveform->handles()) if (h.id == id) return h.point;
        return QPointF(-100, -100);
    };
    settle();
    const auto state = controller->samplerSnapshot(m_binding->channel(), m_binding->slot());
    if (!state.sample || m_waveform->peaks.isEmpty()) return fail("rack sampler waveform fixture");
    QMimeData invalid;
    invalid.setUrls({QUrl("https://example.invalid/sample.wav"), QUrl::fromLocalFile("missing.mid")});
    QDragEnterEvent invalidEnter(QPoint(20, 20), Qt::CopyAction, &invalid, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &invalidEnter);
    if (invalidEnter.isAccepted() || m_waveform->dropActive) return fail("rack sampler rejects nonlocal/non-audio drops");
    QMimeData valid;
    const QString replacement = QString::fromStdString(state.path) + ".replacement.wav";
    if (!QFile::copy(QString::fromStdString(state.path), replacement)) return fail("rack sampler replacement fixture");
    valid.setUrls({QUrl::fromLocalFile(replacement)});
    const auto enter = [&] {
        QDragEnterEvent event(QPoint(20, 20), Qt::CopyAction | Qt::MoveAction, &valid, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(this, &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction && m_waveform->dropActive;
    };
    if (!enter()) return fail("rack sampler accepts audio as a copy and highlights the waveform");
    QDragLeaveEvent leave; QApplication::sendEvent(this, &leave);
    if (m_waveform->dropActive) return fail("rack sampler clears drop highlight on leave");
    if (!enter()) return fail("rack sampler accepts a second drag");
    const auto beforeLoad = controller->undoDepth(), loadRevision = controller->projectRevision();
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction | Qt::MoveAction, &valid, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(this, &drop); settle();
    if (!drop.isAccepted() || drop.dropAction() != Qt::CopyAction || m_waveform->dropActive ||
        controller->undoDepth() != std::min(beforeLoad + 1, controller->undoLimit()) ||
        controller->projectRevision() != loadRevision + 1 ||
        controller->samplerSnapshot(m_binding->channel(), m_binding->slot()).path != replacement.toStdString())
        return fail(QString("rack sampler audio drop: accepted=%1 action=%2 highlight=%3 undo=%4->%5 info=%6")
            .arg(drop.isAccepted()).arg(int(drop.dropAction())).arg(m_waveform->dropActive)
            .arg(beforeLoad).arg(controller->undoDepth()).arg(m_info->text()));
    controller->undo(); settle();
    if (controller->samplerSnapshot(m_binding->channel(), m_binding->slot()).path != state.path)
        return fail("rack sampler Undo restores the sample replaced by a drop");
    controller->redo(); settle();
    if (controller->samplerSnapshot(m_binding->channel(), m_binding->slot()).path != replacement.toStdString())
        return fail("rack sampler Redo restores the dropped sample");
    controller->undo(); settle();
    if (!m_envelope->isChecked()) m_envelope->click();
    settle();
    if (!m_waveform->envelopeOn() || m_tabs->currentIndex() != 1 || m_waveform->handles().size() != 8)
        return fail("rack sampler Envelope enables DSP, visible handles and envelope controls together");
    for (const auto& [id, value] : std::array<std::pair<const char*, double>, 6>{{
            {"amp.delay", .02}, {"amp.att", .1}, {"amp.hold", .08},
            {"amp.dec", .25}, {"amp.sus", .55}, {"amp.rel", .18}}}) m_binding->write(id, value);
    m_binding->finishAll(); settle();
    const auto original = handle("amp.att"), grab = original + QPointF(3, 1);
    const auto beforeDrag = controller->undoDepth(), dragRevision = controller->projectRevision();
    pointer(QEvent::MouseButtonPress, grab);
    if (QLineF(handle("amp.att"), original).length() > .05) return fail("rack sampler envelope jumps on press");
    pointer(QEvent::MouseMove, grab + QPointF(22, 0));
    if (QLineF(handle("amp.att"), original + QPointF(22, 0)).length() > .3)
        return fail("rack sampler envelope must follow the pointer without rescaling during a drag");
    pointer(QEvent::MouseMove, grab + QPointF(8, 0));
    if (QLineF(handle("amp.att"), original + QPointF(8, 0)).length() > .3)
        return fail("rack sampler envelope must reverse immediately");
    pointer(QEvent::MouseButtonRelease, grab + QPointF(8, 0));
    if (controller->undoDepth() != std::min(beforeDrag + 1, controller->undoLimit()) ||
        controller->projectRevision() != dragRevision + 1) return fail("rack sampler gesture must produce one undo entry");
    controller->undo(); settle();
    if (std::abs(m_binding->value("amp.att") - .1) > .00001) return fail("rack sampler envelope undo restores attack");
    const auto beforeCancel = controller->undoDepth();
    const auto cancelAt = handle("amp.dec");
    pointer(QEvent::MouseButtonPress, cancelAt); pointer(QEvent::MouseMove, cancelAt + QPointF(15, 0));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(m_waveform, &escape);
    pointer(QEvent::MouseButtonRelease, cancelAt + QPointF(15, 0));
    if (controller->undoDepth() != beforeCancel || std::abs(m_binding->value("amp.dec") - .25) > .00001)
        return fail("rack sampler Escape cancels a drag without an undo entry");
    const auto sustain = handle("amp.sus");
    pointer(QEvent::MouseButtonPress, sustain);
    pointer(QEvent::MouseMove, sustain + QPointF(0, -10), Qt::ShiftModifier);
    pointer(QEvent::MouseButtonRelease, sustain + QPointF(0, -10));
    if (std::abs(m_binding->value("amp.sus") - (.55 + 1. / m_waveform->area().height())) > .0001)
        return fail("rack sampler Shift must provide precise envelope editing");
    const auto beforeKey = m_binding->value("amp.sus");
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(m_waveform, &key); settle();
    if (std::abs(m_binding->value("amp.sus") - beforeKey + .01) > .00001)
        return fail("rack sampler envelope supports keyboard editing");
    m_envelope->click(); settle();
    if (m_waveform->envelopeOn() || m_waveform->handles().size() != 2)
        return fail("rack sampler disabling envelope hides its overlay");
    const auto startAt = handle("startoffset");
    pointer(QEvent::MouseButtonPress, startAt);
    pointer(QEvent::MouseMove, startAt + QPointF(width() * 2, 0));
    pointer(QEvent::MouseButtonRelease, startAt + QPointF(width() * 2, 0));
    if (m_binding->value("startoffset") >= m_binding->value("endoffset"))
        return fail("rack sampler trim handles cannot cross");
    controller->undo(); settle();
    // An automation or full-editor update must be reflected without clicking the rack toggle.
    controller->setInsertParameter(m_binding->channel(), m_binding->slot(), "amp.on", 1.);
    settle();
    if (!m_waveform->envelopeOn() || !m_envelope->isChecked() || m_waveform->handles().size() != 8)
        return fail("rack sampler overlay follows external envelope changes");
    m_tabs->setCurrentIndex(1); m_waveform->selected.clear(); m_waveform->clearFocus(); settle();
    for (auto* knob : findChildren<ui::Knob*>())
        if (knob->isVisible() && (!rect().contains(QRect(knob->mapTo(this, QPoint()), knob->size())) ||
                                  knob->accessibleName().isEmpty()))
            return fail("rack sampler controls must fit and have accessible names");
    return true;
}
