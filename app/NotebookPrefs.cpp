#include "NotebookPrefs.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui::notebookprefs {
namespace {

constexpr double kMaxCueSeconds = 365.0 * 24.0 * 60.0 * 60.0;

QString key(const char* name) {
    return QStringLiteral("notebook/") + QLatin1String(name);
}

QString normalizedExistingFile(const QString& path) {
    const QFileInfo info(path);
    return info.exists() && info.isFile()
               ? QDir::cleanPath(info.absoluteFilePath())
               : QString();
}

bool hasSuffix(const QString& path, const QStringList& suffixes) {
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

QVector<TimedCue> normalizedCues(QVector<TimedCue> cues) {
    QVector<TimedCue> result;
    result.reserve(std::min<qsizetype>(cues.size(), 2000));
    for (TimedCue& cue : cues) {
        cue.text = cue.text.simplified().left(500);
        if (!std::isfinite(cue.seconds) || cue.seconds < 0.0 ||
            cue.seconds > kMaxCueSeconds ||
            cue.text.isEmpty())
            continue;
        result.push_back(std::move(cue));
        if (result.size() == 2000) break;
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const TimedCue& a, const TimedCue& b) {
                         return a.seconds < b.seconds;
                     });
    return result;
}

} // namespace

bool isSupportedBackground(const QString& path) {
    static const QStringList suffixes = {
        QStringLiteral("png"),  QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("webp"),
        QStringLiteral("bmp"),  QStringLiteral("gif"),
        QStringLiteral("avif"), QStringLiteral("mp4"),
        QStringLiteral("m4v"),  QStringLiteral("webm"),
        QStringLiteral("ogv"),  QStringLiteral("ogg"),
        QStringLiteral("mov")};
    return hasSuffix(path, suffixes);
}

bool isSupportedFont(const QString& path) {
    static const QStringList suffixes = {
        QStringLiteral("ttf"), QStringLiteral("otf"),
        QStringLiteral("ttc"), QStringLiteral("woff"),
        QStringLiteral("woff2")};
    return hasSuffix(path, suffixes);
}

QString backgroundPath() {
    const QString path = normalizedExistingFile(
        QSettings().value(key("background")).toString());
    return isSupportedBackground(path) ? path : QString();
}

bool setBackgroundPath(const QString& path) {
    const QString normalized = normalizedExistingFile(path);
    if (normalized.isEmpty() || !isSupportedBackground(normalized)) return false;
    QSettings().setValue(key("background"), normalized);
    return true;
}

void clearBackground() { QSettings().remove(key("background")); }

int backgroundVisibility() {
    return std::clamp(QSettings().value(key("backgroundVisibility"), 38).toInt(),
                      0, 100);
}

void setBackgroundVisibility(int percent) {
    QSettings().setValue(key("backgroundVisibility"),
                         std::clamp(percent, 0, 100));
}

bool animatedBackgroundsEnabled() {
    return QSettings().value(key("animatedBackgrounds"), true).toBool();
}

void setAnimatedBackgroundsEnabled(bool enabled) {
    QSettings().setValue(key("animatedBackgrounds"), enabled);
}

QStringList customFontFiles() {
    QStringList result;
    for (const QString& stored :
         QSettings().value(key("customFonts")).toStringList()) {
        const QString path = normalizedExistingFile(stored);
        if (!path.isEmpty() && isSupportedFont(path) && !result.contains(path))
            result.push_back(path);
    }
    return result;
}

bool addCustomFontFile(const QString& path) {
    const QString normalized = normalizedExistingFile(path);
    if (normalized.isEmpty() || !isSupportedFont(normalized)) return false;
    QStringList files = customFontFiles();
    if (!files.contains(normalized, Qt::CaseInsensitive)) files.push_back(normalized);
    QSettings().setValue(key("customFonts"), files);
    return true;
}

void removeCustomFontFile(const QString& path) {
    QStringList files = customFontFiles();
    files.removeAll(path);
    QSettings().setValue(key("customFonts"), files);
}

bool visible() { return QSettings().value(key("visible"), false).toBool(); }

void setVisible(bool on) { QSettings().setValue(key("visible"), on); }

QString dataDirectory() {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        const QString testRoot = app->property("dawHeadlessDataRoot").toString();
        if (!testRoot.isEmpty())
            return QDir(testRoot).filePath(QStringLiteral("notebook"));
    }
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.vlt");
    return QDir(root).filePath(QStringLiteral("notebook"));
}

QString contentFilePath() {
    return QDir(dataDirectory()).filePath(QStringLiteral("note.html"));
}

QString assetDirectory() {
    return QDir(dataDirectory()).filePath(QStringLiteral("assets"));
}

