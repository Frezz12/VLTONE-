#!/usr/bin/env bash
#
# Build VLTONE.app and wrap it in macOS PKG and DMG installers.
#
#   packaging/macos/build-pkg.sh [version] [channel label]
#
# Produces  build-pkg/stage-vlt/VLTONE.app — the self-contained bundle,
#           build-pkg/VLTONE-<version>.pkg — Installer package, and
#           build-pkg/VLTONE-<version>.dmg — drag-to-Applications image.
#
# The bundle carries its own Qt, PortAudio and libsndfile (macdeployqt copies
# every non-system dylib and rewrites the load commands). The locally patched
# macOS RtMidi backend is linked statically. The bundle also carries the four
# helper executables the app looks for *next to itself*: daw_scan, which loads
# third-party plugins out of process, daw_guard, the network-free crash
# watchdog, daw_reporter, the restricted diagnostics courier, and daw_worker,
# the disposable offline worker. The PKG installs into /Applications and its
# postinstall removes only com.apple.quarantine from the installed app.
#
# Signing: the app is ad-hoc signed, which is all an arm64 binary needs to run
# on the machine that built it. The package is unsigned, so Gatekeeper will ask
# on any other machine — pass DAW_SIGN_ID / DAW_INSTALLER_ID to sign properly:
#
#   DAW_SIGN_ID="Developer ID Application: …" \
#   DAW_INSTALLER_ID="Developer ID Installer: …" packaging/macos/build-pkg.sh
#
# DAW_ENABLE_COLLABORATION=OFF builds the local-project edition, matching the
# Windows -DisableCollaboration option. Enabled collaboration always enforces
# the command-coverage release gate.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-pkg"
# Keep VLTONE releases separate from the legacy root-owned DAW.app
# staging directory that may exist on developer machines.
STAGE="$BUILD/stage-vlt"
APP_NAME="VLTONE"
APP_BUNDLE="$APP_NAME.app"
ARTIFACT_NAME="VLTONE"
IDENTIFIER="com.vltstudio.pro"
# The project's own version, not the `cmake_minimum_required` line above it.
VERSION="${1:-$(sed -n 's/^[[:space:]]*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid or missing project version: $VERSION" >&2; exit 1; }
CHANNEL="${2-beta}"
[[ -z "$CHANNEL" || "$CHANNEL" =~ ^[A-Za-z0-9]+([.\ -][A-Za-z0-9]+)*$ ]] || { echo "Invalid release channel: $CHANNEL" >&2; exit 1; }
ARTIFACT_CHANNEL="${CHANNEL// /-}"
ARTIFACT_VERSION="$VERSION${ARTIFACT_CHANNEL:+-$ARTIFACT_CHANNEL}"
DISPLAY_VERSION="$VERSION${CHANNEL:+ $CHANNEL}"
PKG="$BUILD/$ARTIFACT_NAME-$ARTIFACT_VERSION.pkg"
DMG="$BUILD/$ARTIFACT_NAME-$ARTIFACT_VERSION.dmg"
# A distributable build must use the hosted account platform. Local developer
# builds keep CMake's localhost default; CI/release automation may override
# this value without changing source.
API_ORIGIN="${VLT_DEFAULT_API_ORIGIN:-https://vltstudio.ru/api/v1}"
COLLABORATION="${DAW_ENABLE_COLLABORATION:-ON}"
[[ "$COLLABORATION" == ON || "$COLLABORATION" == OFF ]] || \
    { echo "DAW_ENABLE_COLLABORATION must be ON or OFF" >&2; exit 1; }

echo "── configure ─────────────────────────────────────────────"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DDAW_BUILD_APP=ON -DDAW_PACKAGE=ON -DDAW_BUILD_TESTS=OFF \
    -DDAW_ENABLE_COLLABORATION="$COLLABORATION" \
    -DDAW_ENFORCE_COLLABORATION_RELEASE_GATES="$COLLABORATION" \
    -DVLTONE_RELEASE_CHANNEL="$CHANNEL" \
    -DVLT_DEFAULT_API_ORIGIN="$API_ORIGIN" \
    -DCMAKE_BUILD_TYPE=Release

