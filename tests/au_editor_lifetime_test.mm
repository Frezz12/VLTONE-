// A deterministic AU view with the usual autoreleased factory return. No
// third-party plugin, MIDI server, audio device or application window is used.
#include "Au/AuInstance.hpp"
#import <AppKit/AppKit.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <cstdio>
#include <memory>

namespace {
bool unitAlive = false;
unsigned destroyedViews = 0;
unsigned viewsDestroyedAfterUnit = 0;
unsigned createdViews = 0;
unsigned factoriesDestroyedBeforeView = 0;
}

@interface VLTTestAuView : NSView
@end
@implementation VLTTestAuView
- (void)dealloc {
    ++destroyedViews;
    if (!unitAlive) ++viewsDestroyedAfterUnit;
    [super dealloc];
}
@end

@interface VLTTestAuViewFactory : NSObject <AUCocoaUIBase>
@end
@implementation VLTTestAuViewFactory
- (unsigned)interfaceVersion { return 0; }
- (NSString*)description { return @"test"; }
- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size {
    (void)unit; (void)size;
    ++createdViews;
    NSView* view = [[[VLTTestAuView alloc] initWithFrame:NSMakeRect(0, 0, 80, 60)] autorelease];
    // Cocoa/plugin setup may put the view in a temporary collection. Its
    // delayed release is independent of ARC's return-value optimization.
    (void)[[[NSArray alloc] initWithObjects:view, nil] autorelease];
    return view;
}
- (void)dealloc {
    if (destroyedViews != createdViews) ++factoriesDestroyedBeforeView;
    [super dealloc];
}
@end

extern "C" OSStatus AudioUnitGetPropertyInfo(AudioUnit, AudioUnitPropertyID property,
    AudioUnitScope, AudioUnitElement, UInt32* size, Boolean* writable) {
    if (property != kAudioUnitProperty_CocoaUI) return kAudioUnitErr_InvalidProperty;
    *size = sizeof(AudioUnitCocoaViewInfo);
    *writable = false;
    return noErr;
}

extern "C" OSStatus AudioUnitGetProperty(AudioUnit, AudioUnitPropertyID property,
    AudioUnitScope, AudioUnitElement, void* data, UInt32* size) {
    if (property != kAudioUnitProperty_CocoaUI || *size < sizeof(AudioUnitCocoaViewInfo))
        return kAudioUnitErr_InvalidProperty;
    auto* info = static_cast<AudioUnitCocoaViewInfo*>(data);
    info->mCocoaAUViewBundleLocation =
        (CFURLRef)[[[NSBundle mainBundle] bundleURL] retain];
    info->mCocoaAUViewClass[0] = (CFStringRef)[@"VLTTestAuViewFactory" retain];
    *size = sizeof(AudioUnitCocoaViewInfo);
    return noErr;
}

extern "C" OSStatus AudioUnitRemovePropertyListenerWithUserData(
    AudioUnit, AudioUnitPropertyID, AudioUnitPropertyListenerProc, void*) {
    return noErr;
}

extern "C" OSStatus AudioComponentInstanceDispose(AudioComponentInstance) {
    unitAlive = false;
    return noErr;
}

int main() {
    bool ok = true;
    @autoreleasepool {
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        for (unsigned i = 0; i < 3; ++i) {
            unitAlive = true;
            auto plugin = std::make_unique<daw::plugins::AuInstance>(
                reinterpret_cast<AudioComponentInstance>(1), daw::plugins::PluginDescriptor{});
            if (!plugin->openEditor(parent, nullptr)) {
                std::fprintf(stderr, "FAIL fixture editor did not open\n");
                ok = false;
                break;
            }
            if (i % 2 == 0) plugin->closeEditor();
            plugin.reset();
            // The GUI's surrounding pool has NOT drained. Neither explicit
            // close nor implicit destruction may leave a view using a dead AU.
            if (destroyedViews != i + 1) {
                std::fprintf(stderr, "FAIL AU view outlives its unit in caller's pool\n");
                ok = false;
            }
        }
        [parent release];
    }
    if (viewsDestroyedAfterUnit != 0) {
        std::fprintf(stderr, "FAIL %u views deallocated after AU disposal\n", viewsDestroyedAfterUnit);
        ok = false;
    }
    if (factoriesDestroyedBeforeView != 0) {
        std::fprintf(stderr, "FAIL %u editor factories deallocated before their views\n",
                     factoriesDestroyedBeforeView);
        ok = false;
    }
    if (ok) std::puts("PASS AU editor references drain before the AudioUnit is disposed");
    return ok ? 0 : 1;
}