QString timedCuesFilePath() {
    return QDir(dataDirectory()).filePath(QStringLiteral("timed-text.json"));
}

QVector<TimedCue> timedCues() {
    QFile file(timedCuesFilePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) return {};
    QVector<TimedCue> cues;
    cues.reserve(std::min<qsizetype>(document.array().size(), 2000));
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        cues.push_back({object.value(QStringLiteral("seconds")).toDouble(-1.0),
                        object.value(QStringLiteral("text")).toString()});
    }
    return normalizedCues(std::move(cues));
}

bool saveTimedCues(QVector<TimedCue> cues, QString* error) {
    cues = normalizedCues(std::move(cues));
    QJsonArray array;
    for (const TimedCue& cue : std::as_const(cues)) {
        array.push_back(QJsonObject{
            {QStringLiteral("seconds"), cue.seconds},
            {QStringLiteral("text"), cue.text},
        });
    }
    QDir().mkpath(dataDirectory());
    QSaveFile file(timedCuesFilePath());
    const QByteArray bytes = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        if (error) *error = QStringLiteral("could not save timed text");
        return false;
    }
    return true;
}

int timedCueIndexAt(const QVector<TimedCue>& cues, double seconds) {
    const auto it = std::upper_bound(
        cues.begin(), cues.end(), seconds,
        [](double position, const TimedCue& cue) {
            return position < cue.seconds;
        });
    return it == cues.begin() ? -1 : int(std::distance(cues.begin(), it) - 1);
}

QString timedCueTimeText(double seconds) {
    if (!std::isfinite(seconds)) seconds = 0.0;
    const qint64 totalMilliseconds =
        qint64(std::llround(
            std::clamp(seconds, 0.0, kMaxCueSeconds) * 1000.0));
    const qint64 whole = totalMilliseconds / 1000;
    const int milliseconds = int(totalMilliseconds % 1000);
    const int hours = int(whole / 3600);
    const int minutes = int((whole / 60) % 60);
    const int secs = int(whole % 60);
    if (hours > 0)
        return QStringLiteral("%1:%2:%3.%4")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'))
            .arg(milliseconds, 3, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2.%3")
        .arg(whole / 60, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

bool parseTimedCueTime(const QString& text, double& seconds) {
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (parts.isEmpty() || parts.size() > 3) return false;
    bool ok = false;
    const double tail = parts.back().toDouble(&ok);
    if (!ok || tail < 0.0 || (parts.size() > 1 && tail >= 60.0)) return false;
    seconds = tail;
    double multiplier = 60.0;
    for (qsizetype i = parts.size() - 2; i >= 0; --i) {
        const int unit = parts.at(i).toInt(&ok);
        if (!ok || unit < 0 || (i > 0 && unit >= 60)) return false;
        seconds += unit * multiplier;
        multiplier *= 60.0;
    }
    return std::isfinite(seconds) && seconds <= kMaxCueSeconds;
}

bool timedTextEnabled() {
    return QSettings().value(key("timedTextEnabled"), false).toBool();
}

void setTimedTextEnabled(bool enabled) {
    QSettings().setValue(key("timedTextEnabled"), enabled);
}

QString timedTextFontFamily() {
    return QSettings().value(key("timedTextFontFamily")).toString().trimmed();
}

void setTimedTextFontFamily(const QString& family) {
    QSettings().setValue(key("timedTextFontFamily"), family.trimmed());
}

bool checkPreferencesForTest(QString* error) {
    const QVector<TimedCue> cues{{10.0, QStringLiteral("First")},
                                 {20.0, QStringLiteral("Second")}};
    double parsedTime = -1.0;
    const bool ok = isSupportedBackground(QStringLiteral("cover.gif")) &&
                    isSupportedBackground(QStringLiteral("loop.webm")) &&
                    !isSupportedBackground(QStringLiteral("run.exe")) &&
                    isSupportedFont(QStringLiteral("notes.woff2")) &&
                    !isSupportedFont(QStringLiteral("font.txt")) &&
                    !contentFilePath().isEmpty() &&
                    timedCueIndexAt(cues, 9.99) == -1 &&
                    timedCueIndexAt(cues, 10.0) == 0 &&
                    timedCueIndexAt(cues, 19.99) == 0 &&
                    timedCueIndexAt(cues, 20.0) == 1 &&
                    timedCueTimeText(70.25) == QStringLiteral("01:10.250") &&
                    parseTimedCueTime(QStringLiteral("01:10.250"),
                                      parsedTime) &&
                    std::abs(parsedTime - 70.25) < 0.0001 &&
                    !parseTimedCueTime(QStringLiteral("00:60.000"),
                                       parsedTime);
    if (!ok && error) *error = QStringLiteral("notebook file validation failed");
    return ok;
}

} // namespace ui::notebookprefs
