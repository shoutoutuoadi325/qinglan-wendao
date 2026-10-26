#!/usr/bin/env bash
# Upload the unsigned macOS DMG as a draft GitHub Release, verify the asset,
# then publish it. Requires an authenticated gh CLI and a pushed tag.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

fail() { echo "publish-mac-unsigned.sh: ERROR: $*" >&2; exit 1; }

RELEASE="${1:-0.1.0-alpha.1}"
TAG="${QINGLAN_TAG:-v$RELEASE}"
OUT_DIR="${QINGLAN_OUT_DIR:-$REPO_ROOT/dist/mac}"
DMG="$OUT_DIR/Qinglan-$RELEASE-macOS-arm64.dmg"
SUMS="$OUT_DIR/SHA256SUMS"
NOTES="${QINGLAN_NOTES:-$REPO_ROOT/docs/releases/$TAG.md}"
TITLE="${QINGLAN_TITLE:-Qinglan $TAG - macOS (Apple Silicon) unsigned alpha}"

command -v gh >/dev/null 2>&1 || fail "gh CLI is required"
gh auth status >/dev/null 2>&1 || fail "gh is not authenticated"
[ -f "$DMG" ] || fail "DMG not found: $DMG"
[ -f "$SUMS" ] || fail "checksum file not found: $SUMS"
[ -f "$NOTES" ] || fail "release notes not found: $NOTES"
git rev-parse "$TAG" >/dev/null 2>&1 || fail "tag $TAG does not exist locally"
git ls-remote --exit-code --tags origin "$TAG" >/dev/null 2>&1 \
  || fail "tag $TAG is not on origin; run: git push origin $TAG"

SIZE=$(stat -f%z "$DMG")
[ "$SIZE" -lt 2147483648 ] || fail "DMG is $SIZE bytes; GitHub's per-asset limit is 2 GiB"

echo "==> creating draft release $TAG"
gh release create "$TAG" \
  --draft \
  --prerelease \
  --latest=false \
  --verify-tag \
  --title "$TITLE" \
  --notes-file "$NOTES" \
  "$DMG" "$SUMS"

echo "==> uploaded assets"
gh release view "$TAG" --json assets \
  --jq '.assets[] | "\(.name)\t\(.size) bytes"'

REMOTE_SIZE=$(gh release view "$TAG" --json assets \
  --jq ".assets[] | select(.name == \"$(basename "$DMG")\") | .size")
[ "$REMOTE_SIZE" = "$SIZE" ] \
  || fail "uploaded size $REMOTE_SIZE does not match local size $SIZE"

echo
echo "Draft verified. Publish it with:"
echo "  gh release edit $TAG --draft=false"
