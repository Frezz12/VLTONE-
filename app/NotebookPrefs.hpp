#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace ui::notebookprefs {

struct TimedCue {
    double seconds = 0.0;
    QString text;
};

QString backgroundPath();
bool setBackgroundPath(const QString& path);
void clearBackground();

int backgroundVisibility();
void setBackgroundVisibility(int percent);

bool animatedBackgroundsEnabled();
void setAnimatedBackgroundsEnabled(bool enabled);

QStringList customFontFiles();
bool addCustomFontFile(const QString& path);
void removeCustomFontFile(const QString& path);

bool visible();
void setVisible(bool visible);

QString dataDirectory();
QString contentFilePath();
QString assetDirectory();
QString timedCuesFilePath();

QVector<TimedCue> timedCues();
bool saveTimedCues(QVector<TimedCue> cues, QString* error = nullptr);
int timedCueIndexAt(const QVector<TimedCue>& cues, double seconds);
QString timedCueTimeText(double seconds);
bool parseTimedCueTime(const QString& text, double& seconds);

bool timedTextEnabled();
void setTimedTextEnabled(bool enabled);

/// Empty means the transport position display face.
QString timedTextFontFamily();
void setTimedTextFontFamily(const QString& family);

bool isSupportedBackground(const QString& path);
bool isSupportedFont(const QString& path);
bool checkPreferencesForTest(QString* error = nullptr);

} // namespace ui::notebookprefs
