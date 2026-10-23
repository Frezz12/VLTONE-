#include "EngineController.hpp"
#include "ChannelStripPreset.hpp"
#include "ProjectSerializer.hpp"
#include "Nodes/ModulationNode.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Host/PluginModulation.hpp"
#include "Host/PluginNode.hpp"
#include "model/ModulationRouting.hpp"
#include "platform/AudioFileDecoder.hpp"
#if DAW_ENABLE_CLAP
#include "Clap/ClapFactory.hpp"
#endif
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Factory.hpp"
#endif
#if DAW_ENABLE_VST
#include "Vst/VstFactory.hpp"
#endif
#include "serialization/ModulationJson.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <cstdlib>
#include <new>
#include <chrono>

static thread_local bool countAllocations = false;
static thread_local unsigned allocations = 0;
void* operator new(std::size_t n) {
    if (countAllocations) ++allocations;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace daw;
static void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) throw std::runtime_error(message);
}
static bool near(double a, double b, double tolerance = 1e-5) { return std::abs(a - b) < tolerance; }
namespace fs = std::filesystem;
namespace ap = audio::platform;
struct Temp {
    fs::path path = fs::temp_directory_path() / ("daw-rack-modulation-" + newUuid());
    Temp() { fs::create_directories(path); }
    ~Temp() { std::error_code error; fs::remove_all(path, error); }
};
static void writeConstant(const fs::path& path, float value) {
    ap::AudioFileWriter writer; ap::WriteSpec format; format.encoding = ap::Encoding::Float32;
    check(bool(writer.open(path.string(), format, 48000, 2, 48000)), "create detector test audio");
    std::vector<float> data(48000, value); const float* channels[]{data.data(), data.data()};
    check(bool(writer.write(channels, data.size())) && bool(writer.close()), "write detector test audio");
}
static ap::DecodedAudio render(EngineController& controller, const fs::path& path, const std::string& track, unsigned block) {
    rendering::Spec spec; spec.outputDir = path.string(); spec.blockSize = block;
    spec.file.encoding = ap::Encoding::Float32; spec.range = rendering::Range::Custom;
    spec.customStartSeconds = 0; spec.customEndSeconds = .5; spec.tailMaxSeconds = 0;
    spec.sourceTrackIds = {track};
    rendering::Report report; auto status = controller.renderProject(spec, {}, report);
    if (!status) std::fprintf(stderr, "Render error: %s\n", status.message().c_str());
    check(bool(status) && report.files.size() == 1, "render selected modulated track");
    ap::DecodedAudio audio; check(bool(ap::decodeAudioFile(report.files[0], audio)), "decode modulated render"); return audio;
}
static double difference(const ap::DecodedAudio& a, const ap::DecodedAudio& b) {
    if (a.interleaved.size() != b.interleaved.size()) return 100;
    double result = 0;
    for (std::size_t i = 0; i < a.interleaved.size(); ++i) result = std::max(result, std::abs(double(a.interleaved[i]) - b.interleaved[i]));
    return result;
}
static void checkExternal(const plugins::PluginDescriptor& descriptor, const std::string& parameter) {
    std::printf("Format: %s\n", plugins::toString(descriptor.format).data());
    EngineController c; check(bool(c.initialize(48000, 256, false)), "external modulation runtime");
    const auto channel = c.addTrack(TrackKind::Audio, "Native target");
    const auto slot = c.addInsert(channel, descriptor); check(!slot.empty(), "load external fixture");
    check(c.setInsertChannelMode(channel, slot, PluginChannelMode::DualMono), "external Dual Mono");
    c.setInsertParameter(channel, slot, parameter, .3);
    auto source = c.addRackModulator(channel, modulation::Kind::Lfo);
    auto rack = c.rackModulation(channel); rack.sources[0].wave = modulation::Wave::Square; rack.sources[0].smoothMs = 0;
    check(c.setRackModulation(channel, rack), "external source settings");
    check(!c.assignRackModulation(channel, source, slot, parameter).empty(), "assign external stable parameter ID");
    const auto undo = c.undoDepth(), revision = c.parameterTouchRevision(channel);
    audio::AudioBuffer in(2, 256), out(2, 256); c.seekSeconds(0); c.play();
    for (int i = 0; i < 16; ++i) check(c.processDeviceBlockForTest(in, out, 256), "external modulation block");
    c.pumpPluginEvents();
    unsigned modulated = 0;
    const auto graph = c.routingGraph();
    for (const auto& entry : graph->nodes) {
        const auto* node = dynamic_cast<const plugins::PluginNode*>(entry.node);
        if (!node || node->instance()->descriptor().uid != descriptor.uid) continue;
        const auto index = node->instance()->parameterIndexForId(parameter);
        const double base = node->modulation().base(index, -999);
        const double expected = node->instance()->parameterFromNormalized(index,
            std::clamp(node->instance()->parameterToNormalized(index, base) + .2, 0., 1.));
        check(near(node->modulation().effective(index, -999), expected, 1e-4), "external normalized movement");
        ++modulated;
    }
    check(modulated == 2, "both Dual Mono instances receive modulation");
    check(c.undoDepth() == undo && c.parameterTouchRevision(channel) == revision, "external feedback creates no touches or Undo");
    check(near(c.insertParameter(channel, slot, parameter), .3, 1e-4), "external UI reads base, not DSP result");
    const auto recovery = c.captureRecoverySnapshot();
    const auto& savedParameters = recovery.project.findTrack(channel)->inserts.front().parameters;
    const auto savedBase = std::find_if(savedParameters.begin(), savedParameters.end(), [&](const auto& p) { return p.id == parameter; });
    check(savedBase != savedParameters.end() && near(savedBase->value, .3, 1e-4) && savedBase->restoreAfterState,
        "recovery captures the base for restoration after opaque plugin state");
    Temp temp; const auto project = (temp.path / "native-modulation.vlt").string();
    check(bool(c.saveProject(project)), "save modulated native plugin");
    EngineController restored; check(bool(restored.initialize(48000, 256, false)) && bool(restored.openProject(project)), "reopen modulated native plugin");
    check(restored.rackModulation(channel) == c.rackModulation(channel) && near(restored.insertParameter(channel, slot, parameter), .3, 1e-4),
        "reopen restores assignments and base rather than captured modulation phase");
    const auto editRevision = c.parameterTouchRevision(channel);
    const auto* selectedInstance = c.insertInstance(channel, slot);
    bool gestureChecked = false;
    for (const auto& entry : c.routingGraph()->nodes) {
        auto* node = dynamic_cast<plugins::PluginNode*>(entry.node);
        if (!node || node->instance() != selectedInstance) continue;
        gestureChecked = true;
        const auto index = node->instance()->parameterIndexForId(parameter);
        node->onParameterGesture(index, true); c.pumpPluginEvents();
        check(c.lastTouchedAutomation(channel) && c.lastTouchedAutomation(channel)->parameterId == parameter && c.parameterTouchRevision(channel) == editRevision,
            "beginEdit selects ordinary automation without arming a modulation assignment");
        node->onParameterEdited(index, .35); c.pumpPluginEvents();
        check(c.parameterTouchRevision(channel) > editRevision && c.lastEditedPluginParameter(channel) && c.lastEditedPluginParameter(channel)->parameterId == parameter,
            "a fresh native value movement is available for modulation assignment");
        node->onParameterGesture(index, false); c.pumpPluginEvents();
        break;
    }
    check(gestureChecked, "native gesture target node");
    c.stop();
}

