# SIGTRAP while adding Pro-Q 3 — 2026-09-09

## Evidence

The server artifact for report `d6115c35-a152-4936-b86b-0e2b734c38ce`
records the application disconnect at 08:47:15 UTC (11:47:15 Moscow).
The user was opening Pro-Q 3. The last recovery snapshot was at 11:46.

The fatal stack is:

```
PluginQuickAdder::insertCurrent
QWidget::setCursor
QCocoaCursor::createCursorData
NSImage(QtExtras)::imageFromQImage
QImage::toCGImage
CGImageCreate
CGColorSpaceGetType
CFGetTypeID
SIGTRAP
```

`insertCurrent()` sets `Qt::WaitCursor` before calling `addInsert()`. Qt's
Cocoa backend creates that cursor from its embedded PNG. No plugin call is
active in the marker; the snapshot shows five tracks, no clips and no loaded
plugins. This failure occurs before Pro-Q 3 is instantiated, independently of
the recovery snapshot's timestamp.

## Cause and correction

The installed Qt was 6.11.1. Its
[`qt_mac_cgImageFormatForImage()`](https://github.com/qt/qtbase/blob/v6.11.1/src/gui/painting/qcoregraphics.mm)
stores a newly created color space in a local `QCFType`, then returns a
`vImage_CGImageFormat` containing only its raw pointer. Destruction of the
local releases the color space before `QImage::toCGImage()` uses it. System
color-space caching can mask the invalid ownership, so a successful cursor
creation does not disprove this defect.

[Qt 6.11.2](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/painting/qcoregraphics.mm)
returns `QCGImageFormat`, whose color-space member itself owns a `QCFType`.
The required Homebrew Qt modules were updated to 6.11.2. Old versions were
retained; no project snapshots or audio files were removed. Insufficient disk
space interrupted the first attempt; removing 1.91 GiB of regenerable object
files and temporary Go build caches allowed the update to finish.

CMake now rejects Qt 6.11.0–6.11.1 for macOS builds. Windows and the supported
Qt 6.8 line remain accepted. Packaged applications must also be rebuilt to
carry corrected Qt libraries.

## Regression test

`qt_cgimage_lifetime_test` exercises the actual loaded Qt library without a
display or plugin. A test-only inserted library observes CoreFoundation
retains/releases and checks ownership when Qt calls `CGImageCreate`. It
detects a premature release without dereferencing the stale pointer or relying
on allocator luck. The shipping executable does not link this library.

It additionally checks native image dimensions, RGB color space and pixel
buffer lifetime after destroying the source QImage, across 128 conversions
using default sRGB/custom ICC profiles and two image formats.

The same diagnostic binary returned `FAIL: Qt released its colorspace before
CGImageCreate` (exit 3) with Qt 6.11.1, then passed all 128 conversions with
Qt 6.11.2. Configure-time rejection was also verified against 6.11.1 before
the update completed.

Final validation:

- `daw`, `daw_reporter` and `qt_cgimage_lifetime_test` built successfully.
- CTest `qt_cgimage_lifetime_test`: passed; forcing the retained Qt 6.11.1
  frameworks with the same final executable detects the defect (exit 3).
- `QT_QPA_PLATFORM=cocoa NSZombieEnabled=YES build/bin/VLTONE
  --pluginpickercheck`: passed (exit 0), covering deferred pointer activation
  and keyboard activation through the real quick adder and loading cursor.
  No zombie diagnostics were emitted. Existing translator shutdown warnings
  remain unrelated to this crash.
- `otool -L build/bin/VLTONE` identifies QtGui/QtCore 6.11.2; the diagnostic
  interposition library is absent from application dependencies.
- `git diff --check`: passed.

The user's running process was launched from `build/bin/VLTONE`; it was left
running to preserve current work. Restart it after saving to load the fixed Qt.
