# Browser redesign — 2026-09-13

Replaced the integrated browser chrome and the entire built-in start page using the supplied dark browser reference: charcoal surfaces, compact tabs and address bar, a circular sidebar, red search action and quick-access tiles. The final page uses VLTONE's own logo and exactly four pinned services: YouTube, SoundCloud, Splice and Spotify. Discover Music, resource cards and the headphones sidebar button were removed at the user's request.

The existing tabs, navigation, sessions, bookmarks, downloads, audio-import signals and custom background support remain connected. Adding bookmarks is available in the native bookmarks menu; saved bookmarks do not replace the four pinned services. The optional bookmarks bar starts hidden unless explicitly enabled. The browser keeps its own dark palette independently of the arrangement theme.

Validation:
- Release targets `daw` and `browser_navigation_test` built successfully.
- `ctest --test-dir build-windows -R '^browser_(navigation|proxy)_test$' --output-on-failure`: 2/2 passed.
- Browser checks cover responsive layouts at 900/520/320 px, exactly four pinned services, the loaded brand logo, removal of discovery content, native settings and shortcut actions, bookmark persistence and escaping, invalid addresses, search submission, empty history, normal navigation, redirects, popup POST, network recovery and background caching.
- Russian application screenshot verified with isolated preferences and an offline start page.
- `git diff --check` passed for changed source files.

Screenshots are direct Qt application captures:
- `browser-900.png`, `browser-520.png`, `browser-320.png`: browser component.
- `browser-in-app-ru.png`: integrated browser in the Russian desktop application.

Executable: `build-windows/bin/VLTONE.exe`.
