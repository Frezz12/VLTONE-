#include "UiConstants.hpp"

#include <QSettings>
#include <QApplication>
#include <QFontDatabase>

#include <algorithm>

namespace ui {
namespace {

SelectionTint readSelectionTint() {
    const int stored =
        QSettings().value(kSelectionTintSetting, int(SelectionTint::TrackColour))
            .toInt();
    return SelectionTint(std::clamp(stored, 0, int(SelectionTint::Neutral)));
}

SelectionTint& cachedSelectionTint() {
    // Selection washes are queried from several paint paths. QSettings may
    // touch the platform preferences backend even for a read, so initialise
    // once and keep the cache in step through the only public setter.
    static SelectionTint tint = readSelectionTint();
    return tint;
}

double readPlayheadWidth() {
    const double stored =
        QSettings().value(kPlayheadWidthSetting, kPlayheadWidthDefault)
            .toDouble();
    if (!(stored > 0.0)) return kPlayheadWidthDefault;
    return std::clamp(stored, kPlayheadWidthMin, kPlayheadWidthMax);
}

double& cachedPlayheadWidth() {
    // Read on every playhead frame, which is every 16 ms while the transport
    // runs — the same reasoning as the selection tint: read once, and keep the
    // cache in step through the only public setter.
    static double width = readPlayheadWidth();
    return width;
}

bool& cachedPlayheadTrail() {
    static bool trail = QSettings().value(kPlayheadTrailSetting, true).toBool();
    return trail;
}

bool& cachedDuplicateTrackClips() {
    static bool include =
        QSettings().value(kDuplicateTrackClipsSetting, false).toBool();
    return include;
}

} // namespace

QFont transportDisplayFont(int pixelSize, QFont::Weight weight) {
    static const QString family = [] {
#if defined(Q_OS_MACOS)
        const QStringList preferred{QStringLiteral("Menlo"),
                                    QStringLiteral("SF Mono")};
#elif defined(Q_OS_WIN)
        const QStringList preferred{QStringLiteral("Cascadia Mono"),
                                    QStringLiteral("Consolas")};
#else
        const QStringList preferred{QStringLiteral("DejaVu Sans Mono"),
                                    QStringLiteral("Liberation Mono")};
#endif
        const QStringList installed = QFontDatabase::families();
        for (const QString& candidate : preferred) {
            for (const QString& available : installed) {
                if (candidate.compare(available, Qt::CaseInsensitive) == 0)
                    return available;
            }
        }
        // Inter is bundled and has tabular figures enabled below, so even a
        // minimal system image gets stable counter width without an alias scan.
        return QStringLiteral("Inter");
    }();
    QFont font(family);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

QFont transportControlFont(int pixelSize, QFont::Weight weight) {
    QFont font(QStringLiteral("Inter"));
    font.setStyleName(QString());
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    font.setFeature(QFont::Tag("tnum"), 1);
    return font;
}

SelectionTint selectionTint() {
    return cachedSelectionTint();
}

void setSelectionTint(SelectionTint tint) {
    const SelectionTint value = SelectionTint(
        std::clamp(int(tint), 0, int(SelectionTint::Neutral)));
    cachedSelectionTint() = value;
    QSettings().setValue(kSelectionTintSetting, int(value));
}

QColor selectionWash(const QColor& trackColor) {
    const Theme& t = th();
    if (selectionTint() == SelectionTint::Neutral) {
        // Not white, and not the accent: a wash of the foreground, which is
        // whatever reads against the surface in this palette. On a dark theme
        // that is a pale grey, on a light one a dim one — either way it says
        // "selected" without claiming a colour of its own.
        return t.textPrimary;
    }
    // The track's colour, lifted towards the text so a dark track still shows a
    // difference against the row it is sitting on. Its own colour is what the
    // eye is already using to find this track.
    return mixColors(trackColor, t.textPrimary, t.dark ? 0.10 : 0.0);
}

double playheadWidth() {
    return cachedPlayheadWidth();
}

void setPlayheadWidth(double pixels) {
    const double value =
        std::clamp(pixels, kPlayheadWidthMin, kPlayheadWidthMax);
    cachedPlayheadWidth() = value;
    QSettings().setValue(kPlayheadWidthSetting, value);
}

bool playheadTrail() {
    return cachedPlayheadTrail();
}

void setPlayheadTrail(bool enabled) {
    cachedPlayheadTrail() = enabled;
    QSettings().setValue(kPlayheadTrailSetting, enabled);
}

bool duplicateTrackClips() {
    return cachedDuplicateTrackClips();
}

void setDuplicateTrackClips(bool enabled) {
    cachedDuplicateTrackClips() = enabled;
    QSettings().setValue(kDuplicateTrackClipsSetting, enabled);
}

} // namespace ui
