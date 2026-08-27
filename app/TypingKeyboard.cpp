#include "TypingKeyboard.hpp"
#include "KeyboardLayout.hpp"

#include "EngineController.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <algorithm>

namespace {

/// The FL layout, as (Qt key, semitones above the lower row's C).
///
/// Two staggered rows: the white keys along Z X C V B N M and Q W E R T Y U,
/// the black ones on the row above each. The lower row runs a fifth past its
/// octave (, L . ; /) and the upper one a third (I 9 O 0 P), which is what
/// makes a two-octave run playable without moving the hands.
struct KeyNote { int key; int semitone; };
constexpr KeyNote kLayout[] = {
    // Lower octave: Z row, blacks on the A row.
    {Qt::Key_Z, 0},  {Qt::Key_S, 1},  {Qt::Key_X, 2},  {Qt::Key_D, 3},
    {Qt::Key_C, 4},  {Qt::Key_V, 5},  {Qt::Key_G, 6},  {Qt::Key_B, 7},
    {Qt::Key_H, 8},  {Qt::Key_N, 9},  {Qt::Key_J, 10}, {Qt::Key_M, 11},
    {Qt::Key_Comma, 12}, {Qt::Key_L, 13}, {Qt::Key_Period, 14},
    {Qt::Key_Semicolon, 15}, {Qt::Key_Slash, 16},
    // Upper octave: Q row, blacks on the number row.
    {Qt::Key_Q, 12}, {Qt::Key_2, 13}, {Qt::Key_W, 14}, {Qt::Key_3, 15},
    {Qt::Key_E, 16}, {Qt::Key_R, 17}, {Qt::Key_5, 18}, {Qt::Key_T, 19},
    {Qt::Key_6, 20}, {Qt::Key_Y, 21}, {Qt::Key_7, 22}, {Qt::Key_U, 23},
    {Qt::Key_I, 24}, {Qt::Key_9, 25}, {Qt::Key_O, 26}, {Qt::Key_0, 27},
    {Qt::Key_P, 28},
};

/// The velocity every typed note gets. A computer keyboard has no dynamics,
/// and this is the velocity a note drawn in the piano roll gets, so playing a
/// part and drawing it sound the same.
constexpr int kVelocity = 100;

/// Typing into a field must stay typing — a name, a tempo, a search box, and
/// the shortcut recorder, where a keystroke is the value being entered.
bool isTextEntry(const QWidget* widget) {
    for (const QWidget* current = widget; current;
         current = current->parentWidget()) {
        if (current->property("dawWebInput").toBool()) return true;
        if (qobject_cast<const QKeySequenceEdit*>(current) ||
            qobject_cast<const QLineEdit*>(current) ||
            qobject_cast<const QAbstractSpinBox*>(current) ||
            qobject_cast<const QTextEdit*>(current) ||
            qobject_cast<const QPlainTextEdit*>(current) ||
            qobject_cast<const QComboBox*>(current)) {
            return true;
        }
    }
    return false;
}

} // namespace

TypingKeyboard::TypingKeyboard(daw::EngineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {}

TypingKeyboard::~TypingKeyboard() { allNotesOff(); }

int TypingKeyboard::semitoneFor(int key) {
    for (const KeyNote& entry : kLayout)
        if (entry.key == key) return entry.semitone;
    return -1;
}

bool TypingKeyboard::usesKey(int key) {
    return semitoneFor(key) >= 0 || key == Qt::Key_BracketLeft ||
           key == Qt::Key_BracketRight;
}

void TypingKeyboard::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) allNotesOff();
    emit enabledChanged(enabled);
}

void TypingKeyboard::setOctave(int octave) {
    // The top of the layout is 28 semitones above the row's C, so the highest
    // octave that still fits inside 127 is 8.
    const int clamped = std::clamp(octave, 0, 8);
    if (clamped == m_octave) return;
    // The held keys were pressed at the old octave; their note-offs already
    // carry the pitch they started, but leaving them sounding across a jump is
    // confusing — a shifted keyboard should be a silent one.
    allNotesOff();
    m_octave = clamped;
    emit octaveChanged(clamped);
}

void TypingKeyboard::allNotesOff() {
    for (auto it = m_held.constBegin(); it != m_held.constEnd(); ++it)
        m_controller->liveNoteOff(it->trackId, it->pitch);
    m_held.clear();
}

bool TypingKeyboard::handles(const QKeyEvent* event) const {
    if (!m_enabled) return false;
    // Any modifier means a command, not a note: Ctrl+S is Save, Shift+M is not
    // a B. Keypad is let through because a laptop reports it on plain keys.
    if (event->modifiers() & ~Qt::KeypadModifier) return false;
    if (isTextEntry(QApplication::focusWidget())) return false;
    // An open menu or combo popup owns the keyboard while it is up.
    if (QApplication::activePopupWidget()) return false;

    return usesKey(eventKey(event));
}

int TypingKeyboard::eventKey(const QKeyEvent* event) {
    const int physical = ui::physicalUsKey(event);
    return physical ? physical : event->key();
}

void TypingKeyboard::pressKey(int key) {
    if (key == Qt::Key_BracketLeft) {
        setOctave(m_octave - 1);
        return;
    }
    if (key == Qt::Key_BracketRight) {
        setOctave(m_octave + 1);
        return;
    }
    if (m_held.contains(key)) return;   // already down

    const int semitone = semitoneFor(key);
    if (semitone < 0) return;
    const int pitch = m_octave * 12 + semitone;
    if (pitch < 0 || pitch > 127) return;

    const std::string track = m_target ? m_target() : std::string();
    if (track.empty()) return;
    if (!m_controller->liveNoteOn(track, pitch, kVelocity)) return;

    m_held.insert(key, Held{track, pitch});
    emit notePlayed(pitch);
}

void TypingKeyboard::releaseKey(int key) {
    auto found = m_held.find(key);
    if (found == m_held.end()) return;
    m_controller->liveNoteOff(found->trackId, found->pitch);
    m_held.erase(found);
}

bool TypingKeyboard::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::ShortcutOverride: {
            // Claim the key here or the shortcut machinery gets it first and E
            // expands take layers instead of playing a note.
            auto* key = static_cast<QKeyEvent*>(event);
            if (!handles(key)) break;
            key->accept();
            return true;
        }
        case QEvent::KeyPress: {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!handles(key)) break;
            // Auto-repeat is the operating system re-sending a key that never
            // came up; retriggering the note on each one would machine-gun it.
            if (!key->isAutoRepeat()) pressKey(eventKey(key));
            return true;
        }
        case QEvent::KeyRelease: {
            auto* key = static_cast<QKeyEvent*>(event);
            // Not `handles()`: a key that started a note must be able to end it
            // even if the keyboard was switched off, or focus moved into a text
            // field, while it was down.
            if (key->isAutoRepeat()) break;
            const int physical = eventKey(key);
            if (!m_held.contains(physical)) break;
            releaseKey(physical);
            return true;
        }
        case QEvent::ApplicationStateChange:
            // A key let go while another application had focus never reaches
            // us, so anything still held would sound forever.
            if (QApplication::applicationState() != Qt::ApplicationActive)
                allNotesOff();
            break;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}
