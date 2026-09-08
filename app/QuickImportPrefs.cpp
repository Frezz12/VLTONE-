#include "QuickImportPrefs.hpp"

#include "ProjectTemplates.hpp"
#include "ProjectSerializer.hpp"

#include <QSettings>
#include <QHash>
#include <QVariant>
#include <QTemporaryDir>
#include <QDir>

namespace ui::quickimport {
namespace {
constexpr auto kTemplatePath = "quickImport/templatePath";
constexpr auto kTrackId = "quickImport/trackId";
constexpr auto kDetectTempo = "quickImport/detectTempo";
constexpr auto kDetectKey = "quickImport/detectKey";
}

Preferences load() {
    QSettings settings;
    Preferences result;
    result.templatePath = settings.value(QLatin1String(kTemplatePath)).toString();
    result.trackId = settings.value(QLatin1String(kTrackId)).toString();
    result.detectTempo = settings.value(QLatin1String(kDetectTempo), true).toBool();
    result.detectKey = settings.value(QLatin1String(kDetectKey), true).toBool();
    return result;
}

void save(const Preferences& preferences) {
    QSettings settings;
    settings.setValue(QLatin1String(kTemplatePath), preferences.templatePath);
    settings.setValue(QLatin1String(kTrackId), preferences.trackId);
    settings.setValue(QLatin1String(kDetectTempo), preferences.detectTempo);
    settings.setValue(QLatin1String(kDetectKey), preferences.detectKey);
}

bool validate(const Preferences& preferences, QString* error) {
    if (error) error->clear();
    if (preferences.templatePath.isEmpty()) {
        if (error) *error = QObject::tr("Choose a project template for Quick Import.");
        return false;
    }
    if (!projecttemplates::isTemplatePackage(preferences.templatePath)) {
        if (error) *error = QObject::tr("The selected Quick Import template is not a VLTONE template.");
        return false;
    }
    QString loadError;
    const QVector<projecttemplates::AudioTarget> targets =
        projecttemplates::audioTargets(preferences.templatePath, &loadError);
    if (!loadError.isEmpty()) {
        if (error) *error = QObject::tr("The selected template could not be read: %1").arg(loadError);
        return false;
    }
    if (preferences.trackId.isEmpty()) {
        if (error) *error = QObject::tr("Choose an audio track for Quick Import.");
        return false;
    }
    for (const auto& target : targets) {
        if (target.trackId == preferences.trackId) return true;
    }
    if (error) *error = QObject::tr("The selected audio track no longer exists in this template.");
    return false;
}

bool checkPreferencesForTest(QString* error) {
    QSettings settings;
    const QStringList keys = {QLatin1String(kTemplatePath), QLatin1String(kTrackId),
                              QLatin1String(kDetectTempo), QLatin1String(kDetectKey)};
    QHash<QString, QVariant> previous;
    for (const QString& key : keys) previous.insert(key, settings.value(key));
    const auto restore = [&] {
        for (const QString& key : keys) {
            if (previous.value(key).isValid())
                settings.setValue(key, previous.value(key));
            else
                settings.remove(key);
        }
    };

    for (const QString& key : keys) settings.remove(key);
    const Preferences defaults = load();
    if (!defaults.detectTempo || !defaults.detectKey ||
        !defaults.templatePath.isEmpty() || !defaults.trackId.isEmpty()) {
        if (error) *error = QStringLiteral("Quick Import defaults are invalid");
        restore();
        return false;
    }
    const Preferences expected{QStringLiteral("/tmp/test.vltt"),
                               QStringLiteral("track-id"), false, true};
    save(expected);
    const Preferences actual = load();
    const bool ok = actual.templatePath == expected.templatePath &&
                    actual.trackId == expected.trackId &&
                    actual.detectTempo == expected.detectTempo &&
                    actual.detectKey == expected.detectKey;
    if (!ok) {
        if (error) *error = QStringLiteral("Quick Import preferences did not round-trip");
        restore();
        return false;
    }

    QTemporaryDir temporary;
    const QString package = QDir(temporary.path()).filePath(QStringLiteral("Target Test.vltt"));
    daw::ProjectModel model;
    daw::TrackModel folder;
    folder.id = "folder";
    folder.kind = daw::TrackKind::Folder;
    folder.name = "Vocals";
    daw::TrackModel audio;
    audio.id = "audio-target";
    audio.kind = daw::TrackKind::Audio;
    audio.name = "Lead";
    audio.parentId = folder.id;
    daw::TrackModel duplicate = audio;
    duplicate.name = "Duplicate";
    daw::TrackModel midi;
    midi.id = "midi";
    midi.kind = daw::TrackKind::Midi;
    midi.name = "Guide";
    model.tracks = {folder, audio, duplicate, midi};
    if (!temporary.isValid() ||
        !daw::ProjectSerializer::save(model, package.toStdString())) {
        if (error) *error = QStringLiteral("Quick Import target fixture could not be saved");
        restore();
        return false;
    }
    QString targetError;
    const auto targets = projecttemplates::audioTargets(package, &targetError);
    Preferences valid{package, QStringLiteral("audio-target"), true, true};
    Preferences missing{package, QStringLiteral("missing"), true, true};
    const bool targetsOk = targetError.isEmpty() && targets.size() == 1 &&
                           targets.front().trackId == valid.trackId &&
                           targets.front().displayPath ==
                               QStringLiteral("Vocals / Lead") &&
                           validate(valid) && !validate(missing);
    restore();
    if (!targetsOk && error)
        *error = QStringLiteral("Quick Import audio-target filtering or validation failed");
    return targetsOk;
}

} // namespace ui::quickimport
