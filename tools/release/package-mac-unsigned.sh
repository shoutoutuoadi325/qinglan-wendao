#!/usr/bin/env bash
# Build a GitHub-ready unsigned macOS DMG from a staged Unreal Shipping app.
#
# The app is re-signed ad hoc with the app-sandbox entitlement and without the
# debug-only get-task-allow entitlement. Gatekeeper will still block it because
# there is no Developer ID signature or notarization ticket; INSTALL.txt and
# the README explain the per-app override.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TEMPLATE_DIR="$SCRIPT_DIR/mac"

fail() { echo "package-mac-unsigned.sh: ERROR: $*" >&2; exit 1; }
info() { echo "==> $*"; }

APP_SOURCE="${QINGLAN_APP:-$REPO_ROOT/game/Saved/StagedBuilds/Mac/QinglanAscension-Mac-Shipping.app}"
VERSION="${QINGLAN_VERSION:-0.1.0}"
RELEASE="${QINGLAN_RELEASE:-0.1.0-alpha.1}"
BUNDLE_ID="${QINGLAN_BUNDLE_ID:-io.github.shoutoutuoadi325.qinglan}"
DISPLAY_NAME="${QINGLAN_DISPLAY_NAME:-Qinglan - Path of Ascension}"
OUT_DIR="${QINGLAN_OUT_DIR:-$REPO_ROOT/dist/mac}"
DMG_NAME="Qinglan-${RELEASE}-macOS-arm64.dmg"
MAIN_EXEC_NAME="QinglanAscension-Mac-Shipping"

[ -d "$APP_SOURCE" ] || fail "staged app not found: $APP_SOURCE (run tools/build/package-game.sh first)"
[ -f "$APP_SOURCE/Contents/MacOS/$MAIN_EXEC_NAME" ] || fail "unexpected app layout: $APP_SOURCE"
file "$APP_SOURCE/Contents/MacOS/$MAIN_EXEC_NAME" | grep -q "arm64" || fail "app is not arm64"
command -v hdiutil >/dev/null 2>&1 || fail "hdiutil is required"
command -v codesign >/dev/null 2>&1 || fail "codesign is required"

for f in CREDITS.txt EULA.txt INSTALL.txt; do
  [ -f "$TEMPLATE_DIR/$f" ] || fail "missing template: $TEMPLATE_DIR/$f"
done

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/dmg"

APP="$OUT_DIR/dmg/$DISPLAY_NAME.app"
info "copying $APP_SOURCE"
ditto "$APP_SOURCE" "$APP"
xattr -cr "$APP"

PLIST="$APP/Contents/Info.plist"
[ -f "$PLIST" ] || fail "Info.plist missing"

plist_set() {
  local key="$1" value="$2"
  /usr/libexec/PlistBuddy -c "Set :$key $value" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :$key string $value" "$PLIST" \
    || fail "cannot set $key"
}

plist_set CFBundleIdentifier "$BUNDLE_ID"
plist_set CFBundleDisplayName "$DISPLAY_NAME"
plist_set CFBundleName "Qinglan"
plist_set CFBundleShortVersionString "$VERSION"
plist_set CFBundleVersion "1"
plist_set NSHumanReadableCopyright "Copyright © 2026 Qinglan Community. All rights reserved."

RES="$APP/Contents/Resources"
mkdir -p "$RES"
for f in CREDITS.txt EULA.txt INSTALL.txt; do
  cp "$TEMPLATE_DIR/$f" "$RES/$f"
done

# Keep the container the verified build uses, but drop get-task-allow: it is a
# debug entitlement and an unsigned release should not carry it.
ENT="$OUT_DIR/Release.entitlements"
cat > "$ENT" <<'ENT_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.app-sandbox</key>
  <true/>
</dict>
</plist>
ENT_EOF

MAIN_EXEC="$APP/Contents/MacOS/$MAIN_EXEC_NAME"

sign_one() {
  codesign --force --sign - "$1" >/dev/null 2>&1 || fail "codesign failed for $1"
}

info "signing nested Mach-O code (ad-hoc)"
find "$APP" -type f -print0 | while IFS= read -r -d '' f; do
  [ "$f" = "$MAIN_EXEC" ] && continue
  if file "$f" 2>/dev/null | grep -q "Mach-O"; then
    sign_one "$f"
  fi
done

find "$APP" -type d -name "*.framework" -print0 | while IFS= read -r -d '' fw; do
  sign_one "$fw"
done

info "signing main executable and app bundle"
sign_one "$MAIN_EXEC"
codesign --force --sign - --entitlements "$ENT" "$APP" >/dev/null 2>&1 \
  || fail "codesign failed for $APP"

codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | tail -3
codesign -d --entitlements :- "$APP" 2>/dev/null | grep -q "get-task-allow" \
  && fail "get-task-allow is still present"
codesign -d --entitlements :- "$APP" 2>/dev/null | grep -q "app-sandbox" \
  || fail "app-sandbox entitlement missing"

# DMG payload: signed app, Applications shortcut, and player-facing docs.
DMG_ROOT="$OUT_DIR/dmg-root"
rm -rf "$DMG_ROOT"
mkdir -p "$DMG_ROOT"
ditto "$APP" "$DMG_ROOT/$DISPLAY_NAME.app"
ln -s /Applications "$DMG_ROOT/Applications"
for f in CREDITS.txt EULA.txt INSTALL.txt; do
  cp "$TEMPLATE_DIR/$f" "$DMG_ROOT/$f"
done

DMG="$OUT_DIR/$DMG_NAME"
info "creating $DMG"
hdiutil create -volname "$DISPLAY_NAME" -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG" >/dev/null \
  || fail "hdiutil create failed"

( cd "$OUT_DIR" && shasum -a 256 "$DMG_NAME" > SHA256SUMS )

SIZE=$(stat -f%z "$DMG")
HASH=$(shasum -a 256 "$DMG" | awk '{print $1}')
info "done"
printf '\nDMG:    %s\nSize:   %s bytes\nSHA256: %s\n\n' "$DMG" "$SIZE" "$HASH"
printf 'Signature: ad-hoc (not Developer ID signed or notarized).\n'
printf 'Next: smoke-test the app, then publish with:\n'
printf '  tools/release/publish-mac-unsigned.sh %s\n' "$RELEASE"
