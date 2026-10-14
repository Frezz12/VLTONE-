# Browser navigation and background collection

## Browser workspace

The browser chrome and offline start page use the active VLTONE theme, including
the light and custom palettes, shared Inter font, eight-pixel corners and native
glyphs. The start page keeps the four music shortcuts and adds the user's saved
bookmarks and recent pages. Local image/video backgrounds and reduced motion
preferences still apply. Website content keeps its own styling.

The rail opens searchable bookmarks, history and this session's downloads in
the existing browser area. The tab-strip chevron opens searchable tabs. These
views replace the viewport temporarily, preserving each page and its navigation
state; Escape or the close button returns to it. The same views fit a 320-pixel
panel. Bookmark titles and addresses are escaped in the offline page.

Tab context menus include duplicate, mute/unmute, mute other tabs, close others,
close to the right and reopen. The built-in VLT Studio tab remains fixed. Pending
addresses are retained before Chromium commits a page, and closing the panel's
owner flushes the session save timer so a quick exit does not lose new tabs.

Downloads show their size and status, remain listed after completion, and offer
pause/resume/cancel, opening the downloads folder and copying the path. Downloaded
audio retains the existing project-import flow and can be imported again from
its completed row. Files are never launched automatically. The download list is
session-only; downloaded files remain on disk.

| Shortcut | Action |
| --- | --- |
| Ctrl+L / F6 | Focus address field |
| Alt+Enter in address field | Open entered address or search in a new tab |
| Ctrl+T / Ctrl+W / Ctrl+Shift+T | New / close / reopen tab |
| Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+1…9 | Select tab |
| Ctrl+Shift+A | Search open tabs |
| Ctrl+H / Ctrl+J | History / downloads |
| Ctrl+D / Ctrl+Shift+B | Bookmark page / toggle bookmarks bar |
| Ctrl+F / F3 / Shift+F3 | Find / next / previous match, with match count |
| Ctrl++ or Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| F5 / Ctrl+R | Reload page (F5 stops an active load) |
| Escape | Close library/find or cancel address editing; otherwise stop loading |

`browser_navigation_test`, `browser_quick_navigation_test` and
`browser_proxy_test` cover both rendering backends, actual local HTTP navigation,
find results, download pause/resume/cancel, library filtering, theme switching,
pending session saves and the existing popup/POST/proxy/background flows. Pass
`--capture`, `VLT_BROWSER_CAPTURE_DIR` and optionally `VLT_BROWSER_RU_QM` to capture
the real controls at 320/520/900 pixels in dark, light and Studio Gray themes.

The embedded browser keeps Chromium's own network error pages. A cancelled,
superseded or download navigation is not treated as a new error-page navigation.
`QWebEngineLoadingInfo` supplies the real error code and description for the
status message. Reload retains the failed URL. New windows are transferred with
`QWebEngineNewWindowRequest::openIn`, preserving POST bodies and `window.open`
contexts, including initially empty popup URLs.

Video discovery ignores invalid JavaScript results delivered during Chromium
page destruction. QPointer alone is insufficient during the derived destructor.
Closing tabs also clears navigation callbacks before deferred view deletion.

## Proxy and VPN

Application startup explicitly enables `QNetworkProxyFactory` system configuration
before creating networking clients or WebEngine profiles. This uses the OS proxy
configuration, including a local VPN proxy registered as the system proxy/PAC.
A tunnel/TUN VPN continues to use the operating system's routes. A VPN exposing
only a local listening port needs its **System proxy** mode enabled. The browser
does not guess ports or overwrite the computer's proxy settings. Reload tabs
after changing connectivity; restart VLTONE if Chromium retains an old connection.

Certificate validation remains enabled. This does not guarantee compatibility
with every site's Chromium version requirements, codec/DRM requirements or VPN
provider. Exact failing URLs and error codes are needed to diagnose those cases.

Qt reference: <https://doc.qt.io/qt-6/qtwebengine-overview.html#proxy-support>.

## Background collection

In admin, **Фоны браузера** (`/browser-backgrounds`) supports upload, preview,
title, numeric display order, publishing/hiding and deletion. Upload publishes
immediately. Images may be PNG/JPEG/WebP, at most 10 MiB and 16 megapixels; the
collection holds at most 100 entries. Images are decoded and re-encoded to remove
metadata, with a separate thumbnail bounded to 480 × 300. Original dimensions
are preserved; sanitized output is capped at 20 MiB. Mutations require the admin
session and CSRF protection and produce audit records. Hidden images are only
served through authenticated admin endpoints.

In the desktop browser settings, **Коллекция фонов → Открыть коллекцию фонов…**
shows published thumbnails. Choosing an image downloads it from the configured
account API, verifies SHA-256, saves it atomically and applies it through the
existing start-page background preference. Local image/video selection remains
available. The selection and cached catalog work offline. Hiding or deleting a
server entry does not erase an image already selected on a user's computer.
The collection is for the browser start page; external web pages retain their
own content and styling.

Deploy backend migration **000022_browser_backgrounds** and the updated API,
then admin and desktop. Include `StorageRoot/browser-backgrounds` in server
backups along with the database. No migration has been applied to production
by these source changes.

## Verification

- `browser_navigation_test`: real Chromium navigation cancellation, redirects,
  empty popups, POST popup, network failure recovery, rapid tab closure, catalog
  thumbnails, corrupt download rejection, valid download and offline selection.
- `browser_proxy_test`: actual Chromium HTTP traffic through a local forward
  proxy to a reserved `.invalid` destination; no computer proxy changes.
- `web_video_background_test`: video discovery, iframe/dynamic sources and
  background playback, ensuring teardown guards retain ordinary operation.
- Go `TestPostgresAccountFlow/browser_backgrounds`: migration up/down, 4K
  upload, metadata, thumbnail/hash, session/CSRF enforcement, hide and delete.
- Admin Playwright tests: upload/edit/hide/delete at 1440 and 375 px, metadata
  refresh, overflow checks and multipart streaming through the Next server.
