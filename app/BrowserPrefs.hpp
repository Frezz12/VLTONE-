#pragma once

#include <QString>
#include <QStringList>

/// Everything the file browser remembers, in one place.
///
/// Free functions over QSettings rather than state on a widget, because three
/// unrelated things read the same values: the panel itself, the Browser page in
/// Settings, and the shell (which has to know the side and the visibility
/// before the panel exists). Generalises the `static persist…()` idiom the
/// recording page uses. Every key lives under "browser/".
namespace ui::browserprefs {

/// The roots the tree shows. Absolute paths, in the order the user added them.
QStringList folders();
void setFolders(const QStringList& folders);
/// Add a folder if it is not already there. False when it was a duplicate.
bool addFolder(const QString& folder);
void removeFolder(const QString& folder);

/// True when the panel sits left of the inspector, false when it is on the far
/// right of the window.
bool onLeft();
void setOnLeft(bool onLeft);

bool visible();
void setVisible(bool visible);

int width();
void setWidth(int width);

/// Selecting a file starts playing it. The user asked for this; the switch
/// exists for the times a folder is being browsed while something plays.
bool autoPreview();
void setAutoPreview(bool on);

/// The audition repeats until stopped, rather than playing once.
bool previewLoop();
void setPreviewLoop(bool loop);

/// Audition level, 0 … 2 linear (1 = as recorded).
float previewGain();
void setPreviewGain(float gain);

/// How much bigger (or smaller) the browser's own interface is drawn: 1.0 is
/// the application's size, and the range is deliberately narrow at the bottom —
/// a browser smaller than the rest of the window is not a use case, a bigger
/// one on a large screen is.
double zoom();
void setZoom(double factor);
inline constexpr double kMinZoom = 0.85;
inline constexpr double kMaxZoom = 2.0;

/// What to do with the tempo written into an imported MIDI file.
enum class MidiTempo {
    Ask,     ///< offer the choice, with a way to stop being asked
    Keep,    ///< the project's tempo wins, always
    Adopt,   ///< take the file's tempo, always
};
MidiTempo midiTempoPolicy();
void setMidiTempoPolicy(MidiTempo policy);

} // namespace ui::browserprefs
