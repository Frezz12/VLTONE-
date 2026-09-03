#include "Internal/GraphitInstance.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace daw::plugins::graphit {
namespace {

using json = nlohmann::json;

constexpr std::string_view kUid = "daw.graphit";
constexpr int kStateVersion = 2;
constexpr double kPi = std::numbers::pi_v<double>;
constexpr double kSilenceDb = -120.0;

double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }

double gainToDb(double gain) noexcept {
    return gain > 1.0e-12 ? 20.0 * std::log10(gain) : kSilenceDb;
}

double smoothstep(double value) noexcept {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

const std::array<ParameterInfo, kParameterCount>& parametersImpl() {
    static const std::array<ParameterInfo, kParameterCount> table{
        ParameterInfo{std::uint32_t(Param::Amount), "amount", "Amount", "%",
                      0.0, 1.0, 0.35, true, false, false},
        ParameterInfo{std::uint32_t(Param::Mode), "mode", "Mode", {},
                      0.0, 4.0, 0.0, true, true, false},
        ParameterInfo{std::uint32_t(Param::Priority), "priority", "Priority", {},
                      -1.0, 1.0, 0.0, true, false, false},
    };
    return table;
}

const char* modeName(int mode) noexcept {
    static constexpr const char* names[] = {"A", "B", "P", "C", "X"};
    return names[std::clamp(mode, 0, 4)];
}

} // namespace

std::span<const ParameterInfo> parameterTable() noexcept {
    return parametersImpl();
}

std::string parameterText(std::uint32_t index, double value) {
    char text[32]{};
    if (index == std::uint32_t(Param::Amount)) {
        std::snprintf(text, sizeof(text), "%d%%",
                      int(std::lround(std::clamp(value, 0.0, 1.0) * 100.0)));
        return text;
    }
    if (index == std::uint32_t(Param::Mode))
        return modeName(int(std::lround(value)));
    if (index == std::uint32_t(Param::Priority)) {
        const double priority = std::clamp(value, -1.0, 1.0);
        if (std::abs(priority) < 0.005) return "MID";
        std::snprintf(text, sizeof(text), "%s %d%%",
                      priority < 0.0 ? "LOW" : "HIGH",
                      int(std::lround(std::abs(priority) * 100.0)));
        return text;
    }
    return {};
}

GraphitInstance::GraphitInstance() : m_descriptor(staticDescriptor()) {
    for (const ParameterInfo& info : parameterTable())
        m_values[info.index].store(info.defaultValue, std::memory_order_relaxed);
}

const PluginDescriptor& GraphitInstance::staticDescriptor() noexcept {
    static const PluginDescriptor descriptor = [] {
        PluginDescriptor d;
        d.format = Format::Internal;
        d.uid = std::string(kUid);
        d.path = std::string(kUid);
        d.name = "Graphit";
        d.vendor = "VLT Studio Pro";
        d.version = "1.1";
        d.category = "Effect|Distortion|Saturation";
        d.stateSchemaVersion = kStateVersion;
        d.mainInputChannels = 2;
        d.mainOutputChannels = 2;
        return d;
    }();
    return descriptor;
}

std::string_view GraphitInstance::uid() noexcept { return kUid; }

bool GraphitInstance::setBusLayout(const PluginBusLayout& wanted,
                                   PluginBusLayout& accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1) return false;
    const std::uint16_t channels = wanted.inputs.empty() ? 2 : wanted.inputs.front();
    if (channels != 1 && channels != 2) return false;
    if (!wanted.outputs.empty() && wanted.outputs.front() != channels) return false;
    m_layout.inputs = {channels};
    m_layout.outputs = {channels};
    accepted = m_layout;
    return true;
}

