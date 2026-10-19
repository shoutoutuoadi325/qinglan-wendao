# Test Report — Qinglan: Path of Ascension

## 0. Current evidence (2026-10-19) — supersedes the historical section below

Environment: Linux (container, 64 physical cores), UE 5.8.3 source build, headless
(`-nullrhi`), branch `fix/audit-repairs` at `f481f03`.

| Item | Result | Evidence |
|---|---|---|
| Editor target build (Linux, Development, UHT + editor modules) | ✅ Succeeded | `docs/dev/evidence/build-2026-10-19-wave5.log` |
| Automation suite `RunTests Qinglan` | ✅ 49 started / 49 Success / 0 Fail | `docs/dev/evidence/automation-2026-10-19-wave5-49tests.log` |
| Planning validators | ✅ pass | `python3 tools/validate_repository.py`, `tools/validate_story.py` |

Test suite composition: 27 cases from the pre-audit baseline + 22 repair cases
across the audit waves (`Qinglan.Narrative.*` courier/guard/skip cases,
`Qinglan.Persistence.*` delivery/migration/rotation cases, `Qinglan.Trial.*`
phase-machine/hazard cases, `Qinglan.Combat.*` gate/immunity/status cases,
`Qinglan.Progression.*` exit/tier cases, `Qinglan.Anim.*`/`Qinglan.VFX.*`
contract cases, and `Qinglan.Localization.*`).

**Scope:** these results prove compilation and the scripted subsystem behavior
exercised by the specs. They do **not** prove a playable campaign, rendered
output, packaging, or target-platform acceptance. Unresolved audit IDs remain
listed in `docs/dev/completion-audit-todo.md`; no passage here closes them
without the acceptance evidence that document requires.

Build entry: `bash tools/build/build-game.sh` (portable; `QINGLAN_ENGINE_ROOT`
parameterizes the engine checkout). Content regeneration:
`bash tools/build/regenerate-content.sh`.

## 1. Historical results (2026-10-13) — retained for provenance, not current

Date: 2026-10-13 · Engine: UE 5.8.3 (source build) · Platform: Linux (Ubuntu 22.04, Xeon Gold 6530, RTX 4090 x2, driver 570.195)

| Target | Config | Result |
|---|---|---|
| QinglanAscensionEditor (Linux) | Development | ✅ compiles + links (runtime + editor modules, zero errors) |

Build entry at that time: `bash /inspire/hdd/project/qianghuaxuexi/s26044/build-game.sh` (an uncommitted private script; superseded by `tools/build/build-game.sh`).

Notable fixes required to reach green (recorded for posterity): ~40 UE 5.8 API
compatibility corrections (removed/renamed engine APIs, TObjectPtr map access,
TDereferenceWrapper sort predicates, TCheckedFormatString literals, UCommandlet
members), duplicate-symbol and unity-collision cleanups from the parallel module
pass, and missing module links (UMG runtime, InputCore editor).

## 2. Historical automation suite — 12/12 (2026-10-13; current suite is 49 cases)

Run:
```bash
UnrealEditor-Cmd QinglanAscension.uproject -ExecCmds="Automation RunTests Qinglan" \
  -unattended -nullrhi -TestExit="Automation Test Queue Empty"
```

| Test | Area | Covers |
|---|---|---|
| Qinglan.Combat.DamageMath | combat | raw→final damage formula, defense floor (docs/02 §5) |
| Qinglan.Combat.HitKeyDedupe | combat | one damage per victim per swing/cast id |
| Qinglan.Combat.ConductSingleApplication | elements | Conduct reaction applies once per wet window |
| Qinglan.Combat.BurnThreeTickRefresh | elements | Burn 3-tick refresh semantics (docs/02 §7) |
| Qinglan.Combat.BarrierReplaceAbsorb | combat | stronger barrier replaces, absorbs before health |
| Qinglan.Economy.BankingMultipliers | economy | 100/70/50 % banking, per-stack floor |
| Qinglan.Narrative.EndingGating | narrative | three-ending availability from narrative state |
| Qinglan.Narrative.EndingRulesJsonDrift | narrative | hardcoded ending rules == data/story/main-late.json CH06_S04 payloads |
| Qinglan.Persistence.SaveChecksumRoundtrip | save | checksummed container roundtrip, corrupt + future-schema rejection |
| Qinglan.Persistence.TransactionIdempotency | save | grant/spend replay never double-applies |
| Qinglan.Progression.RankCostsAndBreakthroughGates | progression | rank costs, boss/trial gates, breakthrough resets |
| Qinglan.Blessings.Exclusion | run | one-stack-cap blessings never re-offered |

Bug found and fixed by the suite itself: the save container writer hashed only
the body while the parser verified header+body — every verified save failed its
own checksum (Qinglan.Persistence.SaveChecksumRoundtrip).

## 3. Content pipeline (headless commandlets) ✅

All five commandlets run clean and idempotently (`docs/dev/build-and-run.md`):

1. QinglanDataImportCommandlet — 30 main scenes (CH00–CH07), 24 side stages
   (SQ01–SQ08), 24 records, 40 barks, 8 techniques, 2 realms, ST_Game (97 keys
   per locale).
