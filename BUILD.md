# Building VLT Studio Pro

VLT Studio Pro cross-platform digital audio workstation. C++23 graph audio engine + Qt 6
Widgets front-end, audio I/O via PortAudio, file decode via libsndfile, JSON via
nlohmann/json. Build system: CMake (≥ 3.24) + Ninja.

```
engine/       the audio engine: node graph, work-stealing job system, DSP
                (no dependencies beyond the STL and Threads)
core/         platform layer: PortAudio device I/O, libsndfile decode, recording
controller/   framework-agnostic app logic (EngineController, model, serializer)
app/          Qt Widgets front-end and mandatory account gate
reporter/     Qt Core/Network diagnostics courier (separate process)
backend/      Go API, migrations, OpenAPI contract and adminctl
web/          public Next.js site and account cabinet (port 3000)
admin/        independent Next.js admin site (port 3001)
packages/     shared UI tokens, local fonts and generated TypeScript client
tests/        ctest targets
```

The engine is a dependency graph: tracks, clips, faders, sends, buses, meters
and (later) plugins are all nodes, and every block is scheduled across a
work-stealing thread pool by dependencies rather than by track. `controller/`
compiles the document into that graph and republishes it atomically on every
edit; `core/` no longer does any DSP.

Targets: `daw_engine` (static), `daw_core` (static), `daw_controller` (static),
`daw` (Qt app), `daw_reporter`, two independent Next.js applications and the Go
API, plus test executables. `DAW/` is the retired macOS-only Swift/Xcode app — not
part of this build.

---

## Assistant prompts

The AI assistant's instructions live in `prompts/` as Markdown — `main.md` plus
one file per playbook. They are not read at runtime: two generated files are
built from them, and both are checked in.

```
python3 scripts/gen_prompts.py           # after editing anything in prompts/
python3 scripts/gen_prompts.py --check   # fails if the generated files are stale
```

The generator writes `controller/ai/PromptsBuiltin.cpp` (the desktop's offline
fallback) and `backend/internal/promptlib/builtin_gen.go` (what a fresh database
is seeded with, and what "revert" in the admin panel restores). Editing either
by hand is pointless — the next run overwrites it. In production the prompts are
served from the backend and edited in the admin panel, so a change there needs
no build at all; this path is only the floor under that.

---

## Account platform (local development)

Pinned tools are Go **1.26.7**, PostgreSQL **18.6**, Node.js **24 LTS**, pnpm
**11.3.0**, Next.js **16.2.11**, GORM **1.31.2** and Tailwind CSS **4.3.0**.
Docker is not used. Start a local PostgreSQL instance, create the role/database
matching `backend/.env.example`, then run:

```bash
cd backend
cp .env.example .env
set -a; source .env; set +a
go run ./cmd/migrate up
go run ./cmd/adminctl create-owner --email owner@example.com --nickname Owner
go run ./cmd/api
```

In a second terminal from the repository root:

```bash
corepack enable
corepack prepare pnpm@11.3.0 --activate
pnpm install --frozen-lockfile
pnpm dev
```

The public site is `http://localhost:3000`, the separate admin site is
`http://localhost:3001`, and both proxy `/api` to Go on port 8080. The OpenAPI
source is `backend/openapi/openapi.yaml`; update the checked-in client with
`pnpm generate:api`. The backend never calls GORM `AutoMigrate`; schema changes
are versioned SQL in `backend/migrations/`.

Release installers and screenshots live under `STORAGE_ROOT/releases`; production
must mount that directory on persistent storage. The external reverse proxy must
allow request bodies up to 2 GiB and upload timeouts of at least 30 minutes. The
admin uses its dedicated `/release-upload` streaming route for these requests.

Before running the normal DAW, keep the API running. Development defaults to
`http://localhost:8080/v1`; override it with `VLT_API_ORIGIN`. The value is the
versioned API base URL (for production, `https://vltstudio.ru/api/v1`). Headless CTest and
`--selftest` use the in-process test account backend and do not expose a shipping
authorization-bypass flag.

The Demo subscription is indefinite, enables all features and renews a
20,000,000-token AI allowance at each UTC calendar-month boundary. Production
startup requires an Ed25519 seed, HTTPS origins and a positive
`AI_GLOBAL_MONTHLY_TOKEN_LIMIT`. Provider keys stay in the Go process.

Next.js 16.2.11 is intentionally pinned for the initial internal build. A tag or
`VLT_PUBLIC_RELEASE=1` runs `pnpm check:release-security` and is blocked until
the security patch announced for 26 August 2026 is pinned and reviewed.

---

## macOS

Dependencies via Homebrew:

