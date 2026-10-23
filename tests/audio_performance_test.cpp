#include "EngineController.hpp"
#include "Graph/GraphProcessor.hpp"
#include <cstdio>
#include <cmath>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
daw::AudioPerformanceRow rowFor(const daw::EngineController& c, const std::string& slot) {
    for (const auto& row : c.audioPerformanceSnapshot().rows) if (row.address.slotId == slot) return row;
    return {};
}
}
int main() {
    using namespace daw;
    EngineController c;
    check(bool(c.initialize(48000, 64, false)), "prepare controller without hardware");
    const auto descriptors = plugins::factoryFor(plugins::Format::Clap)->inspect(DAW_TEST_RECOVERY_CLAP_PATH);
    if (descriptors.empty()) return 1;
    auto catalog = c.pluginManager().catalogSnapshot();
    PluginCacheEntry cache; cache.format = descriptors[0].format; cache.path = descriptors[0].path;
    cache.ok = true; cache.plugins = descriptors;
    catalog.cache.put(std::move(cache)); c.pluginManager().restoreCatalog(std::move(catalog));
    const auto track = c.addTrack(TrackKind::Audio, "Serial chain");
    const auto parallel = c.addTrack(TrackKind::Audio, "Parallel chain");
    const auto first = c.addInsert(track, descriptors[0]);
    const auto second = c.addInsert(track, descriptors[0]);
    const auto other = c.addInsert(parallel, descriptors[0]);
    check(!first.empty() && !second.empty() && !other.empty(), "create measured native plugins");
    audio::AudioBuffer in(2, 64), out(2, 64);
    const auto render = [&](unsigned count) { for (unsigned i = 0; i < count; ++i) c.processDeviceBlockForTest(in, out, 64); };
    render(8);
    auto a = rowFor(c, first), b = rowFor(c, second);
    check(a.timing.calls >= 8 && a.timing.totalNs > 0 && a.timing.budgetNs > 0 && !a.timing.activeSinceNs,
          "plugin timing remains available with diagnostics closed");
    check(a.latencySamples == 32 && b.latencySamples == 32 && b.pathLatencySamples == 64 &&
          c.audioPerformanceSnapshot().totalLatencySamples == 64,
          "parallel path delays use the maximum, serial plugin delays add");
    const auto id = a.id;
    c.addTrack(TrackKind::Audio, "Unrelated edit");
    check(rowFor(c, first).id == id, "measurement identity survives a graph rebuild");
    c.setAudioDiagnosticsEnabled(true); render(8);
    bool engineMeasured = false;
    for (const auto& row : c.audioPerformanceSnapshot().rows)
        if (row.kind == AudioPerformanceRow::Kind::Processing && row.timing.calls > 0) engineMeasured = true;
    check(engineMeasured, "open monitor includes engine work in channel totals");
    const auto parameter = c.insertParameters(track, first).front().id;
    c.setInsertParameter(track, first, parameter, 1.25);
    c.setInsertBypassed(track, first, true);
    const auto identity = c.insertIdentity(track, first);
    const auto undoDepth = c.undoDepth();
    const auto address = rowFor(c, first).address;
    check(!c.restartInsert(address, identity.project + 1), "stale project cannot restart a plugin");
    check(bool(c.restartInsert(address, identity.project)), "restart healthy plugin with saved and pending settings");
    render(4);
    check(c.insertIdentity(track, first).instance != identity.instance &&
          std::abs(c.insertParameter(track, first, parameter) - 1.25) < 1e-6 && rowFor(c, first).bypassed &&
          c.undoDepth() == undoDepth, "restart preserves parameters, bypass and undo history");
    check(!c.restartInsert(address, identity.project), "stale instance cannot restart its replacement");
    check(rowFor(c, first).latencySamples == 32 && c.audioPerformanceSnapshot().totalLatencySamples == 64,
          "bypass retains reported latency compensation");
    // Hold an actual node call on another thread: the observer must remain nonblocking.
    struct HeldNode final : engine::Node {
        std::atomic<bool> entered{false}, release{false};
        std::string_view name() const noexcept override { return "Held processor"; }
        bool alwaysMeasureProcessing() const noexcept override { return true; }
        void process(const engine::ProcessContext& context) override {
            entered = true; entered.notify_all();
            release.wait(false);
            for (unsigned ch = 0; ch < context.output.numChannels(); ++ch)
                std::fill(context.output.channel(ch).begin(), context.output.channel(ch).end(), 0.f);
        }
    };
    engine::AudioGraph graph;
    auto held = std::make_shared<HeldNode>();
    graph.setSink(graph.adoptNode(held));
    const auto compiled = graph.compile({48000, 64, 2});
    check(bool(compiled), "compile held diagnostic fixture");
    if (compiled) {
        engine::GraphProcessor processor(1); processor.setGraph(*compiled);
        float left[64]{}, right[64]{}; float* channels[]{left, right};
        engine::AudioBlock output{channels, 2, 64};
        std::thread renderer([&] { processor.process(output, 64, 0, false); });
        held->entered.wait(false);
        const auto timing = held->diagnostics.snapshot();
        check(timing.activeSinceNs != 0 && processingUnresponsive(timing, timing.activeSinceNs + 2'000'000'001ull),
              "in-progress processor is observable without waiting on its call");
        held->release = true; held->release.notify_all(); renderer.join();
        check(!held->diagnostics.snapshot().activeSinceNs && held->diagnostics.snapshot().calls == 1,
              "returned call clears the unresponsive marker");
        processor.process(output, 64, 0, false, true);
        check(held->diagnostics.snapshot().calls == 1, "offline rendering does not contaminate live measurements");
    }
    check(c.removeRackSelection(track, {first}), "plugin removal is available through existing undoable command");
    check(rowFor(c, first).address.instance == 0, "removed plugin disappears from the snapshot");
    c.undo(); render(4);
    check(std::abs(c.insertParameter(track, first, parameter) - 1.25) < 1e-6,
          "undo removal restores plugin settings");
    return failures ? 1 : 0;
}
