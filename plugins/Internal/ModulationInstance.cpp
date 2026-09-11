#include "Internal/ModulationInstance.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <nlohmann/json.hpp>

namespace daw::plugins::modulation {
namespace {
using json = nlohmann::json;
constexpr std::array<FactoryPreset, 10> doublerPresets{{{"Velvet Lead", {.45, .35, .65}},
                                                        {"Intimate", {.22, .18, .75}},
                                                        {"Silk Air", {.52, .28, .35}},
                                                        {"Warm Wrap", {.58, .32, .85}},
                                                        {"Natural Double", {.48, .60, .55}},
                                                        {"Wide Whisper", {.72, .25, .70}},
                                                        {"Soft Backs", {.78, .48, .72}},
                                                        {"Gentle Rap", {.35, .20, .60}},
                                                        {"Floating Lead", {.62, .65, .65}},
                                                        {"Wide Halo", {.90, .40, .80}}}};
constexpr std::array<FactoryPreset, 10> chorusPresets{{{"Silk Vocal", {.25, .22, .30, .65}},
                                                       {"Almost Dry", {.12, .12, .18, .70}},
                                                       {"Warm Ensemble", {.38, .30, .45, .85}},
                                                       {"Air Choir", {.32, .24, .38, .30}},
                                                       {"Slow Cloud", {.40, .08, .65, .75}},
                                                       {"Close Harmony", {.22, .35, .25, .60}},
                                                       {"Velvet Backs", {.48, .28, .50, .75}},
                                                       {"Soft Keys", {.45, .45, .45, .65}},
                                                       {"Dream Guitar", {.50, .38, .60, .55}},
                                                       {"Floating Pad", {.65, .16, .75, .80}}}};
constexpr std::array<FactoryPreset, 10> flangerPresets{{{"Satin Sweep", {.18, .08, .25, .70}},
                                                        {"Almost Still", {.10, .04, .15, .80}},
                                                        {"Warm Ribbon", {.28, .10, .40, .90}},
                                                        {"Air Brush", {.18, .15, .22, .40}},
                                                        {"Slow Tide", {.32, .035, .55, .75}},
                                                        {"Vocal Motion", {.20, .12, .30, .75}},
                                                        {"Soft Tape", {.35, .18, .45, .85}},
                                                        {"Velvet Guitar", {.40, .22, .50, .65}},
                                                        {"Floating Keys", {.38, .14, .60, .70}},
                                                        {"Deep Silk", {.48, .06, .75, .90}}}};
constexpr std::array<FactoryPreset, 10> phaserPresets{{{"Velvet Phase", {.22, .10, .30, .65}},
                                                       {"Gentle Voice", {.14, .12, .20, .75}},
                                                       {"Slow Bloom", {.35, .04, .55, .80}},
                                                       {"Warm Current", {.32, .18, .45, .90}},
                                                       {"Air Ripple", {.20, .25, .30, .35}},
                                                       {"Soft Soul", {.38, .35, .45, .70}},
                                                       {"Floating Backs", {.36, .08, .60, .75}},
                                                       {"Round Keys", {.45, .28, .55, .80}},
                                                       {"Satin Guitar", {.42, .45, .50, .60}},
                                                       {"Deep Water", {.52, .06, .75, .85}}}};

double clamped(const ParameterInfo &info, double value) noexcept {
    return std::isfinite(value) ? std::clamp(value, info.minValue, info.maxValue)
                                : info.defaultValue;
}
double inputSample(const PluginProcessContext &ctx, unsigned ch, unsigned frame) noexcept {
    if (!ctx.inputs || !ctx.inputChannels)
        return 0;
    ch = std::min<unsigned>(ch, ctx.inputChannels - 1);
    return ctx.inputs[ch] ? dsp::clean(ctx.inputs[ch][frame]) : 0;
}
void advancePhase(double &phase, double step) noexcept {
    phase += step;
    if (phase >= 1)
        phase -= std::floor(phase);
}
} // namespace

std::span<const FactoryPreset> factoryPresets(Kind kind) noexcept {
    switch (kind) {
    case Kind::Doubler:
        return doublerPresets;
    case Kind::Chorus:
        return chorusPresets;
    case Kind::Flanger:
        return flangerPresets;
    case Kind::Phaser:
        return phaserPresets;
    }
    return {};
}

std::span<const ParameterInfo> parameterTable(Kind kind) noexcept {
    static const std::array<ParameterInfo, 3> doubler{
        {{0, "width", "Width", "%", 0, 1, .45, true, false, false},
         {1, "humanize", "Humanize", "%", 0, 1, .35, true, false, false},
         {2, "softness", "Softness", "%", 0, 1, .65, true, false, false}}};
    static const auto tables = [] {
        std::array<std::array<ParameterInfo, 4>, 3> result;
        constexpr double minimum[]{.05, .03, .03}, maximum[]{1.5, .8, 1.2};
        for (int i = 0; i < 3; ++i) {
            const auto values = factoryPresets(Kind(i + 1)).front().values;
            result[i] = {
                {{0, "amount", "Amount", "%", 0, 1, values[0], true, false, false},
                 {1, "rate", "Rate", "Hz", minimum[i], maximum[i], values[1], true, false, false},
                 {2, "depth", "Depth", "%", 0, 1, values[2], true, false, false},
                 {3, "softness", "Softness", "%", 0, 1, values[3], true, false, false}}};
        }
        return result;
    }();
    if (kind == Kind::Doubler)
        return doubler;
    return tables[std::size_t(kind) - 1];
}

const PluginDescriptor &descriptorFor(Kind kind) noexcept {
    static const auto descriptors = [] {
        std::array<PluginDescriptor, 4> result;
        constexpr const char *uids[]{"daw.doubler", "daw.chorus", "daw.flanger", "daw.phaser"};
        constexpr const char *names[]{"Doubler", "Chorus", "Flanger", "Phaser"};
        for (int i = 0; i < 4; ++i) {
            auto &d = result[i];
            d.format = Format::Internal;
            d.uid = d.path = uids[i];
            d.name = names[i];
            d.vendor = "VLTONE";
            d.version = "1.0";
            d.stateSchemaVersion = 1;
            d.category = i == 0 ? "Effect|Modulation|Stereo" : "Effect|Modulation";
            d.mainInputChannels = d.mainOutputChannels = 2;
        }
        return result;
    }();
    return descriptors[std::size_t(kind)];
}
bool isModulationUid(std::string_view uid) noexcept {
    return uid == "daw.doubler" || uid == "daw.chorus" || uid == "daw.flanger" ||
           uid == "daw.phaser";
}

ModulationInstance::ModulationInstance(Kind kind) : m_kind(kind) {
    for (const auto &p : parameters())
        m_values[p.index].store(p.defaultValue);
    m_presetName = factoryPresets(kind).front().name;
    (void)descriptorFor(kind); // initialize immutable tables on the control thread
}
bool ModulationInstance::setBusLayout(const PluginBusLayout &wanted, PluginBusLayout &accepted) {
    if (wanted.inputs.size() > 1 || wanted.outputs.size() > 1)
        return false;
    const auto channels = wanted.inputs.empty() ? 2 : wanted.inputs.front();
    if ((channels != 1 && channels != 2) ||
        (!wanted.outputs.empty() && wanted.outputs.front() != channels))
        return false;
    m_layout.inputs = {std::uint16_t(channels)};
    m_layout.outputs = {std::uint16_t(channels)};
    accepted = m_layout;
    return true;
}
bool ModulationInstance::activate(const PluginProcessInfo &info) {
    if (!std::isfinite(info.sampleRate) || info.sampleRate < 8000 || info.sampleRate > 384000)
        return false;
    rate = info.sampleRate;
    for (const auto &p : parameters()) {
        const double seconds = p.id == "rate"                            ? .100
                               : (p.id == "depth" || p.id == "humanize") ? .080
                                                                         : .030;
        m_smoothing[p.index] = dsp::pole(seconds, rate);
    }
    m_meterPole = dsp::pole(.1, rate);
    prepareDsp();
    m_active = true;
    reset();
    return true;
}
void ModulationInstance::deactivate() {
    m_processing = false;
    m_active = false;
    reset();
}
std::int32_t ModulationInstance::parameterIndexForId(std::string_view id) const noexcept {
    for (const auto &p : parameters())
        if (p.id == id)
            return std::int32_t(p.index);
    return -1;
}
double ModulationInstance::parameterValue(std::uint32_t index) const noexcept {
    return index < parameters().size() ? m_values[index].load(std::memory_order_relaxed) : 0;
}
std::string ModulationInstance::parameterText(std::uint32_t index, double value) const {
    if (index >= parameters().size())
        return {};
    const auto &p = parameters()[index];
    value = clamped(p, value);
    char text[32];
    if (p.id == "rate")
        std::snprintf(text, sizeof(text), "%.3g Hz", value);
    else
        std::snprintf(text, sizeof(text), "%.0f%%", value * 100);
    return text;
}
void ModulationInstance::setParameterFromHost(std::uint32_t index, double value) {
    if (index < parameters().size())
        m_values[index].store(clamped(parameters()[index], value), std::memory_order_relaxed);
}
void ModulationInstance::setPresetReference(std::string kind, std::string name) {
    m_presetKind = std::move(kind);
    m_presetName = std::move(name);
}
bool ModulationInstance::saveState(std::vector<std::uint8_t> &out) const {
    json j{{"version", 1},
           {"uid", descriptor().uid},
           {"presetKind", m_presetKind},
           {"presetName", m_presetName}};
    for (const auto &p : parameters())
        j["params"][p.id] = parameterValue(p.index);
    const auto text = j.dump();
    out.assign(text.begin(), text.end());
    return true;
}
bool ModulationInstance::loadState(std::span<const std::uint8_t> data) {
    const auto j = json::parse(data.begin(), data.end(), nullptr, false);
    if (!j.is_object() || !j.contains("version") || !j["version"].is_number_integer() ||
        j["version"] != 1 || !j.contains("uid") || j["uid"] != descriptor().uid ||
        !j.contains("params") || !j["params"].is_object())
        return false;
    Values values{};
    for (const auto &p : parameters()) {
        const auto it = j["params"].find(p.id);
        if (it != j["params"].end() && !it->is_number())
            return false;
        values[p.index] = it == j["params"].end() ? p.defaultValue : clamped(p, it->get<double>());
    }
    for (const auto &p : parameters())
        setParameterFromHost(p.index, values[p.index]);
    m_presetKind = j.contains("presetKind") && j["presetKind"].is_string()
                       ? j["presetKind"].get<std::string>()
                       : "custom";
    m_presetName = j.contains("presetName") && j["presetName"].is_string()
                       ? j["presetName"].get<std::string>()
                       : "Custom";
    // Live preset changes keep delay memory and modulation trajectories intact.
    return true;
}
void ModulationInstance::reset() noexcept {
    for (const auto &p : parameters())
        smoothed[p.index] = parameterValue(p.index);
    controlPhase = 0;
    positions = {};
    m_l2 = m_r2 = m_lr = 0;
    for (auto &m : m_meters)
        m.store(0, std::memory_order_relaxed);
    m_meters[2].store(1, std::memory_order_relaxed);
    resetDsp();
    m_meterSerial.fetch_add(1, std::memory_order_release);
}
std::uint32_t ModulationInstance::tailSamples() const noexcept {
    // Conservative -120 dB bound: longest delay/feedback decay plus the
    // slowest pole in each wet path. No latency is added to the direct path.
    const double low = m_kind == Kind::Doubler || m_kind == Kind::Chorus ? 100. : 120.;
    const double filterTail = 14.0 / (2 * dsp::pi * low);
    double seconds = filterTail;
    if (m_kind == Kind::Doubler)
        seconds += .037;
    else if (m_kind == Kind::Chorus)
        seconds += .032;
    else if (m_kind == Kind::Flanger)
        seconds += .0065 * std::ceil(std::log(1.e-6) / std::log(.30));
    else
        seconds += 6 * 14.0 / (2 * dsp::pi * 120.0);
    return std::uint32_t(std::ceil(seconds * rate));
}
void ModulationInstance::render(const PluginProcessContext &ctx, std::uint32_t begin,
                                std::uint32_t end) noexcept {
    Values target{};
    for (const auto &p : parameters())
        target[p.index] = parameterValue(p.index);
    for (auto n = begin; n < end; ++n) {
        for (const auto &p : parameters()) {
            const auto i = p.index;
            smoothed[i] = target[i] + m_smoothing[i] * (smoothed[i] - target[i]);
            if (std::abs(smoothed[i] - target[i]) < 1.e-12)
                smoothed[i] = target[i];
        }
        const double l = inputSample(ctx, 0, n), r = inputSample(ctx, 1, n);
        const auto y = m_active ? sample(l, r, ctx.outputChannels > 1 && m_layout.outputs[0] > 1)
                                : std::array<double, 2>{l, r};
        for (unsigned ch = 0; ch < ctx.outputChannels; ++ch)
            if (ctx.outputs[ch])
                ctx.outputs[ch][n] =
                    ch < 2 ? float(std::clamp(dsp::clean(y[ch]),
                                              -double(std::numeric_limits<float>::max()),
                                              double(std::numeric_limits<float>::max())))
                           : 0;
        m_l2 = dsp::flush(m_meterPole * m_l2 + (1 - m_meterPole) * y[0] * y[0]);
        m_r2 = dsp::flush(m_meterPole * m_r2 + (1 - m_meterPole) * y[1] * y[1]);
        m_lr = dsp::flush(m_meterPole * m_lr + (1 - m_meterPole) * y[0] * y[1]);
        controlPhase = (controlPhase + 1) % 16;
    }
    const double sum = m_l2 + m_r2, side = std::max(0., sum - 2 * m_lr),
                 mid = std::max(0., sum + 2 * m_lr);
    m_meters[0].store(float(std::sqrt(sum * .5)), std::memory_order_relaxed);
    m_meters[1].store(float(std::sqrt(side / std::max(1.e-20, mid))), std::memory_order_relaxed);
    m_meters[2].store(
        sum > 1.e-18 ? float(std::clamp(m_lr / std::sqrt(std::max(1.e-30, m_l2 * m_r2)), -1., 1.))
                     : 1.f,
        std::memory_order_relaxed);
    for (unsigned i = 0; i < 4; ++i)
        m_meters[3 + i].store(positions[i], std::memory_order_relaxed);
    m_meterSerial.fetch_add(1, std::memory_order_release);
}
PluginProcessDisposition ModulationInstance::process(const PluginProcessContext &ctx) noexcept {
    if (!ctx.outputs)
        return PluginProcessDisposition::Continue;
    std::uint32_t cursor = 0;
    for (const auto &e : ctx.inputEvents) {
        const auto at = std::clamp(e.frameOffset, cursor, ctx.frames);
        if (at > cursor)
            render(ctx, cursor, at);
        cursor = at;
        if (e.kind == PluginEvent::Kind::ParamValue && e.paramIndex < parameters().size())
            m_values[e.paramIndex].store(clamped(parameters()[e.paramIndex], e.value),
                                         std::memory_order_relaxed);
    }
    if (cursor < ctx.frames)
        render(ctx, cursor, ctx.frames);
    return PluginProcessDisposition::Continue;
}
Telemetry ModulationInstance::telemetry() const noexcept {
    Telemetry t;
    t.serial = m_meterSerial.load(std::memory_order_acquire);
    t.level = m_meters[0].load(std::memory_order_relaxed);
    t.width = m_meters[1].load(std::memory_order_relaxed);
    t.correlation = m_meters[2].load(std::memory_order_relaxed);
    for (unsigned i = 0; i < 4; ++i)
        t.positions[i] = m_meters[3 + i].load(std::memory_order_relaxed);
    return t;
}

void DoublerInstance::prepareDsp() {
    m_delay.prepare(rate);
    m_energyPole = dsp::pole(.050, rate);
    m_gainAttack = dsp::pole(.005, rate);
    m_gainRelease = dsp::pole(.200, rate);
    m_fastPole = dsp::pole(.001, rate);
    m_slowPole = dsp::pole(.030, rate);
    m_duckRelease = dsp::pole(.080, rate);
}
void DoublerInstance::resetDsp() noexcept {
    m_delay.reset();
    m_tone.reset();
    m_midEnergy = m_sideEnergy = m_addEnergy = m_gain = m_fast = m_slow = 0;
    m_duck = 1;
    constexpr double base[]{16, 22, 29, 35};
    for (unsigned i = 0; i < 4; ++i) {
        m_wander[i].reset(0x9e3779b9u * (i + 1), rate);
        m_reads[i] = (base[i] + (.3 + 1.7 * smoothed[1]) * m_wander[i].from) * .001 * rate;
    }
}
std::array<double, 2> DoublerInstance::sample(double l, double r, bool stereo) noexcept {
    const double mid = .5 * (l + r), side = .5 * (l - r);
    m_delay.write(mid);
    constexpr double base[]{16, 22, 29, 35};
    constexpr double maxStep = 0.004610320423; // 1-2^(-8/1200): conservative in both directions
    double voices[4];
    for (unsigned i = 0; i < 4; ++i) {
        const double movement = m_wander[i].next(rate);
        const double requested = (base[i] + (.3 + 1.7 * smoothed[1]) * movement) * .001 * rate;
        m_reads[i] += std::clamp(requested - m_reads[i], -maxStep, maxStep);
        voices[i] = m_delay.read(m_reads[i]);
        positions[i] = float(movement);
    }
    m_delay.advance();
    if (!controlPhase)
        m_tone.tune(100 + 80 * smoothed[2], 12000 - 7000 * smoothed[2], rate);
    const double addition = m_tone.process(.5 * (voices[0] + voices[2] - voices[1] - voices[3]));
    const auto energy = [&](double &value, double x) {
        value = dsp::flush(m_energyPole * value + (1 - m_energyPole) * x * x);
    };
    energy(m_midEnergy, mid);
    energy(m_sideEnergy, side);
    energy(m_addEnergy, addition);
    // Triangle inequality reserves room for existing Side, including correlation
    // between it and our addition. Never narrow or repair the original signal.
    const double budget = std::max(0., .7 * std::sqrt(m_midEnergy) - std::sqrt(m_sideEnergy));
    const double target = std::min(1.4 * smoothed[0],
                                   smoothed[0] * budget / std::sqrt(std::max(1.e-20, m_addEnergy)));
    const double pole = target < m_gain ? m_gainAttack : m_gainRelease;
    m_gain = target + pole * (m_gain - target);
    m_fast = m_fastPole * m_fast + (1 - m_fastPole) * std::abs(mid);
    m_slow = m_slowPole * m_slow + (1 - m_slowPole) * std::abs(mid);
    const double transient = std::clamp((m_fast - 1.5 * m_slow) / std::max(1.e-8, m_slow), 0., 1.);
    const double duck = 1 - (1 - 0.707945784) * transient;
    m_duck = duck < m_duck ? duck + m_fastPole * (m_duck - duck)
                           : duck + m_duckRelease * (m_duck - duck);
    const double a = stereo && smoothed[0] > 0 ? addition * m_gain * m_duck : 0;
    return {l + a, r - a};
}

void ChorusInstance::prepareDsp() {
    for (auto &d : m_delays)
        d.prepare(rate);
}
void ChorusInstance::resetDsp() noexcept {
    for (auto &d : m_delays)
        d.reset();
    for (auto &t : m_tones)
        t.reset();
    constexpr double base[]{12, 17, 23, 29};
    for (unsigned i = 0; i < 4; ++i) {
        m_phases[i] = i * .25;
        m_reads[i] =
            (base[i] + 3 * smoothed[2] * std::sin(2 * dsp::pi * m_phases[i])) * .001 * rate;
    }
}
std::array<double, 2> ChorusInstance::sample(double l, double r, bool stereo) noexcept {
    m_delays[0].write(l);
    m_delays[1].write(r);
    constexpr double base[]{12, 17, 23, 29}, detune[]{1., 1.037, .973, 1.019};
    constexpr double pan[]{.12, .88, .32, .68};
    double wet[2]{};
    for (unsigned i = 0; i < 4; ++i) {
        advancePhase(m_phases[i], smoothed[1] * detune[i] / rate);
        const double wave = std::sin(2 * dsp::pi * m_phases[i]);
        const double requested = (base[i] + 3 * smoothed[2] * wave) * .001 * rate;
        m_reads[i] += std::clamp(requested - m_reads[i], -.008626912537, .008626912537);
        // Each channel keeps its own source, with mirrored voice weights.
        wet[0] += m_delays[0].read(m_reads[i]) * (stereo ? 1 - pan[i] : .5) * .5;
        wet[1] += m_delays[1].read(m_reads[i]) * (stereo ? pan[i] : .5) * .5;
        positions[i] = float(wave);
    }
    const double mix = .5 * smoothed[0];
    double input[]{l, r};
    for (unsigned ch = 0; ch < 2; ++ch) {
        m_delays[ch].advance();
        if (!controlPhase)
            m_tones[ch].tune(100, 14000 - 9000 * smoothed[3], rate);
        wet[ch] = input[ch] + mix * (m_tones[ch].process(wet[ch]) - input[ch]);
    }
    return {wet[0], wet[1]};
}

void FlangerInstance::prepareDsp() {
    for (auto &d : m_delays)
        d.prepare(rate, .012);
}
void FlangerInstance::resetDsp() noexcept {
    for (auto &d : m_delays)
        d.reset();
    for (auto &t : m_tones)
        t.reset();
    for (auto &t : m_feedbackTones)
        t.reset();
    m_feedback = {};
    m_phase = 0;
}
std::array<double, 2> FlangerInstance::sample(double l, double r, bool stereo) noexcept {
    advancePhase(m_phase, smoothed[1] / rate);
    const double feedback = .30 * smoothed[2] * (1 - .5 * smoothed[3]);
    double input[]{l, r}, output[2];
    for (unsigned ch = 0; ch < 2; ++ch) {
        const double wave = std::sin(2 * dsp::pi * m_phase + (stereo ? ch * dsp::pi / 6 : 0));
        const double delay = (3.5 + 3 * smoothed[2] * wave) * .001 * rate;
        if (!controlPhase) {
            m_tones[ch].tune(0, 12000 - 7500 * smoothed[3], rate);
            m_feedbackTones[ch].tune(120, 12000 - 7500 * smoothed[3], rate);
        }
        m_delays[ch].write(input[ch] + feedback * m_feedback[ch]);
        const double delayed = m_delays[ch].read(delay);
        m_feedback[ch] = m_feedbackTones[ch].process(delayed);
        const double wet = m_tones[ch].process(delayed) * (1 - feedback);
        output[ch] = input[ch] + .45 * smoothed[0] * (wet - input[ch]);
        m_delays[ch].advance();
        positions[ch] = float(delay / rate * 1000);
    }
    return {output[0], output[1]};
}

void PhaserInstance::resetDsp() noexcept {
    m_filters = {};
    m_feedback = {};
    m_phase = 0;
    for (auto &t : m_tones)
        t.reset();
}
std::array<double, 2> PhaserInstance::sample(double l, double r, bool stereo) noexcept {
    advancePhase(m_phase, smoothed[1] / rate);
    const double feedback = .20 * smoothed[2] * (1 - .5 * smoothed[3]);
    constexpr double offsets[]{-.9, -.54, -.18, .18, .54, .9};
    double input[]{l, r}, output[2];
    for (unsigned ch = 0; ch < 2; ++ch) {
        const double wave = std::sin(2 * dsp::pi * m_phase + (stereo ? ch * dsp::pi / 9 : 0));
        double x = input[ch] + feedback * m_feedback[ch];
        for (unsigned i = 0; i < 6; ++i) {
            const double hz = std::clamp(800 * std::exp2(offsets[i] + 1.5 * smoothed[2] * wave),
                                         120., std::min(6000., rate * .4));
            const double g = std::tan(dsp::pi * hz / rate);
            x = m_filters[ch][i].process(x, (g - 1) / (g + 1));
            if (ch == 0 && i < 4)
                positions[i] = float(hz);
        }
        m_feedback[ch] = x;
        if (!controlPhase)
            m_tones[ch].tune(0, 12000 - 7000 * smoothed[3], rate);
        const double wet = m_tones[ch].process(x) * (1 - feedback);
        output[ch] = input[ch] + .45 * smoothed[0] * (wet - input[ch]);
    }
    return {output[0], output[1]};
}
} // namespace daw::plugins::modulation
