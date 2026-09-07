# Adaptive timeline grid

When one bar becomes narrower than 32 logical pixels, Adaptive switches to
whole-bar groups: 2, 4, 8, 16 bars and onward as needed. Lines stay 32–64 logical
pixels apart. The ruler uses the same groups; its labels use power-of-two bar
intervals with at least 60 pixels (or the measured text width plus padding).
Everything remains anchored to bar 1, including when panning.

At 120 BPM in 4/4 and the minimum zoom of 4 px/s, grid lines occur every four
bars and labels read 1, 9, 17, 25. At 240 BPM the grid step is eight bars.
Adaptive snapping follows the visible grid. Closer editing scales retain the
existing subdivision ladder; fixed divisions and Off keep their snap behavior.
Bar length includes the denominator: 6/8 is three quarter-note beats, and 7/8
is three and a half.

| Before | After | Why |
| --- | --- | --- |
| Adaptive stopped at four quarter-note beats | Whole-bar groups continue doubling at overview scales | Distant views show readable structure |
| Every bar and some beats were still drawn | Overview draws only the selected bar groups | Hidden detail cannot make the grid dense again |
| Ruler labels used arbitrary integer strides | Labels follow powers of two from bar 1 | Stable musical grouping and readable labels |

The installed Apple HIG and Emil Design Engineering skills inform the density,
hierarchy and label spacing. `--uiperfcheck` verifies rasterized line spacing
and snap alignment while panned, across three tempos and 4/4, 3/4, 6/8 and 7/8.
It also checks that close zoom, fixed subdivisions and Off retain their behavior.
Set `VLT_GRID_SCREENSHOT` to an image path to capture the overview grid and ruler.