```bash
brew install cmake ninja qt qtwebengine qtserialport portaudio rtmidi libsndfile nlohmann-json
```

Configure, build, test:

```bash
cmake --preset macos
cmake --build build
ctest --test-dir build --output-on-failure
```

Run:

```bash
./build/bin/daw
```

Package a self-contained `.app`, Installer package and drag-install DMG:

```bash
packaging/macos/build-pkg.sh
```

That configures a Release build with `-DDAW_PACKAGE=ON`, stages
`build-pkg/stage-vlt/VLT Studio Pro.app` (macdeployqt copies Qt, Qt WebEngine's
Chromium helper/resources, PortAudio and libsndfile into the bundle and rewrites
their load commands) and wraps it in
`build-pkg/VLT-Studio-Pro-<version>.pkg`, which installs to `/Applications`,
plus `build-pkg/VLT-Studio-Pro-<version>.dmg` with an Applications shortcut.

What `DAW_PACKAGE=ON` changes: the target becomes a real `.app` named
**VLT Studio Pro**
with `app/resources/daw.icns` as its icon and `app/resources/Info.plist.in` as
its identity — including `NSMicrophoneUsageDescription`, without which macOS
gives a non-sandboxed app no input and recording captures silence. The three
helper executables are installed **inside** the bundle (`Contents/MacOS/daw_scan`,
`daw_guard`, `daw_reporter`) rather than in `bin/`, because that is where the app
looks for them: next to its own executable.

The deployment target defaults to the *host's* macOS version, because Homebrew
builds its bottles for the machine you are on — the libsndfile and PortAudio
this links against will not load on anything older, and claiming a lower minimum
only turns a clear "needs a newer macOS" into a crash. For a build that has to
run on older systems, build the dependencies for that target and pass
`-DCMAKE_OSX_DEPLOYMENT_TARGET=…`.

The app is ad-hoc signed (all an arm64 binary needs to run where it was built);
the package is unsigned, so Gatekeeper will ask on any other machine. With a
Developer ID:

```bash
DAW_SIGN_ID="Developer ID Application: …" \
DAW_INSTALLER_ID="Developer ID Installer: …" packaging/macos/build-pkg.sh
```

