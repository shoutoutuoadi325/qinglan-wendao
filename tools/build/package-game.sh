#!/usr/bin/env bash
# Portable Linux client packaging via RunUAT (Shipping by default; set
# QINGLAN_CONFIG=Development for the development package).
#
#   QINGLAN_ENGINE_ROOT  source engine checkout (default: beside the repo)
#   QINGLAN_PROJECT_DIR  game project directory  (default: <repo>/game)
#   QINGLAN_PLATFORM     cook/stage platform     (default: Linux)
#   QINGLAN_CONFIG       client configuration    (default: Shipping)
#   QINGLAN_ARCHIVE_DIR  packaged output root    (default: <repo>/packaging/Linux)
#
# Exits nonzero when inputs are missing, UAT fails, or the archive lacks the
# mandatory packaged artifacts (client binary and pak).
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$REPO_ROOT/../UnrealEngine}"
QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
QINGLAN_PLATFORM="${QINGLAN_PLATFORM:-Linux}"
QINGLAN_CONFIG="${QINGLAN_CONFIG:-Shipping}"
QINGLAN_ARCHIVE_DIR="${QINGLAN_ARCHIVE_DIR:-$REPO_ROOT/packaging/Linux}"

UPROJECT="$QINGLAN_PROJECT_DIR/QinglanAscension.uproject"
RUNUAT="$QINGLAN_ENGINE_ROOT/Engine/Build/BatchFiles/RunUAT.sh"

fail() { echo "package-game.sh: ERROR: $*" >&2; exit 1; }

run_as_build_user() {
  if [ "$(id -u)" = "0" ] && id qdev >/dev/null 2>&1; then
    runuser -u qdev -- "$@"
  else
    "$@"
  fi
}

[ -f "$UPROJECT" ] || fail "project file missing: $UPROJECT (set QINGLAN_PROJECT_DIR)"
[ -f "$RUNUAT" ] || fail "RunUAT missing: $RUNUAT (set QINGLAN_ENGINE_ROOT)"
mkdir -p "$QINGLAN_ARCHIVE_DIR" || fail "cannot create archive directory: $QINGLAN_ARCHIVE_DIR"

echo "=== game package $(date -Is) platform=$QINGLAN_PLATFORM config=$QINGLAN_CONFIG archive=$QINGLAN_ARCHIVE_DIR ==="
run_as_build_user bash -c "cd '$QINGLAN_ENGINE_ROOT' && '$RUNUAT' BuildCookRun -build -project='$UPROJECT' -noP4 -platform=$QINGLAN_PLATFORM -clientconfig=$QINGLAN_CONFIG -cook -stage -pak -compressed -archive -archivedirectory='$QINGLAN_ARCHIVE_DIR' -unattended -nosplash"
rc=$?

if [ "$rc" -eq 0 ]; then
  CLIENT_BIN="$QINGLAN_ARCHIVE_DIR/QinglanAscension/Binaries/$QINGLAN_PLATFORM/QinglanAscension"
  [ -f "$CLIENT_BIN" ] || fail "packaging reported success but the client binary is missing: $CLIENT_BIN"
  find "$QINGLAN_ARCHIVE_DIR/QinglanAscension" -name '*.pak' -print -quit 2>/dev/null | grep -q . \
    || fail "packaging reported success but no pak staged under $QINGLAN_ARCHIVE_DIR/QinglanAscension"
fi
echo "=== game package rc=$rc $(date -Is) ==="
exit "$rc"
