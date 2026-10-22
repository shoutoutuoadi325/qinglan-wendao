# Local Mac acceptance setup

This continues the [remaining acceptance runbook](remaining-acceptance-runbook.md)
on the owner's Apple Silicon Mac. It does not close any runtime acceptance gate.

## Local paths and cleanup

Run from the repository root:

```sh
source tools/build/local-env.sh
```

The engine is in `.local/toolchains/UnrealEngine`, raw inputs in
`.local/assets-raw`, downloaded installers in `.local/downloads`, and the
main dependency/build caches in `.local/cache`. These paths are ignored by
Git. Put the extracted Xcode application at `.local/toolchains/Xcode.app`;
source the environment again afterwards to set `DEVELOPER_DIR` for this shell.
No global `xcode-select` or shell-profile change is required. VS Code can
remain the code editor.

Epic's [Mac requirements](https://dev.epicgames.com/documentation/unreal-engine/macos-development-requirements-for-unreal-engine)
recommend Xcode 26.1.1 for UE 5.8 and explicitly exclude Xcode 26.4. Obtain
the matching application from [Apple Downloads](https://developer.apple.com/download/all/?q=Xcode%2026.1.1).
The user installed Xcode **27.1 (27A9275)** in `/Applications/Xcode.app`.
`xcodebuild -checkFirstLaunchStatus` passes, the selected macOS SDK is 27.0,
and clang reports 21.0.0. The locked engine's `Apple_SDK.json` allows Xcode
through 27.9.0; the native editor now builds successfully with the local
host-tool patch described below. The user's existing Xcode installation and
global developer selection are retained.

Although `xcrun --find metal` resolves a path, executing `metal --version`
initially reported a missing Metal Toolchain. Its official download completed:

```sh
xcodebuild -downloadComponent MetalToolchain \
  -exportPath "$PWD/.local/downloads/MetalToolchain"
```

The approximately 839 MB download also installed Metal Toolchain **27A266a**.
`xcodebuild -showComponent MetalToolchain -json` reports `installed`, and an
actual `xcrun metal --version` invocation succeeds with version **32023.921**.
The exported bundle is under `.local/downloads/MetalToolchain`; the installed
component is an Apple-managed system asset outside the repository. If no
longer needed after development, Apple's removal command is
`xcodebuild -deleteComponent MetalToolchain`. Removing the export alone does
not remove that installed component. The user-installed Xcode application
also remains in `/Applications`.

When development is finished, stop the editor, builds and download processes
before removing `.local`. Generated project files also live in the ignored
`game/Binaries`, `game/Intermediate`, `game/Saved`, `game/DerivedDataCache` and
platform directories under `packaging`. Preserve saves, acceptance evidence
and any wanted packages before cleaning them.

This is a removable workspace, not a claim that every vendor tool writes
only here. In particular, the default UE Zen service installs into the user's
Library even when its data cache is redirected. For commandlet acceptance,
use a file-based DDC graph (`-DDC=(ProjectPak,EnginePak,Local)`) to avoid that
service. Xcode's first-launch components may also require system locations.
The current commandlet also writes its duplicate Mac log under
`~/Library/Logs/Unreal Engine/QinglanAscensionEditor/`; the baseline's captured
stdout remains in its workspace evidence directory. Preserve wanted evidence
before cleaning those project-specific logs.

## Engine preparation

The official source checkout has been acquired and verified:

- UE version: **5.8.3**, compatible changelist **55116800**.
- Engine commit: **396c9f059903aed5fec78ecd3d437a40c6415368**, matching
  [`versions.lock`](../../tools/build/versions.lock).
- Local host: MacBook Air (`Mac16,12`), Apple M4 (10 CPU / 10 GPU cores),
  arm64, 32 GB unified memory, macOS 27.0.1 (`26A434`). System Profiler reports
  Metal 4 support. The attached main display reports 1920×1080 at 60 Hz and
  the machine is on AC power. The configuration snapshot is
  `.local/engine-build/mac-host-evidence.json`; it was taken during compilation
  and is not a game-performance result. Record the actual game resolution,
  quality and power state again for each performance capture.
- Initial developer selection: Command Line Tools only; subsequently the user
  installed Xcode 27.1 as described above.

The dependency download can run before Xcode is installed:

```sh
source tools/build/local-env.sh
"$QINGLAN_ENGINE_ROOT/Engine/Binaries/DotNET/GitDependencies/osx-arm64/GitDependencies" \
  --root="$QINGLAN_ENGINE_ROOT" --force --threads=8 \
  --exclude=Win32 --exclude=Win64 --exclude=Linux --exclude=LinuxArm64 \
  --exclude=Android --exclude=IOS --exclude=TVOS --exclude=VisionOS \
  --cache="$QINGLAN_LOCAL_ROOT/cache/gitdependencies"
```

The completed download log is `.local/downloads/engine-dependencies.log`:
127,139 files, 23,346.6 MiB, process exit 0. This Mac-only selection does not
provide a Windows build toolchain. Do not run a second dependency sync while
another sync is active.

The Mac-only sync excludes three small inputs that AutomationTool still
compiles into its platform modules: `BouncyCastle.Crypto.dll` and the IOS
module's `GreenCheck.png` / `GreyCheck.png`. Restore those exact manifest
entries before running UAT or the acceptance baseline, without enabling the
full IOS dependency download:

```sh
python3 tools/build/restore-uat-inputs.py \
  --engine "$QINGLAN_ENGINE_ROOT" \
  --cache "$QINGLAN_LOCAL_ROOT/cache/gitdependencies" \
  --evidence "$QINGLAN_LOCAL_ROOT/bootstrap/uat-input-evidence.json"
```

The tool reuses verified cache packs, otherwise downloads only the two small
packs from the manifest's official HTTPS CDN. It verifies compressed sizes,
uncompressed pack SHA1 and each extracted file's SHA1, and refuses to
overwrite a different existing input. The current locked manifest needs
1,557,620 compressed bytes for these packs; existing valid inputs require no
download. Use `--verify-only` to check an already prepared checkout. The
original failure and restoration are recorded under `.local/bootstrap/`;
`uat-compile-only-20261021.log` records a subsequent successful
`RunUAT.sh -CompileOnly` run. This establishes script compilation, not a
successful Shipping package.

Call GitDependencies directly rather than `Setup.command`: the standard Mac
setup additionally copies `UnrealEditorServices.app` into `~/Library/Services`.
The APFS checkout here is case-insensitive, so the setup's case-fix pass is
unnecessary. Once dependencies and Xcode are ready, build the source engine's
tools before the project:

```sh
source tools/build/local-env.sh
git -C "$QINGLAN_ENGINE_ROOT" apply \
  "$QINGLAN_PROJECT_DIR/../tools/build/patches/ue583-mac-xcode-stdin.patch"
cd "$QINGLAN_ENGINE_ROOT"
bash Engine/Build/BatchFiles/BuildUBT.sh
bash Engine/Build/BatchFiles/Mac/Build.sh ShaderCompileWorker Mac Development \
  -architecture=arm64 -MaxParallelActions=6 -UBAHost=127.0.0.1
bash Engine/Build/BatchFiles/Mac/Build.sh QinglanAscensionEditor Mac Development \
  "-project=$QINGLAN_PROJECT_DIR/QinglanAscension.uproject" \
  -architecture=arm64 -MaxParallelActions=6 -UBAHost=127.0.0.1
```

Build **and publish** UBT after the native dependency files exist, so its
published `runtimes/osx-arm64/native` directory includes UBA. Merely compiling
UBT before the dependency download finishes can produce a managed assembly
without its native executor library. `BuildUBT.sh` performs the publish step.

Apply the tracked patch once to a fresh matching engine checkout. It gives
Xcode a valid standard-input pipe during app finalization. UBA's Mac spawn
path closes file descriptors that it has not explicitly preserved; a direct
Xcode invocation with closed stdin reproduces the same failed shell
pre-action, while normal stdin succeeds. The native build passes after the
patch and UBT rebuild/publish. Details:
[mac-xcode-stdin-2026-10-20.md](evidence/mac-xcode-stdin-2026-10-20.md).
The engine HEAD still matches `versions.lock`; its local working tree includes
this host-tool patch, which the acceptance manifest records as dirty.

Current native evidence: UBT builds with zero warnings/errors; its target
query resolves the Game and Editor targets. ShaderCompileWorker completed
all 215 actions and is a Mach-O arm64 executable. The project editor's UHT
step and native editor build pass. The successful baseline build log is
`.local/acceptance/20261020T095017.651759Z/01-build-editor.log`. Other logs are in
`.local/bootstrap/` and `.local/engine-build/`. The local environment now sets
`UBA_ROOT` under `.local/cache/UnrealBuildAccelerator`; this path is confirmed
in the build log. Attempt `20261020T095933.667543Z` also passed all six
regeneration commandlets, 72 named regular-suite results, and separately
opted-in 30-scene/24-stage journeys. Later regression work exposed additional
runtime defects; see [test-report.md](test-report.md) for the exact latest
status.

The 2026-10-21 baseline is fully green: 90/90 regular tests, 30/30 main and
24/24 side campaign journeys, and a Mac arm64 Shipping package. UAT's Mac
`-archive` copies the pre-stage `Binaries/Mac` app shell (no `Contents/UE`), so
`package-game.sh` skips that archive and copies the complete
`Saved/StagedBuilds/Mac` app instead. Hashes:
`packaging/evidence/mac-shipping-2026-10-21.txt`.

The packaged client now honors `-windowed` at startup: the graphics settings
service was calling `ApplySettings(false)`, which re-applied the saved
fullscreen mode over the command line and could block in
`FMacWindow::WaitForFullScreenTransition`; it now passes `true`. A windowed
packaged client stays up and renders. Interactive input still needs an
unlocked session (keys do not reach the app while macOS reports
`CGSSessionScreenIsLocked=true`), and Shipping `FScreenshotRequest` produced no
file in that state. A developer `-game` run rendered the cold-start screen on
Metal (`docs/dev/evidence/rendered/metal-editor-game-profile-2026-10-21.png`).
Unlock the Mac for the remaining Metal captures, performance route and
interactive playthroughs. See
[mac-display-locked-2026-10-21.md](evidence/mac-display-locked-2026-10-21.md).

The wrappers select the engine tools from the **host**, independently of the requested target.
Their regression checks use executable fixtures and do not establish a
successful native Unreal build.

After engine preparation, run the reproducible native baseline:

```sh
bash tools/build/run-acceptance.sh
```

It builds the editor modules, regenerates content, runs the full regular
suite and both opt-in campaign journeys in separate editor processes, then
packages Shipping into a fresh `.local/acceptance/<UTC timestamp>/` directory.
Each run records source revisions, dirty status, logs and exported automation
reports. Named test results must match current source registrations; a zero
exit code, missing report or skipped opt-in journey cannot pass. This
baseline does not replace interactive playthrough, controller or device
acceptance. Preserve a successful run's evidence before cleaning `.local`.

## Raw inputs and source repairs

The Quaternius character/monster/nature/weapon archives and the 81 Poly Haven
JPGs corresponding to committed texture slots were downloaded into the local
raw tree. Download receipts, source pages and MD5/SHA256 checks are in
`.local/assets-raw/provenance/acquisition-2026-10-20.json`.

The eight original WAV sources were absent from the repository. They have
now been recovered from the committed SoundWave imported data with official
Oodle and UEWavComp decoders, and all eight recovered MD5 values match the
original import records. The originals are in
`.local/assets-raw/audio/original/{music,sfx}`. No replacement audio was
synthesized. Recovery details and hashes are in
[audio-original-recovery-2026-10-20.json](evidence/audio-original-recovery-2026-10-20.json).
To reproduce with the matching engine and Command Line Tools:

```sh
python3 -m pip install --target .local/python-packages blake3
PYTHONPATH="$PWD/.local/python-packages" python3 tools/recover_audio_originals.py \
  --engine-root "$QINGLAN_ENGINE_ROOT" --assets-raw "$QINGLAN_ASSETS_RAW" \
  --work-dir "$PWD/.local/downloads/oodle-audio-recovery" \
  --report "$PWD/.local/downloads/audio-recovery.json"
```

Use `--verify-only` to repeat the decoder/hash checks without writing WAVs.
The recovery utility rejects unsupported container/codec layouts and hash
mismatches before writing any audio. Native audio regeneration subsequently
passed in the baseline above; listening QA remains open.

The importers now share `-assetsraw=<root>` / `QINGLAN_ASSETS_RAW` instead of
private Linux paths, and missing mandatory inputs fail the operation. The
hand palette recipe is repaired at source level; its FBX/PNG inspection and
remaining rendered checks are documented in
[human-palette-source-2026-10-20.md](evidence/human-palette-source-2026-10-20.md).

Re-run the full current suite and both campaign journeys after repairs, then
package before capturing Metal gameplay and performance evidence. A Mac run
cannot supply Windows DX12 device evidence or a physical controller run
without the corresponding hardware.

On 2026-10-20, the Mac's IOHID inventory contained no game controller
(Generic Desktop usages 4/5/8), and the owner confirmed that no physical
controller is currently available. Keep the controller portion of AUD-58
open; input mappings and keyboard automation cannot replace it.
