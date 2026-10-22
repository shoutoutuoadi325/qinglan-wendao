# Remaining acceptance runbook (owner actions)

This runbook lists the only evidence that cannot be produced in the build
container. Each step names the audit IDs it closes, the exact command or
action, and the evidence to record. Update
`docs/dev/completion-audit-todo.md` only after the evidence exists — the
document's closure rule forbids ticking an item without it.

The acceptance attempt has moved to an Apple Silicon Mac. Follow
[local-mac-setup.md](local-mac-setup.md) for the removable workspace,
matching source engine, Xcode and raw-input preparation. The Linux evidence
below is historical. The Mac editor build now passes with the documented
host-tool patch.

**2026-10-21 native baseline: green.** `bash tools/build/run-acceptance.sh`
passed all six steps in `.local/acceptance/20261021T114346.991305Z/`: editor
build, content regeneration, **90/90** regular tests, the **30/30** main and
**24/24** side campaign journeys, and a Mac arm64 Shipping package. The package
contains the complete staged app (client `24d31c23…`, pak `51dd0db1…`); hashes
are in `packaging/evidence/mac-shipping-2026-10-21.txt`. The previous attempt's
three scene-inspection failures are fixed and covered by this run.

**Remaining gates below need an unlocked display and/or extra hardware.**
Starting with this pass the packaged client honors `-windowed` (the graphics
settings service no longer re-applies the saved fullscreen mode over the
command line) and produced real packaged Metal frames at 1280x720: the
cold-start Profile screen and a trial frame with two strike telegraphs plus a
wave enemy (`docs/dev/evidence/metal-rendering-2026-10-21.md`). The display was
still locked, though: menu keystrokes do not reach the app and `screencapture`
returns the lock screen, so slot selection, the hub gameplay frame and the
interactive routes remain owner actions. Shipping screenshots work when the
hook writes inside the package's container `Saved/Screenshots`.
The display was later unlocked: a real keyboard cold start → slot load → hub
render → WASD movement → hub-boundary `kill -9` → relaunch/resume pass is
recorded in `docs/dev/evidence/mac-interactive-hub-2026-10-21.md`. The rest of
sections 1, 3 and the Windows half of 2/4 still need the full playthrough,
performance route, DX12/Win64 hardware and a physical gamepad. See [test-report.md](test-report.md) for the exact
attempt and scope.

## Before you start (display machine)

1. Check out **`fix/audit-repairs`** (all repairs live there; it is not merged
   into `main` yet) and confirm `git status` is clean.
2. Set up the engine beside the repo and build/test with
   [build-and-run.md](build-and-run.md): `bash tools/build/build-game.sh`,
   then `bash tools/build/regenerate-content.sh`.
3. Re-run the baseline before changing anything: the full current suite, both
   campaign journeys (`QINGLAN_RUN_CAMPAIGN=1`), and
   `bash tools/build/package-game.sh`. Expected results and evidence paths are
   in [test-report.md](test-report.md) §0.
   On the prepared native host, `bash tools/build/run-acceptance.sh` runs the
   build/regeneration/suite/journeys/package sequence and validates exported
   results. It discovers the current test names from source (including newly
   authored cases); the historical baseline below contained 70 cases.
4. Then work through sections 1–5 below. Each closes specific AUD IDs in the
   disposition table of
   [completion-audit-todo.md](completion-audit-todo.md).

A physical gamepad is required for the controller half of AUD-58; a Windows
machine/SDK for DX12 + Win64, and a Mac with Xcode/Metal for Apple Silicon.

Environment blockers already measured in the container (2026-10-20):

- Controller: `/dev/uinput` exists but opening it returns `EPERM` (container
  device cgroup); `/dev/input` is absent. A physical controller or a host with
  uinput access is required.
- Platform builds: UBT rejects `Win64`/`Mac` with “Platform ... is not a valid
  platform to build. SDK validation failed”.
- Performance/rendered long runs: packed `-csvprofile`/`-fpschart` abort in the
  cluster GPU contention failure; editor `-game` aborts in
  `TextLocalizationManager.cpp:629`.

## 1. AUD-01 / AUD-02 / AUD-58 — interactive playthrough

Required machine: display + GPU, keyboard and a physical gamepad.

1. Build and run the Shipping client:
   `bash tools/build/package-game.sh`, then launch the platform's packaged
   client (`packaging/Linux/.../QinglanAscension-Linux-Shipping` on Linux,
   or the staged `QinglanAscension*.app` on Mac) with `-windowed`.
2. Cold start: confirm the 3-slot Profile screen appears (no auto-created
   slot). Select an empty slot, confirm the hub autosave exists.
3. Play CH00–CH07 in order (the HUD objective line names each scene), both
   realms, both trials, all 30 main scenes and 24 side stages/records.
   Play once with keyboard+mouse and once with the gamepad (sticks move/look,
   X attack, A dodge, Y interact, B cycle, shoulders/triggers/d-pad).
4. At each promised boundary (entrance, completed-room reward, exit, hub, trial
   prep, final decision): quit or `kill -9`, relaunch, load the same slot, and
   confirm exact state resumes with no reward replay.
5. Take one defeat (let a trial strike kill you), one retreat (expedition exit
   before completion), and one retry; confirm the banking percentages and the
   retry entry.
