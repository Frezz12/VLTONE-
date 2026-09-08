// Manual local-plugin probe. It deliberately drains Cocoa pools while a unit
// lives and after teardown, where deferred Objective-C lifetime bugs surface.
#include "Host/PluginInstance.hpp"
#import <AppKit/AppKit.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace {
class EditorHost final : public daw::plugins::PluginEditorHost {
public:
    void onEditorResized(std::uint32_t, std::uint32_t) noexcept override {}
    void onEditorClosed() noexcept override {}
};
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: plugin_lifetime_probe <au|vst3|vst> <path> [--editor|--view]\n");
        return 2;
    }
    @autoreleasepool {
        const bool windowed = argc > 3 && std::strcmp(argv[3], "--editor") == 0;
        const bool editor = windowed || (argc > 3 && std::strcmp(argv[3], "--view") == 0);
        if (windowed) [NSApplication sharedApplication];
        auto* factory = daw::plugins::factoryFor(daw::plugins::formatFromString(argv[1]));
        if (!factory) return 2;
        const auto descriptors = factory->inspect(argv[2]);
        if (descriptors.empty()) return 1;
        EditorHost host;
        NSWindow* window = nil;
        NSView* parent = nil;
        if (windowed) {
            window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1100, 800)
                styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
            parent = window.contentView;
        } else if (editor) {
            parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1100, 800)];
        }
        for (unsigned iteration = 0; iteration < 12; ++iteration) {
            std::unique_ptr<daw::plugins::PluginInstance> plugin;
            @autoreleasepool {
                std::printf("%u create\n", iteration);
                plugin = factory->create(descriptors.front());
                if (!plugin) return 1;
                daw::plugins::PluginProcessInfo info;
                info.sampleRate = 48000; info.maxBlockSize = 8;
                if (!plugin->activate(info)) return 1;
                plugin->startProcessing();
                std::printf("%u state / parameter text\n", iteration);
                std::vector<std::uint8_t> state;
                plugin->saveState(state);
                for (const auto& p : plugin->parameters())
                    (void)plugin->parameterText(p.index, plugin->parameterValue(p.index));
                if (!state.empty()) plugin->loadState(state);
                if (editor) {
                    std::printf("%u open / close editor\n", iteration);
                    if (!plugin->openEditor((__bridge void*)parent, &host)) return 1;
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.04, false);
                    plugin->closeEditor();
                }
                std::array<float, 8> in{}, left{}, right{};
                const float* inputs[]{in.data(), in.data()};
                float* outputs[]{left.data(), right.data()};
                daw::plugins::PluginProcessContext context;
                context.inputs = inputs; context.inputChannels = 2;
                context.outputs = outputs; context.outputChannels = 2;
                context.frames = 8;
                for (unsigned i = 0; i < 100; ++i) plugin->process(context);
                if (iteration % 2) {
                    std::printf("%u dispose before creation pool drains\n", iteration);
                    plugin.reset();
                    if (editor) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.04, false);
                }
                std::printf("%u drain\n", iteration);
            }
            @autoreleasepool {
                std::printf("%u dispose\n", iteration);
                plugin.reset();
            }
        }
        std::puts("PASS plugin survives state, processing, pool drains and destruction");
    }
}
