#include "PluginEditorWindowMac.hpp"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

void configurePluginEditorWindowForMac(QWidget* widget) {
    if (!widget) return;
    // Self-tests use Qt's offscreen platform even on a macOS build. Its WId is
    // an opaque test handle, not an NSView pointer.
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) return;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    NSWindow* window = view.window;
    if (!window) return;

    NSWindowCollectionBehavior behavior = window.collectionBehavior;
    behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
    behavior &= ~NSWindowCollectionBehaviorFullScreenAllowsTiling;
    behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
    behavior |= NSWindowCollectionBehaviorTransient;
    window.collectionBehavior = behavior;
    window.animationBehavior = NSWindowAnimationBehaviorUtilityWindow;
}

bool pluginEditorContainerReadyForMac(QWidget* widget) {
    if (!widget) return false;
    // The offscreen backend uses opaque WIds that are not Objective-C objects.
    if (QGuiApplication::platformName() != QLatin1String("cocoa")) return true;
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(widget->winId());
    if (view == nil || view.superview == nil || view.window == nil || view.hidden)
        return false;

    // A valid-looking child WId is insufficient if Qt is still rebuilding one
    // of its native ancestors. Every marked ancestor must already belong to
    // the exact same NSWindow before a vendor receives the child NSView.
    NSWindow* hostWindow = view.window;
    for (QWidget* ancestor = widget->parentWidget(); ancestor;
         ancestor = ancestor->parentWidget()) {
        if (!ancestor->testAttribute(Qt::WA_NativeWindow)) continue;
        NSView* ancestorView =
            (__bridge NSView*)reinterpret_cast<void*>(ancestor->winId());
        if (ancestorView == nil || ancestorView.window != hostWindow) return false;
        if (ancestor->isWindow()) break;
    }
    return true;
}
