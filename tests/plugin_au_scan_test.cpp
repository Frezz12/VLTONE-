#include "Au/AuFactory.hpp"
#include "Scan/ScanProtocol.hpp"
#include "plugins/ScanProcess.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <cstdio>

int main() {
    using namespace daw;
    using namespace daw::plugins;
    const std::string path = "/System/Library/Components/CoreAudio.component";
    const auto discovery = ScanProcess::run(DAW_SCAN_PATH,
        {"--discover", "--format=au", "--path=" + path}, std::chrono::seconds(30));
    std::vector<PluginDescriptor> descriptors;
    if (!discovery.succeeded() || !scan::decodeResult(discovery.output, descriptors) || descriptors.empty()) {
        std::fprintf(stderr, "AU plist discovery failed: %s\n%s\n",
                     discovery.failureReason.c_str(), discovery.diagnostics.c_str());
        return 1;
    }
    unsigned checked = 0;
    for (const auto& descriptor : descriptors) {
        if (descriptor.name.find("NBandEQ") == std::string::npos &&
            descriptor.name.find("MatrixReverb") == std::string::npos) continue;
        AudioComponentDescription wanted{};
        if (!au::identityFromString(descriptor.uid, wanted.componentType,
            wanted.componentSubType, wanted.componentManufacturer)) return 1;
        if (!AudioComponentFindNext(nullptr, &wanted)) {
            std::fprintf(stderr, "SKIP: system AU registry unavailable; AU validation is NOT verified.\n");
            return 77;
        }
        const auto validation = ScanProcess::run(DAW_SCAN_PATH,
            {"--validate-descriptor", "--format=au"}, std::chrono::seconds(30),
            {scan::descriptorToJson(descriptor)});
        std::vector<PluginDescriptor> result;
        if (!validation.succeeded() || !scan::decodeResult(validation.output, result) ||
            result.size() != 1 || result[0].format != Format::AudioUnit ||
            result[0].uid != descriptor.uid || result[0].path != path ||
            result[0].parameterSchema.empty()) {
            std::fprintf(stderr, "AU validation failed: %s\n%s\n",
                         validation.failureReason.c_str(), validation.diagnostics.c_str());
            return 1;
        }
        ++checked;
    }
    if (!checked) { std::fprintf(stderr, "No expected system AU components found\n"); return 1; }
    std::printf("Validated %u real system AUs through the descriptor protocol\n", checked);
    return 0;
}
