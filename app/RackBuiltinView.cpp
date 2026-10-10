#include "RackBuiltinView.hpp"
#include "Audio/SampleBuffer.hpp"
#include "Controls.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/EqualizerParams.hpp"
#include "Internal/ModulationRackInstance.hpp"
#include "Internal/SamplerVoice.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace eq = daw::plugins::equalizer;
namespace mod = daw::plugins::modulation;
namespace {
class RackPlot final : public QWidget {
  public:
    static QString tr(const char* text) {
        return QCoreApplication::translate("RackBuiltinView", text);
    }
    enum Kind { Equalizer, Compressor, Meter, Waveform, Modulation, Delay, Pitch, Gravity, Graphit };
    RackPlot(RackParameterBinding* binding, Kind kind, QWidget* parent)
        : QWidget(parent), binding(binding), kind(kind) {
        setMinimumHeight(32);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setObjectName("RackPlot");
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(tr("Device display"));
    }
    std::function<void(int)> bandSelected;
    int band = 0;
    QString uid;
    RackParameterBinding* binding;
    Kind kind;
    daw::EffectMeterSnapshot meter;
    std::optional<daw::EqualizerSnapshot> equalizer;
    std::optional<daw::EqualizerResponse> response;
    std::shared_ptr<const daw::engine::SampleBuffer> audio;
    std::shared_ptr<const daw::plugins::slicer::SliceTable> slices;
    QVector<float> peaks;
    std::vector<double> responseKey;
    QString sourceName;
    bool dragging = false;
    QString dragParameter;
    int sliceDrag = -1;
    std::shared_ptr<daw::plugins::slicer::SliceTable> dragTable;
    daw::plugins::slicer::AnalysisSettings sliceAnalysis;
    QString bandId(eq::BandParam parameter) const {
        const auto index = eq::bandParameter(unsigned(band), parameter);
        const auto& parameters = binding->parameters();
        return index < parameters.size() ? QString::fromStdString(parameters[index].id) : QString{};
    }
    QRectF area() const {
        return QRectF(rect()).adjusted(8, 8, -8, -14);
    }
    QPointF bandPoint(const eq::BandState& state) const {
        const auto r = area();
        return {r.left() +
                    std::log(std::clamp(state.frequency, 20., 20000.) / 20.) / std::log(1000.) * r.width(),
                r.center().y() - state.gainDb / 24. * r.height() / 2.};
    }
    void refresh() {
        if (!binding->available())
            return;
        auto* c = binding->controller();
        if (kind == Equalizer) {
            equalizer = c->equalizerSnapshot(binding->channel(), binding->slot());
            std::vector<double> key;
            if (equalizer)
                for (const auto& b : equalizer->bands)
                    key.insert(key.end(), {double(b.enabled), double(b.type), b.frequency, b.gainDb, b.q,
                                           double(b.slope), double(b.placement)});
            if (key != responseKey) {
                responseKey = std::move(key);
                response = c->equalizerResponse(binding->channel(), binding->slot());
            }
        } else if (kind == Waveform) {
            std::shared_ptr<const daw::engine::SampleBuffer> next;
            if (uid == "daw.slicer") {
                if (const auto s = c->slicerSnapshot(binding->channel(), binding->slot())) {
                    next = s->state.audio;
                    slices = s->state.table;
                    sourceName = QString::fromStdString(s->name);
                }
            } else {
                const auto s = c->samplerSnapshot(binding->channel(), binding->slot());
                if (s.sample)
                    next = s.sample->audio;
                sourceName = QString::fromStdString(s.name);
            }
            if (next != audio) {
                audio = std::move(next);
                peaks.clear();
                if (audio && audio->frames()) {

                    constexpr int bins = 460;
                    peaks.resize(bins);
                    for (int x = 0; x < bins; ++x) {
                        const auto first = audio->frames() * x / bins;
                        const auto last = audio->frames() * (x + 1) / bins;
                        float peak = 0;
                        const auto step = std::max<daw::engine::FrameCount>(1, (last - first) / 64);
                        for (auto frame = first; frame < last; frame += step)
                            peak = std::max(peak, std::abs(audio->readSample(0, frame)));
                        peaks[x] = peak;
                    }
                }
            }
        } else if (kind != Modulation)
            meter = c->effectMeterSnapshot(binding->channel(), binding->slot());
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = ThemeManager::instance().theme();
        const auto r = area();
        if (r.width() < 1 || r.height() < 1)
            return;
        p.fillRect(rect(), t.well());
        p.setPen(QPen(t.separator(), 1));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 3, 3);
        p.setPen(QPen(t.separator(), 1));
        if (kind != Meter && kind != Pitch)
            for (int i = 1; i < 4; ++i)
                p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4),
                           QPointF(r.right(), r.top() + r.height() * i / 4));
        p.setPen(QPen(t.accent, 1.5));
        if (kind == Equalizer && equalizer) {
            if (response) {
                QPainterPath path;
                for (int i = 0; i < int(response->combined.size()); ++i) {
                    const auto db = std::clamp(response->combined[i], -24., 24.);
                    const QPointF point(r.left() + r.width() * i / (response->combined.size() - 1),
                                        r.center().y() - db * r.height() / 48.);
                    if (!i)
                        path.moveTo(point);
                    else
                        path.lineTo(point);
                }
                p.drawPath(path);
            }
            for (int i = 0; i < int(equalizer->bands.size()); ++i)
                if (equalizer->bands[i].enabled) {
                    const auto point = bandPoint(equalizer->bands[i]);
                    p.setBrush(i == band ? t.accent : t.surface);
                    p.setPen(QPen(t.accent, i == band ? 2 : 1));
                    p.drawEllipse(point, 4, 4);
                }
        } else if (kind == Compressor) {
            QPainterPath path;
            for (int i = 0; i <= 100; ++i) {
                const double input = -60 + .6 * i;
                const auto reduction = daw::plugins::compressor::reductionDb(
                    input, binding->value("threshold"), binding->value("ratio"), binding->value("knee"));
                const QPointF point(r.left() + r.width() * i / 100.,
                                    r.bottom() - (input - reduction + 60) / 60. * r.height());
                if (!i)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            p.drawPath(path);
            p.setPen(t.textPrimary);
            p.drawText(r.adjusted(5, 2, -3, -2), Qt::AlignTop | Qt::AlignLeft,
                       tr("GR %1 dB").arg(meter.reduction, 0, 'f', 1));
        } else if (kind == Waveform) {
            if (peaks.isEmpty()) {
                p.setPen(t.textSecondary);
                p.drawText(r, Qt::AlignCenter, tr("Load a sample"));
            } else {
                for (int i = 0; i < peaks.size(); ++i) {
                    const double x = r.left() + r.width() * i / peaks.size(),
                                 h = std::min(1.f, peaks[i]) * r.height() * .45;
                    p.drawLine(QPointF(x, r.center().y() - h), QPointF(x, r.center().y() + h));
                }
                p.setPen(QPen(t.textPrimary, 1));
                if (slices && audio)
                    for (unsigned i = 0; i < slices->count; ++i) {
                        const double x =
                            r.left() + r.width() * double(slices->slices[i].start) / audio->frames();
                        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                    }
                if (uid == "daw.sampler")
                    for (const auto* id : {"startoffset", "endoffset"}) {
                        const double x = r.left() + r.width() * binding->value(id);
                        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                    }
            }
        } else if (kind == Meter) {
            const QRectF gauge = QRectF(rect()).adjusted(12, 4, -12, -4);
            const QPointF pivot(gauge.center().x(), gauge.bottom() + gauge.height() * .5);
            const auto point = [&](double reduction, double scale) {
                const double angle = (215. + 110. * (1. - reduction / 24.)) * 3.141592653589793 / 180.;
                return pivot + QPointF(std::cos(angle) * gauge.width() * .55,
                                       std::sin(angle) * gauge.height() * 1.3) *
                                   scale;
            };
            QPainterPath scale;
            for (int i = 0; i <= 24; ++i) {
                if (i == 0)
                    scale.moveTo(point(i, 1.));
                else
                    scale.lineTo(point(i, 1.));
            }
            p.setPen(QPen(t.textSecondary, 1));
            p.drawPath(scale);
            for (const int mark : {0, 3, 6, 12, 24})
                p.drawLine(point(mark, .90), point(mark, 1.));
            p.setPen(QPen(t.accent, 1.5));
            p.drawLine(pivot, point(std::clamp(double(meter.reduction), 0., 24.), .94));
            p.setPen(t.textPrimary);
            p.drawText(gauge, Qt::AlignLeft | Qt::AlignBottom,
                       tr("GR %1 dB").arg(meter.reduction, 0, 'f', 1));
        } else if (kind == Graphit) {
            p.setPen(t.textPrimary);
            p.drawText(r, Qt::AlignCenter, tr("%1 dB").arg(meter.reduction, 0, 'f', 1));
            p.fillRect(QRectF(r.left(), r.bottom() - 5,
                              r.width() * std::clamp(double(meter.reduction) / 24., 0., 1.), 4),
                       t.accent);
        } else if (kind == Pitch) {
            p.setPen(t.textPrimary);
            p.drawText(
                QRectF(rect()).adjusted(4, 2, -4, -2), Qt::AlignCenter,
                meter.inputHz > 0
                    ? tr("%1 Hz  →  %2 Hz").arg(meter.inputHz, 0, 'f', 1).arg(meter.targetHz, 0, 'f', 1)
                    : tr("Pitch correction"));
        } else if (kind == Gravity) {
            const auto center = r.center();
            const auto size = std::min(r.width(), r.height()) * .4;
            for (int ring = 1; ring <= 3; ++ring)
                p.drawEllipse(center, size * ring / 3., size * ring / 3.);
            const double angle = binding->value("gravity") * 6.28318530718;
            p.setBrush(t.accent);
            p.drawEllipse(center + QPointF(std::cos(angle) * size, std::sin(angle) * size * .6), 4, 4);
        } else if (kind == Delay) {
            QPainterPath path;
            const double low = std::max(20., binding->value("lowCut"));
            const double high = std::max(low, binding->value("highCut"));
            for (int i = 0; i <= 100; ++i) {
                const double hz = 20. * std::pow(1000., i / 100.);
                const double db = -10. * std::log10(1. + std::pow(low / hz, 2)) -
                                  10. * std::log10(1. + std::pow(hz / high, 2));
                const QPointF point(r.left() + r.width() * i / 100.,
                                    r.top() + 3 + std::clamp(-db / 36., 0., 1.) * (r.height() - 6));
                if (!i)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            p.drawPath(path);
        } else {
            // The trace represents the modulation shape/depth, not fake audio.
            const double rate = binding->info("rate") ? binding->value("rate") : 1.;
            const auto* depthInfo = binding->info("depth");
            const double depth = depthInfo ? std::clamp((binding->value("depth") - depthInfo->minValue) /
                                                            (depthInfo->maxValue - depthInfo->minValue),
                                                        0., 1.)
                                           : .5;
            QPainterPath path;
            for (int i = 0; i <= 100; ++i) {
                QPointF pt(r.left() + r.width() * i / 100.,
                           r.center().y() +
                               std::sin(i * .06283185 * std::clamp(rate, .2, 4.)) * r.height() * .45 * depth);
                if (!i)
                    path.moveTo(pt);
                else
                    path.lineTo(pt);
            }
            p.drawPath(path);
        }
        p.setPen(t.textSecondary);
        auto f = font();
        f.setPixelSize(9);
        p.setFont(f);
        if (kind == Equalizer) {
            const QRectF labels(r.left(), height() - 13, r.width(), 12);
            p.drawText(labels, Qt::AlignBottom | Qt::AlignLeft, QStringLiteral("20 Hz"));
            p.drawText(labels, Qt::AlignBottom | Qt::AlignRight, QStringLiteral("20 kHz"));
            const double middle = r.left() + std::log(1000. / 20.) / std::log(1000.) * r.width();
            p.drawText(QRectF(middle - 28, labels.top(), 56, labels.height()),
                       Qt::AlignBottom | Qt::AlignHCenter, QStringLiteral("1 kHz"));
        }
        if (kind == Waveform)
            p.drawText(rect().adjusted(8, 0, -8, -1), Qt::AlignBottom | Qt::AlignLeft,
                       p.fontMetrics().elidedText(sourceName, Qt::ElideMiddle, width() - 16));
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton)
            return QWidget::mousePressEvent(e);
        if (kind == Equalizer && equalizer) {
            int chosen = -1;
            double closest = 100;
            for (int i = 0; i < int(equalizer->bands.size()); ++i)
                if (equalizer->bands[i].enabled) {
                    const auto d = bandPoint(equalizer->bands[i]) - e->position();
                    const auto distance = d.x() * d.x() + d.y() * d.y();
                    if (distance < closest) {
                        closest = distance;
                        chosen = i;
                    }
                }
            if (chosen < 0)
                for (int i = 0; i < int(equalizer->bands.size()); ++i)
                    if (!equalizer->bands[i].enabled) {
                        chosen = i;
                        break;
                    }
            if (chosen < 0)
                return;
            band = chosen;
            if (bandSelected)
                bandSelected(band);
            binding->write(bandId(eq::BandParam::Enabled), 1);
            dragging = true;
            move(e->position());
        } else if (kind == Waveform && uid == "daw.slicer" && audio && slices) {
            sliceDrag = -1;
            for (unsigned i = 1; i < slices->count; ++i) {
                const double x =
                    area().left() + area().width() * double(slices->slices[i].start) / audio->frames();
                if (std::abs(x - e->position().x()) < 8) {
                    sliceDrag = int(i);
                    break;
                }
            }
            const auto state = binding->controller()->slicerSnapshot(binding->channel(), binding->slot());
            if (sliceDrag >= 0 && state &&
                binding->controller()->beginSlicerEdit(binding->channel(), binding->slot())) {
                dragTable = std::make_shared<daw::plugins::slicer::SliceTable>(*slices);
                sliceAnalysis = state->state.analysis;
                dragging = true;
                move(e->position());
            }
        } else if (kind == Waveform && uid == "daw.sampler" && audio) {
            const auto f = std::clamp((e->position().x() - area().left()) / area().width(), 0., 1.);
            dragParameter =
                std::abs(f - binding->value("startoffset")) < std::abs(f - binding->value("endoffset"))
                    ? "startoffset"
                    : "endoffset";
            dragging = true;
            move(e->position());
        }
    }
    void move(QPointF point) {
        const auto r = area();
        const double x = std::clamp((point.x() - r.left()) / r.width(), 0., 1.);
        if (kind == Equalizer) {
            binding->write(bandId(eq::BandParam::Frequency), 20. * std::pow(1000., x));
            binding->write(bandId(eq::BandParam::Gain),
                           std::clamp((r.center().y() - point.y()) * 48. / r.height(), -24., 24.));
        } else if (uid == "daw.slicer" && dragTable && sliceDrag > 0) {
            auto& slice = dragTable->slices[unsigned(sliceDrag)];
            auto& previous = dragTable->slices[unsigned(sliceDrag - 1)];
            const auto frame = std::clamp(daw::engine::FrameCount(x * audio->frames()),
                                          daw::engine::FrameCount(previous.start + 1),
                                          daw::engine::FrameCount(slice.end - 1));
            slice.start = unsigned(frame);
            previous.end = unsigned(frame);
            binding->controller()->updateSlicerEdit(
                std::make_shared<const daw::plugins::slicer::SliceTable>(*dragTable), sliceAnalysis);
        } else {
            const double value = dragParameter == "startoffset"
                                     ? std::min(x, binding->value("endoffset") - .0001)
                                     : std::max(x, binding->value("startoffset") + .0001);
            binding->write(dragParameter, value);
        }
        refresh();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragging)
            move(e->position());
    }
    void finish() {
        if (dragging && sliceDrag >= 0) {
            binding->controller()->commitSlicerEdit("Edit Slicer Boundaries");
            emit binding->edited();
        }
        dragging = false;
        sliceDrag = -1;
        dragTable.reset();
        binding->finishAll();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        finish();
    }
    void hideEvent(QHideEvent*) override {
        finish();
    }
};
QWidget* knobs(RackParameterBinding* binding, const QStringList& ids, int columns, QWidget* parent,
               int width = 62) {
    auto* host = new QWidget(parent);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);
    int i = 0;
    for (const auto& id : ids)
        if (binding->info(id)) {
            auto* knob = binding->knob(id, host, width);
            knob->setVisualStyle(ui::Knob::VisualStyle::RackDigital);
            knob->setFixedSize(width, 60);
            grid->addWidget(knob, i / columns, i % columns, Qt::AlignCenter);
            ++i;
        }
    return host;
}
} // namespace

