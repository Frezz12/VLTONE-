#pragma once

class QWidget;

/// Mark a plugin editor as an auxiliary participant in its parent's fullscreen
/// Space. Implemented with AppKit because Qt has no cross-platform flag for
/// NSWindowCollectionBehaviorFullScreenAuxiliary.
void configurePluginEditorWindowForMac(QWidget* window);

/// True only once Qt's native child is attached to a real Cocoa window. A
/// non-null winId alone is not enough: Qt can create an orphan NSView one event
/// turn before it inserts that view into the visible hierarchy.
bool pluginEditorContainerReadyForMac(QWidget* container);