bool GraphitInstance::activate(const PluginProcessInfo& info) {
    m_sampleRate = info.sampleRate > 0.0 ? info.sampleRate : 48000.0;
    m_maxBlockSize = std::max<std::uint32_t>(1, info.maxBlockSize);
    m_amountSmoothed = parameterValue(std::uint32_t(Param::Amount));
    m_prioritySmoothed = parameterValue(std::uint32_t(Param::Priority));
    m_activeMode = std::clamp(
        int(std::lround(parameterValue(std::uint32_t(Param::Mode)))), 0, 4);
    m_pendingMode = m_activeMode;
    m_fadeState = FadeState::Steady;
    m_modeFade = 1.0;
    m_active = true;
    resetPath();
    return true;
}

void GraphitInstance::deactivate() {
    m_active = false;
    m_processing = false;
    resetPath();
}

std::span<const ParameterInfo> GraphitInstance::parameters() const noexcept {
    return parameterTable();
}

std::int32_t GraphitInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const ParameterInfo& info : parameterTable())
        if (info.id == id) return std::int32_t(info.index);
    return -1;
}

double GraphitInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < kParameterCount
               ? m_values[index].load(std::memory_order_relaxed)
               : 0.0;
}

std::string GraphitInstance::parameterText(std::uint32_t index,
                                           double plainValue) const {
    return graphit::parameterText(index, plainValue);
}

double GraphitInstance::clampParameter(std::uint32_t index,
                                       double value) noexcept {
    if (index >= kParameterCount || !std::isfinite(value))
        return parametersImpl()[std::min<std::uint32_t>(index, kParameterCount - 1)]
            .defaultValue;
    const ParameterInfo& info = parametersImpl()[index];
    const double clamped = std::clamp(value, info.minValue, info.maxValue);
    return info.isStepped ? std::round(clamped) : clamped;
}

void GraphitInstance::setParameterFromHost(std::uint32_t index,
                                           double plainValue) {
    if (index >= kParameterCount) return;
    m_values[index].store(clampParameter(index, plainValue),
                          std::memory_order_relaxed);
}

bool GraphitInstance::saveState(std::vector<std::uint8_t>& out) const {
    json document;
    document["version"] = kStateVersion;
    document["params"] = {
        {"amount", parameterValue(std::uint32_t(Param::Amount))},
        {"mode", parameterValue(std::uint32_t(Param::Mode))},
        {"priority", parameterValue(std::uint32_t(Param::Priority))},
    };
    const std::string text = document.dump();
    out.assign(text.begin(), text.end());
    return true;
}

