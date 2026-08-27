#include "RecoveryPrefs.hpp"

#include <QSettings>
#include <QString>

namespace ui::recoveryprefs {

namespace {
QString key(const char* name) {
    return QStringLiteral("recovery/") + QLatin1String(name);
}
} // namespace

bool enabled() { return QSettings().value(key("enabled"), true).toBool(); }

void setEnabled(bool on) { QSettings().setValue(key("enabled"), on); }

bool watchdog() { return QSettings().value(key("watchdog"), true).toBool(); }

void setWatchdog(bool on) { QSettings().setValue(key("watchdog"), on); }

} // namespace ui::recoveryprefs
