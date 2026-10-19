#!/usr/bin/env bash
# Portable content regeneration: data import -> mesh import -> content
# bootstrap -> animation/UI bootstrap, then mandatory-output validation. Mesh
# import precedes content bootstrap so generated maps can dress rooms with the
# imported nature meshes (AUD-55). The editor commandlets only warn about
# missing sources in-process; this wrapper makes those conditions fail the run
# (docs/dev/completion-audit-todo.md AUD-57).
#
#   QINGLAN_ENGINE_ROOT   source engine checkout (default: beside the repo)
#   QINGLAN_PROJECT_DIR   game project directory  (default: <repo>/game)
#   QINGLAN_ASSETS_RAW    raw imported assets root (default: beside the repo)
#   QINGLAN_LOW_QUALITY   set to 1 to bind solid-colour MICs and halve the
#                         deterministic nature dressing budget (AUD-55)
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
QINGLAN_LOW_QUALITY="${QINGLAN_LOW_QUALITY:-0}"

CONTENT_BOOTSTRAP_ARGS=()
[ "$QINGLAN_LOW_QUALITY" = "1" ] && CONTENT_BOOTSTRAP_ARGS+=("-lowquality")

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
# AUD-56: the eight project-original audio sources (4 music loops, 4 SFX).
for Audio in \
  original/music/music_boss_epic_boss_battle_loop.wav \
  original/music/music_combat_alt_prepare_your_swords.wav \
  original/music/music_combat_fast_fight_loop.wav \
  original/music/music_hub_calm_orient_peace_valley.wav \
  original/sfx/sfx_sword_swing.wav \
  original/sfx/sfx_hit_impact.wav \
  original/sfx/sfx_technique_release.wav \
  original/sfx/sfx_telegraph_warn.wav
do
  [ -f "$QINGLAN_ASSETS_RAW/audio/$Audio" ] \
    || fail "audio source missing: $QINGLAN_ASSETS_RAW/audio/$Audio"
done

run_cmdlet() {
  local name="$1"; shift
  echo "--- $name $(date -Is) ---"
  run_as_build_user bash -c "cd '$QINGLAN_PROJECT_DIR' && '$EDITOR_CMD' '$UPROJECT' -run=$name -unattended -nop4 -stdout -nullrhi $*"
  local rc=$?
  echo "--- $name rc=$rc ---"
  return "$rc"
}

rc_all=0
# Widget blueprints are fully code-authored by UIBootstrap. A package left on
# disk is discovered (partially loaded) by the commandlet process, and UE 5.8
# refuses to save over a partially loaded package, so regeneration starts from
# a clean slate for these four outputs only.
rm -f "$CONTENT/UI/WBP_HUD.uasset" "$CONTENT/UI/WBP_Dialogue.uasset" \
      "$CONTENT/UI/WBP_Journal.uasset" "$CONTENT/UI/WBP_Ending.uasset"

# Generated data assets (ST_Game/DT_Localization/DA_StoryRegistry/techniques/
# realms) are rebuilt from JSON. An existing package is discovered partially
# loaded by the commandlet process, and UE 5.8 then saves it without the new
# keys (observed as a stale string table while the importer reported the full
# key count). Delete the generated Data tree so every run starts clean.
rm -rf "$CONTENT/Data"

# Project commandlets live in the PostEngineInit editor module, which is not
# auto-discovered without the "Module.Class" form (-run appends "Commandlet").
run_cmdlet QinglanAscensionEditor.QinglanDataImport -data="$REPO_ROOT/data" || rc_all=1
# Mesh import runs before the content bootstrap because the map generator
# dresses rooms with the imported Quaternius nature meshes (AUD-55). The
# Interchange pipeline logs per-take frame-alignment errors that set the editor
# process exit code even though every mesh imports. The commandlet's own
# completion marker (written to its saved log) plus the mandatory-output checks
# below are the authority for this step.
if ! run_cmdlet QinglanAscensionEditor.QinglanMeshImport; then
  if grep -q 'QinglanMeshImport finished ok=1' "$QINGLAN_PROJECT_DIR/Saved/Logs/QinglanAscension.log" 2>/dev/null; then
    echo "note: MeshImport exited nonzero after importing every mesh (Interchange frame-alignment errors); continuing"
  else
    rc_all=1
  fi
