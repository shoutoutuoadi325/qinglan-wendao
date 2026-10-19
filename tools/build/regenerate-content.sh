#!/usr/bin/env bash
# Portable content regeneration: data import -> content bootstrap -> mesh
# import -> UI bootstrap, then mandatory-output validation. The editor
# commandlets only warn about missing sources in-process; this wrapper makes
# those conditions fail the run (docs/dev/completion-audit-todo.md AUD-57).
#
#   QINGLAN_ENGINE_ROOT  source engine checkout (default: beside the repo)
#   QINGLAN_PROJECT_DIR  game project directory  (default: <repo>/game)
#   QINGLAN_ASSETS_RAW   raw imported assets root (default: beside the repo)
#
# Exits nonzero when the editor or raw sources are missing, a commandlet
# fails, or the mandatory generated assets are absent afterwards (zero
# technique data assets, zero character meshes, missing maps/input/table).
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

QINGLAN_ENGINE_ROOT="${QINGLAN_ENGINE_ROOT:-$REPO_ROOT/../UnrealEngine}"
QINGLAN_PROJECT_DIR="${QINGLAN_PROJECT_DIR:-$REPO_ROOT/game}"
QINGLAN_ASSETS_RAW="${QINGLAN_ASSETS_RAW:-$REPO_ROOT/../assets-raw}"

EDITOR_CMD="$QINGLAN_ENGINE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd"
UPROJECT="$QINGLAN_PROJECT_DIR/QinglanAscension.uproject"
CONTENT="$QINGLAN_PROJECT_DIR/Content/Qinglan"

fail() { echo "regenerate-content.sh: ERROR: $*" >&2; exit 1; }

run_as_build_user() {
  if [ "$(id -u)" = "0" ] && id qdev >/dev/null 2>&1; then
    runuser -u qdev -- "$@"
  else
    "$@"
  fi
}

[ -f "$EDITOR_CMD" ] || fail "editor command binary missing: $EDITOR_CMD (set QINGLAN_ENGINE_ROOT)"
[ -f "$UPROJECT" ] || fail "project file missing: $UPROJECT (set QINGLAN_PROJECT_DIR)"
[ -d "$QINGLAN_ASSETS_RAW" ] || fail "raw asset root missing: $QINGLAN_ASSETS_RAW (set QINGLAN_ASSETS_RAW)"
# Mandatory raw sources: skeletal/character FBX (QinglanMeshImport raw root).
find "$QINGLAN_ASSETS_RAW/quaternius" -iname '*.fbx' -print -quit 2>/dev/null | grep -q . \
  || fail "skeletal source files missing under $QINGLAN_ASSETS_RAW/quaternius"

run_cmdlet() {
  local name="$1"; shift
  echo "--- $name $(date -Is) ---"
  run_as_build_user bash -c "cd '$QINGLAN_PROJECT_DIR' && '$EDITOR_CMD' '$UPROJECT' -run=$name -unattended -nop4 -stdout -nullrhi $*"
  local rc=$?
  echo "--- $name rc=$rc ---"
  return "$rc"
}

rc_all=0
# Project commandlets live in the PostEngineInit editor module, which is not
# auto-discovered without the "Module.Class" form (-run appends "Commandlet").
run_cmdlet QinglanAscensionEditor.QinglanDataImport -data="$REPO_ROOT/data" || rc_all=1
run_cmdlet QinglanAscensionEditor.QinglanContentBootstrap || rc_all=1
run_cmdlet QinglanAscensionEditor.QinglanMeshImport || rc_all=1
run_cmdlet QinglanAscensionEditor.QinglanUIBootstrap || rc_all=1

# Mandatory outputs: the commandlets can report success without producing
# these (MeshImport logs missing skeletal files; AnimBootstrap pairs zero
# techniques), so verify the committed content actually exists.
find "$CONTENT/Data/Techniques" -name 'DA_Tech_*.uasset' -print -quit 2>/dev/null | grep -q . \
  || { echo "regenerate-content.sh: ERROR: zero technique data assets under $CONTENT/Data/Techniques" >&2; rc_all=1; }
find "$CONTENT/Characters" -name 'SK_*.uasset' -print -quit 2>/dev/null | grep -q . \
  || { echo "regenerate-content.sh: ERROR: zero skeletal meshes under $CONTENT/Characters" >&2; rc_all=1; }
for Map in L_Hub L_BambooHollow L_AshenVerge L_TribulationArena; do
  [ -f "$CONTENT/Maps/$Map.umap" ] \
    || { echo "regenerate-content.sh: ERROR: map $CONTENT/Maps/$Map.umap missing" >&2; rc_all=1; }
done
[ -f "$CONTENT/Input/IMC_Default.uasset" ] \
  || { echo "regenerate-content.sh: ERROR: input mapping context $CONTENT/Input/IMC_Default.uasset missing" >&2; rc_all=1; }
[ -f "$CONTENT/Data/ST_Game.uasset" ] \
  || { echo "regenerate-content.sh: ERROR: string table $CONTENT/Data/ST_Game.uasset missing" >&2; rc_all=1; }

echo "=== content regeneration done rc_all=$rc_all $(date -Is) ==="
exit "$rc_all"
