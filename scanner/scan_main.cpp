// daw_scan — opens one plugin, prints what it found, exits.
//
// Scanning means running third-party code that has every opportunity to crash,
// hang, or call exit(). Doing it in the DAW's own process means one bad plugin
// takes the session with it, and the user has no way back in. So each plugin is
// inspected by a separate short-lived process: a crash costs one fork, the
// parent needs no resynchronisation logic, and "which plugin was loaded when it
// died" answers itself.
//
// Usage:
//   daw_scan --protocol
//   daw_scan --list-paths --format=clap
//   daw_scan --enumerate  --format=clap --dir=<directory>
//   daw_scan --inspect    --format=clap --path=<bundle>
//   daw_scan --probe-crash | --probe-hang     (test harness only)
//
// The result is one line of JSON on the pipe the parent handed us as stdout;
// anything else means failure and the exit code says so.
//
// The catch: the payload channel cannot be left as *this process's* stdout,
// because the plugin gets loaded into this process too and plenty of them log
// to stdout without asking. Universal Audio's units print a page of it. So the
// first thing `main` does is move the pipe somewhere private and point stdout
// at stderr — after that a chatty plugin is merely noisy instead of corrupting
// the payload.

#include "Host/PluginInstance.hpp"
#include "Scan/ScanProtocol.hpp"
#include "platform/PathUtils.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace daw::plugins;

namespace {

std::string optionValue(const std::vector<std::string>& arguments,
                        const char* name) {
    const std::string prefix = std::string(name) + "=";
    for (const std::string& argument : arguments) {
        if (argument.rfind(prefix, 0) == 0) return argument.substr(prefix.size());
    }
    return {};
}

bool hasFlag(const std::vector<std::string>& arguments, const char* name) {
    return std::find(arguments.begin(), arguments.end(), name) !=
           arguments.end();
}

int fail(const char* message) {
    std::fprintf(stderr, "daw_scan: %s\n", message);
    return 2;
}

/// The parent's pipe, moved off stdout before any plugin code can run.
FILE* g_result = nullptr;

void claimResultChannel() {
#if defined(_WIN32)
    const int duplicate = ::_dup(::_fileno(stdout));
#else
    const int duplicate = ::dup(STDOUT_FILENO);
#endif
    if (duplicate < 0) {
        g_result = stdout;   // nothing better to do; behave as before
        return;
    }
    // Everything a plugin writes to stdout from here on lands on stderr, where
    // it is a diagnostic rather than a protocol violation.
#if defined(_WIN32)
    ::_dup2(::_fileno(stderr), ::_fileno(stdout));
    ::SetStdHandle(STD_OUTPUT_HANDLE, ::GetStdHandle(STD_ERROR_HANDLE));
    ::SetHandleInformation(reinterpret_cast<HANDLE>(::_get_osfhandle(duplicate)),
                           HANDLE_FLAG_INHERIT, 0);
    g_result = ::_fdopen(duplicate, "w");
#else
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    ::fcntl(duplicate, F_SETFD, FD_CLOEXEC);
    g_result = ::fdopen(duplicate, "w");
#endif
    if (!g_result) g_result = stdout;
}

void writeResult(const std::string& json) {
    std::fprintf(g_result ? g_result : stdout, "%s\n", json.c_str());
    std::fflush(g_result ? g_result : stdout);
}

int scannerMain(const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    // A bad plugin must fail silently inside this disposable process. Windows
    // Error Reporting can otherwise keep the scanner alive behind a crash UI,
    // making a crash look like a twenty-second plugin hang to the parent.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                   SEM_NOOPENFILEERRORBOX);
#endif
    // Before anything, and certainly before any plugin is loaded.
    claimResultChannel();

    // The host checks compatibility once before opening any plugins. Keep
    // this independent of format factories and third-party module loading.
    if (hasFlag(arguments, "--protocol")) {
        writeResult(scan::encodeHandshake());
        return 0;
    }

    // ── Harness probes ──
    //
    // The parent's crash and timeout handling is the part most likely to be
    // wrong, and the only honest way to test it is against a process that
    // really does crash or really does hang.
    if (hasFlag(arguments, "--probe-crash")) {
        std::fflush(stdout);
        int* nowhere = nullptr;
        *nowhere = 1;          // deliberate: the harness asserts on this
        return 0;
    }
    if (hasFlag(arguments, "--probe-hang")) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }

    const std::string formatName = optionValue(arguments, "--format");
    const Format format = formatFromString(formatName);
    if (format == Format::Unknown) return fail("missing or unknown --format");

    PluginFactory* factory = factoryFor(format);
    if (!factory) return fail("this build does not support that format");

