// A hand-run diagnostic for the *slot* path: what a real plugin does once it is
// in a real project, loaded exactly the way the mixer loads it.
//
// Not a ctest target — like `plugin_probe`, it needs the plugins installed on
// the machine it runs on. It answers the questions a unit test with a fixture
// cannot: does the slot end up with a live instance, does the node consider
// itself ready, does audio actually change on its way through, and does all of
// that survive a Replace.
//
//   plugin_slot_probe <name fragment> [second name fragment]
#include "EngineController.hpp"
#include "Host/PluginInstance.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace plugins = daw::plugins;

namespace {

/// "Effectrix" matches any format; "au:Effectrix" pins one. A plugin installed
/// in two formats is the normal case, and which one gets loaded is exactly the
/// kind of thing being investigated here.
const plugins::PluginDescriptor* findByName(
    const std::vector<plugins::PluginDescriptor>& all, const std::string& query) {
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    };
    plugins::Format format = plugins::Format::Unknown;
    std::string fragment = query;
    if (const std::size_t colon = query.find(':'); colon != std::string::npos) {
        format = plugins::formatFromString(query.substr(0, colon));
        fragment = query.substr(colon + 1);
    }
    const std::string wanted = lower(fragment);
    // An exact uid wins over a name match, so a sweep can address one plugin of
    // the several a shell advertises under similar names.
    for (const plugins::PluginDescriptor& d : all) {
        if (format != plugins::Format::Unknown && d.format != format) continue;
        if (d.uid == fragment) return &d;
    }
    for (const plugins::PluginDescriptor& d : all) {
        if (format != plugins::Format::Unknown && d.format != format) continue;
        if (lower(d.name).find(wanted) != std::string::npos) return &d;
    }
    return nullptr;
}

void report(daw::EngineController& controller, const std::string& track,
            const std::string& slot, const char* stage) {
    plugins::PluginInstance* instance = controller.insertInstance(track, slot);
    std::printf("  %-18s instance %-3s", stage, instance ? "yes" : "NO");
    // The document's uid and the instance's must agree: the editor window is
    // kept alive by matching one against the other, so a plugin whose instance
    // renames its own identity gets its window closed the moment it opens.
    if (const std::vector<daw::InsertModel>* slots = controller.channelInserts(track)) {
        for (const daw::InsertModel& model : *slots) {
            if (model.id != slot || !instance) continue;
            if (model.uid != instance->descriptor().uid)
                std::printf("  UID MISMATCH doc='%s' instance='%s'",
                            model.uid.c_str(), instance->descriptor().uid.c_str());
        }
    }
    if (instance) {
        std::printf("  name '%s'  active %s  processing %s  editor %s  params %zu",
                    instance->descriptor().name.c_str(),
                    instance->isActive() ? "yes" : "NO",
                    instance->isProcessing() ? "yes" : "NO",
                    instance->hasEditor() ? "yes" : "no",
                    instance->parameters().size());
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "usage: plugin_slot_probe <name> [replacement name]\n");
        return 2;
    }

    daw::EngineController controller;
    if (!controller.initialize(48000, 512, /*openDevice=*/false).isOk()) {
        std::fprintf(stderr, "the controller did not initialise\n");
        return 1;
    }
    controller.pluginManager().load();   // the user's own scan cache
    const std::vector<plugins::PluginDescriptor>& all =
        controller.pluginManager().plugins();
    std::printf("cache holds %zu plugins\n", all.size());

    const plugins::PluginDescriptor* first = findByName(all, argv[1]);
    if (!first) {
        std::fprintf(stderr, "nothing in the cache matches '%s'\n", argv[1]);
        return 1;
    }
    std::printf("first  : %s (%s, %s)\n", first->name.c_str(),
                std::string(plugins::toString(first->format)).c_str(),
                first->uid.c_str());

    const std::string track = controller.addTrack(daw::TrackKind::Audio, "Probe");
    const std::string slot = controller.addInsert(track, *first);
    if (slot.empty()) {
        std::fprintf(stderr, "addInsert refused the plugin\n");
        return 1;
    }
    report(controller, track, slot, "after add");

    if (argc >= 3) {
        const plugins::PluginDescriptor* second = findByName(all, argv[2]);
        if (!second) {
            std::fprintf(stderr, "nothing in the cache matches '%s'\n", argv[2]);
            return 1;
        }
        std::printf("second : %s (%s, %s)\n", second->name.c_str(),
                    std::string(plugins::toString(second->format)).c_str(),
                    second->uid.c_str());
        controller.replaceInsert(track, slot, *second);
        report(controller, track, slot, "after replace");
        controller.undo();
        report(controller, track, slot, "after undo");
        controller.redo();
        report(controller, track, slot, "after redo");
    }
    return 0;
}