class RackBuiltinView::Impl {
  public:
    RackPlot* plot = nullptr;
    std::vector<RackPlot*> plots;
    QWidget* bandControls = nullptr;
    QVBoxLayout* root = nullptr;
    QHBoxLayout* moduleRow = nullptr;
    QList<QWidget*> modules;
    QList<QToolButton*> notes;
    mod::ModulationRackInstance::Order order{};
};
int RackBuiltinView::preferredWidth(const QString& uid) {
    if (uid == "daw.equalizer")
        return 420;
    if (uid == "daw.sampler" || uid == "daw.slicer")
        return 460;
    if (uid == "daw.modulation")
        return 960;
    if (uid == "daw.doubler-pro")
        return 320;
    if (uid == "daw.chorus" || uid == "daw.flanger" || uid == "daw.phaser" || uid == "daw.doubler")
        return 260;
    if (uid == "daw.cla2a" || uid == "daw.graphit")
        return 288;
    return 360;
}
bool RackBuiltinView::supports(const QString& uid) {
    return QStringList{"daw.equalizer",       "daw.compressor",  "daw.cla2a",  "daw.delay",
                       "daw.pitch-corrector", "daw.sampler",     "daw.slicer", "daw.modulation",
                       "daw.doubler",         "daw.doubler-pro", "daw.chorus", "daw.flanger",
                       "daw.phaser",          "daw.graphit",     "daw.gravity"}
        .contains(uid);
}
RackBuiltinView::RackBuiltinView(RackParameterBinding* binding, QString uid, QWidget* parent)
    : QWidget(parent), m_binding(binding), m_uid(std::move(uid)), m_impl(new Impl) {
    connect(this, &QObject::destroyed, [impl = m_impl] { delete impl; });
    auto* root = new QVBoxLayout(this);
    m_impl->root = root;
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(3);
    const auto plot = [&](RackPlot::Kind kind, int height) {
        auto* p = new RackPlot(binding, kind, this);
        p->uid = m_uid;
        p->setMinimumHeight(std::min(32, height));
        m_impl->plot = p;
        m_impl->plots.push_back(p);
        root->addWidget(p, 1);
        return p;
    };
    const auto choices = [&](QWidget* first, QWidget* second = nullptr) {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        row->addWidget(first);
        if (second)
            row->addWidget(second);
        root->addLayout(row);
    };
    if (m_uid == "daw.equalizer") {
        auto* graph = plot(RackPlot::Equalizer, 90);
        const auto rebuild = [this, graph, binding](int band) {
            binding->finishAll();
            graph->band = band;
            if (m_impl->bandControls) {
                m_impl->root->removeWidget(m_impl->bandControls);
                m_impl->bandControls->hide();
                m_impl->bandControls->deleteLater();
            }
            auto* host = new QWidget(this);
            m_impl->bandControls = host;
            auto* row = new QHBoxLayout(host);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(3);
            auto* column = new QVBoxLayout;
            auto* selector = new QComboBox(host);
            for (int i = 0; i < 24; ++i)
                selector->addItem(tr("Band %1").arg(i + 1));
            selector->setCurrentIndex(band);
            selector->setAccessibleName(tr("Equalizer band"));
            connect(selector, &QComboBox::activated, this, [graph](int b) {
                if (graph->bandSelected)
                    graph->bandSelected(b);
            });
            auto* bandRow = new QHBoxLayout;
            bandRow->setSpacing(2);
            bandRow->addWidget(selector, 1);
            bandRow->addWidget(binding->toggle(graph->bandId(eq::BandParam::Enabled), tr("On"), host));
            column->addLayout(bandRow);
            column->addWidget(
                binding->choice(graph->bandId(eq::BandParam::Type),
                                {tr("Bell"), tr("Low shelf"), tr("High shelf"), tr("Low cut"), tr("High cut"),
                                 tr("Notch"), tr("Band pass"), tr("Tilt"), tr("All pass")},
                                host));
            row->addLayout(column, 1);
            for (auto field : {eq::BandParam::Frequency, eq::BandParam::Gain, eq::BandParam::Q}) {
                auto* knob = binding->knob(graph->bandId(field), host, 70);
                knob->setCaption(field == eq::BandParam::Frequency ? tr("Frequency")
                                 : field == eq::BandParam::Gain    ? tr("Gain")
                                                                   : tr("Q"));
                knob->setVisualStyle(ui::Knob::VisualStyle::RackDigital);
                knob->setFixedSize(70, 60);
                if (field == eq::BandParam::Frequency)
                    knob->setLogarithmic(true);
                row->addWidget(knob);
            }
            m_impl->root->addWidget(host);
        };
        graph->bandSelected = rebuild;
        rebuild(0);
    } else if (m_uid == "daw.compressor") {
        plot(RackPlot::Compressor, 74);
        choices(binding->choice("mode", {tr("Soft"), tr("Punch")}, this),
                binding->toggle("autoGain", tr("Auto gain"), this));
        root->addWidget(
            knobs(binding, {"threshold", "ratio", "attack", "release", "knee", "makeup"}, 6, this, 54));
    } else if (m_uid == "daw.cla2a") {
        plot(RackPlot::Meter, 76);
        choices(binding->choice("mode", {tr("Compress"), tr("Limit")}, this));
        root->addWidget(knobs(binding, {"peakReduction", "gain"}, 2, this, 86));
    } else if (m_uid == "daw.delay") {
        plot(RackPlot::Delay, 48);
        choices(binding->choice("timeMode", {tr("Host sync"), tr("Local tempo"), tr("Milliseconds")}, this),
                binding->choice("mode", {tr("Stereo"), tr("Ping-pong")}, this));
        root->addWidget(knobs(binding, {"timeMs", "division", "bpm", "feedback", "mix", "lowCut", "highCut"},
                              7, this, 45));
    } else if (m_uid == "daw.pitch-corrector") {
        plot(RackPlot::Pitch, 42);
        auto* tonality = new QWidget(this);
        tonality->setFixedWidth(104);
        auto* toneColumn = new QVBoxLayout(tonality);
        toneColumn->setContentsMargins(0, 0, 0, 0);
        toneColumn->setSpacing(2);
        toneColumn->addWidget(binding->choice(
            "key", {"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"}, tonality));
        toneColumn->addWidget(binding->choice("scale",
                                              {tr("Chromatic"), tr("Major"), tr("Minor"),
                                               tr("Harmonic minor"), tr("Melodic minor"),
                                               tr("Pentatonic major"), tr("Pentatonic minor"), tr("Custom")},
                                              tonality));
        auto* notes = new QHBoxLayout;
        notes->setSpacing(1);
        for (int i = 0; i < 12; ++i) {
            auto* key = new QToolButton(this);
            key->setText(QStringList{"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"}[i]);
            key->setCheckable(true);
            key->setMinimumSize(24, 24);
            notes->addWidget(key);
            m_impl->notes.push_back(key);
            connect(key, &QToolButton::clicked, this, [binding, i] {
                int mask = int(binding->value("note_mask")) ^ (1 << i);
                binding->write("note_mask", std::max(1, mask));
                binding->write("scale", 7);
                binding->finishAll();
            });
        }
        root->addLayout(notes);
        auto* controls = new QHBoxLayout;
        controls->setSpacing(3);
        controls->addWidget(tonality);
        controls->addWidget(knobs(binding, {"tune", "humanize", "amount", "output_db"}, 4, this, 54), 1);
        root->addLayout(controls);
    } else if (m_uid == "daw.sampler" || m_uid == "daw.slicer") {
        plot(RackPlot::Waveform, 70);
        auto* tabRow = new QHBoxLayout;
        auto* tabs = new QTabBar(this);
        tabs->addTab(tr("Playback"));
        tabs->addTab(tr("Envelope"));
        tabRow->addWidget(tabs, 1);
        auto* load = new QToolButton(this);
        load->setText(tr("Load…"));
        tabRow->addWidget(load);
        root->addLayout(tabRow);
        connect(load, &QToolButton::clicked, this, [binding, this] {
            const auto file = QFileDialog::getOpenFileName(this, tr("Load sample"));
            if (file.isEmpty())
                return;
            binding->finishAll();
            if (m_uid == "daw.sampler")
                binding->controller()->loadSamplerSample(binding->channel(), binding->slot(),
                                                         file.toStdString());
            else
                binding->controller()->loadSlicerSample(binding->channel(), binding->slot(),
                                                        file.toStdString());
            emit binding->edited();
        });
        auto* pages = new QStackedWidget(this);
        root->addWidget(pages);
        connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
        pages->addWidget(
            knobs(binding,
                  m_uid == "daw.sampler"
                      ? QStringList{"startoffset", "endoffset", "loop.mode", "stretch.pitch", "vol", "pan"}
                      : QStringList{"playmode", "tune", "root", "vol", "pan", "gate"},
                  6, pages, 68));
        pages->addWidget(knobs(binding,
                               m_uid == "daw.sampler"
                                   ? QStringList{"amp.att", "amp.hold", "amp.dec", "amp.sus", "amp.rel"}
                                   : QStringList{"att", "dec", "sus", "rel"},
                               5, pages, 72));
    } else if (m_uid == "daw.graphit") {
        plot(RackPlot::Graphit, 76);
        choices(binding->choice("mode", {"A", "B", "P", "C", "X"}, this));
        root->addWidget(knobs(binding, {"amount", "priority"}, 2, this, 86));
    } else if (m_uid == "daw.gravity") {
        plot(RackPlot::Gravity, 64);
        choices(binding->choice("algorithm", {"Orbit", "Fall", "Rise", "Void", "Collapse", "Zero G"}, this));
        root->addWidget(knobs(binding, {"gravity", "pitch", "feedback", "decay", "size"}, 5, this, 62));
    } else if (m_uid == "daw.modulation") {
        m_impl->moduleRow = new QHBoxLayout;
        root->addLayout(m_impl->moduleRow);
        const auto& params = binding->parameters();
        using Rack = mod::ModulationRackInstance;
        for (unsigned module = 0; module < 4; ++module) {
            auto* host = new QWidget(this);
            auto* col = new QVBoxLayout(host);
            col->setContentsMargins(4, 0, 4, 0);
            col->setSpacing(2);
            auto* head = new QHBoxLayout;
            head->addWidget(binding->toggle(QString::fromStdString(params[Rack::offsets[module]].id),
                                            QStringList{"Chorus", "Doubler", "Flanger", "Phaser"}[module],
                                            host),
                            1);
            for (int delta : {-1, 1}) {
                auto* move = new QToolButton(host);
                move->setText(delta < 0 ? "‹" : "›");
                move->setMinimumSize(24, 24);
                move->setToolTip(tr("Move module"));
                head->addWidget(move);
                connect(move, &QToolButton::clicked, this, [this, binding, module, delta] {
                    using Rack = mod::ModulationRackInstance;
                    auto order = Rack::decodeOrder(unsigned(binding->value(
                        QString::fromStdString(binding->parameters()[Rack::orderParameter].id))));
                    auto it = std::find(order.begin(), order.end(), module);
                    int from = int(it - order.begin()), to = std::clamp(from + delta, 0, 3);
                    std::swap(order[from], order[to]);
                    auto id = QString::fromStdString(binding->parameters()[Rack::orderParameter].id);
                    binding->write(id, Rack::encodeOrder(order));
                    binding->finish(id);
                    refresh();
                });
            }
            col->addLayout(head);
            auto* graph = new RackPlot(binding, RackPlot::Modulation, host);
            col->addWidget(graph, 1);
            m_impl->plots.push_back(graph);
            QStringList ids;
            const auto end = module == 3 ? Rack::orderParameter : Rack::offsets[module + 1];
            for (auto i = Rack::offsets[module] + 1; i < end; ++i)
                ids.push_back(QString::fromStdString(params[i].id));
            col->addWidget(knobs(binding, ids, 4, host, 50));
            m_impl->modules.push_back(host);
            m_impl->moduleRow->addWidget(host);
        }
    } else {
        plot(RackPlot::Modulation, 42);
        QStringList ids;
        for (const auto& p : binding->parameters())
            if (!p.isBypass && ids.size() < 8)
                ids.push_back(QString::fromStdString(p.id));
        root->addWidget(knobs(binding, ids, m_uid == "daw.doubler-pro" ? 6 : std::max(1, int(ids.size())),
                              this, m_uid == "daw.doubler-pro" ? 48 : 58));
    }
    refresh();
}
void RackBuiltinView::refresh() {
    if (!isVisible())
        return;
    for (auto* plot : m_impl->plots)
        plot->refresh();
    for (int i = 0; i < m_impl->notes.size(); ++i) {
        const QSignalBlocker block(m_impl->notes[i]);
        m_impl->notes[i]->setChecked(int(m_binding->value("note_mask")) & (1 << i));
    }
    if (m_impl->moduleRow) {
        using Rack = mod::ModulationRackInstance;
        const auto next = Rack::decodeOrder(unsigned(
            m_binding->value(QString::fromStdString(m_binding->parameters()[Rack::orderParameter].id))));
        if (next != m_impl->order) {
            m_impl->order = next;
            for (auto index : next) {
                m_impl->moduleRow->removeWidget(m_impl->modules[index]);
                m_impl->moduleRow->addWidget(m_impl->modules[index]);
            }
        }
    }
}
