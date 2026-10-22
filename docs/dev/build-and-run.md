# Build & Run

For the current removable Apple Silicon setup, start with
[local-mac-setup.md](local-mac-setup.md). Source `tools/build/local-env.sh`
to select the workspace engine, assets, Xcode and caches. Build, content and
package wrappers now select native host tools and default the target to the
host; set `QINGLAN_PLATFORM` explicitly for a different target.

The earlier Linux host (2x RTX 4090, Vulkan/Xvfb, `/inspire/hdd/...` checkouts)
produced the historical Linux evidence. Use [local-mac-setup.md](local-mac-setup.md)
for the current Apple Silicon environment.

## Build (editor target)

```bash
bash tools/build/build-game.sh
```

The script is committed and portable: it resolves the repository root from its
own location and takes external roots from the environment —
`QINGLAN_ENGINE_ROOT` (engine checkout; defaults beside the repository),
`QINGLAN_PROJECT_DIR`, `QINGLAN_TARGET`, `QINGLAN_CONFIG`, `QINGLAN_PLATFORM`.
The tested engine revision and toolchain are locked in
[`tools/build/versions.lock`](../../tools/build/versions.lock). It exits
nonzero when inputs are missing or when UBT reports success without producing
the binaries. It prefers the `qdev` account when it exists and otherwise builds
as the current user. The editor executable is the engine's
`$ENGINE/Engine/Binaries/Linux/UnrealEditor`; the project's module libraries are
written under `game/Binaries/Linux/*.so`.

## Content pipeline (editor commandlets, run once after build / after data changes)

The committed wrapper runs the whole sequence and fails when mandatory outputs
are missing (AUD-57):

```bash
bash tools/build/regenerate-content.sh
```

Headless invocation form for individual steps (the module-qualified commandlet
name is required: project commandlets live in the PostEngineInit editor module):

```bash
E="${QINGLAN_ENGINE_ROOT:-$PWD/../UnrealEngine}"
P="${QINGLAN_PROJECT_DIR:-$PWD/game}/QinglanAscension.uproject"
DATA="${QINGLAN_PROJECT_DIR:-$PWD/game}/../data"

# 1) Story data -> /Game/Qinglan/Data assets (registry, techniques, realms, ST_Game)
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanDataImportCommandlet -data="$DATA" -unattended -nop4 -nosplash

# 2) Core content: four maps, master materials, input assets
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanContentBootstrapCommandlet -unattended -nop4 -nosplash

# 3) Meshes: FBX import (characters/monsters/weapons/nature) from assets-raw
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanMeshImportCommandlet -unattended -nop4 -nosplash

# 4) Animation: combat montages from imported takes, paired into technique assets
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanAnimBootstrapCommandlet -unattended -nop4 -nosplash

# 5) UI widget blueprints
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanUIBootstrapCommandlet -unattended -nop4 -nosplash

# 6) Project-original audio (4 music loops + 4 SFX)
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAscensionEditor.QinglanAudioBootstrapCommandlet -unattended -nop4 -nosplash
```

## Automation tests

```bash
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -ExecCmds="Automation RunTests Qinglan" \
    -unattended -nop4 -nosplash -nullrhi -log=RunTests.log -TestExit="Automation Test Queue Empty"
```

All tests live under the `Qinglan.` prefix: `Qinglan.Combat.*`,
`Qinglan.Narrative.*`, `Qinglan.Persistence.*`, `Qinglan.Trial.*`,
`Qinglan.Progression.*`, `Qinglan.Anim.*`, `Qinglan.VFX.*`, `Qinglan.Content.*`,
`Qinglan.UI.*`, `Qinglan.Input.*`, `Qinglan.Abilities.*`, and
`Qinglan.Localization.*` (90 cases on the 2026-10-21 native Mac baseline; the
historical Linux run had 70). The two opt-in campaign journeys require
`QINGLAN_RUN_CAMPAIGN=1` and are named `Qinglan.Campaign.NewProfileMainJourney`
and `Qinglan.Campaign.NewProfileSideQuestJourney`.

## Play (with a display + Vulkan)

On a machine with X/Wayland and a working Vulkan loader:

```bash
$E/Engine/Binaries/Linux/UnrealEditor "$P"          # editor
# or the packaged Shipping client (see packaging/):
./packaging/Linux/QinglanAscension/Binaries/Linux/QinglanAscension-Linux-Shipping -windowed
```

Cold start shows the 3-slot Profile screen; select a slot to begin (no profile
is auto-created). On a headless display, drive it with `xdotool key Return`.

Maps: `L_Hub` (sanctuary), `L_BambooHollow`, `L_AshenVerge` (expedition realms),
`L_TribulationArena` (finale). `GameDefaultMap` is L_Hub.

## Rendering notes (historical Linux container)

- `-nullrhi` works for functional/automation runs (logic, saves, story branching).
- Rendered checks work with `Xvfb :1 -screen 0 1280x720x24` plus
  `DISPLAY=:1 XDG_RUNTIME_DIR=/tmp/xdg VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json`
  and the packaged Shipping client (precompiled shaders). Capture with
  `import -window root shot.png`.
- Sustained performance capture remains unverified: old attempts passed bare
  `-csvprofile` / `-fpschart` and aborted in the cluster GPU-contention
  `MallocCrash`. Those tokens do not prove profiling started; the default
  Shipping target disables the CSV profiler and command-line `ExecCmds`
  processing. Use a packaged **Development** client and the supported
  `startfpschart` / `stopfpschart` plus `t.FPSChart.DoCsvProfile 1` procedure in
  [performance-capture.md](performance-capture.md). It records raw frame/GPU
  timings and synchronized process-memory counters for the hub, ordinary
  combat and a normally admitted trial; Development results must retain their
  configuration label and cannot be called Shipping performance. Separately,
  editor `-game` aborts at startup with
  `Assertion failed: !InLocalizationTargetPath.IsEmpty()`
  (`TextLocalizationManager.cpp:629`). Short rendered runs (boot, frontend,
  trial wave 2) succeed.

## QA capture flags

All capture flags are harmless when absent.

- `-QinglanShot=<id>` takes one screenshot and quits; `-QinglanShotDelay=<seconds>`
  defers that capture so a timed route can be recorded (the hub's timer dies
  with the map travel, so the arena's own delayed capture is the one that
  fires); `-QinglanTrial=<seconds>` travels from the hub to
  `L_TribulationArena` and starts the foundation trial (capture-only admission
  bypass) so hazards/enemies can be rendered without a full run. Screenshots
  are written to `<Saved>/Screenshots/qinglan_shot_<id>.png`; the old `/tmp`
  path was denied by the sandboxed Mac package, and a `-UserDir` outside the
  sandbox container is not writable either.
- `-QinglanHandShot=<id>` renders a QA close-up of the hero's sword hand at
  near (35 cm) and normal (150 cm) distances into the same `Screenshots`
  directory, for the AUD-55 hand UV/palette check. It needs no player input;
  `-QinglanHandShotDelay=<seconds>` defaults to 3.
- `-QinglanAutoSlot=<0-2>` confirms a profile slot through the real menu rows
  after a 2 s settle, letting an unattended capture reach the hub. It does not
  bypass save validation; interactive slot selection still needs real input.