echo "── build ─────────────────────────────────────────────────"
cmake --build "$BUILD"

echo "── stage (macdeployqt runs here) ─────────────────────────"
# A clean staging root: pkgbuild packages whatever it finds, so a leftover file
# from an older layout would be installed into /Applications for good.
rm -rf "$STAGE"
# macdeployqt treats the WASI sysroot's .so linker stubs as native libraries
# and repeatedly tries to rewrite them. Keep the sysroot out of its scan, then
# restore it to both bundles before validating and signing the release.
BUILD_SYSROOT="$BUILD/bin/$APP_BUNDLE/Contents/MacOS/CreatorTools/sysroot"
TOOLS_SYSROOT="$BUILD/bin/CreatorTools/sysroot"
SAVED_SYSROOT="$(mktemp -d "$BUILD/.creator-sysroot.XXXXXX")"
restore_creator_sysroot() {
    if [[ -d "$SAVED_SYSROOT/sysroot" ]]; then
        mv "$SAVED_SYSROOT/sysroot" "$BUILD_SYSROOT"
    fi
    if [[ -d "$SAVED_SYSROOT/tools-sysroot" ]]; then
        mv "$SAVED_SYSROOT/tools-sysroot" "$TOOLS_SYSROOT"
    fi
    rmdir "$SAVED_SYSROOT"
}
trap restore_creator_sysroot EXIT
if [[ -d "$BUILD_SYSROOT" ]]; then
    mv "$BUILD_SYSROOT" "$SAVED_SYSROOT/sysroot"
fi
if [[ -d "$TOOLS_SYSROOT" ]]; then
    mv "$TOOLS_SYSROOT" "$SAVED_SYSROOT/tools-sysroot"
fi
# macdeployqt reports unresolved *optional* Qt modules (QtPdf, QtSvg, the
# virtual keyboard) that this Qt install does not have; the app does not use
# them and the deploy still completes, so its exit status is not fatal here.
cmake --install "$BUILD" --prefix "$STAGE" || true
restore_creator_sysroot
trap - EXIT
if [[ -d "$TOOLS_SYSROOT" ]]; then
    ditto "$TOOLS_SYSROOT" \
        "$STAGE/$APP_BUNDLE/Contents/MacOS/CreatorTools/sysroot"
fi
test -x "$STAGE/$APP_BUNDLE/Contents/MacOS/$APP_NAME" ||
    { echo "no app was staged"; exit 1; }
python3 "$ROOT/packaging/prune-qml.py" "$STAGE/$APP_BUNDLE" \
    --source "$ROOT/app/graphics/qml"
for helper in daw_scan daw_guard daw_reporter daw_worker; do
    test -x "$STAGE/$APP_BUNDLE/Contents/MacOS/$helper" ||
        { echo "$helper is missing from the bundle"; exit 1; }
    # macdeployqt can rewrite a helper's libraries to @rpath without adding
    # an LC_RPATH to that executable. The GUI then works while every decode
    # worker exits in dyld before it can import a sample or finish a take.
    if ! otool -l "$STAGE/$APP_BUNDLE/Contents/MacOS/$helper" |
        awk '/cmd LC_RPATH/{rpath=1; next} rpath && /path /{print $2; rpath=0}' |
        grep -Fx '@executable_path/../Frameworks' >/dev/null; then
        install_name_tool -add_rpath '@executable_path/../Frameworks' \
            "$STAGE/$APP_BUNDLE/Contents/MacOS/$helper"
    fi
done

# Qt WebEngine is more than a framework: Chromium runs in a helper process and
# loads resource packs at runtime. A bundle without either works on the build
# machine surprisingly often, then opens a blank panel on a clean machine.
web_engine_process="$(find "$STAGE/$APP_BUNDLE" -type f -name QtWebEngineProcess -print -quit)"
test -n "$web_engine_process" && test -x "$web_engine_process" ||
    { echo "QtWebEngineProcess is missing from the bundle"; exit 1; }
