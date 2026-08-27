#include "ExportPrefs.hpp"

#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace ui::exportprefs {
namespace {

QString key(const char* name) {
    return QStringLiteral("export/") + QLatin1String(name);
}

/// Enums are stored as ints and read back through an explicit range check.
/// A blind static_cast turns a settings file from a newer build — or a corrupt
/// one — into an out-of-range enum that every switch below silently mishandles.
template <typename Enum>
Enum readEnum(const char* name, Enum fallback, int count) {
    const int stored = QSettings().value(key(name), int(fallback)).toInt();
    return (stored >= 0 && stored < count) ? Enum(stored) : fallback;
}

constexpr int kContainerCount = 8;  // Wav … W64
constexpr int kEncodingCount = 8;   // Int16 … Mp3
constexpr int kRangeCount = 3;
constexpr int kTailCount = 3;
constexpr int kChannelsCount = 2;

} // namespace

daw::rendering::Spec load() {
    QSettings settings;
    daw::rendering::Spec spec;

    spec.file.container = readEnum(
        "container", audio::platform::Container::Wav, kContainerCount);
    spec.file.encoding = readEnum(
        "encoding", audio::platform::Encoding::Int24, kEncodingCount);
    spec.file.vbrQuality =
        std::clamp(settings.value(key("vbrQuality"), 0.7).toDouble(), 0.0, 1.0);
    spec.file.bitrateKbps =
        std::clamp(settings.value(key("bitrateKbps"), 320).toInt(), 0, 320);

    spec.sampleRate =
        std::clamp(settings.value(key("sampleRate"), 0.0).toDouble(), 0.0, 192000.0);
    spec.channels = readEnum("channels", daw::rendering::Channels::Stereo,
                             kChannelsCount);

    spec.range = readEnum("range", daw::rendering::Range::WholeProject, kRangeCount);
    spec.tail = readEnum("tail", daw::rendering::Tail::None, kTailCount);
    spec.tailSeconds =
        std::clamp(settings.value(key("tailSeconds"), 2.0).toDouble(), 0.0, 600.0);
    spec.tailSilenceDb = std::clamp(
        settings.value(key("tailSilenceDb"), -96.0).toDouble(), -160.0, -20.0);
    spec.tailMaxSeconds = std::clamp(
        settings.value(key("tailMaxSeconds"), 30.0).toDouble(), 1.0, 600.0);

    spec.writeMixdown = settings.value(key("writeMixdown"), true).toBool();
    spec.bypassChannelInserts =
        settings.value(key("bypassChannelInserts"), false).toBool();
    spec.bypassMasterChain =
        settings.value(key("bypassMasterChain"), false).toBool();
    spec.ignoreMuteSolo = settings.value(key("ignoreMuteSolo"), false).toBool();
    spec.stemsPreFader = settings.value(key("stemsPreFader"), false).toBool();
    // Dither defaults on: at 16 bits the alternative is audible distortion on
    // every quiet fade, and it costs nothing anywhere else because the writer
    // ignores it for float and lossy targets.
    spec.file.dither = settings.value(key("dither"), true).toBool();
    spec.preRollSeconds =
        std::clamp(settings.value(key("preRoll"), 0.0).toDouble(), 0.0, 60.0);
    spec.tags.artist = settings.value(key("artist")).toString().toStdString();
    return spec;
}

void save(const daw::rendering::Spec& spec) {
    QSettings settings;
    settings.setValue(key("container"), int(spec.file.container));
    settings.setValue(key("encoding"), int(spec.file.encoding));
    settings.setValue(key("vbrQuality"), spec.file.vbrQuality);
    settings.setValue(key("bitrateKbps"), spec.file.bitrateKbps);
    settings.setValue(key("sampleRate"), spec.sampleRate);
    settings.setValue(key("channels"), int(spec.channels));
    settings.setValue(key("range"), int(spec.range));
    settings.setValue(key("tail"), int(spec.tail));
    settings.setValue(key("tailSeconds"), spec.tailSeconds);
    settings.setValue(key("tailSilenceDb"), spec.tailSilenceDb);
    settings.setValue(key("tailMaxSeconds"), spec.tailMaxSeconds);
    settings.setValue(key("writeMixdown"), spec.writeMixdown);
    settings.setValue(key("bypassChannelInserts"), spec.bypassChannelInserts);
    settings.setValue(key("bypassMasterChain"), spec.bypassMasterChain);
    settings.setValue(key("ignoreMuteSolo"), spec.ignoreMuteSolo);
    settings.setValue(key("stemsPreFader"), spec.stemsPreFader);
    settings.setValue(key("dither"), spec.file.dither);
    settings.setValue(key("preRoll"), spec.preRollSeconds);
    settings.setValue(key("artist"), QString::fromStdString(spec.tags.artist));
}

QString lastFolder() {
    const QString stored = QSettings().value(key("folder")).toString();
    if (!stored.isEmpty()) return stored;
    const QString music =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return music.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
               : music;
}

void setLastFolder(const QString& folder) {
    if (!folder.isEmpty()) QSettings().setValue(key("folder"), folder);
}

} // namespace ui::exportprefs