bool GraphitInstance::loadState(std::span<const std::uint8_t> state) {
    if (state.empty()) return false;
    const json document = json::parse(state.begin(), state.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
    for (const ParameterInfo& info : parameterTable())
        m_values[info.index].store(info.defaultValue, std::memory_order_relaxed);
    if (const auto values = document.find("params");
        values != document.end() && values->is_object()) {
        for (const auto& [id, value] : values->items()) {
            if (!value.is_number()) continue;
            const std::int32_t index = parameterIndexForId(id);
            if (index < 0) continue;
            m_values[std::uint32_t(index)].store(
                clampParameter(std::uint32_t(index), value.get<double>()),
                std::memory_order_relaxed);
        }
    }
    return true;
}

const GraphitInstance::Profile& GraphitInstance::profile(int mode) noexcept {
    using F = FilterType;
    using C = Curve;
    static const std::array<Profile, 5> profiles{
        Profile{{EqBand{F::Bell, 320.0, -1.5, 0.7, true},
                 EqBand{F::HighShelf, 9500.0, 6.5, 0.707, true}},
                C::Tanh, 4.0, 0.40, -12.0, 1.5, 35.0, 180.0, 8.0,
                0.45, 1.5, 0.0},
        Profile{{EqBand{F::LowShelf, 120.0, 6.0, 0.707, true},
                 EqBand{F::Bell, 420.0, 2.5, 0.8, true},
                 EqBand{F::HighShelf, 7500.0, -2.0, 0.707, true}},
                C::Tanh, 8.0, 0.70, -16.0, 2.5, 20.0, 140.0, 6.0,
                0.65, 4.0, -1.0},
        Profile{{EqBand{F::LowShelf, 80.0, 3.5, 0.707, true},
                 EqBand{F::Bell, 300.0, -3.0, 1.0, true},
                 EqBand{F::Bell, 3200.0, 4.5, 0.9, true}},
                C::Atan, 7.0, 0.60, -14.0, 4.0, 25.0, 85.0, 5.0,
                0.70, 4.5, -0.5},
        Profile{{EqBand{F::LowShelf, 130.0, -3.0, 0.707, true},
                 EqBand{F::Bell, 1400.0, 6.0, 0.8, true},
                 EqBand{F::HighShelf, 5500.0, 3.0, 0.707, true},
                 EqBand{F::HighCut, 15000.0, 0.0, 0.707, true}},
                C::Cubic, 14.0, 0.90, -18.0, 6.0, 5.0, 65.0, 4.0,
                0.90, 7.0, -2.0},
        Profile{{EqBand{F::LowShelf, 90.0, 6.0, 0.707, true},
                 EqBand{F::Bell, 450.0, -5.0, 0.8, true},
                 EqBand{F::Bell, 2800.0, 7.0, 0.7, true},
                 EqBand{F::HighCut, 10000.0, 0.0, 0.707, true}},
                C::Hard, 22.0, 1.0, -22.0, 10.0, 1.5, 45.0, 2.0,
                1.0, 9.0, -4.0},
    };
    return profiles[std::size_t(std::clamp(mode, 0, 4))];
}

GraphitInstance::Coefficients GraphitInstance::design(
    FilterType type, double frequency, double gainDb, double q,
    double sampleRate) noexcept {
    const double f = std::clamp(frequency, 10.0, sampleRate * 0.45);
    const double w0 = 2.0 * kPi * f / sampleRate;
    const double cosine = std::cos(w0);
    const double sine = std::sin(w0);
    const double quality = std::clamp(q, 0.1, 10.0);
    const double alpha = sine / (2.0 * quality);
    const double a = std::pow(10.0, gainDb / 40.0);
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

    if (type == FilterType::Bell) {
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosine;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha / a;
    } else if (type == FilterType::HighCut) {
        b0 = (1.0 - cosine) * 0.5;
        b1 = 1.0 - cosine;
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosine;
        a2 = 1.0 - alpha;
    } else {
        const double shelfAlpha = sine * 0.5 *
            std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / quality - 1.0) + 2.0));
        const double beta = 2.0 * std::sqrt(a) * shelfAlpha;
        if (type == FilterType::LowShelf) {
            b0 = a * ((a + 1.0) - (a - 1.0) * cosine + beta);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosine);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosine - beta);
            a0 = (a + 1.0) + (a - 1.0) * cosine + beta;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosine);
            a2 = (a + 1.0) + (a - 1.0) * cosine - beta;
        } else {
            b0 = a * ((a + 1.0) + (a - 1.0) * cosine + beta);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosine - beta);
            a0 = (a + 1.0) - (a - 1.0) * cosine + beta;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosine);
            a2 = (a + 1.0) - (a - 1.0) * cosine - beta;
        }
    }

    if (!std::isfinite(a0) || std::abs(a0) < 1.0e-12) return {};
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double GraphitInstance::Biquad::process(double input) noexcept {
    const double output = coefficients.b0 * input + z1;
    z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
    z2 = coefficients.b2 * input - coefficients.a2 * output;
    if (std::abs(z1) < 1.0e-30) z1 = 0.0;
    if (std::abs(z2) < 1.0e-30) z2 = 0.0;
    return std::isfinite(output) ? output : 0.0;
}

