#pragma once

#include <QString>
#include <QJsonObject>

namespace ui::timelinebackgroundprefs {

enum class MediaKind {
    None,
    Image,
    AnimatedImage,
    Video,
};

/// How local media is mapped onto a resizable UI surface. `Fill` is the
/// resolution-independent default: it preserves aspect ratio and centre-crops
/// to whatever monitor/window the surface currently occupies.
enum class Placement {
    Fill,
    Stretch,
    Tile,
    Center,
};

bool enabled();
void setEnabled(bool enabled);

QString path();
bool setPath(const QString& path);
void clear();

// Internet media overlays the retained local file. An empty object selects
// local media; this state belongs to appearance settings, not the project.
QJsonObject webSource();
void setWebSource(const QJsonObject& source);
void clearWebSource();
quint64 sourceRevision();

int visibility();
void setVisibility(int percent);

int blurRadius();
void setBlurRadius(int pixels);

bool animatedBackgroundsEnabled();
void setAnimatedBackgroundsEnabled(bool enabled);

Placement placement();
void setPlacement(Placement placement);

MediaKind mediaKind(const QString& path);
bool isSupported(const QString& path);
bool checkPreferencesForTest(QString* error = nullptr);

} // namespace ui::timelinebackgroundprefs

namespace ui::headerbackgroundprefs {

bool enabled();
void setEnabled(bool enabled);

QString path();
bool setPath(const QString& path);
void clear();

int visibility();
void setVisibility(int percent);

int blurRadius();
void setBlurRadius(int pixels);

bool animatedBackgroundsEnabled();
void setAnimatedBackgroundsEnabled(bool enabled);

timelinebackgroundprefs::Placement placement();
void setPlacement(timelinebackgroundprefs::Placement placement);

} // namespace ui::headerbackgroundprefs
