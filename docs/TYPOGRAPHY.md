# VLTONE typography

The default application family is **Inter**, embedded from the 18 original TTF
files in `fonts/`. Qt registers the complete family before any windows are
created. Packaging includes the font resources and the upstream OFL license.
The source folder and a system font installation are not needed at runtime.

| Role | Face | Purpose |
| --- | --- | --- |
| Body text, menus, parameter fields, secondary labels | Regular 400 | Readable compact text without excess weight |
| Buttons, tool buttons, combo boxes | Medium 500 | Distinguish controls from surrounding labels |
| Section and group headings, startup status | SemiBold 600 | A restrained hierarchy in dense panels |
| Main transport position and tempo, product title | Bold 700 | Make primary readings easy to find |
| Emphasis in notebook text | Italic, matching the selected weight | Use real italic outlines |
| Light, ExtraLight, Thin, ExtraBold, Black and their italics | Registered and available | Available for authored text; not defaults for small controls |

Existing component-specific font sizes and stronger emphasis are retained. The
transport uses Inter with OpenType `tnum` so digits have equal advance widths.
Intentional monospaced code and diagnostic readouts remain monospaced. Third-party
plugin editors and external websites retain their own typography.

App-owned browser pages and the notebook load the same embedded files through
`@font-face` and a CORS-enabled, read-only font resource handler; the notebook's existing explicit document formatting is preserved.
The interface font import remains available. Resetting it restores Inter and
keeps each control's size, weight and numeric features.

| Before | After | Why |
| --- | --- | --- |
| Platform font for ordinary widgets; separate transport families | Bundled Inter with real weights and tabular transport figures | Consistent layout and typography on different computers |
| System fonts inside app-owned HTML views | Embedded Inter font faces | Match native panels without requiring installed fonts |
| Default font described as a system font | Default font explicitly shown as Inter | Make the active default and reset behavior clear |

Validation: desktop selftest checks all 18 resolved faces, Cyrillic coverage,
custom font import/reset, preservation of an existing bold control, and actual
WebEngine font loading through the notebook's content security policy.