fi
run_cmdlet QinglanAscensionEditor.QinglanContentBootstrap "${CONTENT_BOOTSTRAP_ARGS[@]}" || rc_all=1
# Combat montages (AUD-44/45) are rebuilt after the mesh import so each one is
# saved to its own package with the source skeleton assigned. Existing montage
# packages are removed first for the same partial-load reason as the widgets.
find "$CONTENT/Characters" \( -path '*/Montages/AM_*.uasset' -o -name 'Montages.uasset' \) -delete 2>/dev/null
run_cmdlet QinglanAscensionEditor.QinglanAnimBootstrap || rc_all=1
# AUD-56: import the project-original music/SFX (deterministic synthesized WAVs).
run_cmdlet QinglanAscensionEditor.QinglanAudioBootstrap || rc_all=1
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
# AUD-48: the runtime bilingual table is generated by QinglanDataImport; both
# locales must be present in the packaged asset.
[ -f "$CONTENT/Data/DT_Localization.uasset" ] \
  || { echo "regenerate-content.sh: ERROR: bilingual localization table $CONTENT/Data/DT_Localization.uasset missing" >&2; rc_all=1; }

# AUD-56: QinglanAudioBootstrap imports 4 music + 4 SFX sound waves. The
# commandlet only warns on a missing source, so require the committed assets.
AUDIO_COUNT=$(find "$CONTENT/Audio" -type f -name '*.uasset' 2>/dev/null | wc -l | tr -d ' ')
if [ "${AUDIO_COUNT:-0}" -lt 8 ]; then
  echo "regenerate-content.sh: ERROR: only ${AUDIO_COUNT:-0} audio assets under $CONTENT/Audio (expected >= 8)" >&2
  rc_all=1
fi

# AUD-55: the Poly Haven maps must be real Texture2D package assets, not just
# transient textures referenced by MICs that then fail to save. Require at
# least eight imported maps and the core surface MICs (both the textured
# instance and its solid-colour companion).
TEXTURE_COUNT=$(find "$CONTENT/Textures" -type f -name '*.uasset' 2>/dev/null | wc -l | tr -d ' ')
if [ "${TEXTURE_COUNT:-0}" -lt 8 ]; then
  echo "regenerate-content.sh: ERROR: only ${TEXTURE_COUNT:-0} Texture2D assets under $CONTENT/Textures (expected >= 8)" >&2
  rc_all=1
fi
[ -f "$CONTENT/Materials/M_QinglanSolid.uasset" ] \
  || { echo "regenerate-content.sh: ERROR: solid fallback master $CONTENT/Materials/M_QinglanSolid.uasset missing" >&2; rc_all=1; }
for MIC in M_BambooWall M_Cobblestone M_CobblestoneAlt M_StonePath M_GrassRock M_Cliff M_DirtFloor M_VolcanicRock M_RoofTiles; do
  [ -f "$CONTENT/Materials/$MIC.uasset" ] \
    || { echo "regenerate-content.sh: ERROR: surface material instance $CONTENT/Materials/$MIC.uasset missing" >&2; rc_all=1; }
  [ -f "$CONTENT/Materials/${MIC}_Solid.uasset" ] \
    || { echo "regenerate-content.sh: ERROR: solid fallback instance $CONTENT/Materials/${MIC}_Solid.uasset missing" >&2; rc_all=1; }
done

echo "=== content regeneration done rc_all=$rc_all $(date -Is) ==="
exit "$rc_all"
