#!/usr/bin/env bash
# Native UAT packaging (Shipping by default). QINGLAN_PLATFORM defaults to host;
# QINGLAN_ARCHIVE_DIR defaults to packaging/<platform>. External engine/project
# paths can be supplied with QINGLAN_ENGINE_ROOT/QINGLAN_PROJECT_DIR.
set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
fail() { echo "package-game.sh: ERROR: $*" >&2; exit 1; }
source "$SCRIPT_DIR/common.sh"
qinglan_init_host
QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$REPO_ROOT/../UnrealEngine}"
QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
QINGLAN_CONFIG="${QINGLAN_CONFIG:-Shipping}"
QINGLAN_ARCHIVE_DIR="${QINGLAN_ARCHIVE_DIR:-$REPO_ROOT/packaging/$QINGLAN_PLATFORM}"
UPROJECT="$QINGLAN_PROJECT_DIR/QinglanAscension.uproject"
RUNUAT="$(qinglan_uat_tool)"
[ -f "$UPROJECT" ] || fail "project file missing: $UPROJECT (set QINGLAN_PROJECT_DIR)"
[ -f "$RUNUAT" ] || fail "RunUAT missing: $RUNUAT (set QINGLAN_ENGINE_ROOT)"
mkdir -p "$QINGLAN_ARCHIVE_DIR" || fail "cannot create archive directory: $QINGLAN_ARCHIVE_DIR"
NATIVE_PROJECT="$(qinglan_native_path "$UPROJECT")" || exit 1
NATIVE_ARCHIVE="$(qinglan_native_path "$QINGLAN_ARCHIVE_DIR")" || exit 1
DDC_ARGS=()
[ -n "${QINGLAN_DDC:-}" ] && DDC_ARGS+=("-ddc=$QINGLAN_DDC")
# UAT's Mac -archive copies only the pre-stage app from Binaries/Mac, which
# omits the cooked content it stages under Contents/UE. Archive the complete
# staged bundle ourselves instead so the delivered app can actually boot.
ARCHIVE_ARGS=(-archive "-archivedirectory=$NATIVE_ARCHIVE")
[ "$QINGLAN_PLATFORM" = Mac ] && ARCHIVE_ARGS=()
echo "=== game package $(qinglan_timestamp) host=$QINGLAN_HOST platform=$QINGLAN_PLATFORM config=$QINGLAN_CONFIG archive=$QINGLAN_ARCHIVE_DIR ==="
(cd "$QINGLAN_ENGINE_ROOT" && qinglan_run_engine "$RUNUAT" BuildCookRun -build "-project=$NATIVE_PROJECT" -noP4 "-platform=$QINGLAN_PLATFORM" "-clientconfig=$QINGLAN_CONFIG" -cook -stage -pak -compressed "${ARCHIVE_ARGS[@]}" -unattended -nosplash "${DDC_ARGS[@]}")
rc=$?
if [ "$rc" -eq 0 ] && [ "$QINGLAN_PLATFORM" = Mac ]; then
  STAGED_ROOT="$QINGLAN_PROJECT_DIR/Saved/StagedBuilds/$QINGLAN_PLATFORM"
  # Development is the default config, so its .app has no "-<Config>" suffix.
  STAGED_APP="$(find "$STAGED_ROOT" -maxdepth 1 -type d -name 'QinglanAscension*.app' -print -quit 2>/dev/null)"
  [ -n "$STAGED_APP" ] || fail "packaging reported success but staged no Mac app under $STAGED_ROOT"
  mkdir -p "$QINGLAN_ARCHIVE_DIR/$QINGLAN_PLATFORM" || fail "cannot create archive platform directory"
  rm -rf "$QINGLAN_ARCHIVE_DIR/$QINGLAN_PLATFORM/$(basename "$STAGED_APP")"
  ditto "$STAGED_APP" "$QINGLAN_ARCHIVE_DIR/$QINGLAN_PLATFORM/$(basename "$STAGED_APP")" \
    || fail "failed to archive the complete staged Mac app"
fi
if [ "$rc" -eq 0 ]; then
  [ -n "$(qinglan_find_client "$QINGLAN_ARCHIVE_DIR" "$QINGLAN_PLATFORM")" ] \
    || fail "packaging reported success but no $QINGLAN_PLATFORM client binary was staged under $QINGLAN_ARCHIVE_DIR"
  [ -n "$(find "$QINGLAN_ARCHIVE_DIR" -type f -name '*.pak' -print -quit 2>/dev/null)" ] \
    || fail "packaging reported success but no pak staged under $QINGLAN_ARCHIVE_DIR"
fi
echo "=== game package rc=$rc $(qinglan_timestamp) ==="
exit "$rc"
