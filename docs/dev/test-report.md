# Test Report — Qinglan: Path of Ascension

## 0. Current evidence (2026-10-20)

Environment: Linux container, UE 5.8.3 source build (`tools/build/versions.lock`),
headless `-nullrhi` for automation; rendered checks use Xvfb :1 with the NVIDIA
Vulkan ICD. Branch `fix/audit-repairs`. Evidence files live in
`docs/dev/evidence/` (logs are gitignored by `*.log`) and
`packaging/evidence/`.

| Item | Result | Evidence |
|---|---|---|
| Editor target build (Linux, Development, UHT + editor modules) | ✅ Succeeded | `docs/dev/evidence/build-2026-10-19-wave6.log` |
| Automation suite `RunTests Qinglan` | ✅ **70/70** Success, 0 Fail | `docs/dev/evidence/automation-2026-10-20-wave44-70tests.log` |
| Clean content regeneration (`tools/build/regenerate-content.sh`) | ✅ `rc_all=0` from a deleted `Data`/`UI`/montage state; Niagara systems, per-rig ABPs, hit-react montages, maps, 966-key string table, 199-key bilingual JSON parity, audio, textures present | `docs/dev/evidence/content-regeneration-2026-10-19-niagara-abp.log` |
| Content bootstrap (Niagara + Human sockets) | ✅ `NS_Impact/NS_Telegraph/NS_Cast` saved with one emitter each; `sword_base`/`sword_tip`/`cast_source` verified | `docs/dev/evidence/content-bootstrap-2026-10-19-niagara-sockets.log` |
| Functional new-profile campaign journey (`QINGLAN_RUN_CAMPAIGN=1`) | ✅ **30/30 main scenes**, both trials, breakthroughs, ending confirmation, 0 blocked | `docs/dev/evidence/automation-2026-10-19-campaign-30of30-wave37.log` |
| Functional new-profile side-quest journey (`QINGLAN_RUN_CAMPAIGN=1`) | ✅ **24/24 side stages**, courier round trips, 0 blocked | `docs/dev/evidence/automation-2026-10-19-sidequests-24of24-wave37.log` |
| Linux Shipping package (UAT BuildCookRun) | ✅ `rc=0`; all four maps, localization tables, registry, widgets, montages, meshes, audio, `NS_*` cue systems, `ABP_*` slot graphs in the IoStore inventory | `packaging/evidence/linux-shipping-2026-10-20-wave44.txt`, `docs/dev/evidence/package-linux-2026-10-20-wave44.log` |
| Rendered packaged boot, English | ✅ hub renders; HUD text, skill slots, talisman charges, vitals, camera and character scale correct | `docs/dev/evidence/rendered/hub-packaged-shipping-wave36-2026-10-19.png` |
| Rendered packaged boot, Simplified Chinese | ✅ every HUD string resolves in `zh-Hans`; CJK glyphs render (no tofu) | `docs/dev/evidence/rendered/hub-packaged-shipping-zh-Hans-2026-10-19.png` |
| Rendered packaged boot, Low + low-flash + `zh-Hans` | ✅ reduced-effects configuration boots and renders a readable HUD | `docs/dev/evidence/rendered/hub-packaged-shipping-low-lowflash-zh-2026-10-19.png` |
| Sky atmosphere and hero skin material | ✅ lit sky/horizon; `MIC_HumanCharacter.BaseColor` resolves to `T_Human_Clothed` (`Qinglan.Content.HeroSkinBinding`); hand UV/palette art pass still open | `docs/dev/evidence/rendered/hub-sky-atmosphere-2026-10-19.png` |
| Cold-start profile frontend (packaged, keyboard) | ✅ 3-slot Profile screen; Return creates slot 1 and enters the hub; Down+Return creates slot 2; both saves written | `docs/dev/evidence/rendered/profile-frontend-{cold-start,selected-slot1,slot-navigation}-2026-10-19.png` |
| Packaged input smoke (keyboard + mouse) | ✅ WASD moves, relative mouse motion rotates the camera, menu/slot navigation works; `J` is a keyboard attack key | `docs/dev/evidence/rendered/interactive-hub-{spawn-keyboard,walk-approach,camera-look}-2026-10-19.png` |
| Packaged `SIGKILL` at the hub boundary | ✅ save hash byte-identical before/after `kill -9`; relaunch → slot load → hub renders and writes a new autosave | `docs/dev/evidence/rendered/kill-restart-hub-{before-sigkill,after-restart}-2026-10-19.png` |
| Trial hazards under Low + low-flash (`-QinglanTrial=90` QA flag) | ✅ arena trial runs; wave-2 strike ground outlines render around the player and a spawned enemy renders with its mesh | `docs/dev/evidence/rendered/trial-lowflash-telegraph-wave2-dense-2026-10-19.png`, `trial-arena-enemy-visible-2026-10-19.png` |
| Cast cue and skill-slot cooldown | ✅ technique cast renders a cue with the slot entering cooldown | `docs/dev/evidence/rendered/trial-cast-cue-2026-10-20.png` |
| Cross-build probes (AUD-59) | ❌ UBT rejects Win64 and Mac: “Platform ... is not a valid platform to build. SDK validation failed” | `docs/dev/evidence/cross-build-win64-2026-10-20.log`, `cross-build-mac-2026-10-20.log` |
| Planning validators | ✅ pass | `python3 tools/validate_repository.py`, `tools/validate_story.py` |