    if (hasFlag(arguments, "--list-paths")) {
        nlohmann::json paths = nlohmann::json::array();
        for (const std::string& path : factory->defaultSearchPaths()) {
            paths.push_back(path);
        }
        writeResult(nlohmann::json{{"paths", paths}}.dump());
        return 0;
    }

    if (hasFlag(arguments, "--enumerate")) {
        const std::string directory = optionValue(arguments, "--dir");
        if (directory.empty()) return fail("--enumerate needs --dir");
        nlohmann::json candidates = nlohmann::json::array();
        for (const std::string& candidate : factory->enumerateCandidates(directory)) {
            candidates.push_back(candidate);
        }
        writeResult(nlohmann::json{{"candidates", candidates}}.dump());
        return 0;
    }

    if (hasFlag(arguments, "--inspect") || hasFlag(arguments, "--discover")) {
        const std::string path = optionValue(arguments, "--path");
        if (path.empty()) return fail("--inspect needs --path");
        // Everything past this line is the plugin's code. If it takes the
        // process down, that is precisely what this process exists to absorb.
        const std::vector<PluginDescriptor> plugins = hasFlag(arguments, "--discover")
            ? factory->discover(path) : factory->inspect(path);
        if (plugins.empty()) return fail("no plugins found in that module");
        writeResult(scan::encodeResult(plugins));
        return 0;
    }