2. QinglanContentBootstrapCommandlet — 15 input assets, 24 materials (21
   PolyHaven surface sets + 3 VFX cue materials), 4 maps.
3. QinglanMeshImportCommandlet — 6 skeletal characters + ~45 static
   meshes/weapons/nature from CC0 Quaternius packs.
4. QinglanAnimBootstrapCommandlet — 8 combat montages with hit-window
   notifies, paired into all 8 technique assets.
5. QinglanUIBootstrapCommandlet — 4 widget blueprints (HUD/Dialogue/Journal/Ending).

## 4. Headless gameplay smoke (-nullrhi) ✅

Boot via GameDefaultMap (L_Hub):

- `Hub mode active (sanctuary)` — game mode boots
- First launch: profile slot 0 created; `SaveNow: slot 0 sequence 1 committed`
- Relaunch: `Continuing from profile slot 0`; `sequence N+1 committed` with
  backup rotation (.bak1/.bak2) — full save lifecycle verified on disk
  (`game/Saved/SaveGames/`)
- Story registry resolves (StaticLoadObject path)

Functional story-branch/save coverage additionally comes from the automation
suite (ending gating + JSON drift guard + save roundtrip); interactive
playthrough needs a display, see §6.

## 5. Bilingual check ✅

- `localization/en-US.json` / `zh-Hans.json`: 97/97 key parity; all story
  chapter/side-quest/lore/bark files verified key-identical across locales
  (audit 2026-10-13), first-launch locale auto-pick implemented
  (zh-Hans when OS language matches).

## 6. GPU rendering on this container (2026-10-14 status)

Achieved from inside the container: installed the Vulkan loader, ran a
headless Xorg on the idle 4090 (NVIDIA screen up at 1280x720), enumerated the
GPU through the proprietary Vulkan driver (vulkaninfo full pass), and brought
the game up on the Vulkan RHI — UE created a live swapchain on the 4090
(AcquireNextImage cycle in the editor -game log). Fixes that came out of this
and ship in the repo: `r.WarnOfBadDrivers=0` (the denylisted-driver modal
dialog blocks forever under no window manager), XDG_RUNTIME_DIR ownership
rule, zen-store recovery steps (docs/dev/build-and-run.md).

Blocked at host level, not fixable in-container: mid-session the cluster
re-assigned the GPUs and re-injected driver 570.124.06 userspace, zeroing the
nvidia X driver files (`nvidia_drv.so` 0 bytes everywhere; Ubuntu archives
ship doc-only stubs for NVIDIA blobs, and downloads.download.nvidia.com
returns 403 through the cluster proxy). A full gameplay session with frame
capture therefore needs either a host driver re-injection with a consistent
userspace (rerun the steps in build-and-run.md — everything else is in place)
or a display machine.

Still pending on target machines:

- **GPU frame-time pass** (docs/06 §5 budgets: 16.6 ms @ Medium) — the
  swapchain ran; sustained frame stats need the stable-display session above.
- **Windows DX12 / Apple Silicon Metal** builds compile-configured
  (DefaultEngine.ini sets D3D12/Metal/Vulkan per platform) but are **not built
  or validated** on this machine — pending target-platform verification per
  docs/09.
- Interactive controller/keyboard play (Enhanced Input mappings authored and
  assets generated; not drivable headless).

## 7. Known deferred items

- Benchmark frontend map (docs/06 §4) not authored; first launch goes straight
  to the hub with profile auto-creation.
- Profile-select menu (3 slots) — boot continues from slot 0 by default.
- Equipped-talisman selection UI — slots seed from charged talismans in
  catalog order (documented in QinglanTalismanComponent).
- Niagara particle systems for technique cues — the soft references stay null
  (material/decal VFX and the audio pass cover presentation); authoring
  Niagara assets programmatically is not supported, needs an editor session.
- Herbal Knowledge healing percent folds into talisman heals and regen; the
  projectile heal-tick keeps its FIXME-CONTRACT note.
- Audio pass (2026-10-14): 4 music loops (Hub/Explore/Combat/Boss, crossfade
  state machine) + 4 combat SFX imported and wired; mix polish pending real
  speakers.

### Playable-loop layer (2026-10-14)

The "is it actually complete" audit found the systems all present and tested,
but the glue that lets a player reach them was missing (interact stub, no HUD
creation, no scene/travel/room/death callers, no Wet source, no boss proof
banking, no objective completion callers). All landed and verified
(12/12 suite, hub spawns 25 interactables, realm 11 + director):

- interaction dispatch at every bootstrap anchor; synthesized gates and room
  doors; blessing draft at doors; cultivation shrine purchases/breakthroughs;
  workbench relic/passive shop; defeat banking + hub return
- objective-proof dispatcher (14 choice-free scenes previously never
  completed: the campaign stalled at CH00_S02)
- remaining presentation gaps: WBP_HUD/WBP_Dialogue widget *assets* are
  bootstrapped shells, so text currently renders through on-screen messages
  (C++ setters + relay are in place for authored UMG fronts); trial
  waves/timers (CH03_S02/CH06_S02) stand in as anchor re-press proofs until
  the trial-storm system exists; five scaffold notices use NSLOCTEXT pending
  string-table keys.
