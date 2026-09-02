#!/usr/bin/env bash
#
# Build VLT Studio Pro.app and wrap it in macOS PKG and DMG installers.
#
#   packaging/macos/build-pkg.sh [version]
#
# Produces  build-pkg/stage-vlt/VLT Studio Pro.app — the self-contained bundle,
#           build-pkg/VLT-Studio-Pro-<version>.pkg — Installer package, and
#           build-pkg/VLT-Studio-Pro-<version>.dmg — drag-to-Applications image.
#
# The bundle carries its own Qt, PortAudio, RtMidi and libsndfile (macdeployqt
# copies every non-system dylib and rewrites the load commands), plus the three helper
# executables the app looks for *next to itself*: daw_scan, which loads
# third-party plugins out of process, daw_guard, the network-free crash
# watchdog, and daw_reporter, the restricted diagnostics courier.
#
# Signing: the app is ad-hoc signed, which is all an arm64 binary needs to run
# on the machine that built it. The package is unsigned, so Gatekeeper will ask
# on any other machine — pass DAW_SIGN_ID / DAW_INSTALLER_ID to sign properly:
#
#   DAW_SIGN_ID="Developer ID Application: …" \
#   DAW_INSTALLER_ID="Developer ID Installer: …" packaging/macos/build-pkg.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-pkg"
# Keep VLT Studio Pro releases separate from the legacy root-owned DAW.app
# staging directory that may exist on developer machines.
STAGE="$BUILD/stage-vlt"
APP_NAME="VLT Studio Pro"
APP_BUNDLE="$APP_NAME.app"
ARTIFACT_NAME="VLT-Studio-Pro"
IDENTIFIER="com.vltstudio.pro"
# The project's own version, not the `cmake_minimum_required` line above it.
VERSION="${1:-$(sed -n 's/^[[:space:]]*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)}"
VERSION="${VERSION:-0.1.2}"
PKG="$BUILD/$ARTIFACT_NAME-$VERSION.pkg"
DMG="$BUILD/$ARTIFACT_NAME-$VERSION.dmg"
# A distributable build must use the hosted account platform. Local developer
# builds keep CMake's localhost default; CI/release automation may override
# this value without changing source.
API_ORIGIN="${VLT_DEFAULT_API_ORIGIN:-https://vltstudio.ru/api/v1}"

echo "── configure ─────────────────────────────────────────────"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DDAW_BUILD_APP=ON -DDAW_PACKAGE=ON -DDAW_BUILD_TESTS=OFF \
    -DVLT_DEFAULT_API_ORIGIN="$API_ORIGIN" \
    -DCMAKE_BUILD_TYPE=Release

echo "── build ─────────────────────────────────────────────────"
cmake --build "$BUILD"

echo "── stage (macdeployqt runs here) ─────────────────────────"
# A clean staging root: pkgbuild packages whatever it finds, so a leftover file
# from an older layout would be installed into /Applications for good.
rm -rf "$STAGE"
# macdeployqt reports unresolved *optional* Qt modules (QtPdf, QtSvg, the
# virtual keyboard) that this Qt install does not have; the app does not use
# them and the deploy still completes, so its exit status is not fatal here.
cmake --install "$BUILD" --prefix "$STAGE" || true
test -x "$STAGE/$APP_BUNDLE/Contents/MacOS/$APP_NAME" ||
    { echo "no app was staged"; exit 1; }
for helper in daw_scan daw_guard daw_reporter; do
    test -x "$STAGE/$APP_BUNDLE/Contents/MacOS/$helper" ||
        { echo "$helper is missing from the bundle"; exit 1; }
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
find "$STAGE/$APP_BUNDLE/Contents/Frameworks" -type f \
    -name 'librtmidi*.dylib' -print -quit | grep -q . ||
    { echo "RtMidi is missing from the bundle"; exit 1; }

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
bad_dependency=0
while IFS= read -r -d '' binary; do
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
        "$STAGE/$APP_BUNDLE/Contents/MacOS/daw_reporter"
    codesign --force --timestamp --options runtime --deep \
        --sign "$DAW_SIGN_ID" "$STAGE/$APP_BUNDLE"
else
    # install_name_tool invalidates the ad-hoc signature produced by
    # macdeployqt. Re-sign the complete bundle after normalising dylib IDs.
    codesign --force --deep --sign - "$STAGE/$APP_BUNDLE"
fi
codesign --verify --deep --strict "$STAGE/$APP_BUNDLE"

echo "── package ───────────────────────────────────────────────"
COMPONENT="$BUILD/$ARTIFACT_NAME-component.pkg"
pkgbuild --root "$STAGE" \
         --identifier "$IDENTIFIER" \
         --version "$VERSION" \
         --install-location /Applications \
         "$COMPONENT" >/dev/null

# A distribution package rather than the bare component: it is what gives the
# installer a title, a minimum-OS check and room for a licence later.
DIST="$BUILD/distribution.xml"
cat > "$DIST" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>$APP_NAME $VERSION</title>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <!-- Spelled out: without it `installer -target CurrentUserHomeDirectory`
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
# ditto preserves bundle metadata, resource forks and executable modes.
ditto "$STAGE/$APP_BUNDLE" "$DMG_ROOT/$APP_BUNDLE"
ln -s /Applications "$DMG_ROOT/Applications"
rm -f "$DMG"
hdiutil create -volname "$APP_NAME $VERSION" \
    -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG" >/dev/null
hdiutil verify "$DMG" >/dev/null
rm -rf "$DMG_ROOT"

echo
echo "app: $STAGE/$APP_BUNDLE  ($(du -sh "$STAGE/$APP_BUNDLE" | cut -f1))"
echo "pkg: $PKG  ($(du -h "$PKG" | cut -f1))"
echo "dmg: $DMG  ($(du -h "$DMG" | cut -f1))"
