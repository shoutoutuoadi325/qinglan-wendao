# Test Report — Qinglan: Path of Ascension

## Native Mac baseline passed (2026-10-21; rendered/interactive gates still open)

`bash tools/build/run-acceptance.sh` completed green on the owner Apple Silicon
Mac: `.local/acceptance/20261021T114346.991305Z/` (`status: passed`; engine
`396c9f059903aed5fec78ecd3d437a40c6415368` = UE 5.8.3; branch
`fix/audit-repairs` at `8fe547a` with the repair working tree).

| Step | Result |
|---|---|
| Editor build | passed |
| Content regeneration (6 commandlets) | passed |
| Regular suite | **90/90** named results (0 failed, 0 not run) |
| New-profile main journey | **30/30 scenes**, 0 blocked |
| New-profile side-quest journey | **24/24 stages**, 0 blocked |
| Mac Shipping package (arm64) | UAT `BuildCookRun` rc=0; complete staged `.app` archived |

The Mac archive is now the complete staged bundle. UAT's `-archive` copies the
pre-stage `Binaries/Mac` app shell, which has no `Contents/UE`, so
`package-game.sh` skips that archive on Mac and copies
`Saved/StagedBuilds/Mac/QinglanAscension-*.app` itself. Artifact hashes and
host details: `packaging/evidence/mac-shipping-2026-10-21.txt`
(client `24d31c23…`, pak `51dd0db1…`, ucas `43aad57f…`).

Three scene-inspection regressions failed in the previous attempt and are
fixed and covered by this run:

- `Qinglan.Narrative.InspectionShutterOpenClose` /
  `Qinglan.Narrative.SceneInspectionRealReadFlow`: the fixture controller had
  no `PlayerState`, but UE 5.8 `APawn::IsPlayerControlled()` requires a
  non-bot PlayerState. The fixtures now supply one, as a logged-in player has.
- `Qinglan.Narrative.AshenInspectionDialogueAnchors`: the chapter scenes pause
  at an unselected choice, so `SkipScene` left playback active and blocked the
  next anchor. The test now re-imports its fixture state between anchors.
- `QinglanHUDStatics::CreateWidgetById` now returns no widget when the world's
  controller is not a local player controller, matching UMG's ownership rule
  instead of logging a PIE error in headless/automation worlds.

The pass also fixed a startup settings bug: `QinglanGraphicsSettingsService`
called `UGameUserSettings::ApplySettings(false)`, whose bool is
`bCheckForCommandLineOverrides`, not a validation switch. That silently
re-applied the saved fullscreen mode over `-windowed`/`-Res` and could block
before the first frame. It now passes `true`, and a packaged windowed client
stays up and renders on this Mac (verified by process state and the Metal
frames below).

The hero palette was also repaired in the content bootstrap. `M_QinglanSurface`
failed to compile on Metal because its Roughness/AO samplers defaulted to the
color `DefaultTexture`, and `MIC_HumanCharacter` lacked the SkeletalMesh usage
flag, so the pawn fell back to the grey Default Material. The master now uses
linear-grayscale defaults and both materials declare their mesh usages; the
packaged Metal frames show tan skin, brown hair and the blue clothed bands,
including near/normal hand close-ups.

Host-side wrappers/parsers/localization tests: **47 cases, 45 passed, 2
skipped** with the updated Mac packaging contract (the two skips are the
optional native Oodle/UEWavComp decode cases, which need the local official
libraries and blake3).

**Rendered status:** the packaged Shipping client starts windowed, stays up
and produced real Metal frames at 1280x720:
`docs/dev/evidence/rendered/metal-shipping-hub-2026-10-21.png` (hub courtyard
with the upright, palette-textured hero after the QA slot confirmation),
`docs/dev/evidence/rendered/metal-shipping-profile-2026-10-21.png` (3-slot
cold-start screen over the hub) and
`docs/dev/evidence/rendered/metal-shipping-trial-telegraphs-2026-10-21.png`
(two lightning-strike ground telegraphs and a wave enemy in one frame), plus
hand close-ups at 35 cm and 150 cm showing the palette skin
(`metal-shipping-hand-{near,normal}-2026-10-21.png`). The QA
screenshot hook now writes under the package's `Saved/Screenshots`; the old
`/tmp` path was denied by the sandbox, and `-UserDir` outside the container is
not writable. Developer `-game` comparison frames are in the same directory.
Details: [metal-rendering-2026-10-21.md](evidence/metal-rendering-2026-10-21.md).

**HUD crash root cause fixed (2026-10-21):** the invalid-`FName` crash was
caused by `UQinglanTechniqueComponent::Loadout` and `CurrentTechnique` holding
`TObjectPtr` values without `UPROPERTY()`, so GC freed the technique
DataAssets while the HUD still read them. Both fields are now `UPROPERTY()`;
the 60 s Shipping hub route completes without a crash (it previously crashed
within ~10 s). `GameText` and the HUD technique loop also guard invalid `FName`s
defensively.