    if (hasFlag(arguments, "--validate") || hasFlag(arguments, "--validate-descriptor")) {
        PluginDescriptor descriptor;
        const bool direct = hasFlag(arguments, "--validate-descriptor");
        if (direct) {
            if (hasFlag(arguments, "--shared-state"))
                return fail("shared-state requires fresh --validate metadata");
            std::string request;
            char buffer[4096];
            while (const auto count = std::fread(buffer, 1, sizeof(buffer), stdin)) {
                request.append(buffer, count);
                if (request.size() > 1024 * 1024) return fail("request exceeds 1 MiB");
            }
            if (std::ferror(stdin) || !scan::descriptorFromJson(request, descriptor) ||
                descriptor.format != format || descriptor.path.empty() ||
                !daw::platform::pathFromUtf8(descriptor.path).is_absolute())
                return fail("invalid validation descriptor");
        } else {
            const std::string path = optionValue(arguments, "--path");
            const std::string uid = optionValue(arguments, "--uid");
            if (path.empty() || uid.empty()) return fail("--validate needs --path and --uid");
            // Shared project probes must read current vendor/version metadata,
            // never trust a descriptor from another machine or an old cache.
            const auto described = factory->inspect(path);
            const auto found = std::find_if(described.begin(), described.end(),
                [&](const auto& item) { return item.uid == uid; });
            if (found == described.end()) return fail("plugin class disappeared during validation");
            descriptor = *found;
        }
        std::unique_ptr<PluginInstance> instance = factory->create(descriptor);
        if (!instance) return fail("plugin could not be initialized");

        const std::string statePath = optionValue(arguments, "--state");
        const bool sharedState = hasFlag(arguments, "--shared-state");
        if (!statePath.empty()) {
            const auto stateFile = daw::platform::pathFromUtf8(statePath);
            std::error_code error;
            const auto size = std::filesystem::file_size(stateFile, error);
            if (!stateFile.is_absolute() || error || size == 0 || size > 64u * 1024u * 1024u)
                return fail("plugin state asset is missing or exceeds 64 MiB");
            std::ifstream input(stateFile, std::ios::binary);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
            if (input.gcount() != std::streamsize(bytes.size()) || !instance->loadState(bytes))
                return fail("plugin could not restore the shared state");
        }
        if (sharedState) {
            if (!instance->supportsState()) return fail("plugin has no portable state interface");
            std::vector<std::pair<std::string, double>> values;
            for (const auto& parameter : instance->parameters())
                values.emplace_back(parameter.id, instance->parameterValue(parameter.index));
            std::vector<std::uint8_t> bytes;
            if (!instance->saveState(bytes) || bytes.empty() || bytes.size() > 64u * 1024u * 1024u || !instance->loadState(bytes))
                return fail("plugin could not round-trip shared state");
            for (const auto& [id, before] : values) {
                const auto index = instance->parameterIndexForId(id);
                if (index < 0) return fail("plugin parameter identity changed during state restore");
                const auto after = instance->parameterValue(std::uint32_t(index));
                if (!std::isfinite(before) || !std::isfinite(after) ||
                    std::abs(before - after) > 1e-7 * std::max(1.0, std::abs(before)))
                    return fail("plugin parameters did not survive state restore");
            }
        }

        PluginProcessInfo setup;
        setup.sampleRate = 48000.0;
        const std::string sampleRateText = optionValue(arguments, "--sample-rate");
        if (!sampleRateText.empty()) {
            try { setup.sampleRate = std::stod(sampleRateText); } catch (...) { return fail("invalid probe sample rate"); }
            if (!std::isfinite(setup.sampleRate) || setup.sampleRate < 8000 || setup.sampleRate > 768000)
                return fail("invalid probe sample rate");
        }
        setup.maxBlockSize = 64;
        if (!instance->activate(setup)) return fail("plugin refused activation");
        instance->startProcessing();
        if (!instance->isProcessing()) {
            instance->deactivate();
            return fail("plugin refused processing");
        }

        const PluginBusLayout layout = instance->busLayout();
        const std::uint16_t inputChannels = layout.inputs.empty() ? 0 : layout.inputs[0];
        const std::uint16_t outputChannels = layout.outputs.empty() ? 0 : layout.outputs[0];
        std::vector<float> inputStorage(std::size_t(inputChannels) * 64, 0.0f);
        std::vector<float> outputStorage(std::size_t(outputChannels) * 64, 0.0f);
        std::vector<const float*> inputs(inputChannels);
        std::vector<float*> outputs(outputChannels);
        for (std::uint16_t channel = 0; channel < inputChannels; ++channel) {
            inputs[channel] = inputStorage.data() + std::size_t(channel) * 64;
        }
        for (std::uint16_t channel = 0; channel < outputChannels; ++channel) {
            outputs[channel] = outputStorage.data() + std::size_t(channel) * 64;
        }
        PluginEvent note;
        note.kind = PluginEvent::Kind::NoteOn;
        note.key = 60;
        note.value = 0.5;
        PluginProcessContext context;
        context.inputs = inputs.empty() ? nullptr : inputs.data();
        context.inputChannels = inputChannels;
        context.outputs = outputs.empty() ? nullptr : outputs.data();
        context.outputChannels = outputChannels;
        context.frames = 64;
        context.inputEvents = instance->descriptor().isInstrument || instance->descriptor().wantsMidi
                                  ? std::span<const PluginEvent>(&note, 1)
                                  : std::span<const PluginEvent>{};
        instance->process(context);
        const bool nonFiniteAudio = sharedState && std::any_of(outputStorage.begin(), outputStorage.end(),
                [](float sample) { return !std::isfinite(sample); });
        instance->stopProcessing();
        instance->deactivate();
        if (nonFiniteAudio) return fail("plugin produced non-finite audio");
        // Bundle metadata cannot tell whether a VST3/CLAP controller can
        // actually create a platform view. Validation has a live instance, so
        // persist the real answer instead of leaving every descriptor at the
        // inspect-time default (`false`).
        PluginDescriptor validated = descriptor;
        // Legacy shells only knew UID/name until create(). AU fallback keeps
        // its original AU identity and path even when a VST3 instance runs it.
        if (descriptor.format == Format::Vst) validated = instance->descriptor();
        validated.format = descriptor.format;
        validated.uid = descriptor.uid;
        validated.path = descriptor.path;
        validated.isInstrument = instance->descriptor().isInstrument;
        validated.wantsMidi = instance->descriptor().wantsMidi;
        validated.producesMidi = instance->descriptor().producesMidi;
        validated.hasEditor = instance->hasEditor();
        validated.mainInputChannels = inputChannels;
        validated.mainOutputChannels = outputChannels;
        nlohmann::json parameters = nlohmann::json::array();
        std::vector<ParameterInfo> sorted(instance->parameters().begin(), instance->parameters().end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (const auto& parameter : sorted) {
            parameters.push_back({{"id", parameter.id}, {"unit", parameter.unit},
                {"minimum", parameter.minValue}, {"maximum", parameter.maxValue},
                {"default", parameter.defaultValue}, {"stepped", parameter.isStepped},
                {"automatable", parameter.isAutomatable}, {"bypass", parameter.isBypass}});
        }
        validated.parameterSchema = nlohmann::json{{"version", 2},
            {"parameters", parameters}, {"inputs", layout.inputs}, {"outputs", layout.outputs},
            {"wantsMidi", validated.wantsMidi}, {"producesMidi", validated.producesMidi}}.dump();
        writeResult(scan::encodeResult({validated}));
        return 0;
    }

    return fail("expected --list-paths, --enumerate, --inspect, --discover, --validate or --validate-descriptor");
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? std::size_t(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        arguments.push_back(daw::platform::pathToUtf8(
            std::filesystem::path(std::wstring(argv[i]))));
    }
    return scannerMain(arguments);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? std::size_t(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    return scannerMain(arguments);
}
#endif
