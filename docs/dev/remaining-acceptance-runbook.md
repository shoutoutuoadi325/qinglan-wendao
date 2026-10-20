# Remaining acceptance runbook (owner actions)

This runbook lists the only evidence that cannot be produced in the build
container. Each step names the audit IDs it closes, the exact command or
action, and the evidence to record. Update
`docs/dev/completion-audit-todo.md` only after the evidence exists — the
document's closure rule forbids ticking an item without it.

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
   `bash tools/build/package-game.sh && ./packaging/Linux/QinglanAscension/Binaries/Linux/QinglanAscension-Linux-Shipping -windowed`
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

## 2. AUD-42 — second graphics backend capture

1. On Windows: install the project and engine, run
   `QINGLAN_PLATFORM=Win64 bash tools/build/package-game.sh` (adjust engine
   root), then run the Windows client and use `-QinglanTrial=90` to start the
   arena trial; capture overlapping strike outlines + wave enemies in one frame
   with DX12.
2. On macOS (Apple Silicon): same with `QINGLAN_PLATFORM=Mac` and the Metal
   RHI; capture the same trial frame.
3. Save both images under `docs/dev/evidence/rendered/metal-*` /
   `dx12-*` and note the RHI + GPU model.

## 3. AUD-55 — hand art/UV and performance

1. Art: fix hand UVs/palette mapping on `SK_Human` against
   `T_Human_Clothed` (the material binding is verified by
   `Qinglan.Content.HeroSkinBinding`; the remaining gap is the art pass).
   Re-run `bash tools/build/regenerate-content.sh` and capture a close-up.
2. Performance: on target hardware run the Shipping client with
   `-ExecCmds="stat unit"` (Development/Test config if CSV profiling is
   needed), play the hub, a combat room, and the trial; capture average frame
   time, GPU time, and memory. Save under `docs/dev/evidence/rendered/perf-*`.
3. Optional objective metric: `UnrealInsights` trace of the same route.

## 4. AUD-59 — target platform builds

1. Windows: build `QinglanAscension Win64 Shipping` with a Windows SDK on a
   Windows host; run `BuildCookRun -platform=Win64 -clientconfig=Shipping`.
2. macOS: build/cook `Mac` (Apple Silicon) on a Mac with Xcode/Metal.
3. Boot each package once, capture the hub frame, and record the binary/pak
   hashes in `packaging/evidence/<platform>-shipping-<date>.txt`.

## 5. Closing the audit

For each ID above, replace the disposition row's “Remaining acceptance” with
“—”, link the new evidence, and keep the Environment evidence section as the
record of why the container could not produce it. Do not tick anything that
lacks the evidence named in the item's acceptance section.
