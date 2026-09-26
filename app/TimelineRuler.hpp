#pragma once

namespace ui {

// Stored as a bit mask so the two ruler rows can be enabled independently.
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