**Shipping Metal HUD hub measurement (2026-10-21):** packaged Shipping client,
M4, Metal, 1280x720, 60 s hub route with movement and camera rotation. External
Apple Metal HUD per-frame log: frame interval mean/P95 33.34/33.34 ms (30 FPS
cap; max 50.00), GPU mean/P95 21.39/23.82 ms (max 40.22, no unavailable
samples), 1,830 frames. Evidence:
[perf-mac-shipping-hub-2026-10-21.md](evidence/perf-mac-shipping-hub-2026-10-21.md).
Combat-room and normal-admission trial segments remain open.

**Development performance attempt (2026-10-21):** the Development package now
builds and archives correctly (the wrapper accepts the config-less
`QinglanAscension.app` name). An FPS-chart run started
(`LogChartCreation: Started creating FPS charts`) but did not finalize a frame
table, and the Development client crashed twice in
`UQinglanHUDWidget::RefreshFromPlayer -> GameText -> FName::ToString` with an
invalid `FName`. `GameText` now checks `FName::IsValid()` and returns an
`<invalid text key>` placeholder; the client stayed alive afterwards, but the
root cause of the corrupted name is unresolved and the skill HUD showed the
placeholder. Performance capture therefore remains open:
[mac-performance-attempt-2026-10-21.md](evidence/mac-performance-attempt-2026-10-21.md).

**Interactive Mac hub (2026-10-21):** after the display was unlocked, the
packaged Shipping client accepted real keyboard input: Return loaded the saved
slot, W moved the character, the hub autosave was written, a real `kill -9` left
the save files byte-identical, and relaunch → slot load resumed the hub and
wrote a new autosave. Evidence:
[mac-interactive-hub-2026-10-21.md](evidence/mac-interactive-hub-2026-10-21.md).
A complete CH00–CH07 keyboard playthrough, the gamepad half, realm/trial/ending
boundaries and defeat/retreat/retry remain. AUD-42 still needs the DX12 capture; AUD-55
performance capture needs an unlocked interactive route; AUD-59 still needs
the hub frame and the Win64 package. Lock evidence:
[mac-display-locked-2026-10-21.md](evidence/mac-display-locked-2026-10-21.md).

## Local Mac continuation (2026-10-21; interactive acceptance still open)

The matching UE 5.8.3 source checkout is now present under `.local`; native
dependencies and the separate Metal Toolchain are installed;
Xcode 27.1's first-launch check and the actual Metal compiler probe pass.
UBT builds with zero warnings/errors, the Game/Editor target query passes,
and ShaderCompileWorker completed 215 actions as native arm64. The project
editor's UHT and native build now pass with the documented Xcode-stdin host
patch. Evidence: `.local/acceptance/20261020T095017.651759Z/01-build-editor.log`
(`Result: Succeeded`, wrapper exit 0). The host is an Apple M4 Mac
with 32 GB RAM. Source changes repair host dispatch, raw-asset paths and the
hand palette import recipe. The native attempt
`.local/acceptance/20261020T095933.667543Z/` passed editor build and all six
regeneration commandlets, then **72/72** named suite results with zero errors
(54 successes without warnings, 18 with warnings). Its separately opted-in
journeys completed **30/30 main scenes** and **24/24 side stages**, zero blocked.
These are headless subsystem/content checks, not interactive playthroughs.

That attempt stopped before native game compilation/cooking because UAT
needed three files excluded by the Mac-only dependency selection. The exact
official manifest inputs were restored from cached packs with SHA1/size
verification (zero download bytes). A real `RunUAT.sh -CompileOnly` now exits
0; evidence is `.local/bootstrap/uat-input-evidence.json` and
`.local/bootstrap/uat-compile-only-20261021.log`. This does not prove packaging.

The warning review exposed missing blessing text, malformed named backup
formatting, obsolete AI audio and hero animation paths, and ineffective Low
GI/reflection switches. Repairs and regression coverage are being validated.
The next native attempt `.local/acceptance/20261021T011426.683609Z/` passed
build and regeneration, but its expanded suite hit a genuine localization
resource-target assertion in the new dual-culture recovery test. The runtime
resource identity has been repaired; a fresh complete run remains required.
Do not treat either incomplete attempt as Mac Shipping/runtime acceptance.

Build-wrapper regression checks pass (8 executable-fixture cases), and
acceptance-runner checks pass (13 report/process-fixture cases). Eight
original WAVs were recovered with exact import-MD5 matches; the recovery
format and native-decoder regression checks also pass. Together with the
performance-summary fixtures, localization and UAT-input checks, the host-side suite passes **46/46** cases
with native audio decoding enabled. These are host-side checks, not Unreal
runtime acceptance. Raw-root precedence and read-only checkpoint inspection
both passed in the 72-case native attempt. Further regressions expand current
source discovery; the runner requires the exact current set. The inspection commandlet and evidence
procedure are described in [checkpoint-evidence.md](checkpoint-evidence.md).
Planning/story validators and the
18 generated bilingual reading editions check also pass. CH04–CH07 reading locations were regenerated from current
story data. Setup and remaining gates:
[local-mac-setup.md](local-mac-setup.md).

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
# automation (all current source registrations; historical Linux run: 70 cases)
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
