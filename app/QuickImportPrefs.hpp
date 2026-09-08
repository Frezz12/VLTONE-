#pragma once

#include <QString>

namespace ui::quickimport {

struct Preferences {
    QString templatePath;
    QString trackId;
    bool detectTempo = true;
    bool detectKey = true;
};

Preferences load();
void save(const Preferences& preferences);

/// Validate the saved template and stable target-track identity against the
/// document on disk. Returns user-facing detail in `error`.
bool validate(const Preferences& preferences, QString* error = nullptr);

bool checkPreferencesForTest(QString* error = nullptr);

} // namespace ui::quickimport
