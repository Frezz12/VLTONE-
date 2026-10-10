# Command menu synchronization

The menu additions from the previous pass were already in `main`. This pass
finishes the shared context-menu paths and exposes additional existing controls.

| Before | After | Why |
| --- | --- | --- |
| Selection command synthesized a right-click after scrolling to a clip | Edit → Selection Actions populates by clip identity | Overlapping or hidden clips cannot redirect the action; opening a menu preserves the viewport |
| Track and browser commands opened separate popups | Track and Browser expose the same actions as live submenus | Keyboard menu navigation works without an extra popup |
| Take-row and automation-point actions required pointing at the canvas | Selection Actions includes named takes and automation points | Each target is explicit and reachable from the menu bar |
| Fade type depended on the pointer being inside that fade | Both existing fades have their own submenu | Fade In and Fade Out remain independently addressable |
| Editor commands were confined to their editor toolbar | Edit includes Piano Roll, Pattern Editor and Automation Editor actions | Shared QAction instances or builders retain the existing execution paths |
| Secondary Stretch/Glue and the independent position-counter format were absent | Tools and View include these commands and their shortcut-registry entries | Toolbar and menu offer the same choices |
| Empty-lane MIDI/Pattern creation required a pointer location | Edit can create those clips at the playhead on the selected compatible track | A defined insertion position makes the command usable from the keyboard |
| Semantic choices lacked current-state marks | Grid, snap, secondary tools, playback, counter/ruler and browser preview modes refresh their checks on opening | Changes made on the toolbar are reflected in menus |

Context actions show shortcut hints without creating competing global shortcut
bindings. Dynamic menu containers own and discard their handlers; borrowed editor
actions retain their original owner and shortcut scope. Native macOS menus get
separate containers because a QMenu can only occupy one native menu location.

## Validation

- `cmake --build build --target daw -j 6`
- `ctest --test-dir build -R 'command_menu_(en|ru)_ui_test' --output-on-failure`
- Focused browser UI self-test with Russian localization, using isolated settings.
- `VLTONE --patterncheck` and `VLTONE --warpcheck` with the offscreen Qt platform.
- Visual inspection of rendered Russian selection, track and take submenus.

Menu tests cover registry coverage, identity-based targeting, unchanged viewport,
exactly one undoable action, repeat-open ownership, duplicate shortcut prevention,
take/point actions, clip creation, shared editor actions, toolbar transforms and
existing edit-chord routing. The browser test needs permission to start Qt
WebEngine's macOS Mach-port service outside the execution sandbox.

Final result: both registered menu UI tests passed (English and Russian), including
14 behavioral assertions per locale and the existing edit-chord routing checks.
See `menu-sync-ctest.txt` for the CTest result.