The icon is generated from `app/resources/icon-1024.png` (a squircle-masked
1024×1024 master, artwork inset to Apple's 824 px grid); regenerate the `.icns`
with `iconutil -c icns` over an iconset built from it.

---

## Windows

The release toolchain is deliberately narrow and reproducible:

- Visual Studio 2022 Build Tools 17.14 toolset: **MSVC 14.44.35211**
  (`v143`, compiler 19.44), selected with `-vcvars_ver=14.44`;
- Windows SDK **10.0.26100.0**;
- CMake ≥ 3.24 and Ninja;
- Qt **6.8.3** `msvc2022_64`, with the mandatory **Qt WebEngine** and
  **Qt SerialPort** modules;
- Inno Setup **6.7.1** for the production installer;
- Git (the build script checks out the pinned vcpkg revision itself).

A newer Visual Studio installation is fine when it still has the MSVC 14.44
and Windows SDK 10.0.26100.0 optional components installed. Qt is rejected at
configure time if it is not exactly 6.8.3 in a Windows release build.

The vcpkg registry is pinned by `vcpkg-configuration.json` to commit
`ddd0023b0eee70986e42ed49d9d4afb8098f212e`. That baseline fixes the native
dependency graph (PortAudio, libsndfile, nlohmann/json and all transitives).
The prebuilt Qt SDK itself comes from the official Qt installer.

From an ordinary PowerShell prompt, the complete build is one command:

```powershell
.\packaging\windows\build.ps1
```

The script locates Visual Studio, activates the pinned compiler/SDK, validates
Qt and its two additional modules, bootstraps the pinned vcpkg checkout,
configures and builds Release x64, runs the **21 Windows CTest tests**, deploys
Qt, checks PE imports, and runs `--selftest` with the offscreen backend from:

```text
build-windows\Тест сборки\VLT Studio Pro\
```

That last run covers Unicode, spaces and quoting in the deployed executable,
Qt WebEngine helper and scanner paths. Artifacts are written to
`build-windows\artifacts`:

- `VLT-Studio-Pro-0.0.1-windows-x64.zip` — CI/developer artifact;
- `VLT-Studio-Pro-0.0.1-x64-Setup.exe` — production installer.

The computed install prefix is always the absolute
`<repository>\build-windows\stage`; no machine-specific path is stored in the
preset or documentation.

The installer installs under Program Files, runs the matching
`vc_redist.x64.exe` silently, adds Start-menu and optional desktop shortcuts,
registers `.vlt`, and provides normal Windows uninstall/upgrade entries. User
settings and plugin scan data live outside the install directory and are left
untouched during upgrade and uninstall.

For a signed production release, provide a PFX and keep its password out of
the command line:

```powershell
$env:WINDOWS_SIGN_CERT_PASSWORD = "..."
.\packaging\windows\build.ps1 -SignPfxPath C:\secure\vlt-release.pfx -RequireSignature
```

The four project executables are signed before packaging, and the installer
is signed after it is built, using an RFC 3161 timestamp. Qt and vcpkg DLLs
retain their upstream signatures. Release tags in CI fail when the signing
secrets are absent; ordinary branch artifacts may remain unsigned.

---

## ASIO on Windows

ASIO is Steinberg's low-latency driver API and the one every Windows interface
ships a driver for. WASAPI works, but ASIO is what gets the buffer down to a
handful of milliseconds.

It is enabled entirely by the dependency manifest — `vcpkg.json` asks for
`portaudio[asio]` on Windows, and vcpkg's `asiosdk` port downloads the SDK from
Steinberg during the install. Nothing needs to be fetched by hand and nothing
ASIO-related is vendored into this repository.

```
vcpkg install --triplet x64-windows
```

**Licensing.** The ASIO SDK is Steinberg's, not open source, and it is not
redistributable. vcpkg downloads it for the build; shipping a binary with ASIO
enabled is subject to Steinberg's ASIO SDK licensing agreement, which is free to
accept but does have to be accepted, and which carries attribution and trademark
requirements. Confirm the current terms at
<https://www.steinberg.net/developers/> before distributing a release build.
A build without the feature simply has no ASIO devices; nothing else changes.

**What the code does with it.** `core/Device/AudioDeviceManager.cpp` compiles the
ASIO extras only when `<pa_asio.h>` is present (`__has_include`), so a Windows
build without the feature still compiles:

- Devices carry their host API (`DeviceInfo::hostApi`) and the settings page puts
  it in the label. On Windows the same interface appears once per driver family
  under the same name, and picking the wrong one is the difference between 5 ms
  and 50 ms.
- `probeDevice` asks the selected device for its sample rates, and an ASIO driver
  for the buffer sizes it actually allows. Passing a size an ASIO driver does not
  allow makes PortAudio interpose a converting adaptor that quietly adds a block
  of latency.
- `showControlPanel` opens the driver's own dialog (`PaAsio_ShowControlPanel`),
  which is where an ASIO device's rate and buffer size really live.

Probing is done for the **selected** device only, never during enumeration:
measured on macOS, probing every device took 3.5 s, and on Windows it would mean
loading every ASIO driver on the machine and taking exclusive ones away from
whatever else is using them.

**Channel selection.** The Audio page reads the driver's physical channel names.
`Input Config` enables up to 32 inputs and `Output Config` maps the stereo master
to one physical output pair. PortAudio receives those selectors through
`PaAsioStreamInfo`; full multi-output buses are deliberately outside this scope.

## Windows validation status

The results from the first native Windows bring-up are now captured as
repeatable checks in `build.ps1` and the `windows-2022` workflow:

- [x] pinned vcpkg manifest resolves PortAudio/ASIO, libsndfile and
      nlohmann/json for `x64-windows`;
- [x] Release x64 builds with MSVC and links Qt Widgets, WebEngine and
      SerialPort;
- [x] all **21** Windows CTest tests pass;
- [x] the offscreen application self-test passes, including deterministic Edit
      chord routing that does not depend on a real active/focused native window;
- [x] `windeployqt` output is checked for unresolved DLL imports;
- [x] the deployed application self-tests from a path containing Cyrillic,
      spaces and quotes-sensitive process arguments;
- [x] CI is configured to produce both the ZIP artifact and the Inno Setup
      installer.

CI also runs the PostgreSQL integration suite, Go race tests, generated-contract
check, TypeScript checks, independent Next.js production builds and Playwright
flows. The macOS job builds `daw_reporter`, runs CTest and the offscreen self-test.
PDB and dSYM bundles are retained as private workflow artifacts keyed by commit
build ID.

These hardware and clean-machine checks remain release qualification work and
cannot be replaced by a headless CI runner:

- [ ] WASAPI and ASIO entries are visible for a real interface;
- [ ] the ASIO control panel opens and the advertised buffer sizes work without
      xruns under load;
- [ ] recording writes a valid WAV from a real input;
- [ ] export and `.vlt` Save/Open are exercised on Windows storage;
- [ ] representative VST3 and CLAP plugins scan, instantiate, process audio and
      open their native editors;
- [ ] the signed installer is installed, upgraded and uninstalled in a clean
      Windows VM with no development tools present.