6. Capture screenshots/short clips per chapter + one per boundary and save
   under `docs/dev/evidence/rendered/playthrough-*`.

Evidence to record: checkpoint resume hashes, screenshots, controller model +
connection method, and the per-chapter manifest.
Use [checkpoint-evidence.md](checkpoint-evidence.md) to preserve the real
files and inspect checkpoint/ledger state without rewriting the save. The
export must accompany the actual fresh-process/UI verification.

## 2. AUD-42 — second graphics backend capture

1. On Windows: install the project and engine, run
   `QINGLAN_PLATFORM=Win64 bash tools/build/package-game.sh` (adjust engine
   root), then run the Windows client and use `-QinglanTrial=90` to start the
   arena trial; capture overlapping strike outlines + wave enemies in one frame
   with DX12.
2. On macOS (Apple Silicon): same with `QINGLAN_PLATFORM=Mac` and the Metal
   RHI. **Done 2026-10-21:** the packaged Shipping client produced
   `docs/dev/evidence/rendered/metal-shipping-trial-telegraphs-2026-10-21.png`
   (two strike telegraphs + a wave enemy in one frame); details in
   `docs/dev/evidence/metal-rendering-2026-10-21.md`. The package and hashes
   are in `packaging/evidence/mac-shipping-2026-10-21.txt`.
3. Save both images under `docs/dev/evidence/rendered/metal-*` /
   `dx12-*` and note the RHI + GPU model.

## 3. AUD-55 — hand art/UV and performance

1. Art: fix hand UVs/palette mapping on `SK_Human` against
   `T_Human_Clothed`. **Done 2026-10-21:** the grey blockout was a material
   compile fallback, not a UV defect. `M_QinglanSurface` now defaults the
   Roughness/AO samplers to a linear grayscale texture (so it compiles on
   Metal) and the master/MIC declare `MATUSAGE_SkeletalMesh`. Packaged
   close-ups: `docs/dev/evidence/rendered/metal-shipping-hand-near-2026-10-21.png`
   and `...-normal-...png`; details in
   `docs/dev/evidence/metal-rendering-2026-10-21.md`.
2. Performance: measure the actual Mac Shipping package with the external
   Apple Metal HUD/report procedure in
   [performance-capture.md](performance-capture.md), including real hub,
   combat-room and trial segments. Keep its report, settings and package hash.
   For separate Unreal diagnostics, the default Shipping
   target disables CSV profiling and command-line `ExecCmds` processing;
   `-ExecCmds="stat unit"`, bare `-csvprofile`, and bare `-fpschart` do not
   establish a Shipping performance capture. Build a **Development** package
   with `QINGLAN_CONFIG=Development bash tools/build/package-game.sh`, then
   follow [performance-capture.md](performance-capture.md). Enable
   `t.FPSChart.DoCsvProfile 1`; start/stop each hub, ordinary combat-room and
   normal-admission trial segment with `startfpschart <label>` /
   `stopfpschart`. Outputs go under the chosen `UserDir`'s
   `Saved/Profiling/FPSChartStats/<capture-date-time>/`. Preserve mean/P95
   frame/GPU times and sampled process-memory statistics with configuration,
   hardware, RHI, resolution, quality settings and actual duration. Save real
   evidence under `docs/dev/evidence/rendered/perf-*`; label Development/Test
   measurements separately from Shipping performance.
   **Shipping hub segment done 2026-10-21:** external Apple Metal HUD, 60 s,
   1,830 frames, frame interval mean/P95 33.34/33.34 ms, GPU mean/P95
   21.39/23.82 ms — `docs/dev/evidence/perf-mac-shipping-hub-2026-10-21.md`.
   The ordinary combat-room and normal-admission trial segments remain.
3. Optional objective metric: `UnrealInsights` trace of the same route.

## 4. AUD-59 — target platform builds

1. Windows: build `QinglanAscension Win64 Shipping` with a Windows SDK on a
   Windows host; run `BuildCookRun -platform=Win64 -clientconfig=Shipping`.
2. macOS: build/cook `Mac` (Apple Silicon) on a Mac with Xcode/Metal.
   **Done 2026-10-21:** arm64 Shipping `BuildCookRun` rc=0 and a complete
   staged app were archived by `run-acceptance.sh`; see
   `packaging/evidence/mac-shipping-2026-10-21.txt`.
3. Boot each package once, capture the hub frame, and record the binary/pak
   hashes in `packaging/evidence/<platform>-shipping-<date>.txt`.
   Mac hashes are recorded, the packaged client boots and renders, and both
   the cold-start Profile and hub courtyard frames are captured
   (`docs/dev/evidence/rendered/metal-shipping-hub-2026-10-21.png`; the hub
   frame uses the `-QinglanAutoSlot` capture aid). A separate real-keyboard
   hub pass (slot load, WASD, `kill -9`, relaunch) is recorded in
   `docs/dev/evidence/mac-interactive-hub-2026-10-21.md`. Win64 remains.

## 5. Closing the audit

For each ID above, replace the disposition row's “Remaining acceptance” with
“—”, link the new evidence, and keep the Environment evidence section as the
record of why the container could not produce it. Do not tick anything that
lacks the evidence named in the item's acceptance section.
