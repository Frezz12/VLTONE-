#include "RackBuiltinView.hpp"
#include "RackSamplerView.hpp"
#include "UtilityPanel.hpp"
#include "Internal/UtilityInstance.hpp"
#include "Audio/SampleBuffer.hpp"
#include "Controls.hpp"
#include "EqualizerStyle.hpp"
#include "EqualizerAnalyzerView.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/DelayInstance.hpp"
#if DAW_ENABLE_REVERB
#include "Internal/ReverbInstance.hpp"
#endif
#include "Internal/EqualizerParams.hpp"
#include "Internal/ModulationRackInstance.hpp"
#include "Internal/SamplerVoice.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QApplication>
#include <QCoreApplication>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <array>
#include <tuple>
#include <functional>

namespace eq = daw::plugins::equalizer;
namespace mod = daw::plugins::modulation;
namespace delay = daw::plugins::delay;
namespace {
delay::Timing delayTiming(RackParameterBinding* binding) {
    std::array<double, delay::kParameterCount> values{};
    for (const auto& p : delay::parameterTable()) values[p.index] = binding->value(QString::fromStdString(p.id));
    daw::engine::TransportInfo transport;
    transport.tempo = binding->controller()->tempo();
    transport.timeSigNumerator = binding->controller()->project().timeSigNumerator;
    transport.timeSigDenominator = binding->controller()->project().timeSigDenominator;
    return delay::timing(values, transport);
}
class RackPlot final : public QWidget {
  public:
    static QString tr(const char* text) {
        return QCoreApplication::translate("RackBuiltinView", text);
    }
    enum Kind { Equalizer, Compressor, Waveform, Modulation, Delay, Pitch, Graphit };
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
    QString prefix;
    RackParameterBinding* binding;
    Kind kind;
    daw::EffectMeterSnapshot meter;
    std::optional<daw::EqualizerSnapshot> equalizer;
    EqualizerAnalyzerView* analyzer = nullptr;
    std::optional<daw::EqualizerResponse> response;
    std::shared_ptr<const daw::engine::SampleBuffer> audio;
    std::shared_ptr<const daw::plugins::slicer::SliceTable> slices;
    QVector<float> peaks;
    std::vector<double> responseKey;
    QString sourceName;
    bool dragging = false;
    bool haveDisplay = false;
    std::array<double, 3> compressorCurve{};
    std::vector<double> displayParameters;
    QPointF dragOffset;
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
    void paintModulation(QPainter& p) {
        const auto& t = th();
        auto font = p.font(); font.setPixelSize(9); font.setFeature(QFont::Tag("tnum"), 1); p.setFont(font);
        const auto value = [this](const char* id) { return binding->value(prefix + id); };
        const bool doubler = uid == "daw.doubler" || uid == "daw.doubler-pro";
        const bool phaser = uid == "daw.phaser";
        const bool chorus = uid == "daw.chorus";
        const bool active = prefix.isEmpty() || value("enabled") >= .5;
        const QColor ink = active ? t.accent : t.textSecondary;
        const QRectF chart = QRectF(rect()).adjusted(28, 25, -9, -19);
        const QString title = doubler ? tr("Stereo voices") : phaser ? tr("Filter sweep · Hz") : tr("Voice delay · ms");
        p.setPen(t.textPrimary);
        p.drawText(QRect(7, 3, width() - 14, 17), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(active ? title : tr("Bypassed"), Qt::ElideRight, width() - 14));
        if (chart.width() < 1 || chart.height() < 1) return;
        if (doubler) {
            const double spread = value("width"), humanize = value("humanize");
            const auto center = chart.center();
            p.setPen(QPen(t.separator(), 1));
            p.drawLine(QPointF(center.x(), chart.top()), QPointF(center.x(), chart.bottom()));
            p.drawLine(QPointF(chart.left(), center.y()), QPointF(chart.right(), center.y()));
            p.setPen(t.textSecondary);
            p.drawText(QRectF(chart.left() - 14, center.y() - 7, 12, 14), Qt::AlignCenter, "L");
            p.drawText(QRectF(chart.right(), center.y() - 7, 9, 14), Qt::AlignCenter, "R");
            for (int voice = 0; voice < 4; ++voice) {
                const double side = voice % 2 ? 1. : -1.;
                const QPointF point(center.x() + side * chart.width() * .42 * spread,
                    chart.top() + chart.height() * (.15 + .7 * voice / 3.));
                p.setPen(QPen(mixColors(t.well(), ink, .35), 1));
                p.drawLine(center, point);
                auto fill = ink; fill.setAlpha(30);
                p.setBrush(fill); p.setPen(Qt::NoPen);
                p.drawEllipse(point, 4. + humanize * 7., 4. + humanize * 7.);
                p.setBrush(t.well()); p.setPen(QPen(ink, 1.3)); p.drawEllipse(point, 4, 4);
            }
            p.setBrush(t.textPrimary); p.setPen(Qt::NoPen); p.drawEllipse(center, 2.5, 2.5);
            p.setPen(t.textSecondary);
            p.drawText(QRect(5, height() - 17, width() - 10, 15), Qt::AlignCenter,
                       tr("4 voices"));
            return;
        }
        const double rate = std::max(.001, value("rate")), depth = value("depth");
        const double period = 1. / rate;
        const double maximum = chorus ? 40. : phaser ? 8000. : 10.;
        const auto y = [&](double v) {
            const double fraction = phaser ? std::log(std::clamp(v, 100., 8000.) / 100.) / std::log(80.)
                                          : std::clamp(v / maximum, 0., 1.);
            return chart.bottom() - chart.height() * fraction;
        };
        for (double v : phaser ? std::array{100., 900., 8000.} : std::array{0., maximum / 2, maximum}) {
            p.setPen(QPen(t.separator(), 1));
            p.drawLine(QPointF(chart.left(), y(v)), QPointF(chart.right(), y(v)));
            p.setPen(t.textSecondary);
            p.drawText(QRectF(1, y(v) - 6, 24, 12), Qt::AlignRight | Qt::AlignVCenter,
                       v >= 1000 ? QString::number(v / 1000., 'g', 2) + "k" : QString::number(v, 'g', 3));
        }
        // A complete parameter-driven cycle, not an oscilloscope or invented live audio.
        for (int voice = 0; voice < (chorus || phaser ? 4 : 2); ++voice) {
            QPainterPath path;
            constexpr double base[]{12, 17, 23, 29}, detune[]{1., 1.19, .83, 1.07};
            constexpr double offsets[]{-1.2, -.5, .15, .85};
            for (int step = 0; step <= 96; ++step) {
                const double phase = 6.28318530718 * step / 96.;
                const double wave = std::sin(chorus ? phase * detune[voice] + voice * 1.57079632679
                                                   : phase + (phaser ? 0. : voice * .5235987756));
                const double v = chorus ? base[voice] + 9 * depth * wave
                    : phaser ? std::clamp(900 * std::exp2(offsets[voice] + 2.2 * std::min(1., 2 * depth) * wave), 120., 8000.)
                             : 1.6 * std::exp2(2.4 * std::min(1., 2 * depth) * wave);
                const QPointF point(chart.left() + chart.width() * step / 96., y(v));
                if (step == 0) path.moveTo(point); else path.lineTo(point);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(mixColors(t.textSecondary, ink, .45 + .55 * (voice + 1) / 4.), 1.3,
                          voice % 2 ? Qt::DashLine : Qt::SolidLine));
            p.drawPath(path);
        }
        p.setPen(t.textSecondary);
        p.drawText(QRectF(chart.left(), height() - 17, chart.width(), 15), Qt::AlignLeft | Qt::AlignVCenter, "0");
        p.drawText(QRectF(chart.left(), height() - 17, chart.width(), 15), Qt::AlignRight | Qt::AlignVCenter,
                   tr("%1 s").arg(period, 0, 'f', 1));
    }
    void paintDelay(QPainter& p) {
        const auto& t = th();
        auto font = p.font(); font.setPixelSize(10); p.setFont(font);
        const auto timing = delayTiming(binding);
        const double ms = timing.milliseconds;
        const double span = std::max(1000., std::pow(2., std::ceil(std::log2(ms * 4 / 1000.))) * 1000.);
        const double wet = binding->value("mix") / 100., feedback = binding->value("feedback") / 100.;
        const bool ping = binding->value("mode") >= .5;
        const QRectF chart = QRectF(rect()).adjusted(23, 24, -12, -19);
        p.setPen(t.textPrimary);
        p.drawText(QRect(7, 3, width() - 14, 17), Qt::AlignVCenter,
                   tr("Repeats · %1 ms").arg(ms, 0, 'f', ms < 10 ? 1 : 0));
        for (int lane = 0; lane < 2; ++lane) {
            const double baseline = chart.top() + chart.height() * (lane + 1) / 2.;
            p.setPen(t.textSecondary);
            p.drawText(QRectF(4, baseline - 15, 14, 14), Qt::AlignCenter, lane ? "R" : "L");
            p.setPen(QPen(t.separator(), 1));
            p.drawLine(QPointF(chart.left(), baseline), QPointF(chart.right(), baseline));
            for (int echo = 0; echo <= 8 && echo * ms <= span; ++echo) {
                if (echo && ping && (echo - 1) % 2 != lane) continue;
                const double gain = echo == 0 ? 1 - wet : wet > 0 ? std::pow(feedback, echo - 1) : 0.;
                if (gain < .001) continue;
                const double x = chart.left() + chart.width() * echo * ms / span;
                p.setPen(QPen(echo ? t.accent : t.textSecondary, 3, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(x, baseline - 1), QPointF(x, baseline - gain * chart.height() * .42 - 1));
            }
        }
        p.setPen(t.textSecondary);
        p.drawText(QRectF(chart.left(), height() - 17, chart.width(), 15), Qt::AlignLeft | Qt::AlignVCenter, tr("0 s"));
        p.drawText(QRectF(chart.left(), height() - 17, chart.width(), 15), Qt::AlignRight | Qt::AlignVCenter,
                   tr("%1 s").arg(span / 1000., 0, 'g', 3));
    }
    void paintCompressor(QPainter& p, const QRectF& frame) {
        const auto& t = ThemeManager::instance().theme();
        const double reduction = meter.available ? std::max(0., double(meter.reduction)) : 0.;
        auto small = font();
        small.setPixelSize(9);
        small.setFeature(QFont::Tag("tnum"), 1);
        p.setFont(small);
        p.setPen(t.textSecondary);
        p.drawText(frame.adjusted(9, 7, -9, -7), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("GR"));
        auto valueFont = small;
        valueFont.setPixelSize(frame.width() >= 150 ? 18 : 15);
        valueFont.setWeight(QFont::DemiBold);
        p.setFont(valueFont);
        p.setPen(t.textPrimary);
        const QString reading = !meter.available ? QStringLiteral("— dB")
            : reduction < .05 ? QStringLiteral("0.0 dB")
            : QStringLiteral("−%1 dB").arg(reduction, 0, 'f', 1);
        p.drawText(frame.adjusted(27, 4, -9, -4), Qt::AlignTop | Qt::AlignRight, reading);

        const double side = std::min(frame.width() - 44., frame.height() - 53.);
        const QRectF graph(frame.left() + 20, frame.top() + 30, side, side);
        const auto point = [&](double input, double output) {
            return QPointF(graph.left() + (input + 60.) / 60. * side,
                           graph.bottom() - (output + 60.) / 60. * side);
        };
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(t.separator(), 1));
        for (const double db : {-60., -48., -36., -24., -12., 0.}) {
            p.drawLine(point(db, -60.), point(db, 0.));
            p.drawLine(point(-60., db), point(0., db));
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(mixColors(t.well(), t.textSecondary, .45), 1));
        p.drawLine(graph.bottomLeft(), graph.topRight());
        const auto threshold = binding->value("threshold");
        const auto ratio = binding->value("ratio");
        const auto knee = binding->value("knee");
        QPainterPath curve;
        for (int i = 0; i <= 100; ++i) {
            const double input = -60. + .6 * i;
            const double output = input - daw::plugins::compressor::reductionDb(input, threshold, ratio, knee);
            if (i == 0) curve.moveTo(point(input, output));
            else curve.lineTo(point(input, output));
        }
        p.setPen(QPen(t.accent, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(curve);
        if (meter.available && meter.input > .001f) {
            const double input = std::clamp(20. * std::log10(double(meter.input)), -60., 0.);
            const auto current = point(input, std::clamp(input - reduction, -60., 0.));
            p.setPen(QPen(t.accent, 1));
            p.drawLine(point(input, input), current);
            p.setBrush(t.accent);
            p.setPen(QPen(t.well(), 1.5));
            p.drawEllipse(current, 3.3, 3.3);
            p.setBrush(Qt::NoBrush);
        }
        const QRectF bar(graph.right() + 10, graph.top(), 4, graph.height());
        p.setPen(Qt::NoPen);
        p.setBrush(t.separator());
        p.drawRoundedRect(bar, 2, 2);
        p.setBrush(t.accent);
        if (reduction > .01)
            p.drawRoundedRect(QRectF(bar.topLeft(), QSizeF(bar.width(),
                bar.height() * std::clamp(reduction / 24., 0., 1.))), 2, 2);
        p.setFont(small);
        p.setPen(t.textSecondary);
        for (const int db : {-60, -30, 0}) {
            const double y = point(0., db).y();
            p.drawText(QRectF(frame.left() + 1, y - 6, 16, 12), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(db));
        }
        const auto level = [](float amplitude) {
            return amplitude < .000001f ? QStringLiteral("−∞")
                : QString::number(20. * std::log10(double(amplitude)), 'f', 1);
        };
        p.drawText(frame.adjusted(8, 0, -8, -5), Qt::AlignBottom | Qt::AlignLeft,
                   QStringLiteral("IN %1").arg(level(meter.input)));
        p.drawText(frame.adjusted(8, 0, -8, -5), Qt::AlignBottom | Qt::AlignRight,
                   QStringLiteral("OUT %1").arg(level(meter.output)));
    }
    QPointF bandPoint(const eq::BandState& state) const {
        const auto r = area();
        return {r.left() +
                    std::log(std::clamp(state.frequency, 10., 30000.) / 10.) / std::log(3000.) * r.width(),
                r.center().y() - state.gainDb / 24. * r.height() / 2.};
    }
    void refresh() {
        if (!binding->available())
            return;
        auto* c = binding->controller();
        const auto previousAudio = audio;
        const auto previousSlices = slices;
        const auto previousName = sourceName;
        bool changed = !haveDisplay;
        const auto oldMeter = meter;
        if (kind == Equalizer) {
            auto next = c->equalizerSnapshot(binding->channel(), binding->slot());
            if (analyzer && next) analyzer->refresh(next->analyzer);
            changed |= bool(next) != bool(equalizer) || (next && equalizer &&
                (next->telemetry.pre != equalizer->telemetry.pre ||
                 next->telemetry.post != equalizer->telemetry.post ||
                 next->analyzer.pre != equalizer->analyzer.pre ||
                 next->analyzer.post != equalizer->analyzer.post));
            equalizer = std::move(next);
            std::vector<double> key{binding->value("gain.scale"), binding->value("processing.mode"),
                                    c->sampleRate()};
            if (equalizer)
                for (const auto& b : equalizer->bands)
                    key.insert(key.end(), {double(b.enabled), double(b.type), b.frequency, b.gainDb, b.q,
                                           double(b.slope), double(b.placement)});
            if (key != responseKey) {
                changed = true;
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
        } else if (kind != Modulation && kind != Delay)
            meter = c->effectMeterSnapshot(binding->channel(), binding->slot());
        changed |= audio != previousAudio || slices != previousSlices || sourceName != previousName;
        const auto meterKey = [](const auto& m) { return std::tuple(m.available, m.input, m.output, m.reduction,
            m.wet, m.inputHz, m.targetHz, m.correctionCents, m.latencySamples, m.quality, m.qualityPending); };
        changed |= meterKey(oldMeter) != meterKey(meter);
        if (kind == Compressor) {
            const std::array next{binding->value("threshold"), binding->value("ratio"), binding->value("knee")};
            changed |= next != compressorCurve; compressorCurve = next;
            if (changed)
                setAccessibleDescription(tr("Gain reduction: %1 dB").arg(meter.reduction, 0, 'f', 1));
        }
        std::vector<double> parameters;
        const auto read = [&](const char* id) { parameters.push_back(binding->info(id) ? binding->value(id) : 0.); };
        if (kind == Waveform && uid == "daw.sampler") { read("startoffset"); read("endoffset"); }

        if (kind == Delay) {
            for (const auto& p : delay::parameterTable()) parameters.push_back(binding->value(QString::fromStdString(p.id)));
            parameters.push_back(delayTiming(binding).milliseconds);
        }
        if (kind == Modulation)
            for (const auto& p : binding->parameters())
                if (prefix.isEmpty() || QString::fromStdString(p.id).startsWith(prefix))
                    parameters.push_back(binding->value(QString::fromStdString(p.id)));
        changed |= parameters != displayParameters;
        displayParameters = std::move(parameters);
        if (changed) update();
        haveDisplay = true;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto& t = ThemeManager::instance().theme();
        const auto r = area();
        if (r.width() < 1 || r.height() < 1)
            return;
        QRectF frame = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
        if (kind == Compressor) {
            const double side = std::min(frame.width(), frame.height());
            frame = QRectF(frame.center() - QPointF(side / 2., side / 2.), QSizeF(side, side));
        }
        p.setBrush(kind == Equalizer ? eqUi::canvas() : t.well());
        p.setPen(kind == Equalizer ? QPen(Qt::NoPen) : QPen(t.separator(), 1));
        p.drawRoundedRect(frame, 6, 6);
        p.setBrush(Qt::NoBrush);
        if (kind == Compressor) {
            paintCompressor(p, frame);
            return;
        }
        if (kind == Modulation) { paintModulation(p); return; }
        if (kind == Delay) { paintDelay(p); return; }
        p.setPen(QPen(kind == Equalizer ? mixColors(eqUi::canvas(), t.textPrimary, .09)
                                        : t.separator(), 1));
        if (kind != Pitch)
            for (int i = 1; i < 4; ++i)
                p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4),
                           QPointF(r.right(), r.top() + r.height() * i / 4));
        p.setPen(QPen(t.accent, 1.5));
        if (kind == Equalizer && equalizer) {
            auto scale = font();
            scale.setPixelSize(9);
            p.setFont(scale);
            for (double frequency : {100., 1000., 10000.}) {
                const double x = r.left() + std::log(frequency / 10.) / std::log(3000.) * r.width();
                p.setPen(QPen(mixColors(eqUi::canvas(), t.textPrimary, .07), 1));
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
                p.setPen(t.textSecondary);
                if (frequency != 1000.)
                    p.drawText(QRectF(x - 15, r.bottom() + 2, 30, 12), Qt::AlignCenter,
                               frequency < 1000 ? QStringLiteral("100")
                                                : QStringLiteral("%1k").arg(frequency / 1000));
            }
            p.save();
            p.setClipRect(r);
            const auto spectrum = [&](const auto& bins) {
                QPainterPath path;
                const double left = r.left() + std::log(2.) / std::log(3000.) * r.width();
                const double span = std::log(1000.) / std::log(3000.) * r.width();
                for (std::size_t i = 0; i < bins.size(); ++i) {
                    const QPointF point(left + span * i / (bins.size() - 1),
                        r.bottom() - (std::clamp(double(bins[i]), -96., 6.) + 96.) / 102. * r.height());
                    if (i == 0) path.moveTo(point);
                    else path.lineTo(point);
                }
                return path;
            };
            if (equalizer->analyzer.pre) {
                auto input = spectrum(equalizer->telemetry.pre);
                input.lineTo(r.bottomRight());
                input.lineTo(r.bottomLeft());
                input.closeSubpath();
                auto fill = mixColors(eqUi::canvas(), t.textPrimary, t.dark ? .20 : .30);
                fill.setAlpha(t.dark ? 120 : 90);
                p.fillPath(input, fill);
            }
            if (equalizer->analyzer.post) {
                auto ink = mixColors(t.textPrimary, eqUi::canvas(), .30);
                ink.setAlpha(t.dark ? 150 : 170);
                p.setPen(QPen(ink, 1., Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(spectrum(equalizer->telemetry.post));
            }
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
                auto fill = path;
                fill.lineTo(r.bottomRight());
                fill.lineTo(r.bottomLeft());
                fill.closeSubpath();
                QLinearGradient wash(r.topLeft(), r.bottomLeft());
                auto tint = t.accent; tint.setAlpha(35);
                wash.setColorAt(0, tint); tint.setAlpha(0); wash.setColorAt(1, tint);
                p.fillPath(fill, wash);
                p.setPen(QPen(t.accentHighlight, 1.7));
                p.drawPath(path);
            }
            p.restore();
            for (int i = 0; i < int(equalizer->bands.size()); ++i)
                if (equalizer->bands[i].enabled) {
                    const auto point = bandPoint(equalizer->bands[i]);
                    const QColor color = eqUi::bandColor(i, t.dark);
                    p.setBrush(i == band ? color : eqUi::canvas());
                    p.setPen(QPen(color, 1.5));
                    p.drawEllipse(point, i == band ? 6 : 4, i == band ? 6 : 4);
                    if (i == band) {
                        p.setPen(t.dark ? QColor("#15191c") : Qt::white);
                        p.drawText(QRectF(point.x() - 9, point.y() - 7, 18, 14),
                                   Qt::AlignCenter, QString::number(i + 1));
                    }
                }
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
        }
        p.setPen(t.textSecondary);
        auto f = font();
        f.setPixelSize(9);
        p.setFont(f);
        if (kind == Equalizer) {
            const QRectF labels(r.left(), height() - 13, r.width(), 12);
            p.drawText(labels, Qt::AlignBottom | Qt::AlignLeft, QStringLiteral("10 Hz"));
            p.drawText(labels, Qt::AlignBottom | Qt::AlignRight, QStringLiteral("30 kHz"));
            const double middle = r.left() + std::log(1000. / 10.) / std::log(3000.) * r.width();
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
            if (!area().contains(e->position()))
                return;
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
            const bool creating = chosen < 0;
            if (creating)
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
            dragOffset = creating ? QPointF{} : e->position() - bandPoint(equalizer->bands[band]);
            if (creating) {
                const double x = std::clamp((e->position().x() - area().left()) / area().width(), 0., 1.);
                const auto type = eq::filterTypeForNewBand(10. * std::pow(3000., x));
                binding->write(bandId(eq::BandParam::Type), double(type));
                binding->write(bandId(eq::BandParam::Q), eq::qForNewBand(type));
            }
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
        if (kind == Equalizer)
            point -= dragOffset;
        const double x = std::clamp((point.x() - r.left()) / r.width(), 0., 1.);
        if (kind == Equalizer) {
            binding->write(bandId(eq::BandParam::Frequency), 10. * std::pow(3000., x));
            const auto type = eq::FilterType(int(binding->value(bandId(eq::BandParam::Type))));
            const bool cut = type == eq::FilterType::LowCut || type == eq::FilterType::HighCut;
            binding->write(bandId(eq::BandParam::Gain),
                           cut ? 0. : std::clamp((r.center().y() - point.y()) * 48. / r.height(), -24., 24.));
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
    void keyPressEvent(QKeyEvent* event) override {
        if (kind == Equalizer && bandSelected &&
            (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
            bandSelected((band + (event->key() == Qt::Key_Right ? 1 : int(eq::kBandCount) - 1)) %
                         int(eq::kBandCount));
            update();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
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
    RackSamplerView* sampler = nullptr;
    UtilityPanel* utility = nullptr;
    RackPlot* plot = nullptr;
    std::vector<RackPlot*> plots;
    QWidget* bandControls = nullptr;
    ui::Knob* equalizerQ = nullptr;
    ui::Knob* equalizerGain = nullptr;
    QVBoxLayout* root = nullptr;
    QHBoxLayout* moduleRow = nullptr;
    QList<QWidget*> modules;
    QList<QLabel*> moduleTitles;
    QList<ui::IconButton*> moduleMoves;
    QComboBox *delayMode = nullptr, *delayDivision = nullptr;
    QDoubleSpinBox *delayMilliseconds = nullptr, *delayTempo = nullptr;
    QList<QToolButton*> notes;
    mod::ModulationRackInstance::Order order{};
};
int RackBuiltinView::preferredWidth(const QString& uid) {
    if (uid == "daw.reverb") return 240;
    if (uid == "daw.gain") return 184;
    if (uid == "daw.stereo-utility" || uid == "daw.reverse") return 360;
    if (uid == "daw.equalizer")
        return 420;
    if (uid == "daw.compressor")
        return 480;
    if (uid == "daw.sampler") return 560;
    if (uid == "daw.slicer")
        return 460;
    if (uid == "daw.modulation")
        return 960;
    if (uid == "daw.delay") return 520;
    if (uid == "daw.doubler-pro")
        return 320;
    if (uid == "daw.chorus" || uid == "daw.flanger" || uid == "daw.phaser" || uid == "daw.doubler")
        return 360;
    if (uid == "daw.graphit")
        return 288;
    return 360;
}
bool RackBuiltinView::supports(const QString& uid) {
    if (daw::plugins::utility::isUtilityUid(uid.toStdString())) return true;
    return QStringList{"daw.equalizer",       "daw.compressor",  "daw.delay",
#if DAW_ENABLE_REVERB
                       "daw.reverb",
#endif
                       "daw.pitch-corrector", "daw.sampler",     "daw.slicer", "daw.modulation",
                       "daw.doubler",         "daw.doubler-pro", "daw.chorus", "daw.flanger",
                       "daw.phaser",          "daw.graphit"}
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
    const auto modulationPane = [&](const QString& uid, const QString& prefix, QWidget* parent) {
        auto* pane = new QWidget(parent);
        auto* row = new QHBoxLayout(pane);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto* graph = new RackPlot(binding, RackPlot::Modulation, pane);
        graph->uid = uid; graph->prefix = prefix;
        graph->setObjectName("RackModulationDisplay." + (prefix.isEmpty() ? uid.mid(4) : prefix.chopped(1)));
        graph->setMinimumWidth(96);
        graph->setToolTip(uid.contains("doubler")
            ? tr("Voice spread from left to right. Halos show humanization; the center is the original signal.")
            : tr("One modulation cycle. Horizontal: time. Vertical: voice delay or filter frequency."));
        graph->setAccessibleName(graph->toolTip());
        m_impl->plots.push_back(graph);
        row->addWidget(graph, 1);
        auto* fields = new QGridLayout;
        fields->setHorizontalSpacing(4);
        fields->setVerticalSpacing(0);
        const QStringList ids = uid.contains("doubler") ? QStringList{"width", "humanize", "softness"}
                                                        : QStringList{"amount", "rate", "depth", "softness"};
        const QStringList labels = uid.contains("doubler") ? QStringList{tr("Width"), tr("Humanize"), tr("Softness")}
                                                           : QStringList{tr("Amount"), tr("Rate"), tr("Depth"), tr("Softness")};
        for (int i = 0; i < ids.size(); ++i) {
            auto* number = binding->number(prefix + ids[i], pane);
            number->setFixedSize(64, 24);
            auto* label = new QLabel(labels[i], pane);
            auto font = label->font(); font.setPixelSize(10); label->setFont(font);
            label->setBuddy(number);
            fields->addWidget(label, i, 0);
            fields->addWidget(number, i, 1);
        }
        row->addLayout(fields);
        return pane;
    };
    if (daw::plugins::utility::isUtilityUid(m_uid.toStdString())) {
        root->setContentsMargins(2, 2, 2, 2);
        m_impl->utility = new UtilityPanel(binding, m_uid, this, true);
        root->addWidget(m_impl->utility);
    } else if (m_uid == "daw.equalizer") {
        setObjectName(QStringLiteral("RackEqualizer"));
        const auto restyle = [this] {
            setStyleSheet(QStringLiteral(
                "#RackEqualizer { background: %1; border: none; }"
                "#RackEqualizer QComboBox { background: %2; color: %3; border: none;"
                "border-radius: 4px; padding: 2px 8px; min-height: 22px; font-size: 11px; }"
                "#RackEqualizer QComboBox::drop-down { border: none; width: 16px; }")
                .arg(eqUi::canvas().name(), eqUi::field().name(), th().textPrimary.name()));
        };
        setAttribute(Qt::WA_StyledBackground);
        restyle();
        connect(&ThemeManager::instance(), &ThemeManager::changed, this, restyle);
        auto* graph = plot(RackPlot::Equalizer, 90);
        graph->analyzer = new EqualizerAnalyzerView(binding->controller(), binding->channel(),
                                                    binding->slot(), graph);
        graph->setAccessibleName(tr("EQ spectrum and frequency response"));
        graph->setToolTip(tr("Click a band to select it; drag to edit. Click empty space to add a band. "
                            "Left and Right select the previous or next band."));
        const auto rebuild = [this, graph, binding](int band) {
            binding->finishAll();
            graph->band = band;
            graph->setAccessibleDescription(tr("Band %1").arg(band + 1));
            graph->update();
            if (m_impl->bandControls) {
                m_impl->root->removeWidget(m_impl->bandControls);
                m_impl->bandControls->hide();
                m_impl->bandControls->deleteLater();
            }
            auto* host = new QWidget(this);
            m_impl->bandControls = host;
            auto* row = new QHBoxLayout(host);
            row->setContentsMargins(3, 0, 3, 2);
            row->setSpacing(5);
            row->addWidget(binding->toggle(graph->bandId(eq::BandParam::Enabled), tr("On"), host));
            row->addWidget(
                binding->choice(graph->bandId(eq::BandParam::Type),
                                {tr("Bell"), tr("Low shelf"), tr("High shelf"), tr("Low cut"), tr("High cut"),
                                 tr("Notch"), tr("Band pass"), tr("Tilt"), tr("All pass")},
                                host), 1);
            for (auto field : {eq::BandParam::Frequency, eq::BandParam::Gain, eq::BandParam::Q}) {
                auto* knob = binding->knob(graph->bandId(field), host, 70);
                knob->setCaption(field == eq::BandParam::Frequency ? tr("Frequency")
                                 : field == eq::BandParam::Gain    ? tr("Gain")
                                                                   : tr("Q"));
                knob->setVisualStyle(ui::Knob::VisualStyle::RackDigital);
                knob->setFixedSize(70, 60);
                knob->setArcColor(eqUi::bandColor(band, th().dark));
                if (field == eq::BandParam::Frequency || field == eq::BandParam::Q)
                    knob->setLogarithmic(true);
                if (field == eq::BandParam::Q) m_impl->equalizerQ = knob;
                if (field == eq::BandParam::Gain) m_impl->equalizerGain = knob;
                row->addWidget(knob);
            }
            m_impl->root->addWidget(host);
        };
        graph->bandSelected = rebuild;
        rebuild(0);
    } else if (m_uid == "daw.compressor") {
        auto* content = new QHBoxLayout;
        content->setSpacing(12);
        auto* graph = new RackPlot(binding, RackPlot::Compressor, this);
        graph->setObjectName("RackCompressorDisplay");
        graph->setAccessibleName(tr("Compression curve and gain reduction"));
        graph->setToolTip(tr("GR: gain reduction in dB. IN / OUT: input and output levels in dBFS."));
        graph->setMinimumHeight(128);
        graph->setFixedWidth(128);
        m_impl->plot = graph;
        m_impl->plots.push_back(graph);
        content->addWidget(graph);
        auto* settings = new QVBoxLayout;
        settings->setSpacing(4);
        auto* mode = new QHBoxLayout;
        mode->setSpacing(6);
        mode->addWidget(binding->choice("mode", {tr("Soft"), tr("Punch")}, this), 1);
        mode->addWidget(binding->toggle("autoGain", tr("Auto gain"), this));
        settings->addLayout(mode);
        auto* sliders = new QGridLayout;
        sliders->setHorizontalSpacing(6);
        sliders->setVerticalSpacing(4);
        int index = 0;
        for (const auto* id : {"threshold", "ratio", "knee", "attack", "release", "makeup"}) {
            sliders->addWidget(binding->slider(id, this), index / 3, index % 3, Qt::AlignVCenter);
            ++index;
        }
        for (int column = 0; column < 3; ++column) sliders->setColumnStretch(column, 1);
        settings->addLayout(sliders, 1);
        content->addLayout(settings, 1);
        root->addLayout(content);
#if DAW_ENABLE_REVERB
    } else if (m_uid == "daw.reverb") {
        connect(binding, &RackParameterBinding::edited, this, [binding] {
            if (binding->available()) binding->controller()->setInsertPresetReference(binding->channel(), binding->slot(), "custom", "Custom");
        });
        QStringList spaces;
        for (unsigned i = 0; i < daw::plugins::reverb::kModeCount; ++i)
            spaces.push_back(QCoreApplication::translate("ReverbPanel", daw::plugins::reverb::modeName(i).data()));
        auto* choice = binding->choice("mode", spaces, this);
        choice->setObjectName("RackReverbSpace");
        root->addWidget(choice);
        root->addStretch();
        auto* mix = binding->knob("mix", this, 86);
        mix->setArcColor(th().dark ? QColor("#b8a0ed") : QColor("#7352ac"));
        root->addWidget(mix, 0, Qt::AlignCenter);
        root->addStretch();
#endif
    } else if (m_uid == "daw.delay") {
        auto* timing = new QHBoxLayout;
        timing->setSpacing(4);
        m_impl->delayMode = binding->choice("timeMode", {tr("Project tempo"), tr("Local tempo"), tr("Milliseconds")}, this);
        m_impl->delayMode->setToolTip(tr("Time source"));
        QStringList divisions;
        for (unsigned i = 0; i < delay::kDivisionCount; ++i) {
            auto name = QString::fromUtf8(delay::divisionName(i).data(), qsizetype(delay::divisionName(i).size()));
            name.replace(" D", tr(" dotted")); name.replace(" T", tr(" triplet"));
            if (i >= 21) name = i == 21 ? tr("1 bar") : tr("2 bars");
            divisions.push_back(name);
        }
        m_impl->delayDivision = binding->choice("division", divisions, this);
        m_impl->delayDivision->setToolTip(tr("Note length"));
        m_impl->delayMilliseconds = binding->number("timeMs", this);
        m_impl->delayTempo = binding->number("bpm", this);
        m_impl->delayTempo->setFixedWidth(84);
        const int timeMode = int(binding->value("timeMode"));
        m_impl->delayDivision->setVisible(timeMode != 2);
        m_impl->delayMilliseconds->setVisible(timeMode == 2);
        m_impl->delayTempo->setVisible(timeMode == 1);
        timing->addWidget(m_impl->delayMode, 1);
        timing->addWidget(m_impl->delayDivision, 1);
        timing->addWidget(m_impl->delayMilliseconds, 1);
        timing->addWidget(m_impl->delayTempo);
        auto* routing = binding->choice("mode", {tr("Stereo"), tr("Ping-pong")}, this);
        routing->setToolTip(tr("Stereo repeats together; ping-pong alternates left and right."));
        timing->addWidget(routing, 1);
        root->addLayout(timing);
        auto* content = new QHBoxLayout;
        content->setSpacing(6);
        auto* graph = new RackPlot(binding, RackPlot::Delay, this);
        graph->setObjectName("RackDelayDisplay");
        graph->setAccessibleName(tr("Echo timing and level, left and right"));
        graph->setToolTip(tr("Each colored bar is a repeat. Spacing is delay time; height is relative to the first repeat. Feedback sets the decay."));
        m_impl->plots.push_back(graph);
        content->addWidget(graph, 1);
        content->addWidget(knobs(binding, {"feedback", "mix", "lowCut", "highCut"}, 4, this, 62));
        root->addLayout(content, 1);
        connect(m_impl->delayMode, &QComboBox::activated, this, [this] { refresh(); });
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
    } else if (m_uid == "daw.sampler") {
        m_impl->sampler = new RackSamplerView(binding, this);
        root->addWidget(m_impl->sampler, 1);
    } else if (m_uid == "daw.slicer") {
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
    } else if (m_uid == "daw.modulation") {
        root->setContentsMargins(3, 0, 3, 0);
        m_impl->moduleRow = new QHBoxLayout;
        m_impl->moduleRow->setSpacing(8);
        root->addLayout(m_impl->moduleRow);
        const auto& params = binding->parameters();
        using Rack = mod::ModulationRackInstance;
        for (unsigned module = 0; module < 4; ++module) {
            auto* host = new QWidget(this);
            auto* col = new QVBoxLayout(host);
            col->setContentsMargins(0, 0, 0, 0);
            col->setSpacing(2);
            auto* head = new QHBoxLayout;
            head->setSpacing(2);
            const auto name = QStringList{"Chorus", "Doubler", "Flanger", "Phaser"}[module];
            auto* power = binding->toggle(QString::fromStdString(params[Rack::offsets[module]].id), {}, host);
            power->setObjectName("RackModulePower." + name.toLower());
            power->setAccessibleName(tr("Enable %1").arg(name));
            power->setToolTip(power->accessibleName());
            power->setFixedSize(24, 24);
            power->setFocusPolicy(Qt::StrongFocus);
            const auto powerIcon = [power] { power->setIcon(icons::icon(icons::Glyph::Power, th().textPrimary, 14)); };
            powerIcon();
            connect(&ThemeManager::instance(), &ThemeManager::changed, power, powerIcon);
            head->addWidget(power);
            auto* title = new QLabel(name, host);
            auto font = title->font(); font.setPixelSize(11); font.setBold(true); title->setFont(font);
            m_impl->moduleTitles.push_back(title);
            head->addWidget(title, 1);
            for (int delta : {-1, 1}) {
                auto* move = new ui::IconButton(delta < 0 ? icons::Glyph::ArrowLeft : icons::Glyph::ArrowRight,
                    (delta < 0 ? tr("Move %1 earlier") : tr("Move %1 later")).arg(name), host);
                move->setObjectName("RackModuleMove." + name.toLower() + (delta < 0 ? ".left" : ".right"));
                move->setButtonSize(24, 24);
                move->setAccessibleName(move->toolTip());
                move->setFocusPolicy(Qt::StrongFocus);
                m_impl->moduleMoves.push_back(move);
                head->addWidget(move);
                connect(move, &QAbstractButton::clicked, this, [this, binding, module, delta] {
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
            col->addWidget(modulationPane("daw." + name.toLower(), name.toLower() + ".", host), 1);
            m_impl->modules.push_back(host);
            m_impl->moduleRow->addWidget(host);
        }
    } else if (m_uid != "daw.doubler-pro") {
        root->addWidget(modulationPane(m_uid, {}, this));
    } else {
        auto* graph = plot(RackPlot::Modulation, 48);
        graph->setMinimumHeight(48);
        QStringList ids;
        for (const auto& p : binding->parameters())
            if (!p.isBypass && ids.size() < 8)
                ids.push_back(QString::fromStdString(p.id));
        root->addWidget(knobs(binding, ids, m_uid == "daw.doubler-pro" ? 6 : std::max(1, int(ids.size())),
                              this, m_uid == "daw.doubler-pro" ? 48 : 58));
    }
    refresh();
}
void RackBuiltinView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_uid == "daw.compressor" && m_impl->plot)
        m_impl->plot->setFixedWidth(std::clamp(height() - 8, 128, 192));
}
void RackBuiltinView::refresh() {
    if (!isVisible())
        return;
    if (m_impl->delayMode) {
        const int mode = int(m_binding->value("timeMode"));
        m_impl->delayDivision->setVisible(mode != 2);
        m_impl->delayMilliseconds->setVisible(mode == 2);
        m_impl->delayTempo->setVisible(mode == 1);
    }
    if (m_impl->utility) m_impl->utility->refresh();
    if (m_impl->sampler) m_impl->sampler->refresh();
    for (auto* plot : m_impl->plots)
        plot->refresh();
    if (m_uid == "daw.equalizer" && m_impl->plot && m_impl->equalizerQ) {
        auto* plot = m_impl->plot;
        const auto type = eq::FilterType(int(m_binding->value(plot->bandId(eq::BandParam::Type))));
        const auto slope = eq::Slope(int(m_binding->value(plot->bandId(eq::BandParam::Slope))));
        const bool hasQ = eq::supportsQ(type, slope);
        m_impl->equalizerQ->setEnabled(m_binding->available() && hasQ);
        m_impl->equalizerQ->setToolTip(hasQ ? tr("Bandwidth / resonance (Q)")
            : tr("A 6 dB/oct cut has no Q. Choose a steeper slope in the EQ editor."));
        const bool gain = type == eq::FilterType::Bell || type == eq::FilterType::LowShelf ||
                          type == eq::FilterType::HighShelf || type == eq::FilterType::Tilt;
        m_impl->equalizerGain->setEnabled(m_binding->available() && gain);
        for (auto* knob : m_impl->bandControls->findChildren<ui::Knob*>())
            knob->setArcColor(eqUi::bandColor(plot->band, th().dark));
    }
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
        for (int position = 0; position < 4; ++position) {
            const auto module = next[position];
            const auto name = QStringList{"Chorus", "Doubler", "Flanger", "Phaser"}[module];
            m_impl->moduleTitles[module]->setText(QString::number(position + 1) + " · " + name);
            m_impl->moduleMoves[module * 2]->setEnabled(position > 0 && m_binding->available());
            m_impl->moduleMoves[module * 2 + 1]->setEnabled(position < 3 && m_binding->available());
        }
    }
}

bool RackBuiltinView::checkEqualizerForTest(QString* error) {
    auto* graph = m_impl->plots.front();
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const auto settle = [&] {
        QApplication::processEvents();
        m_binding->controller()->pumpPreviewPluginEvents();
        graph->refresh();
    };
    const auto pointer = [&](QEvent::Type type, QPointF point) {
        QMouseEvent event(type, point, graph->mapToGlobal(point),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(graph, &event);
        settle();
    };
    const auto create = [&](double frequency, double gain) {
        const auto r = graph->area();
        const QPointF point(r.left() + std::log(frequency / 10.) / std::log(3000.) * r.width(),
                            r.center().y() - gain * r.height() / 48.);
        pointer(QEvent::MouseButtonPress, point);
        pointer(QEvent::MouseButtonRelease, point);
        return graph->equalizer->bands[graph->band];
    };
    settle();
    const auto bell = create(180., 12.);
    if (bell.type != eq::FilterType::Bell || std::abs(bell.frequency - 180.) > .1)
        return fail("rack EQ creates a bell at the pointer frequency");
    const auto& curve = graph->response->combined;
    const auto peak = std::max_element(curve.begin(), curve.end()) - curve.begin();
    const double peakX = graph->area().left() + graph->area().width() * peak / (curve.size() - 1);
    const auto handle = graph->bandPoint(bell);
    if (std::abs(handle.x() - peakX) > 2.)
        return fail("rack EQ handle is centered on the response peak");
    const QPointF grab = handle + QPointF(3., 2.);
    pointer(QEvent::MouseButtonPress, grab);
    if (std::abs(graph->equalizer->bands[graph->band].frequency - bell.frequency) > .1)
        return fail("rack EQ handle jumps on grab");
    pointer(QEvent::MouseMove, grab + QPointF(15., -4.));
    pointer(QEvent::MouseButtonRelease, grab + QPointF(15., -4.));
    if (QLineF(graph->bandPoint(graph->equalizer->bands[graph->band]), handle + QPointF(15., -4.)).length() > .2)
        return fail("rack EQ drag does not track the pointer");
    const auto low = create(75., -8.);
    const auto high = create(18000., 8.);
    if (low.type != eq::FilterType::LowCut || high.type != eq::FilterType::HighCut ||
        low.gainDb != 0. || high.gainDb != 0. ||
        std::abs(low.q - eq::qForNewBand(low.type)) > 1.e-6 ||
        std::abs(high.q - eq::qForNewBand(high.type)) > 1.e-6)
        return fail("rack EQ edge bands must be cuts with zero gain and no default resonance");
    m_binding->write(graph->bandId(eq::BandParam::Q), 2.2);
    m_binding->finishAll();
    settle();
    const auto cutHandle = graph->bandPoint(high);
    pointer(QEvent::MouseButtonPress, cutHandle);
    pointer(QEvent::MouseMove, cutHandle - QPointF(24., 0.));
    pointer(QEvent::MouseButtonRelease, cutHandle - QPointF(24., 0.));
    if (graph->equalizer->bands[graph->band].type != eq::FilterType::HighCut ||
        std::abs(graph->equalizer->bands[graph->band].q - 2.2) > 1.e-6)
        return fail("rack EQ must preserve an existing cut's type and user resonance when dragged");
    for (auto type : {eq::FilterType::HighShelf, eq::FilterType::HighCut, eq::FilterType::LowCut}) {
        m_binding->write(graph->bandId(eq::BandParam::Type), double(type));
        m_binding->write(graph->bandId(eq::BandParam::Slope), double(eq::Slope::Db12));
        m_binding->write(graph->bandId(eq::BandParam::Frequency), 3000.);
        m_binding->write(graph->bandId(eq::BandParam::Gain), 9.);
        m_binding->write(graph->bandId(eq::BandParam::Q), .5);
        m_binding->finishAll();
        settle();
        m_binding->refresh();
        refresh();
        const auto before = graph->response;
        const auto depth = m_binding->controller()->undoDepth();
        m_impl->equalizerQ->editValue(2.5);
        m_impl->equalizerQ->finishEditing();
        settle();
        double delta = 0.;
        if (before && graph->response)
            for (std::size_t i = 0; i < before->combined.size(); ++i)
                delta = std::max(delta, std::abs(before->combined[i] - graph->response->combined[i]));
        if (std::abs(m_binding->value(graph->bandId(eq::BandParam::Q)) - 2.5) > 1.e-6 || delta < .1 ||
            m_binding->controller()->undoDepth() != std::min(depth + 1, m_binding->controller()->undoLimit()))
            return fail(QStringLiteral("rack Q: type=%1 value=%2 responseDelta=%3 undoSteps=%4")
                .arg(int(type)).arg(m_binding->value(graph->bandId(eq::BandParam::Q)))
                .arg(delta).arg(m_binding->controller()->undoDepth() - depth));
        m_binding->controller()->undo();
        settle();
        if (std::abs(m_binding->value(graph->bandId(eq::BandParam::Q)) - .5) > 1.e-6 ||
            int(m_binding->value(graph->bandId(eq::BandParam::Type))) != int(type))
            return fail("rack Q undo must restore only the edited width");
        m_binding->controller()->redo();
        settle();
        if (std::abs(m_binding->value(graph->bandId(eq::BandParam::Q)) - 2.5) > 1.e-6)
            return fail("rack Q redo must restore the edited width");
    }
    m_binding->write(graph->bandId(eq::BandParam::Slope), double(eq::Slope::Db6));
    m_binding->finishAll();
    settle();
    refresh();
    if (m_impl->equalizerQ->isEnabled())
        return fail("rack first-order cuts must mark Q unavailable");
    m_binding->write(graph->bandId(eq::BandParam::Slope), double(eq::Slope::Db12));
    m_binding->finishAll();
    settle();
    refresh();
    if (!m_impl->equalizerQ->isEnabled())
        return fail("rack Q must become available again with a resonant slope");
    static_assert(eq::filterTypeForNewBand(100.) == eq::FilterType::Bell);
    static_assert(eq::filterTypeForNewBand(15000.) == eq::FilterType::Bell);
    return true;
}