Suite composition (70 cases): 27 pre-audit baseline cases plus 43 repair cases
covering narrative (courier/guard/skip), persistence (delivery/migration/
rotation/fault injection/process restart), trials (phase machine/hazards/
admission), combat (damage/status/gates/immunity), progression, animation and
VFX contracts, content/input/map-collision contracts, UI, and localization.

Scope: these results prove compilation, scripted subsystem behavior, content
generation, packaging, and the rendered checks listed. They do **not** prove a
complete interactive playthrough or target-platform acceptance. Current
dispositions for all 60 audit IDs are in
`docs/dev/completion-audit-todo.md`; owner steps for the remaining gates are in
`docs/dev/remaining-acceptance-runbook.md`.

## 1. Reproduce

```sh
bash tools/build/build-game.sh
bash tools/build/regenerate-content.sh
# automation (70 cases)
UnrealEditor-Cmd QinglanAscension.uproject \
  -ExecCmds="Automation RunTests Qinglan" -unattended -nop4 -nosplash \
  -nullrhi -TestExit="Automation Test Queue Empty"
# functional journeys
QINGLAN_RUN_CAMPAIGN=1 UnrealEditor-Cmd QinglanAscension.uproject \
  -ExecCmds="Automation RunTests Qinglan.Campaign.NewProfileMainJourney" ...
bash tools/build/package-game.sh
```

Rendered recipe (Xvfb + NVIDIA Vulkan ICD, packaged Shipping client,
`-QinglanTrial=<seconds>` for the arena, `-QinglanShot=<id>` for a single
screenshot): see `docs/dev/build-and-run.md`.

## 2. Deferred or environment-blocked items

- **Full interactive playthrough** (CH00–CH07, both realms/trials, every ending
  branch, defeat/retreat/retry, both input devices): functional journeys pass
  in automation, but a complete human-driven run on a stable display machine is
  still required (AUD-01/02/58).
- **Controller**: the kernel exposes uinput, but opening `/dev/uinput` returns
  `EPERM` (container device cgroup) and there is no `/dev/input`. The mapping
  contract is asserted by `Qinglan.Input.BothDeviceMappingContract`; a physical
  controller run is still needed (AUD-58).
- **Process kills at every promised boundary**: the hub boundary is exercised
  with a real `SIGKILL`; the realm/reward/exit/trial-prep boundaries are covered
  by `ProcessRestartResume` and fault-injection tests but not yet interactively.
- **Target graphics backends**: only the Linux Vulkan backend has rendered
  evidence; DX12/Metal captures require target builds/devices (AUD-42/AUD-59).
- **Hand art/UV pass** for the Quaternius palette skin (binding is verified; the
  visual art remains open, AUD-55).
- **Target-hardware performance capture**: `-csvprofile`/`-fpschart` packaged
  runs abort in the cluster GPU-contention `MallocCrash`; editor `-game` aborts
  with `TextLocalizationManager.cpp:629`. Needs a stable display machine
  (AUD-55).
- **Benchmark frontend map** (docs/06 §4) not authored.
- **Equipped-talisman selection UI** — slots seed from charged talismans in
  catalog order (`QinglanTalismanComponent`).
- **Niagara user parameters**: stock emitter templates expose no
  `User.Color`/`User.SpriteSize`, so element tinting/size stay at template
  defaults.
- **Herbal Knowledge** healing percent folds into talisman heals/regen; the
  projectile heal-tick keeps its FIXME-CONTRACT note.
- **Audio mix polish** pending listening QA on real speakers (4 project-original
  loops + 4 SFX are imported and wired).

## 3. Historical provenance

- 2026-10-13: first green Linux editor build and a 12-case automation suite.
- 2026-10-14: playable-loop layer landed (interaction dispatch at every anchor,
  HUD creation, scene/travel/room/death callers, objective-proof dispatcher,
  defeat banking). That work is included in the current 70-case suite.
- 2026-10-14: completion audit opened `docs/dev/completion-audit-todo.md`.
- 2026-10-19/20: repair waves (build/content repairs, narrative/persistence/
  trials/combat/UI, content regeneration, Niagara/ABP/sockets, cold-start
  frontend, rendered QA and packaging). Per-wave commits are in `git log`
  (`1c7e3a0`, `ed02e88`, `16b1a7b`, `f481f03`, `f438ee1`, `40213bd`,
  `36e16c5`, `73d9882`, `1c61a3c`, `b0088cf`, `96a8b7d`, `7aea4d8`,
  `8a0f72d`, `885da66`).
