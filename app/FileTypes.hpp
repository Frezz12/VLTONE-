#pragma once

#include "platform/AudioFileDecoder.hpp"

#include <QByteArray>
#include <QFileInfo>
#include <QMimeData>
#include <QString>
#include <QStringList>

#include <string>

/// What this application can open, in one place.
///
/// There used to be four hand-written extension lists — the import dialog, the
/// timeline's drop target, and two in the sampler — and they had drifted apart:
/// one accepted containers the decoder rejected, none offered containers it
/// accepted. Every one of them now asks `audio::platform::decodableExtensions`,
/// so adding a format to the decoder adds it to the whole interface.
namespace ui {

/// True when the audio decoder can open this path, judged by its extension.
/// A cheap gate for a drag-enter or a file listing — it says nothing about
/// whether the file is intact, only whether offering it is honest.
inline bool isAudioFile(const QString& path) {
    return audio::platform::isDecodableExtension(
        QFileInfo(path).suffix().toLower().toStdString());
}

/// Standard MIDI files. `.midi` as well as `.mid`: both are in the wild, and a
/// browser that hid half of them would look broken.
inline bool isMidiFile(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("mid") || suffix == QLatin1String("midi");
}

/// Anything the project can take in — what a browser row may be dragged from
/// and what an import dialog should offer.
inline bool isImportableFile(const QString& path) {
    return isAudioFile(path) || isMidiFile(path);
}

/// "*.wav *.aiff …" — the glob list, built from the decoder's own extensions.
inline QString audioGlobs() {
    QStringList globs;
    for (std::string_view extension : audio::platform::decodableExtensions())
        globs << QStringLiteral("*.%1").arg(QString::fromUtf8(extension.data(),
                                                             int(extension.size())));
    return globs.join(QLatin1Char(' '));
}

inline QString midiGlobs() {
    return QStringLiteral("*.mid *.midi");
}

/// Name filter for a file dialog that wants audio only (loading a sample).
inline QString audioNameFilter() {
    return QStringLiteral("Audio Files (%1);;All Files (*)").arg(audioGlobs());
}

/// Name filter for a dialog that takes anything the project can import.
inline QString importNameFilter() {
    return QStringLiteral("Audio & MIDI (%1 %2);;Audio Files (%1);;"
                          "MIDI Files (%2);;All Files (*)")
        .arg(audioGlobs(), midiGlobs());
}

// ── Dragging a plugin ──────────────────────────────────────────────────────
//
// The browser lists the scanned plugins beside the sample folders, and they are
// dragged onto a track or a clip the same way a file is. A file drag carries
// URLs; a plugin has no file the target could import, so it carries a reference
// instead — the format and the plugin's stable uid — and the drop site resolves
// it through the plugin manager it already has. Nothing but two identifiers
// crosses the drag, so a stale payload cannot load the wrong plugin.

inline constexpr const char* kPluginDragMime = "application/x-daw-plugin";

inline QByteArray encodePluginRef(int format, const QString& uid) {
    return (QString::number(format) + QLatin1Char('\n') + uid).toUtf8();
}

/// Pull a plugin reference out of a drag. False when the drag is not one.
inline bool decodePluginRef(const QMimeData* mime, int& format, QString& uid) {
    if (!mime || !mime->hasFormat(QLatin1String(kPluginDragMime))) return false;
    const QString payload =
        QString::fromUtf8(mime->data(QLatin1String(kPluginDragMime)));
    const int split = payload.indexOf(QLatin1Char('\n'));
    if (split <= 0) return false;
    bool ok = false;
    format = payload.left(split).toInt(&ok);
    uid = payload.mid(split + 1);
    return ok && !uid.isEmpty();
}

} // namespace ui
