#include "BrowserPrefs.hpp"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace ui::browserprefs {

namespace {

QString key(const char* name) {
    return QStringLiteral("browser/") + QLatin1String(name);
}

/// What a first run shows, so the panel is not an empty box: the user's Music
/// folder, and Downloads, which is where a sample pack lands. Only the ones
/// that exist — offering a folder that is not there reads as a bug.
QStringList defaultFolders() {
    QStringList out;
    for (QStandardPaths::StandardLocation location :
         {QStandardPaths::MusicLocation, QStandardPaths::DownloadLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && QDir(path).exists()) out << path;
    }
    return out;
}

} // namespace

QStringList folders() {
    QSettings settings;
    if (!settings.contains(key("folders"))) return defaultFolders();
    QStringList out = settings.value(key("folders")).toStringList();
    out.removeAll(QString());
    return out;
}

void setFolders(const QStringList& folders) {
    QSettings().setValue(key("folders"), folders);
}

bool addFolder(const QString& folder) {
    const QString clean = QDir::cleanPath(folder);
    if (clean.isEmpty()) return false;
    QStringList current = folders();
    if (current.contains(clean)) return false;
    current << clean;
    setFolders(current);
    return true;
}

void removeFolder(const QString& folder) {
    QStringList current = folders();
    current.removeAll(QDir::cleanPath(folder));
    // Written even when nothing was removed: the first removal has to stop the
    // defaults from coming back, and only a stored (possibly empty) list does.
    setFolders(current);
}

bool onLeft() { return QSettings().value(key("onLeft"), true).toBool(); }
void setOnLeft(bool onLeft) { QSettings().setValue(key("onLeft"), onLeft); }

bool visible() { return QSettings().value(key("visible"), true).toBool(); }
void setVisible(bool visible) { QSettings().setValue(key("visible"), visible); }

int width() {
    return std::clamp(QSettings().value(key("width"), 240).toInt(), 160, 720);
}
void setWidth(int width) {
    QSettings().setValue(key("width"), std::clamp(width, 160, 720));
}

bool autoPreview() { return QSettings().value(key("autoPreview"), true).toBool(); }
void setAutoPreview(bool on) { QSettings().setValue(key("autoPreview"), on); }

bool previewLoop() { return QSettings().value(key("previewLoop"), false).toBool(); }
void setPreviewLoop(bool loop) { QSettings().setValue(key("previewLoop"), loop); }

float previewGain() {
    return std::clamp(QSettings().value(key("previewGain"), 1.0).toFloat(), 0.0f,
                      2.0f);
}
void setPreviewGain(float gain) {
    QSettings().setValue(key("previewGain"), std::clamp(gain, 0.0f, 2.0f));
}

MidiTempo midiTempoPolicy() {
    const QString stored =
        QSettings().value(key("midiTempo"), QStringLiteral("ask")).toString();
    if (stored == QLatin1String("keep")) return MidiTempo::Keep;
    if (stored == QLatin1String("adopt")) return MidiTempo::Adopt;
    return MidiTempo::Ask;
}

void setMidiTempoPolicy(MidiTempo policy) {
    const char* stored = policy == MidiTempo::Keep    ? "keep"
                         : policy == MidiTempo::Adopt ? "adopt"
                                                      : "ask";
    QSettings().setValue(key("midiTempo"), QLatin1String(stored));
}

double zoom() {
    const double stored = QSettings().value(key("zoom"), 1.0).toDouble();
    return std::clamp(stored, kMinZoom, kMaxZoom);
}

void setZoom(double factor) {
    QSettings().setValue(key("zoom"), std::clamp(factor, kMinZoom, kMaxZoom));
}

} // namespace ui::browserprefs