for resource in qtwebengine_resources.pak icudtl.dat; do
    find "$STAGE/$APP_BUNDLE" -type f -name "$resource" -print -quit | grep -q . ||
        { echo "$resource is missing from the bundle"; exit 1; }
done
# QML imports are runtime dependencies even though the scene's own QML and
# shaders are compiled into the executable. Missing these produces a blank
# workspace only on machines without the developer's Qt installation.
for module in QtQuick QtMultimedia QtWebEngine; do
    test -f "$STAGE/$APP_BUNDLE/Contents/Resources/qml/$module/qmldir" ||
        { echo "Required QML module $module is missing from the bundle"; exit 1; }
done
for framework in QtQuick QtQml QtMultimedia QtWebEngineQuick; do
    test -d "$STAGE/$APP_BUNDLE/Contents/Frameworks/$framework.framework" ||
        { echo "Required $framework framework is missing from the bundle"; exit 1; }
done
# Some Homebrew dylibs carry their Cellar/opt path as their own install ID.
# macdeployqt normally rewrites these, but not every leaf library (brotli has
# been one such case).  A bundled dylib must identify itself through @rpath so
# another Mac never needs the build machine's /opt/homebrew tree.
find "$STAGE/$APP_BUNDLE/Contents/Frameworks" -type f -name '*.dylib' -print0 |
    while IFS= read -r -d '' library; do
        install_id="$(otool -D "$library" 2>/dev/null | sed -n '2p')"
        case "$install_id" in
            /opt/homebrew/*|"$ROOT"/*)
                install_name_tool -id "@rpath/$(basename "$library")" "$library"
                ;;
        esac
    done

# macdeployqt rewrites framework dependencies relative to the main executable.
# QtWebEngineProcess is a nested .app, so the same @executable_path would point
# into the helper's Contents/Frameworks instead of the host app's Frameworks.
# The helper already carries the correct @loader_path rpath back to the host;
# make every bundled Qt reference use it. Homebrew's Qt helper also retains
# absolute opt paths that must not leak into a distributable bundle.
bundle_contents="$STAGE/$APP_BUNDLE/Contents"
while IFS= read -r -d '' binary; do
    file "$binary" | grep -q 'Mach-O' || continue
    while IFS= read -r dependency; do
        replacement=""
        relative=""
        case "$dependency" in
            @executable_path/../Frameworks/*)
                relative="${dependency#@executable_path/../Frameworks/}"
                ;;
            /opt/homebrew/*/lib/*.framework/*|/opt/homebrew/*/lib/*.dylib)
                relative="${dependency#*/lib/}"
                ;;
        esac
        if [[ -n "$relative" && -e "$bundle_contents/Frameworks/$relative" ]]; then
            replacement="@rpath/$relative"
        fi
        if [[ -n "$replacement" && "$replacement" != "$dependency" ]]; then
            install_name_tool -change "$dependency" "$replacement" \
                "$binary" 2>/dev/null
        fi
    done < <(otool -L "$binary" 2>/dev/null |
                 sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')

    install_id="$(otool -D "$binary" 2>/dev/null | sed -n '2p')"
    case "$install_id" in
        @executable_path/../Frameworks/*)
            install_name_tool -id \
                "@rpath/${install_id#@executable_path/../Frameworks/}" \
                "$binary" 2>/dev/null
            ;;
        /opt/homebrew/*/lib/*.framework/*|/opt/homebrew/*/lib/*.dylib)
            id_relative="${install_id#*/lib/}"
            if [[ -e "$bundle_contents/Frameworks/$id_relative" ]]; then
                install_name_tool -id "@rpath/$id_relative" \
                    "$binary" 2>/dev/null
            fi
            ;;
    esac
done < <(find "$STAGE/$APP_BUNDLE" -type f -print0)

