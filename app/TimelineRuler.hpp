#pragma once

namespace ui {

// Keep legacy settings compatible. Timeline normalizes Time to BarsAndTime:
// bars are fixed in the header, while time is an optional arrangement row.
enum class RulerFormat { Bars = 1, Time = 2, BarsAndTime = 3 };

constexpr RulerFormat rulerFormatFromInt(int value) {
    return value >= 1 && value <= 3 ? static_cast<RulerFormat>(value)
                                  : RulerFormat::Bars;
}
constexpr bool rulerShowsBars(RulerFormat format) {
    return (static_cast<int>(format) & 1) != 0;
}
constexpr bool rulerShowsTime(RulerFormat format) {
    return (static_cast<int>(format) & 2) != 0;
}
inline constexpr const char* kRulerFormatSetting = "timeline/rulerFormat";

} // namespace ui
