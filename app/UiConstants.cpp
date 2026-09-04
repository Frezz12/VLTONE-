#include "UiConstants.hpp"

#include <QSettings>

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

} // namespace

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

} // namespace ui