# A package built on a Homebrew machine must not silently depend on that same
# machine. Fail staging when any Mach-O still names Homebrew or the source tree.
# Static archives are skipped: they are linked by the bundled clang, never
# loaded at run time, and `otool -L` lists their member paths (which live under
# the build tree) instead of load commands.
bad_dependency=0
while IFS= read -r -d '' binary; do
    case "$binary" in *.a) continue ;; esac
    file "$binary" | grep -q 'Mach-O' || continue
    while IFS= read -r dependency; do
        case "$dependency" in
            /opt/homebrew/*|"$ROOT"/*)
                echo "unbundled dependency: $binary -> $dependency"
                bad_dependency=1
                ;;
        esac
    done < <(otool -L "$binary" 2>/dev/null |
                 sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
done < <(find "$STAGE/$APP_BUNDLE" -type f -print0)
test "$bad_dependency" -eq 0 || exit 1

if [[ -n "${DAW_SIGN_ID:-}" ]]; then
    echo "── sign ──────────────────────────────────────────────────"
    # Inside out: every nested binary before the bundle that contains them.
    find "$STAGE/$APP_BUNDLE/Contents/Frameworks" \
         "$STAGE/$APP_BUNDLE/Contents/PlugIns" \
        -type f \( -name '*.dylib' -o -perm -u+x \) -print0 2>/dev/null |
        xargs -0 -I{} codesign --force --timestamp --options runtime \
            --sign "$DAW_SIGN_ID" {} || true
    while IFS= read -r web_helper_app; do
        codesign --force --timestamp --options runtime --deep \
            --sign "$DAW_SIGN_ID" "$web_helper_app"
    done < <(find "$STAGE/$APP_BUNDLE" -type d \
                    -name 'QtWebEngineProcess.app' -print)
    codesign --force --timestamp --options runtime --sign "$DAW_SIGN_ID" \
        "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_scan" \
        "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_guard" \
        "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_reporter" \
        "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_worker"
    codesign --force --timestamp --options runtime --deep \
        --sign "$DAW_SIGN_ID" "$STAGE/$APP_BUNDLE"
else
    # install_name_tool invalidates the ad-hoc signature produced by
    # macdeployqt. Re-sign the complete bundle after normalising dylib IDs.
    codesign --force --deep --sign - "$STAGE/$APP_BUNDLE"
fi
codesign --verify --deep --strict "$STAGE/$APP_BUNDLE"

# A dependency-name audit cannot detect an unresolved @rpath. Actually load
# the signed worker with no IPC arguments: reaching its argument check exits 2.
python3 - "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_worker" <<'PY'
import os
import subprocess
import sys

environment = {key: value for key, value in os.environ.items()
               if not key.startswith("DYLD_")}
result = subprocess.run([sys.argv[1]], env=environment, capture_output=True,
                        text=True, timeout=15)
if result.returncode != 2:
    sys.exit(f"Packaged audio worker failed to load ({result.returncode}):\n"
             f"{result.stdout}{result.stderr}")
print("Packaged audio worker loads without developer library paths")
PY

echo "── package ───────────────────────────────────────────────"
COMPONENT="$BUILD/$ARTIFACT_NAME-component.pkg"
COMPONENT_PLIST="$BUILD/components.plist"
# Always install into /Applications, even when Launch Services knows another
# copy of VLTONE. Replace obsolete bundle contents when upgrading.
pkgbuild --analyze --root "$STAGE" "$COMPONENT_PLIST"
python3 - "$COMPONENT_PLIST" <<'PY'
import plistlib
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    components = plistlib.load(stream)

def configure(items):
    for item in items:
        item["BundleIsRelocatable"] = False
        item["BundleOverwriteAction"] = "upgrade"
        configure(item.get("ChildBundles", []))

configure(components)
with open(path, "wb") as stream:
    plistlib.dump(components, stream)
PY
pkgbuild --root "$STAGE" \
         --component-plist "$COMPONENT_PLIST" \
         --scripts "$ROOT/packaging/macos/scripts" \
         --identifier "$IDENTIFIER" \
         --version "$VERSION" \
         --install-location /Applications \
         "$COMPONENT" >/dev/null

# A distribution package rather than the bare component: it is what gives the
# installer a title, a minimum-OS check and room for a licence later.
DIST="$BUILD/distribution.xml"
HOST_ARCHITECTURES="$(lipo -archs "$STAGE/$APP_BUNDLE/Contents/MacOS/$APP_NAME" | tr ' ' ',')"
cat > "$DIST" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$APP_NAME $DISPLAY_VERSION</title>
    <options customize="never" require-scripts="false" hostArchitectures="$HOST_ARCHITECTURES"/>
    <!-- Spelled out: without it installer -target CurrentUserHomeDirectory
         reports success and writes nothing at all. This app goes to
         /Applications, and says so. -->
    <domains enable_anywhere="false" enable_currentUserHome="false"
             enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions><os-version min="$(/usr/libexec/PlistBuddy -c 'Print LSMinimumSystemVersion' "$STAGE/$APP_BUNDLE/Contents/Info.plist")"/></allowed-os-versions>
    </volume-check>
    <choices-outline><line choice="default"/></choices-outline>
    <choice id="default" title="$APP_NAME"><pkg-ref id="$IDENTIFIER"/></choice>
    <pkg-ref id="$IDENTIFIER" version="$VERSION" onConclusion="none">$(basename "$COMPONENT")</pkg-ref>
</installer-gui-script>
XML

if [[ -n "${DAW_INSTALLER_ID:-}" ]]; then
    productbuild --distribution "$DIST" --package-path "$BUILD" \
                 --sign "$DAW_INSTALLER_ID" "$PKG" >/dev/null
else
    productbuild --distribution "$DIST" --package-path "$BUILD" "$PKG" >/dev/null
fi
rm -f "$COMPONENT"

echo "── dmg ───────────────────────────────────────────────────"
DMG_ROOT="$BUILD/dmg-root"
rm -rf "$DMG_ROOT"
mkdir -p "$DMG_ROOT"
# Use an APFS clone to avoid another full temporary copy of the large bundle.
# Preserve metadata and fall back to ditto on filesystems without cloning.
if ! cp -cRp "$STAGE/$APP_BUNDLE" "$DMG_ROOT/$APP_BUNDLE"; then
    rm -rf "$DMG_ROOT/$APP_BUNDLE"
    ditto "$STAGE/$APP_BUNDLE" "$DMG_ROOT/$APP_BUNDLE"
fi
# Validate the copy as well: the staged signature says nothing about bytes
# lost or changed during copying/packaging.
codesign --verify --deep --strict "$DMG_ROOT/$APP_BUNDLE"
ln -s /Applications "$DMG_ROOT/Applications"
rm -f "$DMG"
hdiutil create -volname "$APP_NAME $DISPLAY_VERSION" \
    -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG" >/dev/null
hdiutil verify "$DMG" >/dev/null
# A valid image checksum does not guarantee a valid application signature.
# Check the actual shipped bytes from a read-only mount before publishing.
DMG_VERIFY_MOUNT="$(mktemp -d "${TMPDIR:-/tmp}/vltone-dmg-verify.XXXXXX")"
cleanup_dmg_verify() {
    hdiutil detach "$DMG_VERIFY_MOUNT" >/dev/null 2>&1 || true
    rmdir "$DMG_VERIFY_MOUNT" 2>/dev/null || true
}
trap cleanup_dmg_verify EXIT
hdiutil attach "$DMG" -readonly -nobrowse \
    -mountpoint "$DMG_VERIFY_MOUNT" >/dev/null
codesign --verify --deep --strict "$DMG_VERIFY_MOUNT/$APP_BUNDLE"
hdiutil detach "$DMG_VERIFY_MOUNT" >/dev/null
rmdir "$DMG_VERIFY_MOUNT"
trap - EXIT
rm -rf "$DMG_ROOT"

echo
echo "app: $STAGE/$APP_BUNDLE  ($(du -sh "$STAGE/$APP_BUNDLE" | cut -f1))"
echo "pkg: $PKG  ($(du -h "$PKG" | cut -f1))"
echo "dmg: $DMG  ($(du -h "$DMG" | cut -f1))"
