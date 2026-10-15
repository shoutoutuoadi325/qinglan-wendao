#!/usr/bin/env bash
# Portable editor-target build for QinglanAscension.
#
# External roots come from the environment; defaults stay relative to this
# repository so a second machine needs no private paths:
#   QINGLAN_ENGINE_ROOT  source engine checkout (default: beside the repo)
#   QINGLAN_PROJECT_DIR  game project directory  (default: <repo>/game)
#   QINGLAN_TARGET       build target            (default: QinglanAscensionEditor)
#   QINGLAN_CONFIG       build configuration     (default: Development)
#   QINGLAN_PLATFORM     target platform         (default: Linux)
#
# Exits nonzero when the engine/project inputs are missing or the build
# reports success without producing the mandatory binaries.
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$REPO_ROOT/../UnrealEngine}"
QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
QINGLAN_TARGET="${QINGLAN_TARGET:-QinglanAscensionEditor}"
QINGLAN_CONFIG="${QINGLAN_CONFIG:-Development}"
QINGLAN_PLATFORM="${QINGLAN_PLATFORM:-Linux}"

UPROJECT="$QINGLAN_PROJECT_DIR/QinglanAscension.uproject"
BUILD_SH="$QINGLAN_ENGINE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh"

fail() { echo "build-game.sh: ERROR: $*" >&2; exit 1; }

# Prefer the dedicated qdev build account when it exists and we may use it.
run_as_build_user() {
  if [ "$(id -u)" = "0" ] && id qdev >/dev/null 2>&1; then
    runuser -u qdev -- "$@"
  else
    "$@"
  fi
}

[ -f "$UPROJECT" ] || fail "project file missing: $UPROJECT (set QINGLAN_PROJECT_DIR)"
[ -f "$BUILD_SH" ] || fail "engine build script missing: $BUILD_SH (set QINGLAN_ENGINE_ROOT)"

echo "=== game build $(date -Is) target=$QINGLAN_TARGET config=$QINGLAN_CONFIG platform=$QINGLAN_PLATFORM ==="
run_as_build_user bash -c "cd '$QINGLAN_ENGINE_ROOT' && '$BUILD_SH' '$QINGLAN_TARGET' '$QINGLAN_PLATFORM' '$QINGLAN_CONFIG' -project='$UPROJECT' -waitmutex"
rc=$?

if [ "$rc" -eq 0 ]; then
  EDITOR_BIN="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Linux/UnrealEditor"
  [ -f "$EDITOR_BIN" ] || fail "build reported success but the editor binary is missing: $EDITOR_BIN"
  ls "$QINGLAN_PROJECT_DIR"/Binaries/Linux/*.so >/dev/null 2>&1 \
    || fail "build reported success but no project modules under $QINGLAN_PROJECT_DIR/Binaries/Linux"
fi
echo "=== game build rc=$rc $(date -Is) ==="
exit "$rc"
