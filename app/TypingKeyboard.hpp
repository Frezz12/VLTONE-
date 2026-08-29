#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>
#include <string>

class QKeyEvent;

namespace daw { class EngineController; }

/// The computer keyboard as a MIDI keyboard, FL Studio's layout.
///
/// Two rows of keys an octave apart: Z S X D C V G B H N J M (plus , L . ; /)
/// is the lower octave, Q 2 W 3 E R 5 T 6 Y 7 U (plus I 9 O 0 P) the one above
/// it, so a two-and-a-bit-octave keyboard sits under the hands with no black
/// key out of reach. `[` and `]` move the whole thing down and up an octave.
///
/// Installed as an *application* event filter, so a key plays whatever window
/// has focus — the arrangement, the piano roll, a plugin editor. That is also
/// why it must swallow the keys it uses rather than merely reacting to them:
/// while it is on, E, R, C, X and the rest are notes, and the single-letter
/// command shortcuts they would otherwise fire have to stay quiet. Qt's
/// ShortcutOverride is where that is decided, before the shortcut machinery
/// looks at the key, and it is why plain KeyPress interception is not enough.
///
/// Nothing here writes to the document: notes go straight to the track's
/// instrument (see EngineController::liveNoteOn) and are gone when the key
/// comes up.
class TypingKeyboard : public QObject {
    Q_OBJECT
public:
    explicit TypingKeyboard(daw::EngineController* controller,
                            QObject* parent = nullptr);
    /// Ends anything still held — a stuck note outlives this object otherwise.
    ~TypingKeyboard() override;

    bool isEnabled() const { return m_enabled; }
    /// Off by default: the letter keys are command shortcuts until the user
    /// asks for a keyboard.
    void setEnabled(bool enabled);

    /// Where notes go, asked on every key press rather than cached — the target
    /// follows the selection and the focused piano roll with nothing to keep in
    /// sync. Returning an empty id means "nowhere", and the press is ignored.
    void setTargetProvider(std::function<std::string()> provider) {
        m_target = std::move(provider);
    }

    /// The octave of the lower row's C, in the app's numbering (5 = middle C,
    /// MIDI 60). Clamped so the two rows stay inside 0 … 127.
    int octave() const { return m_octave; }
    void setOctave(int octave);

    /// Whether `key` (a Qt::Key) is part of the layout, octave keys included.
    /// The menus ask, so a command bound to a bare letter can stand aside while
    /// the keyboard is on — on macOS the menu bar is the system's, and AppKit
    /// takes a key equivalent before any Qt filter is consulted.
    static bool usesKey(int key);

    /// How many keys are sounding right now. Only interesting to the headless
    /// check, which has no ears.
    int heldCount() const { return int(m_held.size()); }

    /// Release every held key. Called when the keyboard is switched off, when
    /// the application loses focus (a key let go elsewhere never reaches us),
    /// and from the destructor.
    void allNotesOff();

signals:
    void enabledChanged(bool enabled);
    void octaveChanged(int octave);
    /// A key went down, with the pitch it played — for the status bar.
    void notePlayed(int pitch);
    /// A successfully routed note changed state, including its owning track.
    /// MainWindow merges this with hardware MIDI for the Piano Roll keyboard.
    void noteStateChanged(const QString& trackId, int pitch, bool down);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Semitones above the lower row's C for a Qt key code, or −1 when the key
    /// is not part of the layout.
    static int semitoneFor(int key);
    /// Whether this key event is ours to take: the keyboard is on, the key is
    /// in the layout (or an octave key), nothing is being typed into a text
    /// field, and no modifier is held — Ctrl+S must stay Save.
    bool handles(const QKeyEvent* event) const;
    /// Layout-independent key used for both note-on and note-off identity.
    static int eventKey(const QKeyEvent* event);

    void pressKey(int key);
    void releaseKey(int key);

    daw::EngineController* m_controller = nullptr;
    std::function<std::string()> m_target;
    bool m_enabled = false;
    int m_octave = 5;

    /// Qt key code → the note it started, as (track, pitch). The track is
    /// remembered with the note because the selection can change while a key is
    /// down, and the note-off has to go where the note-on went.
    struct Held {
        std::string trackId;
        int pitch = 0;
    };
    QHash<int, Held> m_held;
};
