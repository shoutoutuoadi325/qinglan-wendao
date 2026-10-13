# Build & Run (Linux dev environment)

Environment used for development:

- Engine: UE 5.8.3 source build at `/inspire/hdd/project/qianghuaxuexi/s26044/UnrealEngine`
  (engine binaries built from source; game modules build against it)
- Project: `/inspire/hdd/project/qianghuaxuexi/s26044/qinglan-wendao/game/QinglanAscension.uproject`
- Raw CC0 assets: `/inspire/hdd/project/qianghuaxuexi/s26044/assets-raw` (Quaternius characters/props, PolyHaven HDRIs/textures)
- GPU: 2x NVIDIA RTX 4090 (driver 570.195, CUDA 12.8). The NVIDIA Vulkan ICD is installed
  (`/usr/share/vulkan/icd.d/nvidia_icd.json`) but **no Vulkan loader library (`libvulkan.so.1`)
  is present in the container** and there is no X display, so on-machine GPU rendering is not
  available to this session; see "Rendering notes" below.

## Build (editor target)

```bash
bash /inspire/hdd/project/qianghuaxuexi/s26044/build-game.sh
tail -5 /inspire/hdd/project/qianghuaxuexi/s26044/game-build.log   # rc=0 on success
```

The script prefers the `qdev` account when it exists and otherwise builds as the current user.
Output binary: `game/Binaries/Linux/UnrealEditor-QinglanAscension` (+ module `.so` files under
`game/Binaries/Linux/`).

## Content pipeline (editor commandlets, run once after build / after data changes)

Run from the engine root with the project path; every commandlet is headless (`-unattended`):

```bash
E=/inspire/hdd/project/qianghuaxuexi/s26044/UnrealEngine
P=/inspire/hdd/project/qianghuaxuexi/s26044/qinglan-wendao/game/QinglanAscension.uproject

# 1) Story data -> /Game/Qinglan/Data assets (registry, techniques, realms, ST_Game)
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanDataImport -unattended -nop4 -nosplash

# 2) Core content: four maps, master materials, input assets
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanContentBootstrap -unattended -nop4 -nosplash

# 3) Meshes: FBX import (characters/monsters/weapons/nature) from assets-raw
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanMeshImport -unattended -nop4 -nosplash

# 4) Animation: combat montages from imported takes, paired into technique assets
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanAnimBootstrap -unattended -nop4 -nosplash

# 5) UI widget blueprints
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -run=QinglanUIBootstrap -unattended -nop4 -nosplash
```

## Automation tests

```bash
$E/Engine/Binaries/Linux/UnrealEditor-Cmd "$P" -ExecCmds="Automation RunTests Qinglan" \
    -unattended -nop4 -nosplash -nullrhi -log=RunTests.log -TestExit="Automation Test Queue Empty"
```

All tests live under the `Qinglan.` prefix (`Qinglan.Combat.*`, `Qinglan.Economy.*`,
`Qinglan.Narrative.*`, `Qinglan.Save.*`).

## Play (with a display + Vulkan)

On a machine with X/Wayland and a working Vulkan loader:

```bash
$E/Engine/Binaries/Linux/UnrealEditor "$P"          # editor
# or the packaged game (see packaging/):
./packaging/Linux/QinglanAscension/Binaries/Linux/QinglanAscension
```

Maps: `L_Hub` (sanctuary), `L_BambooHollow`, `L_AshenVerge` (expedition realms),
`L_TribulationArena` (finale). `GameDefaultMap` is L_Hub.

## Rendering notes (this container)

- `-nullrhi` works for functional/automation runs (logic, saves, story branching).
- Real rendering requires `libvulkan.so.1` (e.g. `apt install libvulkan1`) and a display;
  with the NVIDIA ICD present the packaged game should then run on the RTX 4090s.
  Until then, GPU-side performance numbers cannot be captured on this machine —
  recorded as pending for a display-capable Linux box (docs/09 distribution checklist).
