#!/usr/bin/env bash
# Native-host editor/game build. QINGLAN_ENGINE_ROOT defaults beside the repo;
# QINGLAN_PROJECT_DIR defaults to game/. Target platform defaults to the host.
# QINGLAN_TARGET=QinglanAscensionEditor, QINGLAN_CONFIG=Development by default.
set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
fail() { echo "build-game.sh: ERROR: $*" >&2; exit 1; }
source "$SCRIPT_DIR/common.sh"
qinglan_init_host
QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$REPO_ROOT/../UnrealEngine}"
QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
QINGLAN_TARGET="${QINGLAN_TARGET:-QinglanAscensionEditor}"
QINGLAN_CONFIG="${QINGLAN_CONFIG:-Development}"
UPROJECT="$QINGLAN_PROJECT_DIR/QinglanAscension.uproject"
BUILD_TOOL="$(qinglan_build_tool)"
[ -f "$UPROJECT" ] || fail "project file missing: $UPROJECT (set QINGLAN_PROJECT_DIR)"
[ -f "$BUILD_TOOL" ] || fail "engine build script missing: $BUILD_TOOL (set QINGLAN_ENGINE_ROOT)"
if [[ "$QINGLAN_TARGET" = *Editor ]] && [ "$QINGLAN_PLATFORM" != "$QINGLAN_HOST" ]; then
  fail "editor target must match its host ($QINGLAN_HOST); use QINGLAN_TARGET=QinglanAscension for a cross-target game build"
fi
NATIVE_PROJECT="$(qinglan_native_path "$UPROJECT")" || exit 1
echo "=== game build $(qinglan_timestamp) host=$QINGLAN_HOST target=$QINGLAN_TARGET platform=$QINGLAN_PLATFORM config=$QINGLAN_CONFIG ==="
(cd "$QINGLAN_ENGINE_ROOT" && qinglan_run_engine "$BUILD_TOOL" "$QINGLAN_TARGET" "$QINGLAN_PLATFORM" "$QINGLAN_CONFIG" "-project=$NATIVE_PROJECT" -waitmutex)
rc=$?
if [ "$rc" -eq 0 ]; then
  if [[ "$QINGLAN_TARGET" = *Editor ]]; then
    case "$QINGLAN_HOST" in
      Linux) EDITOR_BIN="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Linux/UnrealEditor"; MODULE_PATTERN='*.so' ;;
      Mac) EDITOR_BIN="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"; MODULE_PATTERN='*.dylib' ;;
      Win64) EDITOR_BIN="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Win64/UnrealEditor.exe"; MODULE_PATTERN='*.dll' ;;
    esac
    [ -f "$EDITOR_BIN" ] || fail "build reported success but editor binary is missing: $EDITOR_BIN"
    [ -n "$(find "$QINGLAN_PROJECT_DIR/Binaries/$QINGLAN_HOST" -maxdepth 1 -type f -name "$MODULE_PATTERN" -print -quit 2>/dev/null)" ] \
      || fail "build reported success but no project modules under $QINGLAN_PROJECT_DIR/Binaries/$QINGLAN_HOST"
  else
    [ -n "$(qinglan_find_client "$QINGLAN_PROJECT_DIR/Binaries" "$QINGLAN_PLATFORM")" ] \
      || fail "build reported success but no $QINGLAN_PLATFORM game binary under $QINGLAN_PROJECT_DIR/Binaries"
  fi
fi
echo "=== game build rc=$rc $(qinglan_timestamp) ==="
exit "$rc"
