#include "EngineController.hpp"
#include "MediaWorker.hpp"
#include "Host/PluginInstance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace {
using Controller = daw::EngineController;
int failures = 0;
bool check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
    return ok;
}
void installCatalog(Controller& controller, const daw::plugins::PluginDescriptor& descriptor) {
    auto catalog = controller.pluginManager().catalogSnapshot();
    daw::PluginCacheEntry entry;
    entry.format = descriptor.format; entry.path = descriptor.path;
    entry.ok = true; entry.plugins.push_back(descriptor);
    catalog.cache.put(std::move(entry));
    controller.pluginManager().restoreCatalog(std::move(catalog));
}
double render(Controller& controller) {
    audio::AudioBuffer input(2, 256), output(2, 256);
    input.clear(); output.clear();
    controller.seekSeconds(0);
    controller.play();
    double sum = 0;
    for (unsigned block = 0; block < 64; ++block) {
        if (!controller.processDeviceBlockForTest(input, output, 256)) return -1;
        for (unsigned frame = 0; frame < 256; ++frame)
            sum += std::abs(output.getChannel(0)[frame]);
    }
    controller.stop();
    return sum;
}
}

int main(int argc, char** argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    daw::MediaWorker::install(daw::PluginManager::helperPath("daw_worker"));
    const auto root = std::filesystem::temp_directory_path() /
        ("vlt-embedded-runtime-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    auto* factory = daw::plugins::factoryFor(daw::plugins::Format::Clap);
    const auto plugins = factory->inspect(DAW_TEST_CLAP_PATH);
    const auto found = std::find_if(plugins.begin(), plugins.end(),
        [](const auto& plugin) { return plugin.uid == "com.daw.test.gain"; });
    if (!check(found != plugins.end(), "real CLAP fixture resolves")) return 1;
    const auto descriptor = *found;
    Controller controller{};
    if (!check(bool(controller.initialize(48000, 256, false)), "desktop runtime initializes")) return 1;
    installCatalog(controller, descriptor);

    audio::AudioBuffer source(2, 48000);
    std::fill_n(source.getChannel(0), 48000, .01f);
    std::fill_n(source.getChannel(1), 48000, .01f);
    const auto wave = (root / "source.wav").string();
    if (!check(bool(audio::AudioRecorder::writeWAVFile(wave, source, 48000)), "source audio created")) return 1;
    std::vector<std::pair<std::string, std::string>> slots;
    for (unsigned i = 0; i < 12; ++i) {
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Track " + std::to_string(i));
        if (!check(!controller.importAudio(wave, track, 0).empty(), "audio imported")) return 1;
        const auto slot = controller.addInsert(track, descriptor);
        if (!check(!slot.empty(), "external plugin loads without a helper executable")) return 1;
        controller.setInsertParameter(track, slot, "0", .5 + .025 * i);
        slots.emplace_back(track, slot);
    }
    const double before = render(controller);
    check(before > 100, "new runtime renders the external plugin graph");
    const auto package = (root / "Embedded.vlt").string();
    if (!check(bool(controller.saveProject(package)), "project saves native plugin states")) return 1;
    controller.newProject();
    const auto started = std::chrono::steady_clock::now();
    if (!check(bool(controller.openProject(package)), "project reloads all external plugins")) return 1;
    std::printf("PROJECT_LOAD embedded plugins=%zu milliseconds=%.3f\n", slots.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    std::set<daw::plugins::PluginInstance*> instances;
    bool local = true, states = true;
    for (unsigned i = 0; i < slots.size(); ++i) {
        const auto& [track, slot] = slots[i];
        const auto editor = controller.insertEditorSnapshot(track, slot);
        local &= bool(editor);
        instances.insert(controller.insertInstance(track, slot));
        states &= std::abs(controller.insertParameter(track, slot, "0") - (.5 + .025 * i)) < 1e-6;
    }
    check(local && !instances.contains(nullptr) && instances.size() == slots.size(),
        "each slot has independent state in the application, with no plugin process");
    check(states, "all twelve parameter states survive project loading");
    check(std::abs(render(controller) - before) < .01, "reloaded project renders the same audio");

    const auto& [track, slot] = slots.front();
    const auto identity = controller.insertIdentity(track, slot);
    const auto depth = controller.undoDepth();
    controller.setInsertParameter(track, slot, "0", .9);
    controller.commitInsertParameterEdit(track, slot, "0", .5, "Gain");
    check(controller.undoDepth() == depth + 1, "queued CLAP edit commits before the next DSP block");
    controller.undo();
    controller.pumpPreviewPluginEvents();
    check(std::abs(controller.insertParameter(track, slot, "0") - .5) < 1e-6 &&
        controller.insertIdentity(track, slot) == identity, "parameter Undo preserves the loaded instance");
    controller.redo();
    controller.pumpPreviewPluginEvents();
    check(std::abs(controller.insertParameter(track, slot, "0") - .9) < 1e-6, "parameter Redo reaches DSP");

    std::shared_ptr<Controller> draft;
    check(bool(controller.createPluginBatchDraft({track, {}}, draft)) && draft &&
        draft->pluginManager().plugins().size() == controller.pluginManager().plugins().size(),
        "preview drafts inherit embedded hosting");
    if (draft) {
        check(bool(controller.startPluginAudition(draft)), "embedded preview uses the current runtime");
        controller.stopPluginAudition();
    }
    const auto audible = render(controller);
    // Retain a host edit that has not yet reached the next CLAP process call.
    controller.setInsertParameter(track, slot, "0", .8);
    check(bool(controller.setSampleRateHz(44100)) && bool(controller.setBufferSizeFrames(512)),
          "device-free session replaces its sample rate and block size");
    controller.pumpPreviewPluginEvents();
    check(std::abs(controller.insertParameter(track, slot, "0") - .8) < 1e-6,
          "format replacement preserves a queued host edit over the native checkpoint");
    for (unsigned i = 1; i < slots.size(); ++i)
        check(std::abs(controller.insertParameter(slots[i].first, slots[i].second, "0") - (.5 + .025 * i)) < 1e-6,
              "format replacement preserves each independent external plugin state");
    const auto currentIdentity = controller.insertIdentity(track, slot);
    check(!controller.setSampleRateHz(0) && controller.insertIdentity(track, slot) == currentIdentity,
          "rejected audio format leaves the running plugin generation intact");
    check(bool(controller.setSampleRateHz(48000)) && bool(controller.setBufferSizeFrames(256)),
          "original render format can be restored");
    controller.setInsertParameter(track, slot, "0", .9);
    check(std::abs(render(controller) - audible) < .01,
          "format round trip preserves the complete rendered signal");
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
