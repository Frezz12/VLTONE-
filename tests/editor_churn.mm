// Open and close every scanned plugin's editor, repeatedly, in a real window.
// Under Guard Malloc an out-of-bounds access in the host's editor plumbing
// faults on the instruction that does it.
#include "Host/PluginInstance.hpp"

#import <AppKit/AppKit.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace plugins = daw::plugins;

namespace {
class Listener final : public plugins::PluginListener {
public:
    void onParameterChanged(std::uint32_t, double) noexcept override {}
    void onParameterGesture(std::uint32_t, bool) noexcept override {}
    void onLatencyChanged() noexcept override {}
    void onRestartRequested() noexcept override {}
    void onReloadRequested() noexcept override {}
};
class Host final : public plugins::PluginEditorHost {
public:
    void onEditorResized(std::uint32_t, std::uint32_t) noexcept override {}
    void onEditorClosed() noexcept override {}
};
void pump(double seconds) {
    const double until = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < until) {
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (e) [NSApp sendEvent:e];
    }
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int cycles = 3;
    std::string only;
    bool vst3Only = false, auOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--vst3")) vst3Only = true;
        else if (!std::strcmp(argv[i], "--au")) auOnly = true;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        struct Target { plugins::Format format; std::string path; };
        std::vector<Target> targets;
        if (!vst3Only) for (const char* dir : {"/Library/Audio/Plug-Ins/Components"}) {
            NSArray* files = [[NSFileManager defaultManager]
                contentsOfDirectoryAtPath:@(dir) error:nil];
            for (NSString* f in files) {
                if (![f hasSuffix:@".component"]) continue;
                targets.push_back({plugins::Format::AudioUnit,
                                   std::string(dir) + "/" + f.UTF8String});
            }
        }
        if (!auOnly) for (const char* dir : {"/Library/Audio/Plug-Ins/VST3"}) {
            NSArray* files = [[NSFileManager defaultManager]
                contentsOfDirectoryAtPath:@(dir) error:nil];
            for (NSString* f in files) {
                if (![f hasSuffix:@".vst3"]) continue;
                targets.push_back({plugins::Format::Vst3,
                                   std::string(dir) + "/" + f.UTF8String});
            }
        }
        if (!only.empty()) {
            std::erase_if(targets, [&](const Target& t) {
                return t.path.find(only) == std::string::npos;
            });
        }
        std::printf("── %zu plugins, %d open/close cycles each ──\n", targets.size(), cycles);

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, 1000, 700)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];

        int opened = 0, failed = 0, skipped = 0;
        for (std::size_t t = 0; t < targets.size(); ++t) {
            const Target& target = targets[t];
            auto* factory = plugins::factoryFor(target.format);
            if (!factory) { ++skipped; continue; }
            std::vector<plugins::PluginDescriptor> found;
            found = factory->inspect(target.path);
            if (found.empty()) { ++skipped; continue; }
            std::printf("[%3zu/%zu] %-46s ", t + 1, targets.size(),
                        found.front().name.substr(0, 45).c_str());
            auto plugin = factory->create(found.front());
            if (!plugin) { std::printf("create failed\n"); ++failed; continue; }
            Listener listener;
            plugin->setListener(&listener);
            plugins::PluginProcessInfo info;
            info.sampleRate = 48000.0;
            info.maxBlockSize = 512;
            plugin->activate(info);

            Host host;
            int ok = 0;
            for (int c = 0; c < cycles; ++c) {
                // A pool per cycle: without it every closed view stays alive
                // until the run ends, which is the harness's problem and not
                // the host's. Draining here makes the plugin's own dealloc run
                // while its AudioUnit is still there to be told about it.
                if (plugin->hasEditor() &&
                    plugin->openEditor((__bridge void*)window.contentView, &host)) {
                    ++ok;
                    pump(0.12);
                    plugin->closeEditor();
                    pump(0.04);
                }
                plugin->pumpMainThread();
            }
            // And one more turn of the loop after the last close, so a view
            // released above gets its dealloc before the unit is disposed.
            pump(0.08);
            plugin->deactivate();
            std::printf("%d/%d opens\n", ok, cycles);
            opened += ok;
        }
        std::printf("\n── %d editor opens, %d failed to create, %d skipped ──\n",
                    opened, failed, skipped);
    }
    return 0;
}
