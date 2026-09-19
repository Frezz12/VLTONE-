# VLTONE 0.2.4 — release preparation, 2026-09-19

## Windows artifacts

- Installer: `build-windows/artifacts/VLTONE-0.2.4-x64-Setup.exe` (235,211,817 bytes).
- SHA-256: `ea422c6aa4556c67b07051c56e85874e93c3bceae772a8ebd0849d419a626854`.
- Portable archive: `build-windows/artifacts/VLTONE-0.2.4-windows-x64.zip`.
- Application and installer report version `0.2.4.0`.
- Release build uses the existing Windows packaging workflow, with collaboration disabled as in the previous Windows package.
- The Windows ICO was regenerated from the light application artwork at 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 pixels. Icons extracted from the actual application and installer were visually checked.
- Runtime dependency validation passed for all 86 PE files. Packaging completed successfully.
- Real VLT Pitch editor, compact startup and startup-scan settings screenshots were captured and inspected.

## Application validation

83 of 84 CTest suites passed after environment-related retries. The initial update-checker DLL-path and account-restore local-socket failures passed with the Qt runtime on PATH and local network access available.

`audio_timeline_scroll_test` remains a performance failure. A serial retry after the builds finished reported paint p95 of 51.369 ms for the 64-frame, non-recording scenario; the other seven scenarios passed the existing paint threshold. Audio callbacks reported zero deadline overruns in that run, and the navigation behavior assertions passed. The threshold was not changed. This is an unresolved performance validation item, not a fully green test run.

Logs: `.tmp/release-0.2.4/ctest.log`, `ctest-recheck.log`, `ctest-scroll-idle.log`, and `windows-package.log`.

## Admin dashboard

- Added global application hours, launch/user counters for the last 24 hours, per-user usage with search and pagination, and the 20 latest launches with version and session state.
- Durations stop at the last report for unfinished sessions, clamp negative/future durations, and count overlapping sessions separately. The UI explicitly labels this as application-open time rather than active editing time.
- Sidebar is compact, divided into five categories, with flat active states and no decorative glow.
- Type checking, production builds and relevant Playwright tests passed. Desktop/mobile screenshots were inspected; the usage tables scroll inside their containers on small screens.
- PostgreSQL account-flow integration, including duration/search/pagination fixtures, passed on a separate disposable database. The test database was dropped afterwards.

## Server deployment

API and admin were deployed to `/opt/vlt-account-platform/releases/0.2.4-admin-20260919-174226`. The public web build was preserved byte-for-byte and its service was not restarted. All 24 migration files matched the deployed schema; no migration was required.

Rollback release: `/opt/vlt-account-platform/releases/0.2.4-platform-20260915`.
Pre-deployment database backup: `/var/backups/vlt/before-admin024-20260919-174226/database.dump`.

The Linux admin was first started on a separate port and its login page, JS/CSS assets and authentication boundary were checked. After switching, API readiness, admin login, public website and all three services passed checks. Authenticated API totals matched direct SQL: 861,977.387 seconds across 682 sessions; 31 launches and 10 users over 24 hours at verification time. Search and pagination passed. Unauthenticated dashboard requests return 401.

## Release draft

Draft ID: `275fc96b-d54b-464d-8a3e-e85f5baa8fd6`, version `0.2.4`.

Russian and English release notes were saved, with a separate VLT Pitch description and screenshots. Four screenshots were initially uploaded; subsequent user edits to captions and removal of two screenshots were preserved. Final verification found two screenshots, no installer artifacts and no publication timestamp. Each remaining screenshot's downloaded SHA-256 matched storage metadata. The public release endpoint returns 404 while the release is a draft.

The installer was not uploaded and the release was not published. The source template is `admin/release-template.json`; the server draft includes the user's subsequent edits.
