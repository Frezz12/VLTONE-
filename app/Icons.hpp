#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>
#include <QString>

class QPainter;

/// Vector icon set drawn with QPainter. No image assets and no platform icon
/// theme, so the same glyphs render identically on macOS and Windows and can be
/// tinted to whatever the active theme wants.
namespace icons {

enum class Glyph {
    Play, Pause, Stop, Record, Loop, ClipLoop, Metronome,
    SkipStart, SkipEnd, Rewind, Forward, Restart,
    Magnet, Grid, TimeFormat, GridDivision, Undo, Redo,
    ZoomIn, ZoomOut, ZoomFit,
    Save, Folder,
    /// A folder that sums: the same folder with its contents funnelling down
    /// into a bus, which is the whole difference between the two kinds.
    FolderSum,
    Import, Export,
    Mixer, Inspector, Detach, Sidebar,
    Plus, Minus, Chevron, ChevronUp, ChevronRight, Gear, Headphones, Waveform,
    /// Full arrows, for moving something rather than adding to it — the piano
    /// roll's transpose buttons, where a plus sign would read as "new note".
    ArrowUp, ArrowDown, ArrowLeft, ArrowRight,
    Home, Globe, Reload, Download,
    ResizeHorizontal, ResizeVertical, WindowMaximize,
    Trash, Layers,
    Pointer, Knife, Eraser, Crosshair, Brush, Ghost, NoteStyle,
    // Note tools, for the piano roll's context panel.
    Arpeggio, Chord, Strum, Glue, Dice, Invert, NoteMute, Articulate,
    /// A breakpoint curve — an automation lane, and anything that edits one.
    Automation,
    /// The same curve with a plus: arm the gesture that creates automation.
    AutomationCreate,
    MonoRing, StereoRings,
    Power, Clock, MidiKeys, CountIn,
    // Context-panel controls: an icon each, with the value only shown while it
    // is being changed.
    Volume, Pan, FadeIn, FadeOut,
    // Plugin quick-adder: the glass search that grows out of the "+ Add Plugin"
    // button on a selected track.
    Search, Star, Close, Mic, Eq, Synth, Plugin, Image,
    /// A bound writing page used by the standalone rich-text notebook.
    Notebook,
    /// A compact assistant/chat mark used by the global AI panel toggle.
    Assistant,
    // Collaboration surfaces: the session status strip, the join checklist and
    // the cloud project browser.
    Cloud, CloudUpload, CloudOff, Users, Link, Key, Check, Warning,
    /// An open arc, meant to be rotated by the caller to indicate work in
    /// flight. It carries no animation of its own.
    Spinner
};

/// Rasterise every glyph and report the first that draws nothing. The switch in
/// paint() has no default, so a value added to the enum but not to the switch
/// renders as empty space rather than failing to build — as does a case whose
/// path is malformed. `Spinner` is assumed last; anything appended after it is
/// covered automatically.
bool checkGlyphCoverageForTest(QString* error = nullptr);

/// Draw `glyph` centred in `rect`, tinted `color`.
void paint(QPainter& painter, Glyph glyph, const QRectF& rect,
           const QColor& color);

/// Rasterise a glyph into a QIcon (for menus, window icons, …).
QIcon icon(Glyph glyph, const QColor& color, int size = 18);

/// Render a monochrome SVG from the embedded /icons resource and tint it with
/// the active theme. Multiple device-pixel-ratio variants keep toolbar assets
/// sharp on scaled displays.
QIcon svgIcon(const QString& fileName, const QColor& color, int size = 18);

} // namespace icons
