#include "StripSilencePreferences.hpp"
#include "EngineController.hpp"
#include <QSettings>

namespace ui::silence {
namespace {
struct Field { const char* key; double daw::StripSilenceSettings::* value; };
constexpr Field fields[] = {
    {"thresholdDb", &daw::StripSilenceSettings::thresholdDb},
    {"hysteresisDb", &daw::StripSilenceSettings::hysteresisDb},
    {"minimumSilenceMs", &daw::StripSilenceSettings::minimumSilenceMs},
    {"minimumSoundMs", &daw::StripSilenceSettings::minimumSoundMs},
    {"preRollMs", &daw::StripSilenceSettings::preRollMs},
    {"postRollMs", &daw::StripSilenceSettings::postRollMs},
    {"fadeMs", &daw::StripSilenceSettings::fadeMs},
    {"gridBeats", &daw::StripSilenceSettings::gridBeats}
};
}
void restore(daw::EngineController& controller) {
    QSettings stored;
    auto prefs = controller.recordingPrefs();
    prefs.autoSilence = stored.value("recording/autoSilence", false).toBool();
    stored.beginGroup("stripSilence");
    for (const auto& field : fields)
        prefs.stripSilence.*(field.value) = stored.value(field.key, prefs.stripSilence.*(field.value)).toDouble();
    prefs.stripSilence.splitInternal = stored.value("splitInternal", true).toBool();
    controller.setRecordingPrefs(prefs);
}
void persist(const daw::StripSilenceSettings& settings) {
    QSettings stored;
    const auto safe = daw::sanitizedStripSilenceSettings(settings);
    stored.beginGroup("stripSilence");
    for (const auto& field : fields) stored.setValue(field.key, safe.*(field.value));
    stored.setValue("splitInternal", safe.splitInternal);
}
void setAutomatic(daw::EngineController& controller, bool enabled) {
    auto prefs = controller.recordingPrefs();
    prefs.autoSilence = enabled;
    controller.setRecordingPrefs(prefs);
    QSettings().setValue("recording/autoSilence", enabled);
}
}
