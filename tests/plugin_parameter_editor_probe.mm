// Manual diagnostic: a native plugin editor in a real controller/insert chain.
// Loads installed plugins, emits only silence, and never saves a project.
#include "EngineController.hpp"
#import <AppKit/AppKit.h>
#include <cstdio>

class ParameterProbeHost final : public daw::plugins::PluginEditorHost {
public:
    NSWindow* window = nil;
    void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept override {
        [window setContentSize:NSMakeSize(width, height)];
    }
    void onEditorClosed() noexcept override { [window close]; }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fputs("usage: plugin_parameter_editor_probe <au|vst3> <name> [--chain]\n", stderr);
        return 2;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        daw::EngineController controller;
        if (!controller.initialize(48000, 32, true)) return 1;
        controller.pluginManager().load();
        const auto format = daw::plugins::formatFromString(argv[1]);
        const auto add = [&](const std::string& track, const std::string& name) {
            for (const auto& descriptor : controller.pluginManager().plugins()) {
                if (descriptor.format != format || descriptor.name.find(name) == std::string::npos)
                    continue;
                std::printf("Adding %s (%s)\n", descriptor.name.c_str(), argv[1]);
                return controller.addInsert(track, descriptor);
            }
            return std::string();
        };
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Parameter probe");
        if (argc > 3) { add(track, "Pro-Q 3"); add(track, "Pro-C 2"); }
        const auto slot = add(track, argv[2]);
        auto* plugin = controller.insertInstance(track, slot);
        if (!plugin) return 1;
        ParameterProbeHost host;
        host.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1100, 600)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        host.window.title = @"VLT Parameter Probe";
        host.window.releasedWhenClosed = NO;
        if (!plugin->openEditor((__bridge void*)host.window.contentView, &host)) return 1;
        std::uint32_t width = 0, height = 0;
        if (plugin->editorSize(width, height)) host.onEditorResized(width, height);
        std::vector<double> values;
        for (const auto& p : plugin->parameters()) {
            values.push_back(plugin->parameterValue(p.index));
            std::printf("PARAM %u %s = %.9g [%g,%g]\n", p.index, p.name.c_str(),
                values.back(), p.minValue, p.maxValue);
        }
        [host.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp finishLaunching];
        const auto deadline = [NSDate dateWithTimeIntervalSinceNow:300];
        while (host.window.visible && deadline.timeIntervalSinceNow > 0) {
            @autoreleasepool {
                NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                    untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                    inMode:NSDefaultRunLoopMode dequeue:YES];
                if (event) [NSApp sendEvent:event];
                if (controller.pumpPluginEvents()) std::puts("HOST notifications drained");
                for (const auto& p : plugin->parameters()) {
                    const double now = plugin->parameterValue(p.index);
                    if (now == values[p.index]) continue;
                    std::printf("CHANGE %s %.9g -> %.9g\n", p.name.c_str(), values[p.index], now);
                    values[p.index] = now;
                }
            }
        }
        plugin->closeEditor();
        [host.window close];
    }
}
