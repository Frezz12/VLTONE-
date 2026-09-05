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
#include <string>
#include <thread>
#include <algorithm>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
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
    g_result = ::_fdopen(duplicate, "w");
#else
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
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

    if (hasFlag(arguments, "--inspect")) {
        const std::string path = optionValue(arguments, "--path");
        if (path.empty()) return fail("--inspect needs --path");
        // Everything past this line is the plugin's code. If it takes the
        // process down, that is precisely what this process exists to absorb.
        const std::vector<PluginDescriptor> plugins = factory->inspect(path);
        if (plugins.empty()) return fail("no plugins found in that module");
        writeResult(scan::encodeResult(plugins));
        return 0;
    }

    if (hasFlag(arguments, "--validate")) {
        const std::string path = optionValue(arguments, "--path");
        const std::string uid = optionValue(arguments, "--uid");
        if (path.empty() || uid.empty()) return fail("--validate needs --path and --uid");
        const std::vector<PluginDescriptor> described = factory->inspect(path);
        const auto found = std::find_if(described.begin(), described.end(),
                                        [&](const PluginDescriptor& descriptor) {
                                            return descriptor.uid == uid;
                                        });
        if (found == described.end()) return fail("plugin class disappeared during validation");
        std::unique_ptr<PluginInstance> instance = factory->create(*found);
        if (!instance) return fail("plugin could not be initialized");

        PluginProcessInfo setup;
        setup.sampleRate = 48000.0;
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
        context.inputEvents = found->isInstrument || found->wantsMidi
                                  ? std::span<const PluginEvent>(&note, 1)
                                  : std::span<const PluginEvent>{};
        instance->process(context);
        instance->stopProcessing();
        instance->deactivate();
        // Bundle metadata cannot tell whether a VST3/CLAP controller can
        // actually create a platform view. Validation has a live instance, so
        // persist the real answer instead of leaving every descriptor at the
        // inspect-time default (`false`).
        PluginDescriptor validated = *found;
        validated.hasEditor = instance->hasEditor();
        validated.mainInputChannels = inputChannels;
        validated.mainOutputChannels = outputChannels;
        writeResult(scan::encodeResult({validated}));
        return 0;
    }

    return fail("nothing to do: expected --list-paths, --enumerate, --inspect or --validate");
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