double GraphitInstance::transfer(Curve curve, double value) noexcept {
    switch (curve) {
        case Curve::Tanh: return std::tanh(value);
        case Curve::Atan: return (2.0 / kPi) * std::atan(value);
        case Curve::Cubic: {
            const double x = std::clamp(value, -1.0, 1.0);
            return 1.5 * (x - x * x * x / 3.0);
        }
        case Curve::Hard: return std::clamp(value, -1.0, 1.0);
    }
    return value;
}

double GraphitInstance::antiderivative(Curve curve, double value) noexcept {
    const double absolute = std::abs(value);
    switch (curve) {
        case Curve::Tanh:
            return absolute + std::log1p(std::exp(-2.0 * absolute)) -
                   std::log(2.0);
        case Curve::Atan:
            return (2.0 / kPi) *
                   (value * std::atan(value) - 0.5 * std::log1p(value * value));
        case Curve::Cubic:
            if (absolute <= 1.0)
                return 1.5 * (0.5 * value * value -
                              value * value * value * value / 12.0);
            return absolute - 0.375;
        case Curve::Hard:
            return absolute <= 1.0 ? 0.5 * value * value : absolute - 0.5;
    }
    return 0.5 * value * value;
}

double GraphitInstance::saturate(Curve curve, double value,
                                 std::uint16_t channel) noexcept {
    const std::size_t index = std::min<std::size_t>(channel, 1);
    const double previous = m_previousDriven[index];
    m_previousDriven[index] = value;
    const double delta = value - previous;
    if (std::abs(delta) < 1.0e-6)
        return transfer(curve, 0.5 * (value + previous));
    const double output =
        (antiderivative(curve, value) - antiderivative(curve, previous)) / delta;
    return std::isfinite(output) ? output : 0.0;
}

void GraphitInstance::updateFilters(const Profile& selected, double depth,
                                    double priority) noexcept {
    for (std::size_t band = 0; band < selected.bands.size(); ++band) {
        const EqBand& setting = selected.bands[band];
        const double gain = setting.type == FilterType::HighCut
                                ? setting.gainDb
                                : setting.gainDb * depth;
        const Coefficients coefficients = setting.enabled
            ? design(setting.type, setting.frequency, gain, setting.q, m_sampleRate)
            : Coefficients{};
        for (auto& channel : m_filters) channel[band].coefficients = coefficients;
    }

    const double lowGain = depth * (priority < 0.0 ? -4.5 * priority
                                                   : -2.0 * priority);
    const double highGain = depth * (priority > 0.0 ? 4.5 * priority
                                                    : 2.0 * priority);
    const std::array<Coefficients, 2> priorityCoefficients{
        design(FilterType::LowShelf, 160.0, lowGain, 0.707, m_sampleRate),
        design(FilterType::HighShelf, 6500.0, highGain, 0.707, m_sampleRate),
    };
    for (auto& channel : m_priorityFilters) {
        channel[0].coefficients = priorityCoefficients[0];
        channel[1].coefficients = priorityCoefficients[1];
    }
}

void GraphitInstance::requestMode(int mode) noexcept {
    const int requested = std::clamp(mode, 0, 4);
    m_pendingMode = requested;
    if (requested != m_activeMode && m_fadeState != FadeState::Out)
        m_fadeState = FadeState::Out;
}

void GraphitInstance::applyEvent(const PluginEvent& event) noexcept {
    if (event.kind != PluginEvent::Kind::ParamValue ||
        event.paramIndex >= kParameterCount) return;
    const double value = clampParameter(event.paramIndex, event.value);
    m_values[event.paramIndex].store(value, std::memory_order_relaxed);
    if (event.paramIndex == std::uint32_t(Param::Mode))
        requestMode(int(std::lround(value)));
}

