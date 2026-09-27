#include "TrackIcons.hpp"
#include "Theme.hpp"

#include <QBuffer>
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgRenderer>

namespace ui::trackicons {
namespace {
constexpr int kImageSide = 96;
constexpr qint64 kImportLimit = 32 * 1024 * 1024;
constexpr qint64 kStoredLimit = 128 * 1024;
constexpr auto kSettingsKey = "theme/trackIcons";
struct Symbol { const char* id; const char* name; const char* category; const char* drawing; };
// Original 24-unit line drawings. Names are separate from stable project keys.
const Symbol symbols[] = {
    {"drum-kit", "Drum kit", "Drums & percussion", R"(<circle cx="12" cy="15" r="5"/><path d="M3 7h6m6-2h7M6 7v13m13-15v15M2 20h7m7 0h7M9 5v4m6-4v4M9 5h6"/>)"},
    {"kick", "Kick drum", "Drums & percussion", R"(<circle cx="12" cy="11" r="8"/><circle cx="12" cy="11" r="5"/><path d="m5 17-2 4m16-4 2 4m-9-10v10m-3 0h6"/>)"},
    {"snare", "Snare", "Drums & percussion", R"(<ellipse cx="12" cy="8" rx="9" ry="3"/><path d="M3 8v8c0 4 18 4 18 0V8M6 10v7m4-6v7m4-7v7m4-8v7M5 2l13 5M19 2 7 7"/>)"},
    {"tom", "Tom", "Drums & percussion", R"(<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v13c0 4 16 4 16 0V5M7 8v10m10-10v10M12 8v12"/>)"},
    {"hi-hat", "Hi-hat", "Drums & percussion", R"(<path d="M3 7q9-6 18 0H3Zm0 3q9 4 18 0M12 3v17m-5 2 5-4 5 4m-5-8 6 4h3"/>)"},
    {"cymbal", "Cymbal", "Drums & percussion", R"(<path d="M2 8q6-1 8-4h4q2 3 8 4-10 4-20 0ZM12 2v17m-6 3 6-5 6 5"/>)"},
    {"clap", "Claps", "Drums & percussion", R"(<path d="m8 12-3-5q-1-2 1-2l5 6-2-8q0-2 2-1l3 8 1-6q1-2 2 0v8l2-3q2-1 2 1l-3 9q-2 4-7 2l-7-5q-2-2 0-3l4 2M3 3 1 1m20 2 2-2M2 9H0"/>)"},
    {"conga", "Conga", "Drums & percussion", R"(<ellipse cx="12" cy="4" rx="6" ry="2"/><path d="M6 4q-2 9 2 17h8q4-8 2-17M8 7l2 12m6-12-2 12"/>)"},
    {"bongos", "Bongos", "Drums & percussion", R"(<ellipse cx="6" cy="7" rx="5" ry="3"/><ellipse cx="18" cy="6" rx="5" ry="3"/><path d="m1 7 2 12h6l2-12m2-1 2 12h6l2-12M11 10h2M4 10l1 6m14-7v6"/>)"},
    {"tambourine", "Tambourine", "Drums & percussion", R"(<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="6"/><path d="M9 3v4h6V3M9 17v4h6v-4M3 9h4v6H3m14-6h4v6h-4"/>)"},
    {"triangle", "Triangle", "Drums & percussion", R"(<path d="M18 19H3L12 3l9 16m-5-4 6-5M10 2h4"/>)"},
    {"shaker", "Shaker", "Drums & percussion", R"(<path d="m4 15 11-11q3-2 5 1t0 4L9 20q-3 2-5-1t0-4Zm1-1 5 5m4-14 5 5M3 7l2 1M7 3l1 2m10 14 2 1"/>)"},
    {"cowbell", "Cowbell", "Drums & percussion", R"(<path d="M8 4h8l5 17H3L8 4Zm2 0V2h4v2M5 17h14M10 8h4"/>)"},
    {"maracas", "Maracas", "Drums & percussion", R"svg(<ellipse cx="7" cy="7" rx="4" ry="5" transform="rotate(-25 7 7)"/><ellipse cx="17" cy="7" rx="4" ry="5" transform="rotate(25 17 7)"/><path d="m9 11 5 10m1-10-5 10M4 6l6-2m5 0 5 2"/>)svg"},
    {"acoustic-guitar", "Acoustic guitar", "Guitars & strings", R"(<path d="m14 10 6-8 2 2-7 8q2 4-1 7t-7 1q-5-3-3-7t6-3q2 2 4 0Z"/><circle cx="11" cy="14" r="2"/><path d="m7 18 3 2M18 4l3 2"/>)"},
    {"electric-guitar", "Electric guitar", "Guitars & strings", R"(<path d="m13 10 7-8 2 2-8 8 4 1q2 5-3 7t-9-1q-4-4 0-7l1 4 4-2-2-4 4 0Z"/><path d="m7 17 3 2m2-7 2 2m4-9 3 2"/>)"},
    {"bass-guitar", "Bass guitar", "Guitars & strings", R"(<path d="m13 12 6-10 3 1-7 11q3 4-1 7t-8-1q-4-4 0-7l2 3 4-1-2-4 3 1Z"/><path d="m8 18 4 2m6-14 3 1m-4 2 3 1"/>)"},
    {"ukulele", "Ukulele", "Guitars & strings", R"(<path d="m13 11 5-8 3 2-6 8q2 4-1 6t-7 0q-4-3-1-6t5 0l2-2Z"/><circle cx="11" cy="15" r="1.6"/><path d="m8 18 3 2"/>)"},
    {"banjo", "Banjo", "Guitars & strings", R"(<circle cx="9" cy="15" r="6"/><circle cx="9" cy="15" r="4"/><path d="m12 10 7-8 3 2-8 9M6 17l4 2M17 5l3 2"/>)"},
    {"violin", "Violin", "Guitars & strings", R"(<path d="m12 9 5-7 2 2-5 7q3 1 2 3l-2-1-2 3 1 2q-3 5-7 2t-1-7l2 1 3-3-1-2q0-2 3 0Z"/><path d="m7 17 3 2M21 7 13 22"/>)"},
    {"cello", "Cello", "Guitars & strings", R"(<path d="M11 2h2v6q5 1 3 5l-1 1q6 6-3 7-9-1-3-7l-1-1q-2-4 3-5V2ZM12 8v15m-3-6h6M21 3v18"/>)"},
    {"harp", "Harp", "Guitars & strings", R"(<path d="M4 3q7 5 16 0-2 12-9 18H5L4 3Zm1 5h13M8 8v11m4-11v9m4-9v5"/>)"},
    {"strings", "String ensemble", "Guitars & strings", R"(<path d="M5 2v7q-3 1-1 4-4 6 1 7 5-1 1-7 2-3-1-4M13 3v6q-3 1-1 4-4 6 1 7 5-1 1-7 2-3-1-4M20 3v18M3 17h4m4 0h4"/>)"},
    {"piano", "Piano", "Keys & mallets", R"(<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M7 5v14m5-14v14m5-14v14"/><path fill="currentColor" d="M5 5h3v7H5Zm5 0h3v7h-3Zm5 0h3v7h-3Z"/>)"},
    {"grand-piano", "Grand piano", "Keys & mallets", R"(<path d="M4 16V8q0-6 7-6h2q0 7 7 8v6H4Zm0 0v4h16v-4M6 20v2m12-2v2M8 16v4m4-4v4m4-4v4"/>)"},
    {"electric-piano", "Electric piano", "Keys & mallets", R"(<rect x="2" y="6" width="20" height="10" rx="2"/><path d="M2 10h20M6 10v6m4-6v6m4-6v6m4-6v6M6 16l12 6m0-6L6 22M5 8h3"/>)"},
    {"organ", "Organ", "Keys & mallets", R"(<path d="M3 15V7h3v8m3 0V3h3v12m3 0V5h3v10m3 0V9h2v6M2 15h20v6H2Zm4 0v6m4-6v6m4-6v6m4-6v6"/>)"},
    {"accordion", "Accordion", "Keys & mallets", R"(<rect x="2" y="4" width="5" height="16" rx="1"/><rect x="18" y="4" width="4" height="16" rx="1"/><path d="M7 6h11M7 18h11M10 6v12m3-12v12m3-12v12M2 8h4m-4 4h4m-4 4h4m14-8v1m0 4v1"/>)"},
    {"xylophone", "Xylophone", "Keys & mallets", R"(<path d="M3 10h3v11H3Zm5-2h3v12H8Zm5-2h3v13h-3Zm5-2h3v14h-3ZM4 3l12 9M3 4l12 9"/><circle cx="3" cy="3" r="1.5"/>)"},
    {"bells", "Bells", "Keys & mallets", R"(<path d="M4 16q3-3 3-8a5 5 0 0 1 10 0q0 5 3 8H4Zm5 3q3 4 6 0M12 2V1M2 9l2-3m16 0 2 3"/>)"},
    {"music-box", "Music box", "Keys & mallets", R"(<path d="M3 12h18v9H3Zm0 0 3-5h9l6 5M7 15v3m3-3v3m3-3v3m3-3v3m5-1h2v-4M12 7V2l6 1v3"/><circle cx="10" cy="7" r="2"/>)"},
    {"trumpet", "Trumpet", "Winds & brass", R"(<path d="M2 9h12l7-5v15l-7-5H6q-4 0-4-3V9Zm4 5v4h8v-4M7 6v5m4-5v5M6 6h2m2 0h2"/>)"},
    {"saxophone", "Saxophone", "Winds & brass", R"(<path d="M5 3h5q4 0 4 4v8q0 7-6 6-5-1-4-6h5q-1 3 1 3 1 0 1-3V8q0-2-3-2H5M14 14l7-4v7q0 4-6 4"/><path d="M8 8h2m-2 3h2m-2 3h2"/>)"},
    {"flute", "Flute", "Winds & brass", R"(<path d="m2 18 16-16 4 4L6 22Z"/><path d="m6 17 1-1m2-2 1-1m2-2 1-1m2-2 1-1m1-4 3 3M4 16l4 4"/>)"},
    {"clarinet", "Clarinet", "Winds & brass", R"(<path d="m12 2 2 1-2 3v11l3 4H7l3-4V6l2-4ZM9 7h4M9 11h4M9 15h4m1 0 3-1m-3-4 3-1"/>)"},
    {"trombone", "Trombone", "Winds & brass", R"(<path d="M2 8h12l7-5v13l-7-5H5v6h12q5 0 5-4M3 8v10q0 3 3 3h11M8 11v6"/>)"},
    {"tuba", "Tuba", "Winds & brass", R"(<path d="M12 3h10l-3 6v8q0 5-7 5H8q-6 0-6-6V9h4v7q0 2 3 2h4q2 0 2-2V9l-3-6ZM8 6v9m3-9v9m-4-9h5M2 9V5H0"/>)"},
    {"french-horn", "French horn", "Winds & brass", R"(<circle cx="10" cy="14" r="7"/><circle cx="10" cy="14" r="4"/><path d="M10 7V3H3m7 12h5l7-5v11l-7-3M7 8V5m3 3V5m3 4V5"/>)"},
    {"harmonica", "Harmonica", "Winds & brass", R"(<path d="m2 8 16-4 4 4v9L6 21l-4-4V8Zm0 0 4 4 16-4M6 12v9m3-10v8m3-9v8m3-9v8m3-9v8"/>)"},
    {"synth", "Synthesizer", "Electronic", R"(<rect x="2" y="4" width="20" height="16" rx="2"/><path d="M2 12h20M6 12v8m4-8v8m4-8v8m4-8v8M5 7v2m4-2v2m4-2v2m4-2h3"/>)"},
    {"pads", "Pads", "Electronic", R"(<rect x="3" y="3" width="7" height="7" rx="2"/><rect x="14" y="3" width="7" height="7" rx="2"/><rect x="3" y="14" width="7" height="7" rx="2"/><rect x="14" y="14" width="7" height="7" rx="2"/>)"},
    {"arpeggiator", "Arpeggiator", "Electronic", R"(<path d="M2 20h5v-5h5v-5h5V5h5M3 5h4m5 15h4m4-6h2"/><circle cx="5" cy="19" r="1"/><circle cx="10" cy="14" r="1"/><circle cx="15" cy="9" r="1"/>)"},
    {"drum-machine", "Drum machine", "Electronic", R"(<rect x="2" y="4" width="20" height="16" rx="2"/><path d="M5 7h8v3H5Zm12 0h2M5 14h2v3H5Zm5 0h2v3h-2Zm5 0h2v3h-2M20 14v3"/>)"},
    {"sampler", "Sampler", "Electronic", R"(<rect x="2" y="4" width="20" height="16" rx="2"/><path d="M5 9h2l2-3 3 6 2-4 2 1h3M5 16h2m3 0h2m3 0h2m3 0h1"/>)"},
    {"turntable", "Turntable", "Electronic", R"(<rect x="1" y="4" width="22" height="16" rx="2"/><circle cx="9" cy="12" r="6"/><circle cx="9" cy="12" r="1.5"/><path d="M20 7v7l-3 3M19 7h2M3 18h3"/>)"},
    {"tape", "Tape", "Electronic", R"(<rect x="2" y="5" width="20" height="15" rx="2"/><circle cx="7" cy="11" r="2"/><circle cx="17" cy="11" r="2"/><path d="M9 11h6M6 20l2-4h8l2 4M5 3h14"/>)"},
    {"modular", "Modular synth", "Electronic", R"(<rect x="2" y="3" width="20" height="18" rx="1"/><path d="M9 3v18m7-18v18M5 6v2m7-2v2m7-2v2M5 12q8 12 14 0M5 17q7-12 14 0"/>)"},
    {"microphone", "Microphone", "Voice & recording", R"(<rect x="8" y="2" width="8" height="13" rx="4"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3m-4 0h8M9 6h6m-6 3h6"/>)"},
    {"vocals", "Vocals", "Voice & recording", R"(<circle cx="8" cy="6" r="3"/><path d="M2 21v-3a6 6 0 0 1 12 0v3M16 7q4 3 0 6m3-9q7 6 0 12"/>)"},
    {"choir", "Choir", "Voice & recording", R"(<circle cx="12" cy="6" r="3"/><circle cx="4" cy="9" r="2"/><circle cx="20" cy="9" r="2"/><path d="M7 21v-5a5 5 0 0 1 10 0v5M1 21v-5a3 3 0 0 1 5-2m12 0a3 3 0 0 1 5 2v5"/>)"},
    {"headphones", "Headphones", "Voice & recording", R"(<path d="M3 14V11a9 9 0 0 1 18 0v3"/><rect x="2" y="12" width="5" height="9" rx="2"/><rect x="17" y="12" width="5" height="9" rx="2"/>)"},
    {"speaker", "Speaker", "Voice & recording", R"(<rect x="5" y="2" width="14" height="20" rx="2"/><circle cx="12" cy="15" r="4"/><circle cx="12" cy="6" r="1.5"/>)"},
    {"amp", "Amplifier", "Voice & recording", R"(<rect x="2" y="5" width="20" height="16" rx="2"/><path d="M8 5V2h8v3M2 10h20M5 8h1m3 0h1m3 0h1m3 0h1M6 13l12 5m0-5L6 18"/>)"},
    {"field-recording", "Field recording", "Voice & recording", R"(<rect x="7" y="5" width="10" height="17" rx="2"/><path d="m8 5-4-3m12 3 4-3M10 9h4v4h-4Zm0 8h4"/><circle cx="4" cy="2" r="1"/><circle cx="20" cy="2" r="1"/>)"},
    {"waveform", "Waveform", "Sound & effects", R"(<path d="M2 10v4m3-7v10m3-13v16m4-18v20m4-16v12m3-10v8m3-6v4"/>)"},
    {"sine", "Sine wave", "Sound & effects", R"(<path d="M2 12C5-1 8-1 12 12s7 13 10 0"/>)"},
    {"square", "Square wave", "Sound & effects", R"(<path d="M2 18V6h10v12h10V6"/>)"},
    {"saw", "Saw wave", "Sound & effects", R"(<path d="M2 18 10 5v14L22 5v14"/>)"},
    {"pulse", "Pulse", "Sound & effects", R"(<path d="M1 15h5V5h4v10h6V5h4v10h3"/>)"},
    {"noise", "Noise", "Sound & effects", R"(<path d="m1 12 2-4 2 9 2-14 2 16 2-10 2 5 2-11 2 18 2-13 2 7 2-3"/>)"},
    {"sub-bass", "Sub bass", "Sound & effects", R"(<circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/><path d="M4 5q-6 7 0 14m16-14q6 7 0 14"/>)"},
    {"reverb", "Reverb", "Sound & effects", R"(<path d="M3 7v10m4-14v18m4-15v12m4-10v8m4-6v4m3-3v2"/>)"},
    {"delay", "Delay", "Sound & effects", R"(<path d="M2 17V5h3v12H2Zm7 0V8h3v9H9Zm7 0v-6h3v6h-3ZM3 21h17m-3-2 3 2-3 2"/>)"},
    {"equalizer", "Equalizer", "Sound & effects", R"(<path d="M2 20h20M3 16V9m4 7V5m5 11V2m5 14V7m4 9v-5"/>)"},
    {"compressor", "Compressor", "Sound & effects", R"(<path d="M3 3v18h18M6 18l7-8 8-3M8 3v5m-2-2 2 2 2-2M18 20v-6m-2 2 2-2 2 2"/>)"},
    {"filter", "Filter", "Sound & effects", R"(<path d="M2 20h20M3 7h8q4 0 5 5l3 7M3 3v17"/>)"},
    {"riser", "Riser", "Sound & effects", R"(<path d="M2 20q14 0 19-17m-6 1 6-1 1 6M4 15v2m4-6v5m4-9v6m4-10v6"/>)"},
    {"impact", "Impact", "Sound & effects", R"(<path d="m12 2 2 7 7-4-4 7 5 4-8-1-2 7-3-7-7 2 5-6-4-6 7 4Z"/>)"},
    {"loop", "Loop", "Sound & effects", R"(<path d="M4 8a8 8 0 0 1 14-3l3 3m0-6v6h-6M20 16a8 8 0 0 1-14 3l-3-3m0 6v-6h6"/>)"},
    {"mixer", "Mixer", "Sound & effects", R"(<path d="M5 2v5m0 5v10M12 2v10m0 5v5M19 2v3m0 5v12"/><rect x="3" y="7" width="4" height="5" rx="1"/><rect x="10" y="12" width="4" height="5" rx="1"/><rect x="17" y="5" width="4" height="5" rx="1"/>)"},
    {"automation", "Automation", "Sound & effects", R"(<path d="m3 18 8-12 10 8"/><rect x="1" y="16" width="4" height="4" rx="1"/><rect x="9" y="4" width="4" height="4" rx="1"/><rect x="19" y="12" width="4" height="4" rx="1"/>)"},
};

QCache<QString, QIcon>& cache() { static QCache<QString, QIcon> value(12 * 1024); return value; }
QString translated(const char* value) { return QCoreApplication::translate("TrackIcons", value); }
QJsonArray stored() {
    return QJsonDocument::fromJson(QSettings().value(QLatin1String(kSettingsKey)).toByteArray()).array();
}
void saveEntries(const QJsonArray& entries) {
    QSettings().setValue(QLatin1String(kSettingsKey), QJsonDocument(entries).toJson(QJsonDocument::Compact));
    cache().clear();
    emit ThemeManager::instance().trackIconsChanged();
}
bool customId(const QString& id) {
    static const QRegularExpression valid(QStringLiteral("^custom:[0-9a-f]{64}$"));
    return valid.match(id).hasMatch();
}
QString directory() {
    QString root = qApp->property("dawHeadlessDataRoot").toString();
    if (root.isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(root).filePath(QStringLiteral("Themes/track-icons"));
}
QString digestId(const QByteArray& bytes) {
    return QStringLiteral("custom:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool install(const QString& id, const QString& name, const QByteArray& bytes, QString* error) {
    const auto fail = [error](const QString& message) { if (error) *error = message; return false; };
    if (!customId(id) || bytes.isEmpty() || bytes.size() > kStoredLimit || digestId(bytes) != id)
        return fail(translated("The icon file is invalid."));
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format();
    const QSize size = reader.size();
    if ((format != "png" && format != "webp") || size.isEmpty() ||
        size.width() > kImageSide || size.height() > kImageSide || reader.read().isNull())
        return fail(translated("The icon file is invalid."));
    const QString path = QDir(directory()).filePath(id.mid(7) + QLatin1Char('.') + QString::fromLatin1(format));
    if (!QDir().mkpath(directory())) return fail(translated("Could not save the icon."));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(translated("Could not save the icon."));
    QJsonArray entries = stored();
    for (const auto& entry : entries) if (entry.toObject().value("id").toString() == id) {
        cache().clear();
        emit ThemeManager::instance().trackIconsChanged();
        return true;
    }
    const QString displayName = name.trimmed().isEmpty() ? translated("Custom icon") : name.trimmed().left(80);
    entries.append(QJsonObject{{"id", id}, {"name", displayName}});
    saveEntries(entries);
    return true;
}
}

QVector<Entry> builtins() {
    QVector<Entry> result;
    for (const auto& symbol : symbols)
        result.push_back({QStringLiteral("builtin:") + QLatin1String(symbol.id),
                          translated(symbol.name), translated(symbol.category)});
    return result;
}
QVector<Entry> customIcons() {
    QVector<Entry> result;
    for (const auto& value : stored()) {
        const auto entry = value.toObject();
        const QString id = entry.value("id").toString();
        if (customId(id)) result.push_back({id, entry.value("name").toString().left(80), translated("Custom")});
    }
    return result;
}
QString customPath(const QString& id) {
    if (!customId(id)) return {};
    for (const auto* extension : {".webp", ".png"}) {
        const QString path = QDir(directory()).filePath(id.mid(7) + QLatin1String(extension));
        if (QFileInfo::exists(path)) return path;
    }
    return {};
}
QIcon icon(const QString& id, const QColor& color) {
    if (id.isEmpty()) return {};
    const QString key = id.startsWith(QLatin1String("custom:")) ? id : id + color.name(QColor::HexArgb);
    if (const auto* found = cache().object(key)) return *found;
    QIcon result;
    if (customId(id)) {
        const QString path = customPath(id);
        if (!path.isEmpty()) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) && file.size() <= kStoredLimit) {
                const auto bytes = file.readAll();
                QBuffer buffer;
                buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                const auto size = reader.size();
                if (!size.isEmpty() && size.width() <= kImageSide && size.height() <= kImageSide && digestId(bytes) == id)
                    result = QIcon(QPixmap::fromImage(reader.read()));
            }
        }
    } else {
        for (const auto& symbol : symbols) {
            if (id != QStringLiteral("builtin:") + QLatin1String(symbol.id)) continue;
            const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" color=\"" +
                color.name().toUtf8() + "\" stroke=\"currentColor\" stroke-width=\"1.65\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" + symbol.drawing + "</svg>";
            QSvgRenderer renderer(svg);
            QPixmap pixmap(kImageSide, kImageSide);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            renderer.render(&painter);
            painter.end();
            result = QIcon(pixmap);
            break;
        }
    }
    cache().insert(key, new QIcon(result), kImageSide * kImageSide * 4 / 1024);
    return result;
}
QString importFile(const QString& path, QString* error) {
    const auto fail = [error](const QString& message) -> QString { if (error) *error = message; return {}; };
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > kImportLimit)
        return fail(translated("Choose an image smaller than 32 MB."));
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > 16384 || size.height() > 16384 ||
        qint64(size.width()) * size.height() > 64'000'000)
        return fail(translated("This image is too large or cannot be read."));
    reader.setScaledSize(size.scaled(kImageSide, kImageSide, Qt::KeepAspectRatio));
    QImage source = reader.read();
    if (source.isNull()) return fail(translated("This image is too large or cannot be read."));
    source = source.scaled(kImageSide, kImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QImage image(kImageSide, kImageSide, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.drawImage(QPoint((kImageSide - source.width()) / 2, (kImageSide - source.height()) / 2), source);
    painter.end();
    QByteArray bytes;
    QBuffer output(&bytes); output.open(QIODevice::WriteOnly);
    QImageWriter png(&output, "png"); png.setCompression(100);
    if (!png.write(image)) return fail(translated("Could not prepare the icon."));
    if (QImageReader::supportedImageFormats().contains("webp") && QImageWriter::supportedImageFormats().contains("webp")) {
        QByteArray compressed;
        QBuffer webpOutput(&compressed); webpOutput.open(QIODevice::WriteOnly);
        QImageWriter webp(&webpOutput, "webp"); webp.setQuality(82);
        if (webp.write(image) && compressed.size() < bytes.size()) bytes = compressed;
    }
    const QString id = digestId(bytes);
    return install(id, info.completeBaseName(), bytes, error) ? id : QString();
}
bool removeCustom(const QString& id, QString* error) {
    if (!customId(id)) return false;
    const QString path = customPath(id);
    if (!path.isEmpty() && !QFile::remove(path)) {
        if (error) *error = translated("Could not remove the icon.");
        return false;
    }
    QJsonArray entries;
    for (const auto& value : stored()) if (value.toObject().value("id").toString() != id) entries.append(value);
    saveEntries(entries);
    return true;
}
bool restoreCustom(const QString& id, const QString& name, const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kStoredLimit) {
        if (error) *error = translated("The icon file is invalid.");
        return false;
    }
    return install(id, name, file.readAll(), error);
}
}
