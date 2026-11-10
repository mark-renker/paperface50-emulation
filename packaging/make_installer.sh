#!/bin/bash
# Build a macOS .pkg installer for the Paperface 50 emulation (instrument + FX; AU, VST3, standalone app).
#
#   packaging/make_installer.sh [--build] [BUILD_DIR]     # default BUILD_DIR = build-universal
#
# Output: dist/Paperface50-<version>-macOS.pkg (+ a .zip of the raw bundles for manual installs).
# The installer offers Paperface 50 (AU, VST3, app) and Paperface 50 FX (AU, VST3, app) as choices and installs to
# /Library/Audio/Plug-Ins/{Components,VST3} and /Applications. Bundles are ad-hoc signed unless DEVELOPER_ID_APP /
# DEVELOPER_ID_INSTALLER / NOTARY_PROFILE are set. Files installed by a .pkg are not quarantined.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DIST="$ROOT/dist"
WORK="$(mktemp -d /tmp/pf50-pkgwork.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BUILD=""
BUILD_DIR="build-universal"
for a in "$@"; do
  case "$a" in
    --build) BUILD=1 ;;
    *) BUILD_DIR="$a" ;;
  esac
done
VERSION="$(sed -n 's/^project(SergePaperface50 VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
ORG="com.blacktieventures"
if [[ -n "$BUILD" ]]; then
  cmake -S "$ROOT" -B "$ROOT/$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/$BUILD_DIR" -j "$(sysctl -n hw.ncpu)"
fi
ART1="$ROOT/$BUILD_DIR/Paperface50_artefacts/Release"
ART2="$ROOT/$BUILD_DIR/Paperface50FX_artefacts/Release"

check () {
  [[ -d "$1" ]] || { echo "missing bundle: $1" >&2; exit 1; }
  local exe; exe="$(find "$1/Contents/MacOS" -type f | head -1)"
  local archs; archs="$(lipo -archs "$exe")"
  echo "  $(basename "$1"): $archs"
  [[ "$archs" == *arm64* && "$archs" == *x86_64* ]] || echo "  WARNING: $(basename "$1") is not universal ($archs)" >&2
}

SIGN_APP="${DEVELOPER_ID_APP:--}"
SIGN_PKG="${DEVELOPER_ID_INSTALLER:-}"
mkdir -p "$WORK/pkgs" "$WORK/res" "$DIST"

component () { # <bundle> <install-location> <pkg-id>
  local bundle="$1" loc="$2" pid="$3" stage="$WORK/stage-$3"
  check "$bundle"
  mkdir -p "$stage"
  ditto "$bundle" "$stage/$(basename "$bundle")"
  if [[ "$SIGN_APP" == "-" ]]; then
    codesign --force --deep -s - "$stage/$(basename "$bundle")"
  else
    codesign --force --deep --options runtime --timestamp -s "$SIGN_APP" "$stage/$(basename "$bundle")"
  fi
  pkgbuild --analyze --root "$stage" "$WORK/$pid.plist" >/dev/null
  plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/$pid.plist"
  pkgbuild --root "$stage" --component-plist "$WORK/$pid.plist" \
           --install-location "$loc" --identifier "$pid" --version "$VERSION" \
           "$WORK/pkgs/$pid.pkg" >/dev/null
}
component "$ART1/AU/Paperface 50.component"         "/Library/Audio/Plug-Ins/Components" "$ORG.paperface50.au"
component "$ART1/VST3/Paperface 50.vst3"            "/Library/Audio/Plug-Ins/VST3"       "$ORG.paperface50.vst3"
component "$ART1/Standalone/Paperface 50.app"       "/Applications"                      "$ORG.paperface50.app"
component "$ART2/AU/Paperface 50 FX.component"      "/Library/Audio/Plug-Ins/Components" "$ORG.paperface50fx.au"
component "$ART2/VST3/Paperface 50 FX.vst3"         "/Library/Audio/Plug-Ins/VST3"       "$ORG.paperface50fx.vst3"
component "$ART2/Standalone/Paperface 50 FX.app"    "/Applications"                      "$ORG.paperface50fx.app"