void GraphitInstance::renderSlice(const PluginProcessContext& context,
                                  std::uint32_t offset,
                                  std::uint32_t frames) noexcept {
    const double amountTarget = parameterValue(std::uint32_t(Param::Amount));
    const double priorityTarget =
        parameterValue(std::uint32_t(Param::Priority));
    requestMode(int(std::lround(parameterValue(std::uint32_t(Param::Mode)))));
    const double amountCoefficient =
        std::exp(-1.0 / std::max(1.0, 0.020 * m_sampleRate));
    const double fadeStep = 1.0 / std::max(1.0, 0.010 * m_sampleRate);
    float inputPeak[2]{};
    float outputPeak[2]{};
    float maximumReduction = 0.0f;

    for (std::uint32_t frame = offset; frame < offset + frames; ++frame) {
        m_amountSmoothed = amountCoefficient * m_amountSmoothed +
                           (1.0 - amountCoefficient) * amountTarget;
        m_prioritySmoothed = amountCoefficient * m_prioritySmoothed +
                             (1.0 - amountCoefficient) * priorityTarget;
        const double depth = smoothstep(m_amountSmoothed);
        if (m_fadeState == FadeState::Out) {
            m_modeFade = std::max(0.0, m_modeFade - fadeStep);
            if (m_modeFade <= 0.0) {
                m_activeMode = m_pendingMode;
                resetPath();
                m_fadeState = FadeState::In;
            }
        } else if (m_fadeState == FadeState::In) {
            if (m_pendingMode != m_activeMode) {
                m_fadeState = FadeState::Out;
            } else {
                m_modeFade = std::min(1.0, m_modeFade + fadeStep);
                if (m_modeFade >= 1.0) m_fadeState = FadeState::Steady;
            }
        }

        const Profile& selected = profile(m_activeMode);
        if (m_controlCountdown == 0) {
            updateFilters(selected, depth, m_prioritySmoothed);
            m_controlCountdown = 16;
        }
        --m_controlCountdown;

        double dry[2]{};
        double wet[2]{};
        const std::uint16_t channelCount =
            std::min<std::uint16_t>(context.outputChannels, 2);
        for (std::uint16_t channel = 0; channel < channelCount; ++channel) {
            const float* input = context.inputs && context.inputChannels > 0
                ? context.inputs[std::min<std::uint16_t>(channel,
                                                       context.inputChannels - 1)]
                : nullptr;
            dry[channel] = input ? input[frame] : 0.0;
            if (!std::isfinite(dry[channel])) dry[channel] = 0.0;
            inputPeak[channel] = std::max(inputPeak[channel],
                                           float(std::abs(dry[channel])));
            double sample = dry[channel];
            for (std::size_t band = 0; band < selected.bands.size(); ++band) {
                if (!selected.bands[band].enabled) continue;
                const double filtered = m_filters[channel][band].process(sample);
                sample = selected.bands[band].type == FilterType::HighCut
                             ? std::lerp(sample, filtered, depth)
                             : filtered;
            }
            for (Biquad& filter : m_priorityFilters[channel])
                sample = filter.process(sample);
            const double driven = sample * dbToGain(selected.driveDb);
            const double saturated = saturate(selected.curve, driven, channel);
            wet[channel] = std::lerp(
                sample, saturated, std::clamp(selected.saturationMix * depth,
                                              0.0, 1.0));
        }
        if (channelCount == 1) wet[1] = wet[0];

        const double detector = std::max(std::abs(wet[0]), std::abs(wet[1]));
        const double levelDb = gainToDb(detector);
        const double over = levelDb - selected.thresholdDb;
        const double halfKnee = selected.kneeDb * 0.5;
        double targetReduction = 0.0;
        const double compression = 1.0 - 1.0 / selected.ratio;
        if (over >= halfKnee) {
            targetReduction = over * compression;
        } else if (over > -halfKnee) {
            const double inside = over + halfKnee;
            targetReduction = compression * inside * inside /
                              (2.0 * selected.kneeDb);
        }
        const double timeMs = targetReduction > m_gainReductionDb
                                  ? selected.attackMs
                                  : selected.releaseMs;
        const double gainCoefficient = std::exp(
            -1.0 / std::max(1.0, timeMs * 0.001 * m_sampleRate));
        m_gainReductionDb = gainCoefficient * m_gainReductionDb +
                            (1.0 - gainCoefficient) * targetReduction;
        const double compressedGain = dbToGain(-m_gainReductionDb);
        const double compressionMix =
            std::clamp(selected.compressionMix * depth, 0.0, 1.0);
        const double compensation = dbToGain(
            (selected.makeupDb + selected.trimDb) * depth);

        for (std::uint16_t channel = 0; channel < context.outputChannels; ++channel) {
            const std::uint16_t source = std::min<std::uint16_t>(channel, 1);
            const double compressed = wet[source] * compressedGain;
            double processed = std::lerp(wet[source], compressed,
                                         compressionMix) * compensation;
            if (depth < 1.0e-12) processed = dry[source];
            double output = std::lerp(dry[source], processed, m_modeFade);
            if (!std::isfinite(output)) output = 0.0;
            output = std::clamp(output, -4.0, 4.0);
            context.outputs[channel][frame] = float(output);
            outputPeak[source] = std::max(outputPeak[source], float(std::abs(output)));
        }
        maximumReduction = std::max(
            maximumReduction,
            float(m_gainReductionDb * compressionMix * m_modeFade));
    }

    publishPeak(m_inputPeakLeft, inputPeak[0]);
    publishPeak(m_inputPeakRight, inputPeak[1]);
    publishPeak(m_outputPeakLeft, outputPeak[0]);
    publishPeak(m_outputPeakRight, outputPeak[1]);
    publishPeak(m_gainReductionTelemetry, maximumReduction);
}