struct Block {
    std::array<float, 1024> left{}, right{}, out{};
    float* inPointers[2]{left.data(), right.data()}; float* outPointers[1]{out.data()};
    std::array<engine::AudioBlock, 1> inputs{engine::AudioBlock(inPointers, 2, 1024)};
    std::array<engine::InputRole, 1> roles{engine::InputRole::Modulation};
    engine::ProcessContext context;
    Block() {
        context.inputs = inputs; context.inputRoles = roles; context.output = engine::AudioBlock(outPointers, 1, 1024);
        context.frames = 1024; context.sampleRate = 48000; context.transport.tempo = 120;
    }
};
static std::vector<float> lfo(unsigned blockSize, modulation::Source source, bool playing = true) {
    engine::ModulationNode node("LFO"); auto settings = std::make_shared<engine::ModulationNode::Settings>();
    settings->source = source; settings->rate.fallback = source.sync ? 1 / source.beats : source.rate; node.setSettings(settings);
    std::vector<float> result; Block block; block.context.playing = playing; block.context.frames = blockSize;
    for (unsigned position = 0; position < 24000; position += blockSize) {
        block.context.frames = std::min(blockSize, 24000 - position);
        block.context.timelinePosition = position; block.context.transport.ppqPosition = playing ? position / 24000. : .25;
        node.process(block.context); result.insert(result.end(), block.out.begin(), block.out.begin() + block.context.frames);
    }
    return result;
}
int main() try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {
        Temp temp; writeConstant(temp.path / "layers.wav", .2f);
        EngineController c; check(bool(c.initialize(48000, 256, false)), "parallel rack runtime");
        const auto track = c.addTrack(TrackKind::Audio, "Parallel");
        check(!c.importAudio((temp.path / "layers.wav").string(), track, 0).empty(), "parallel input audio");
        const auto descriptor = plugins::equalizer::EqualizerInstance::staticDescriptor();
        const auto first = c.addInsert(track, descriptor), second = c.addInsert(track, descriptor);
        c.setInsertParameter(track, first, "output.gain", -6.);
        c.setInsertParameter(track, second, "output.gain", -6.);
        c.pumpPreviewPluginEvents();
        const auto level = [](const ap::DecodedAudio& audio) {
            double sum = 0;
            const auto count = std::min<std::size_t>(2048, audio.interleaved.size());
            for (auto i = audio.interleaved.size() - count; i < audio.interleaved.size(); ++i) sum += std::abs(audio.interleaved[i]);
            return sum / count;
        };
        const auto serial = level(render(c, temp.path / "serial", track, 64));
        std::string group;
        check(c.createRackGroup(track, {first, second}, "Layers", &group, true), "create two parallel branches");
        const auto original = c.rackGroups(track);
        const auto a = original.front().layers.front().id, b = original.front().layers.back().id;
        const auto parallel = level(render(c, temp.path / "parallel", track, 511));
        check(near(parallel / serial, 2. / std::pow(10., -6. / 20.), .002), "parallel processing splits input instead of chaining gains");
        check(c.setRackLayerState(track, group, a, 1.f, false, true), "solo first branch");
        const auto solo = render(c, temp.path / "solo", track, 64);
        check(near(level(solo), parallel / 2, 1e-5), "Solo isolates one branch without changing its level");
        c.undo(); check(!c.rackGroups(track).front().layers.front().solo, "Solo is undoable");
        c.redo(); check(c.rackGroups(track).front().layers.front().solo, "Solo redo");
        check(c.setRackLayerState(track, group, a, .5f, false, true), "live layer level");
        check(near(level(render(c, temp.path / "half", track, 64)), level(solo) * .5, 1e-5), "layer fader changes audio");
        audio::AudioBuffer input(2, 256), output(2, 256);
        c.seekSeconds(0); c.play();
        for (int i = 0; i < 40; ++i) c.processDeviceBlockForTest(input, output, 256);
        check(near(std::abs(output.getChannel(0)[255]), level(solo) * .5, 1e-5), "live playback matches the offline parallel mix");
        c.stop();
        std::string wrapper;
        check(c.createRackGroup(track, {first, second}, "Nested", &wrapper), "wrap parallel rack in a serial group");
        check(near(level(render(c, temp.path / "nested", track, 64)), level(solo) * .5, 1e-5), "nesting preserves parallel sound");
        const auto beforeDryExport = c.rackGroups(track);
        rendering::Spec dry; dry.outputDir = (temp.path / "dry").string(); dry.sourceTrackIds = {track};
        dry.range = rendering::Range::Custom; dry.customStartSeconds = 0; dry.customEndSeconds = .5;
        dry.tailMaxSeconds = 0; dry.bypassTrackInserts = true; dry.file.encoding = ap::Encoding::Float32;
        rendering::Report dryReport; ap::DecodedAudio dryAudio;
        check(bool(c.renderProject(dry, {}, dryReport)) && dryReport.files.size() == 1 && bool(ap::decodeAudioFile(dryReport.files.front(), dryAudio)), "export with the rack bypassed");
        check(near(level(dryAudio), .2, 1e-5) && c.rackGroups(track) == beforeDryExport, "dry export has one signal path and preserves live groups");
        const auto saved = (temp.path / "layers.vlt").string();
        check(bool(c.saveProject(saved)), "save nested parallel rack");
        EngineController reopened; check(bool(reopened.initialize(48000, 256, false)) && bool(reopened.openProject(saved)), "reopen parallel rack");
        check(reopened.rackGroups(track) == c.rackGroups(track) &&
              near(level(render(reopened, temp.path / "reopened-layers", track, 511)), level(solo) * .5, 1e-5), "reload preserves nested routing and mix");
        EngineController::ChannelSnapshot snapshot;
        check(bool(c.captureRackSelection(track, {first, second}, snapshot)), "copy nested layers");
        const auto destination = c.addTrack(TrackKind::Audio, "Copy");
        check(bool(c.pasteRackSelection(destination, snapshot, 0)), "paste nested layers");
        const auto copies = c.rackGroups(destination);
        check(copies.size() == 2 && copies.front().id != c.rackGroups(track).front().id, "copied containers have independent identities");
        for (const auto& g : copies) if (!g.layers.empty())
            check(g.layers.front().id != a && !g.parentId.empty(), "copied layer IDs and nesting remap together");
        check(c.addRackLayer(track, group), "add empty dry branch");
        auto groups = c.rackGroups(track); std::string empty;
        for (const auto& g : groups) if (g.id == group) empty = g.layers.back().id;
        check(!empty.empty() && bool(c.transferRackSelection(track, {first, second}, track, 0, false, empty)), "move all effects between branches without deleting the container");
        groups = c.rackGroups(track);
        check(groups.size() == 2, "moving all members retains enclosing groups");
        c.undo(); check(c.rackGroups(track).size() == 2, "Undo restores layer membership");
        c.seekSeconds(0); c.play();
        for (int i = 0; i < 40; ++i) c.processDeviceBlockForTest(input, output, 256);
        check(near(std::abs(output.getChannel(0)[255]), level(solo) * .5, 1e-5), "Undo publishes restored branch routing to live playback");
        c.stop();
        check(c.removeRackLayer(track, group, b), "delete layer with its effects");
        check(c.insertModel(track, second) == nullptr, "layer deletion removes owned effects");
        c.undo(); check(c.insertModel(track, second) != nullptr, "Undo restores a deleted layer and its devices");
    }

    modulation::Source source; source.id = newUuid(); source.smoothMs = 0;
    {
        using K = modulation::Kind;
        auto s = source;
        s.kind = K::Shaper;
        check(near(modulation::sourceValue(s, .25), .25), "Shaper interpolates breakpoints");
        s.points = {{0, 1, 0, engine::curve::Shape::Hold}, {.5, .2}, {1, .8}};
        check(near(modulation::sourceValue(s, .499), 1) && near(modulation::sourceValue(s, .5), .2), "Shaper hold has an exact segment boundary");
        for (unsigned preset = 0; preset < 5; ++preset) {
            modulation::shapePreset(s, preset);
            check(modulation::valid(s), "factory Shaper shape is valid");
            const auto a = lfo(64, s), b = lfo(511, s);
            check(a == b && std::all_of(a.begin(), a.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; }), "Shaper presets are bounded and block independent");
        }
        auto invalid = s; invalid.points[1].x = 0;
        check(!modulation::valid(invalid), "duplicate Shaper positions rejected");
        invalid = s; invalid.points.resize(65);
        check(!modulation::valid(invalid), "Shaper point limit enforced");
        s.kind = K::Steps; s.steps = {.1, .4, .9};
        check(near(modulation::sourceValue(s, 1.2), .4) && near(modulation::sourceValue(s, -1), .9), "Steps uses step duration and handles negative preroll");
        s.direction = modulation::Direction::Reverse;
        check(near(modulation::sourceValue(s, 0), .9) && near(modulation::sourceValue(s, 2), .1), "Steps reverse direction");
        s.direction = modulation::Direction::PingPong;
        check(near(modulation::sourceValue(s, 3), .4) && near(modulation::sourceValue(s, 4), .1), "Steps ping pong does not repeat endpoints");
        s.steps.resize(16); modulation::euclideanSteps(s, 5, 3);
        check(std::count(s.steps.begin(), s.steps.end(), 1.) == 5, "Euclidean pattern has exact pulse count");
        s.kind = K::Random; s.seed = 354123; s.randomLoop = 8;
        check(modulation::sourceValue(s, -1) == modulation::sourceValue(s, 7) && modulation::sourceValue(s, 1) == modulation::sourceValue(s, 9), "Random repeats its seed loop including preroll");
        const auto original = modulation::sourceValue(s, 2); ++s.seed;
        check(original != modulation::sourceValue(s, 2), "new seed changes Random pattern");
        s.randomSmooth = true;
        check(std::abs(modulation::sourceValue(s, 8 - 1e-6) - modulation::sourceValue(s, 8)) < 1e-6, "smooth Random is continuous across loop seam");
        for (const auto kind : {K::Shaper, K::Steps, K::Random}) for (const bool sync : {false, true}) {
            s.kind = kind; s.sync = sync; s.smoothMs = 9; s.beats = .25;
            const auto a = lfo(64, s), b = lfo(511, s);
            check(a == b, "new sources with smoothing are block independent in Hz and Sync");
            engine::ModulationNode node("new source"); auto settings = std::make_shared<engine::ModulationNode::Settings>();
            settings->source = s; settings->rate.fallback = 4; node.setSettings(settings); Block block;
            allocations = 0; countAllocations = true;
            for (unsigned i = 0; i < 100; ++i) node.process(block.context);
            countAllocations = false;
            check(allocations == 0, "new source processing allocates no memory");
            const auto loop = [&](unsigned size) {
                engine::ModulationNode looping("automated loop");
                auto automated = std::make_shared<engine::ModulationNode::Settings>(*settings);
                automated->rate.points = {{0, 48}, {.01, 96}, {.02, 160}}; automated->rate.prepare();
                looping.setSettings(automated);
                Block b; b.context.playing = true; b.context.transport.looping = true;
                b.context.transport.loopStartPpq = 0; b.context.transport.loopEndPpq = .02;
                std::vector<float> output;
                for (unsigned start = 0; start < 9600; start += size) {
                    b.context.timelinePosition = start % 480; b.context.transport.ppqPosition = (start % 480) / 24000.;
                    b.context.frames = std::min(size, 9600 - start); looping.process(b.context);
                    output.insert(output.end(), b.out.begin(), b.out.begin() + b.context.frames);
                }
                // Jump backwards after playback: the clock must derive from the timeline, including rate automation.
                b.context.transport.looping = false; b.context.timelinePosition = 240;
                b.context.transport.ppqPosition = .01; b.context.frames = 1; looping.process(b.context);
                check(near(b.out[0], modulation::sourceValue(s, automated->rate.integral(.01) * (sync ? 1 : .5) + s.phase)),
                    "new source seeks to the integrated automated rate");
                return output;
            };
            const auto loopA = loop(64), loopB = loop(511);
            check(std::equal(loopA.begin(), loopA.end(), loopB.begin(), [](float a, float b) { return near(a, b, 1e-6); }),
                "new source rate automation and in-block loop wrap are block independent");
        }
        nlohmann::json old = source;
        for (const auto* field : {"points", "steps", "direction", "seed", "randomLoop", "randomSmooth"}) old.erase(field);
        check(old.get<modulation::Source>() == source, "legacy sources load with compatible defaults");
        for (const auto kind : {K::Shaper, K::Steps, K::Random}) {
            s.kind = kind;
            check(nlohmann::json(s).get<modulation::Source>() == s, "new source settings survive JSON round trip");
        }
        const auto start = std::chrono::steady_clock::now();
        engine::ModulationNode node("benchmark"); auto settings = std::make_shared<engine::ModulationNode::Settings>();
        settings->source = s; settings->rate.fallback = 4; node.setSettings(settings); Block block;
        block.context.playing = true; block.context.frames = 256;
        for (unsigned i = 0; i < 10000; ++i) { block.context.timelinePosition = i * 256; block.context.transport.ppqPosition = i * 256 / 24000.; node.process(block.context); }
        std::printf("BENCH Random 2.56M samples: %.3f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    {
        modulation::Curve curve; curve.fallback = 2; curve.points = {{1, 3}, {2, 5}}; curve.prepare();
        check(near(curve.integral(.5), 1) && near(curve.integral(1), 2) && near(curve.integral(2), 6) && near(curve.integral(3), 11), "rate integral holds default before curve and integrates ramps");
        curve.points = {{-1, 1}, {1, 3}, {1, 5}, {2, 5}}; curve.prepare();
        check(near(curve.integral(0), 0) && near(curve.integral(1), 4) && near(curve.at(1), 5), "rate integration handles preroll and coincident points");
    }
    for (auto wave : {modulation::Wave::Sine, modulation::Wave::Triangle, modulation::Wave::Up, modulation::Wave::Down, modulation::Wave::Square}) {
        source.wave = wave;
        const auto a = lfo(64, source), b = lfo(511, source);
        check(a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float a, float b) { return near(a, b, 1e-6); }), "LFO waveform independent of block size");
        check(std::all_of(a.begin(), a.end(), [](float v) { return v >= 0 && v <= 1; }), "LFO bounded output");
    }
    source.wave = modulation::Wave::Sine;
    const auto held = lfo(64, source, false);
    check(std::all_of(held.begin(), held.end(), [](float v) { return near(v, 1.); }), "sync LFO holds cursor phase at Stop");
    source.sync = false; source.rate = 3;
    const auto free = lfo(64, source, false);
    check(!near(free[0], free[1000]), "Hz LFO continues at Stop");
    const auto hzA = lfo(64, source), hzB = lfo(511, source);
    check(hzA == hzB, "Hz LFO timeline phase is deterministic");
    source.sync = true; source.smoothMs = 8; source.wave = modulation::Wave::Square;
    const auto smoothA = lfo(64, source), smoothB = lfo(511, source);
    check(std::equal(smoothA.begin(), smoothA.end(), smoothB.begin(), [](float a, float b) { return near(a, b, 1e-6); }), "shape smoothing independent of block size");
    {
        const auto loop = [&](unsigned size) {
            engine::ModulationNode node("Loop"); auto settings = std::make_shared<engine::ModulationNode::Settings>();
            settings->source = source; settings->source.sync = false; settings->source.rate = 19; settings->source.phase = .49;
            settings->rate.fallback = 19; node.setSettings(settings);
            Block b; b.context.playing = true; b.context.transport.looping = true; b.context.transport.loopStartPpq = 0; b.context.transport.loopEndPpq = .02;
            std::vector<float> output;
            for (unsigned start = 0; start < 9600; start += size) {
                b.context.timelinePosition = start % 480; b.context.transport.ppqPosition = (start % 480) / 24000.;
                b.context.frames = std::min(size, 9600 - start); node.process(b.context);
                output.insert(output.end(), b.out.begin(), b.out.begin() + b.context.frames);
            }
            return output;
        };
        const auto a = loop(64), b = loop(511);
        check(std::equal(a.begin(), a.end(), b.begin(), [](float a, float b) { return near(a, b, 1e-6); }), "loop wrap inside a block keeps Hz phase and smoothing independent of block size");
    }
    {
        engine::ModulationNode node("Follower"); auto settings = std::make_shared<engine::ModulationNode::Settings>();
        settings->source = source; settings->source.kind = modulation::Kind::Follower; node.setSettings(settings);
        Block b; node.process(b.context); check(node.value() == 0, "follower silence");
        b.left.fill(1); b.right.fill(-1); b.context.frames = 480; node.process(b.context);
        check(near(node.value(), 1 - std::exp(-1.), 2e-5), "stereo antiphase preserves envelope; attack reaches 63% at 10 ms");
        b.left.fill(0); b.right.fill(0); const auto before = node.value(); node.process(b.context);
        check(near(node.value(), before * std::exp(-.01 / .15)), "release follows 150 ms time constant at Stop");
        settings = std::make_shared<engine::ModulationNode::Settings>(*settings); settings->source.gainDb = 6; node.setSettings(settings); node.reset();
        b.left.fill(.25); b.right.fill(-.25); node.process(b.context);
        check(near(node.value(), .25 * std::pow(10., .3) * (1 - std::exp(-1.))), "sensitivity gain precedes detector smoothing");
        settings->source.filter = true; node.setSettings(std::make_shared<engine::ModulationNode::Settings>(*settings)); node.reset();
        for (int i = 0; i < 200; ++i) node.process(b.context);
        check(node.value() < .001, "detector high-pass rejects DC without altering source samples");
        check(b.left[0] == .25f && b.right[0] == -.25f, "detector never modifies audio inputs");
        allocations = 0; countAllocations = true;
        for (int i = 0; i < 100; ++i) node.process(b.context);
        countAllocations = false; check(allocations == 0, "source processing allocates no heap memory");
    }
    {
        plugins::equalizer::EqualizerInstance plugin;
        const auto index = plugin.parameterIndexForId("output.gain"); check(index >= 0, "test parameter found");
        plugins::PluginModulation mod; mod.prepare(4096);
        plugins::PluginModulation::Route route; route.parameter = index; route.bipolar = false; route.depth = .2;
        mod.configure({route}, plugin); mod.edit(index, 0);
        Block b; b.left.fill(1); b.context.frames = 1024;
        std::vector<plugins::PluginEvent> events; events.reserve(4096);
        check(mod.apply(b.context, plugin, events), "modulation events fit prepared buffer");
        check(near(mod.base(index, -99), 0) && near(mod.effective(index, -99), plugin.parameterFromNormalized(index, plugin.parameterToNormalized(index, 0) + .2)), "base retained while effective value moves");
        route.depth = .8;
        auto reverse = route; reverse.depth = -.7;
        mod.configure({route, reverse}, plugin); events.clear(); mod.apply(b.context, plugin, events);
        check(near(mod.effective(index, -99), plugin.parameterFromNormalized(index, plugin.parameterToNormalized(index, 0) + .1)), "sources sum before clamping; negative depth reverses movement");
        mod.configure({}, plugin); events.clear(); mod.apply(b.context, plugin, events);
        check(mod.effective(index, -99) == 0 && !mod.mapped(index), "removing last route restores exact base");
        route.depth = .2;
        mod.configure({route}, plugin); events.clear();
        plugins::PluginEvent event; event.kind = plugins::PluginEvent::Kind::ParamValue; event.paramIndex = index; event.value = -6; event.frameOffset = 400; events.push_back(event);
        mod.apply(b.context, plugin, events);
        check(mod.base(index, -99) == -6 && std::any_of(events.begin(), events.end(), [](const auto& e) { return e.frameOffset == 400; }), "automation base and sample offset retained");
        route.automation.automated = true; route.automation.fallback = -.1;
        mod.configure({route}, plugin); events.clear(); mod.apply(b.context, plugin, events);
        check(near(mod.effective(index, -99), plugin.parameterFromNormalized(index, plugin.parameterToNormalized(index, 0) - .1)), "active constant depth automation overrides manual depth");
        allocations = 0; countAllocations = true;
        for (int i = 0; i < 100; ++i) { events.clear(); mod.apply(b.context, plugin, events); }
        countAllocations = false; check(allocations == 0, "parameter event composition allocates no heap memory");
        const auto frequency = plugin.parameterIndexForId("band.01.frequency");
        check(frequency >= 0 && near(plugin.parameterFromNormalized(frequency, .5), std::sqrt(plugin.parameters()[frequency].minValue * plugin.parameters()[frequency].maxValue), .01), "EQ frequency follows logarithmic knob scale");
        const auto type = plugin.parameterIndexForId("band.01.type");
        check(type >= 0 && plugin.parameterFromNormalized(type, .43) == 3., "discrete parameters snap to supported values");
    }
    {
        EngineController c; check(c.initialize(48000, 256, false).isOk(), "headless runtime");
        const auto a = c.addTrack(TrackKind::Audio, "Modulated"), b = c.addTrack(TrackKind::Audio, "Detector");
        const auto slot = c.addInsert(a, plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto lfoId = c.addRackModulator(a, modulation::Kind::Lfo); check(!lfoId.empty(), "create LFO");
        const auto undo = c.undoDepth();
        const auto routeId = c.assignRackModulation(a, lfoId, slot, "output.gain"); check(!routeId.empty(), "assign parameter");
        check(c.assignRackModulation(a, lfoId, slot, "output.gain") == routeId && c.undoDepth() == undo + 1, "duplicate assignment reuses route without Undo noise");
        c.undo(); check(c.rackModulation(a).routes.empty(), "undo assignment"); c.redo(); check(c.rackModulation(a).routes.size() == 1, "redo assignment");
        const auto detectorTrack = c.addTrack(TrackKind::Audio, "Independent detector");
        const auto follower = c.addRackModulator(a, modulation::Kind::Follower);
        auto rack = c.rackModulation(a); rack.sources.back().audioChannelId = detectorTrack;
        check(c.setRackModulation(a, rack), "external detector source");
        check(!c.assignRackModulation(a, follower, slot, "output.gain").empty(), "multiple sources on one target");
        const auto routingUndo = c.undoDepth();
        check(!c.setTrackOutputBus(a, detectorTrack) && c.project().findTrack(a)->outputBusId.empty() && c.undoDepth() == routingUndo, "later routing change cannot close a modulation feedback cycle");
        const auto masterSlot = c.addInsert("master", plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto masterSource = c.addRackModulator("master", modulation::Kind::Lfo);
        check(!c.assignRackModulation("master", masterSource, masterSlot, "output.gain").empty(), "master Rack supports modulation modules");
        std::string text; check(ProjectSerializer::serializeDocument(c.project(), text).isOk(), "serialize modulation");
        ProjectModel restored; check(ProjectSerializer::deserializeDocument(restored, text).isOk() && restored.findTrack(a)->modulation == c.rackModulation(a), "project round trip preserves all source/route settings");
        check(restored.masterModulation == c.rackModulation("master"), "project round trip preserves master modulation");
        auto target = AutomationTarget{AutomationTargetKind::ModulationDepth, a, routeId, "depth", {}};
        check(near(c.automationToPlain(target, .25), -.5) && near(c.plainToAutomation(target, .2), .6), "route depth automation has signed full range");
        const auto lane = c.ensureAutomation(target); check(!lane.first.empty(), "ordinary automation lane for route depth");
        AutomationPoint start; start.beats = 0; start.value = .6;
        AutomationPoint end; end.beats = 4; end.value = .8;
        c.setAutomationPoints(lane.first, lane.second, {start, end});
        EngineController::ChannelSnapshot selection; check(c.captureRackSelection(a, {slot}, selection).isOk(), "capture modulated device");
        std::vector<std::string> pasted; check(c.pasteRackSelection(b, selection, 0, {}, &pasted).isOk(), "paste device with required sources");
        check(c.rackModulation(b).sources.size() == 2 && c.rackModulation(b).routes.size() == 2 && c.rackModulation(b).sources[0].id != lfoId, "copied source and route identities are independent");
        check(c.rackModulation(b).routes[0].slotId == pasted[0], "copied target follows new device ID");
        const auto copiedAutomation = captureModulationAutomation(c.project(), b, c.rackModulation(b));
        check(copiedAutomation.size() == 1 && copiedAutomation[0].id != lane.first && copiedAutomation[0].clips[0].automation.target.slotId == c.rackModulation(b).routes[0].id && copiedAutomation[0].clips[0].automation.points.size() == 2, "copied depth automation follows fresh route ID and retains curve");
        const auto originalAutomation = captureModulationAutomation(c.project(), a, c.rackModulation(a));
        check(copiedAutomation[0].clips[0].contentId != originalAutomation[0].clips[0].contentId, "copied automation owns independent clip content");
        start.value = .1;
        c.setAutomationPoints(lane.first, lane.second, {start, end});
        check(near(captureModulationAutomation(c.project(), b, c.rackModulation(b))[0].clips[0].automation.points[0].value, .6), "editing original automation does not change copied modulation");
        auto preset = c.copyChannelStrip(a, true); const auto path = std::filesystem::temp_directory_path() / ("modulation-" + newUuid() + ".vlts");
        check(ChannelStripPreset::save(preset, path.string()).isOk(), "save chain preset"); EngineController::ChannelSnapshot loaded;
        check(ChannelStripPreset::load(loaded, path.string()).isOk() && loaded.modulation == preset.modulation && loaded.modulationAutomation.size() == 1 && loaded.modulationAutomation[0].clips[0].automation.points.size() == 2, "preset round trip includes modulation automation"); std::filesystem::remove(path);
        const auto duplicateUndo = c.undoDepth();
        const auto duplicate = c.duplicateRackModulator(a, lfoId);
        auto duplicatedAutomation = captureModulationAutomation(c.project(), a, c.rackModulation(a));
        check(!duplicate.empty() && c.rackModulation(a).sources.size() == 3 && duplicatedAutomation.size() == 2 && c.undoDepth() == duplicateUndo + 1,
            "duplicate module includes assignments and automation in one Undo");
        check(duplicatedAutomation[0].clips[0].contentId != duplicatedAutomation[1].clips[0].contentId, "duplicate module automation is independent");
        c.undo(); check(c.rackModulation(a).sources.size() == 2 && captureModulationAutomation(c.project(), a, c.rackModulation(a)).size() == 1, "Undo removes duplicated module and curves together");
        c.redo(); check(c.rackModulation(a).sources.size() == 3, "Redo restores duplicated module"); c.undo();
        const auto externalRoute = c.rackModulation(a).routes.back().id;
        c.removeTrack(detectorTrack);
        check(c.rackModulation(a).routes.size() == 2 && !c.rackModulationStatus(a, externalRoute).empty(), "missing external source keeps dormant assignment");
        c.undo(); check(c.rackModulationStatus(a, externalRoute).empty(), "Undo restores missing source assignment");
        const auto instrumentTrack = c.addTrack(TrackKind::Instrument, "Instrument");
        check(c.setTrackInstrumentPlugin(instrumentTrack, plugins::sampler::SamplerInstance::staticDescriptor()), "instrument for cycle test");
        const auto detector = c.addRackModulator(instrumentTrack, modulation::Kind::Follower);
        const auto instrument = c.project().findTrack(instrumentTrack)->instrument.id;
        const auto parameters = c.insertParameters(instrumentTrack, instrument);
        const auto p = std::find_if(parameters.begin(), parameters.end(), [](const auto& p) { return p.isAutomatable && !p.isReadOnly && p.minValue < p.maxValue; });
        check(p != parameters.end(), "instrument target metadata");
        const auto cycleUndo = c.undoDepth();
        check(c.assignRackModulation(instrumentTrack, detector, instrument, p->id).empty() && c.undoDepth() == cycleUndo && c.rackModulation(instrumentTrack).routes.empty(), "self-detector instrument cycle rejected without document or Undo changes");
    }
    {
        Temp temp; writeConstant(temp.path / "program.wav", .1f); writeConstant(temp.path / "driver.wav", .5f);
        EngineController c; check(bool(c.initialize(48000, 256, false)), "render modulation runtime");
        const auto a = c.addTrack(TrackKind::Audio, "Program"), b = c.addTrack(TrackKind::Audio, "Driver");
        check(!c.importAudio((temp.path / "program.wav").string(), a, 0).empty() && !c.importAudio((temp.path / "driver.wav").string(), b, 0).empty(), "import render sources");
        const auto slot = c.addInsert(a, plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto fx = c.addInsert(b, plugins::equalizer::EqualizerInstance::staticDescriptor());
        c.setInsertParameter(b, fx, "output.gain", -6); c.setTrackVolume(b, .5f);
        const auto follower = c.addRackModulator(a, modulation::Kind::Follower);
        auto rack = c.rackModulation(a); rack.sources[0].audioChannelId = b; rack.sources[0].attackMs = .1;
        check(c.setRackModulation(a, rack) && !c.assignRackModulation(a, follower, slot, "output.gain").empty(), "external follower render route");
        check(!c.freezeUnavailableReason(a).empty(), "independent Freeze rejects external follower");
        audio::AudioBuffer input(2, 256), output(2, 256);
        double values[3]{};
        for (int tap = 0; tap < 3; ++tap) {
            rack = c.rackModulation(a); rack.sources[0].tap = modulation::Tap(tap); check(c.setRackModulation(a, rack), "change detector tap");
            c.seekSeconds(0); c.play(); for (int i = 0; i < 40; ++i) c.processDeviceBlockForTest(input, output, 256);
            values[tap] = c.rackModulationValue(a, follower); c.stop();
        }
        check(near(values[0], .5, .001) && near(values[1], .5 * std::pow(10., -.3), .001) && near(values[2], values[1] * .5, .001), "before FX, after FX and after fader taps have correct levels");
        const auto undo = c.undoDepth();
        const auto small = render(c, temp.path / "small", a, 64), large = render(c, temp.path / "large", a, 511);
        check(difference(small, large) < 2e-5, "external follower export independent of audio block size");
        check(c.undoDepth() == undo && near(c.insertParameter(a, slot, "output.gain"), 0), "export preserves live base and Undo");
        const auto peak = *std::max_element(small.interleaved.begin(), small.interleaved.end());
        std::printf("Selected render peak: %.7f\n", peak);
        check(peak > .105f && peak < .2f, "external driver affects selected render without being mixed into it");
        const auto repeated = render(c, temp.path / "repeat", a, 64);
        check(difference(small, repeated) < 1e-7, "repeated export restores deterministic detector state");
        rack = c.rackModulation(a); rack.routes[0].enabled = false; check(c.setRackModulation(a, rack), "disable route smoothly");
        const auto dry = render(c, temp.path / "dry", a, 64);
        check(near(dry.interleaved[dry.interleaved.size()/2], .1, .001), "disabled route restores exact audible base without driver bleed");
        c.setTrackInputRouting(b, 0, 2, true); c.setTrackMonitor(b, true); c.stop();
        for (unsigned ch = 0; ch < 2; ++ch) std::fill_n(input.getChannel(ch), 256, .5f);
        for (int i = 0; i < 80; ++i) c.processDeviceBlockForTest(input, output, 256);
        check(c.rackModulationValue(a, follower) > .1, "Follower continues to detect live input at Stop");
        rack = c.rackModulation(a); rack.sources[0].audioChannelId.clear(); rack.routes[0].enabled = true;
        check(c.setRackModulation(a, rack) && c.freezeUnavailableReason(a).empty(), "own-input modulation can be frozen independently");
        const auto beforeFreeze = render(c, temp.path / "before-freeze", a, 64);
        c.setRecordDirectory(temp.path.string()); rendering::Report frozen;
        check(bool(c.freezeTrack(a, {}, frozen)) && c.isTrackFrozen(a), "freeze own-input modulation");
        const auto afterFreeze = render(c, temp.path / "after-freeze", a, 64);
        check(difference(beforeFreeze, afterFreeze) < 2e-5, "Freeze preserves modulated sound");
        c.unfreezeTrack(a);
        const auto depthLane = c.ensureAutomation({AutomationTargetKind::ModulationDepth, a, rack.routes[0].id, "depth", {}});
        AutomationPoint point; point.beats = 0; point.value = .6;
        c.setAutomationPoints(depthLane.first, depthLane.second, {point});
        check(!c.freezeUnavailableReason(a).empty(), "Freeze follows existing external parameter-automation restriction");
    }
    for (const auto kind : {modulation::Kind::Shaper, modulation::Kind::Steps, modulation::Kind::Random}) {
        std::printf("Integration: %s\n", modulation::kindName(kind));
        Temp temp; writeConstant(temp.path / "program.wav", .1f);
        EngineController c; check(bool(c.initialize(48000, 256, false)), "new source integration runtime");
        const auto track = c.addTrack(TrackKind::Audio, "New modulator");
        check(!c.importAudio((temp.path / "program.wav").string(), track, 0).empty(), "new source test audio");
        const auto slot = c.addInsert(track, plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto id = c.addRackModulator(track, kind);
        const auto route = c.assignRackModulation(track, id, slot, "output.gain");
        auto rack = c.rackModulation(track);
        check(!id.empty() && !route.empty() && rack.routes[0].depth == .2 &&
            rack.routes[0].bipolar == (kind != modulation::Kind::Shaper), "new source default routing depth and polarity");
        auto& s = rack.sources[0]; s.beats = .25; s.smoothMs = 4; s.phase = .1;
        modulation::shapePreset(s, 4); s.steps = {.1, .9, .3, .7, 1};
        s.direction = modulation::Direction::PingPong; s.seed = 0x13579bdf; s.randomLoop = 8; s.randomSmooth = true;
        check(c.setRackModulation(track, rack), "custom settings retain the assignment");
        const auto before = render(c, temp.path / "before", track, 64);
        check(difference(before, render(c, temp.path / "large", track, 511)) < 2e-5, "new source export is independent of block size");
        check(difference(before, render(c, temp.path / "repeat", track, 64)) < 1e-7, "new source repeated export is deterministic");
        check(c.freezeUnavailableReason(track).empty(), "new source allows local Freeze");
        c.setRecordDirectory(temp.path.string()); rendering::Report frozen;
        check(bool(c.freezeTrack(track, {}, frozen)) && c.isTrackFrozen(track), "freeze new source");
        check(difference(before, render(c, temp.path / "frozen", track, 64)) < 2e-5, "Freeze retains new source sound");
        c.unfreezeTrack(track);
        const AutomationTarget target{AutomationTargetKind::ModulatorRate, track, id, "rate", {}};
        const auto lane = c.ensureAutomation(target);
        check(!lane.first.empty() && near(c.automationToPlain(target, c.plainToAutomation(target, 12)), 12), "new source exposes rate automation");
        AutomationPoint start; start.beats = 0; start.value = c.plainToAutomation(target, 2);
        AutomationPoint end; end.beats = 1; end.value = c.plainToAutomation(target, 12);
        c.setAutomationPoints(lane.first, lane.second, {start, end});
        const auto automated = render(c, temp.path / "automated", track, 64);
        check(difference(automated, render(c, temp.path / "automated-large", track, 511)) < 2e-5 && difference(before, automated) > .001,
            "new source rate automation changes sound and preserves export parity");
        const auto saved = (temp.path / "new-modulator.vlt").string();
        check(bool(c.saveProject(saved)), "save new source project");
        EngineController reopened; check(bool(reopened.initialize(48000, 256, false)) && bool(reopened.openProject(saved)), "reopen new source project");
        check(reopened.rackModulation(track) == rack && difference(automated, render(reopened, temp.path / "reopened", track, 64)) < 1e-7,
            "project reload preserves source, routes, automation and audio");
        const auto preset = c.copyChannelStrip(track, true); EngineController::ChannelSnapshot loaded;
        const auto presetPath = (temp.path / "new-modulator.vlts").string();
        check(bool(ChannelStripPreset::save(preset, presetPath)) && bool(ChannelStripPreset::load(loaded, presetPath)) &&
            loaded.modulation == rack && loaded.modulationAutomation.size() == 1, "rack preset retains new source and rate automation");
        const auto copiedTrack = c.addTrack(TrackKind::Audio, "Pasted"); EngineController::ChannelSnapshot selection;
        check(bool(c.captureRackSelection(track, {slot}, selection)) && bool(c.pasteRackSelection(copiedTrack, selection, 0)), "copy device with new source");
        auto copied = c.rackModulation(copiedTrack).sources.front(); copied.id = id;
        check(copied == s && captureModulationAutomation(c.project(), copiedTrack, c.rackModulation(copiedTrack)).size() == 1,
            "device copy preserves seed, shape, steps and independent rate automation");
        const auto duplicate = c.duplicateRackModulator(track, id);
        auto duplicated = c.rackModulation(track).sources.back(); duplicated.id = id; duplicated.name = s.name;
        check(!duplicate.empty() && duplicated == s && captureModulationAutomation(c.project(), track, c.rackModulation(track)).size() == 2,
            "duplicate source preserves pattern, assignments and rate automation");
        c.undo(); check(c.rackModulation(track) == rack, "Undo removes duplicate new source in one action");
        c.redo(); check(c.rackModulation(track).sources.size() == 2, "Redo restores duplicate new source"); c.undo();
        const auto duplicateTrack = c.duplicateTrack(track);
        check(!duplicateTrack.empty() && c.rackModulation(duplicateTrack).sources.size() == 1, "track duplication includes new source");
        auto trackSource = c.rackModulation(duplicateTrack).sources.front(); trackSource.id = id;
        check(trackSource == s, "track duplication keeps the exact modulation pattern");
    }
#if DAW_ENABLE_CLAP && defined(DAW_TEST_CLAP_PATH)
    { plugins::ClapFactory factory; const auto descriptors = factory.inspect(DAW_TEST_CLAP_PATH); check(!descriptors.empty(), "CLAP fixture");
      auto instance = factory.create(descriptors.front()); checkExternal(descriptors.front(), instance->parameters().front().id); }
#endif
#if DAW_ENABLE_VST3 && defined(DAW_TEST_VST3_PATH)
    { plugins::Vst3Factory factory; const auto descriptors = factory.inspect(DAW_TEST_VST3_PATH); check(!descriptors.empty(), "VST3 fixture"); checkExternal(descriptors.front(), "100"); }
#endif
#if DAW_ENABLE_VST && defined(DAW_TEST_VST_SHELL_PATH)
    { plugins::VstFactory factory; const auto descriptors = factory.inspect(DAW_TEST_VST_SHELL_PATH);
      auto found = std::find_if(descriptors.begin(), descriptors.end(), [](const auto& d) { return !d.isInstrument; });
      check(found != descriptors.end(), "VST2 fixture"); checkExternal(*found, "0"); }
#endif
    return 0;
} catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