sed -e "s/@VERSION@/$VERSION/g" "$HERE/resources/welcome.html" > "$WORK/res/welcome.html"
cp "$HERE/resources/conclusion.html" "$WORK/res/conclusion.html"
cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Paperface 50 $VERSION</title>
  <organization>$ORG</organization>
  <welcome file="welcome.html" mime-type="text/html"/>
  <conclusion file="conclusion.html" mime-type="text/html"/>
  <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <os-version min="11.0"/>
  <choices-outline>
    <line choice="inst"><line choice="inst_au"/><line choice="inst_vst3"/><line choice="inst_app"/></line>
    <line choice="fx"><line choice="fx_au"/><line choice="fx_vst3"/><line choice="fx_app"/></line>
  </choices-outline>
  <choice id="inst" title="Paperface 50 (instrument)" description="The full three-panel system as a MIDI instrument."/>
  <choice id="inst_au"   title="Audio Unit (AU)" description="Logic, GarageBand, Live, Bitwig. Installs to /Library/Audio/Plug-Ins/Components."><pkg-ref id="$ORG.paperface50.au"/></choice>
  <choice id="inst_vst3" title="VST3"            description="Live, Bitwig, Reaper, Cubase. Installs to /Library/Audio/Plug-Ins/VST3."><pkg-ref id="$ORG.paperface50.vst3"/></choice>
  <choice id="inst_app"  title="Standalone app"  description="Play without a DAW. Installs to /Applications."><pkg-ref id="$ORG.paperface50.app"/></choice>
  <choice id="fx" title="Paperface 50 FX (effect)" description="The same system as an audio effect: the track input feeds the panel's audio inputs."/>
  <choice id="fx_au"   title="Audio Unit (AU)"><pkg-ref id="$ORG.paperface50fx.au"/></choice>
  <choice id="fx_vst3" title="VST3"><pkg-ref id="$ORG.paperface50fx.vst3"/></choice>
  <choice id="fx_app"  title="Standalone app"><pkg-ref id="$ORG.paperface50fx.app"/></choice>
  <pkg-ref id="$ORG.paperface50.au">$ORG.paperface50.au.pkg</pkg-ref>
  <pkg-ref id="$ORG.paperface50.vst3">$ORG.paperface50.vst3.pkg</pkg-ref>
  <pkg-ref id="$ORG.paperface50.app">$ORG.paperface50.app.pkg</pkg-ref>
  <pkg-ref id="$ORG.paperface50fx.au">$ORG.paperface50fx.au.pkg</pkg-ref>
  <pkg-ref id="$ORG.paperface50fx.vst3">$ORG.paperface50fx.vst3.pkg</pkg-ref>
  <pkg-ref id="$ORG.paperface50fx.app">$ORG.paperface50fx.app.pkg</pkg-ref>
</installer-gui-script>
XML

OUT="$DIST/Paperface50-$VERSION-macOS.pkg"
rm -f "$OUT"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK/pkgs" \
             --resources "$WORK/res" ${SIGN_PKG:+--sign "$SIGN_PKG"} "$OUT" >/dev/null
if [[ -n "$SIGN_PKG" && -n "${NOTARY_PROFILE:-}" ]]; then
  xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$OUT"
fi

ZIP="$DIST/Paperface50-$VERSION-macOS-bundles.zip"
rm -f "$ZIP"
BUNDLES="$WORK/Paperface 50 $VERSION"
mkdir -p "$BUNDLES"
for d in "$WORK"/stage-*; do cp -R "$d"/* "$BUNDLES"/; done
cp "$HERE/INSTALL-macOS-manual.txt" "$BUNDLES/"
ditto -c -k --keepParent "$BUNDLES" "$ZIP"
echo "wrote $OUT"
echo "wrote $ZIP"