PluginProcessDisposition GraphitInstance::process(
    const PluginProcessContext& context) noexcept {
    if (!context.outputs || context.outputChannels == 0)
        return PluginProcessDisposition::Continue;
    std::uint32_t cursor = 0;
    for (const PluginEvent& event : context.inputEvents) {
        const std::uint32_t at = std::min(event.frameOffset, context.frames);
        if (at > cursor) {
            renderSlice(context, cursor, at - cursor);
            cursor = at;
        }
        applyEvent(event);
    }
    if (cursor < context.frames)
        renderSlice(context, cursor, context.frames - cursor);
    return PluginProcessDisposition::Continue;
}

void GraphitInstance::resetPath() noexcept {
    for (auto& channel : m_filters)
        for (Biquad& filter : channel) filter.reset();
    for (auto& channel : m_priorityFilters)
        for (Biquad& filter : channel) filter.reset();
    m_previousDriven = {};
    m_gainReductionDb = 0.0;
    m_controlCountdown = 0;
}

void GraphitInstance::reset() noexcept {
    m_activeMode = std::clamp(
        int(std::lround(parameterValue(std::uint32_t(Param::Mode)))), 0, 4);
    m_pendingMode = m_activeMode;
    m_fadeState = FadeState::Steady;
    m_modeFade = 1.0;
    m_amountSmoothed = parameterValue(std::uint32_t(Param::Amount));
    m_prioritySmoothed = parameterValue(std::uint32_t(Param::Priority));
    resetPath();
}

void GraphitInstance::publishPeak(std::atomic<float>& destination,
                                  float value) noexcept {
    float previous = destination.load(std::memory_order_relaxed);
    while (previous < value &&
           !destination.compare_exchange_weak(previous, value,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {}
}

Telemetry GraphitInstance::consumeTelemetry() noexcept {
    Telemetry result;
    result.inputLeft = m_inputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    result.inputRight = m_inputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    result.outputLeft = m_outputPeakLeft.exchange(0.0f, std::memory_order_relaxed);
    result.outputRight = m_outputPeakRight.exchange(0.0f, std::memory_order_relaxed);
    result.gainReductionDb =
        m_gainReductionTelemetry.exchange(0.0f, std::memory_order_relaxed);
    return result;
}

} // namespace daw::plugins::graphit
