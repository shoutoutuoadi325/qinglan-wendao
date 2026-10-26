# AUD-55 performance capture on a display machine

This is a preparation procedure. No real performance samples were collected
while writing it. Shipping remains the package for interactive and visual
acceptance; use a **Development** package for the counters below and label
the resulting numbers Development. They do not establish Shipping performance.

## Shipping measurements on macOS

For the actual Shipping package, use Apple's external Metal Performance HUD;
it does not depend on Unreal's compiled-out console counters. Launch the
staged native executable directly with process-scoped environment variables:

```bash
SHIPPING_PERF_ROOT="$PWD/.local/perf/shipping-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$SHIPPING_PERF_ROOT"
CLIENT='/actual/path/to/the/Shipping/app/Contents/MacOS/executable'
MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1 \
  MTL_HUD_REPORT_URL="$SHIPPING_PERF_ROOT/metal-report.html" \
  "$CLIENT" -windowed -ResX=1280 -ResY=720 \
  "-UserDir=$SHIPPING_PERF_ROOT/user" \
  > "$SHIPPING_PERF_ROOT/client-output.txt" 2>&1
```

Select the real Shipping artifact and preserve its hash. Verify that the HUD
actually appears before measuring. Use its Metal HUD menu to request a
performance report for the warmed gameplay segment. Apple documents a
three-second lead-in before collection starts. Keep the report's actual
duration, start/end frames, metric labels and units with the matching route
clip and settings. Repeat for the hub, ordinary combat and trial routes below.
The environment variable selects an output path; it does not start a report.

Preserve the generated report and per-frame log output, and take a screenshot
showing the game, HUD, device and resolution. If per-frame messages are sent
to the system log instead of the redirected output on this OS version,
capture only this application's HUD messages with Console or `log stream`,
then verify their process identity and measurement interval. Do not collect
unrelated applications' logs. No global `launchctl setenv`, defaults or
application-bundle modification is needed.

HUD presentation interval and GPU timing are external Metal measurements;
label them as such rather than renaming them Unreal game/render-thread
timings. Retain the report's memory definitions and units, and do not add
graphics and process allocations into an invented unified-memory total.
Use the Development procedure below for separate engine diagnostics. The
Unreal CSV parser does not parse Apple's report format. A 60 s packaged Shipping hub
segment has been captured and summarized in
`docs/dev/evidence/perf-mac-shipping-hub-2026-10-21.md`; only the Instruments
HTML report is still absent.

Sources: Apple's [HUD monitoring](https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance),
[HUD customization](https://developer.apple.com/documentation/xcode/customizing-metal-performance-hud)
and [performance reports](https://developer.apple.com/documentation/xcode/generating-performance-reports-with-metal-performance-hud).

### Shipping PID capture with Instruments

The installed Xcode's `xctrace list templates` and template metadata were
checked locally: both **Metal System Trace** and **Game Performance** include
the Displayed Surfaces instrument. `xctrace help record` / `help export` and
the installed GPU package's schemas support this preparation route. **No
trace has yet been recorded, and no actual trace TOC or exported rows have
been validated for this project.** This is not performance acceptance evidence.

Launch the actual Shipping client, verify its PID and warm it as required by
the routes below. In another terminal, substitute that PID and record:

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
SHIPPING_PID=12345 # replace with the verified Shipping client PID
SHIPPING_TRACE="$SHIPPING_PERF_ROOT/shipping-metal.trace"

xcrun xctrace record \
  --template 'Metal System Trace' \
  --attach "$SHIPPING_PID" --time-limit 120s --output "$SHIPPING_TRACE"

xcrun xctrace export "$SHIPPING_TRACE" \
  --toc --output "$SHIPPING_TRACE.toc.xml"
```

The 120-second trace limit is an example collection envelope; preserve the
actual route boundaries and durations required below. Inspect the TOC for
the run number, process identity and available table attributes before using
these export queries. Adjust the run number to the recorded run; absence of
either schema requires investigation rather than reporting an empty capture
as a passing measurement.

```bash
xcrun xctrace export "$SHIPPING_TRACE" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="displayed-surfaces-interval"]' \
  --output "$SHIPPING_TRACE.frames.xml"

xcrun xctrace export "$SHIPPING_TRACE" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-command-buffer-frame-assignment"]' \
  --output "$SHIPPING_TRACE.assignments.xml"
```

The locally installed `GPU.instrdst` package defines
`displayed-surfaces-interval` with `start`, `duration`,
`cpu-to-display-latency`, `display-name`, `surface-id`, `connection-UUID` and
`event-label`. `metal-command-buffer-frame-assignment` supplies `pid`,
`process`, `frame-number`, `has-present` and `present-surface-id`. These are
schema checks, not confirmation that every field will be available in a
future capture. Preserve the trace, TOC and raw exports, including XML value
references and units.

**Do not calculate game per-frame P95 directly from every display-table
`duration`.** The display model can retain compositor swaps and other
processes even when recording an attached PID; its `target-pid` attribute
also selects dependencies and colors rather than guaranteeing exclusive
game rows. Associate the verified game PID, frame numbers and presentation
surfaces, then reconstruct consecutive distinct game-frame display times
on the same display. Deduplicate repeated observations of the same game
frame while retaining the full time it remained displayed, including missed
refreshes; do not deduplicate solely by reusable surface ID. Keep displays
and route segments separate, check association coverage, and report missing
or ambiguous associations instead of silently dropping them. Retain hitches
within the declared measurement interval.

Only those individual frame intervals support a frame-distribution P95
(record the percentile method and sample count). HUD rolling averages and
per-second surface/FPS counts cannot recover that distribution. Label
display intervals, CPU-to-display latency and GPU command-buffer timings as
separate metrics; they are not interchangeable with Unreal game/render
thread timings. The Unreal CSV summary tool below does not parse these XML
exports, and all existing real-route and benchmark requirements still apply.

## Build configuration and verified engine behavior

The following facts were checked in the locked UE **5.8.3** source at commit
`396c9f059903aed5fec78ecd3d437a40c6415368`:

| Facility | Stock Shipping target | Development / Test |
| --- | --- | --- |
| `-ExecCmds="stat unit"` | `UEngine::Init` does not queue `ExecCmds` in default Shipping; this invocation cannot establish visible counters | Supported; `stat fps`, `stat unit`, and `stat summary` provide a visual cross-check |
| CSV profiler | `CSV_PROFILER_ENABLE_IN_SHIPPING` defaults to 0; this project's target supplies no override | `CsvProfile START` / `STOP`, or `CsvProfile FRAMES=<N>`, are console commands; `-csvCaptureFrames=<N>` is the supported boot-capture switch |
| FPS chart | `STARTFPSCHART` / `STOPFPSCHART` are in non-Shipping `Exec_Dev`; Shipping also lacks ordinary debug-file output | Console `startfpschart <label>` / `stopfpschart` write per-frame records |

Bare `-csvprofile` and `-fpschart` are not the capture-start switches used by
these source paths. A successful process boot with either token is not proof
that capture started. Test allows its console/debug files and CSV profiler
under the default `Build.h` / `CsvProfilerConfig.h` rules, but full stats and
`profilegpu` have additional configuration gates. Development is the primary
recipe and needs no target-definition changes.
Test's default `NO_LOGGING` also follows `USE_LOGGING_IN_SHIPPING`, so do not
assume a Test package produces the Development log messages used below to
confirm finalization. Prefer Development for the first instrumented run.

Source locations inside the external engine checkout:

- `Engine/Source/Runtime/Core/Public/ProfilingDebugging/CsvProfilerConfig.h`
- `Engine/Source/Runtime/Core/Public/Misc/Build.h`
- `Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp`:
  `UEngine::Init`, `Exec_Dev`, `HandleStartFPSChartCommand`
- `Engine/Source/Runtime/Engine/Private/ChartCreation.cpp`:
  `StartFPSChart`, `StopFPSChart`, `AnalyzeFrame`,
  `FFineGrainedPerformanceTracker::DumpFrameTimesToStatsLog`
- `Engine/Source/Runtime/Core/Private/ProfilingDebugging/CsvProfiler.cpp`:
  `HandleCSVProfileCommand`, `FCsvStreamWriter::Finalize`, `FCsvProfiler::EndFrame`

## Package and launch

On the Mac, after the engine, Xcode and baseline checks are ready:

```bash
source tools/build/local-env.sh
PERF_ROOT="$PWD/.local/perf/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$PERF_ROOT"
QINGLAN_CONFIG=Development QINGLAN_ARCHIVE_DIR="$PERF_ROOT/package" \
  bash tools/build/package-game.sh
```

For a Test package, replace `Development` with `Test` and record that change.
The wrapper builds the game target and cooks/stages it; an editor `-game`
session is not a substitute for the packaged client.

Find the staged native executable with the existing host helper:

```bash
source tools/build/common.sh
qinglan_init_host
qinglan_find_client "$PERF_ROOT/package" "$QINGLAN_HOST"
```

Set `CLIENT` to the exact native executable printed above (inside the `.app`
on Mac). If several are printed, identify the game executable in the staged
application and record its SHA256. Invoke that file directly so the process
receives the local environment and these arguments:

```bash
CLIENT='/absolute/path/to/the/staged/native/executable'
"$CLIENT" -windowed -ResX=1280 -ResY=720 \
  "-UserDir=$PERF_ROOT/user" "-DDC=$QINGLAN_DDC" \
  -csvCompression=0 -log "-abslog=$PERF_ROOT/client.log" \
  '-ExecCmds=stat unit,stat fps,stat summary,t.FPSChart.DoCsvProfile 1,t.FPSChart.ExcludeIdleTime 0,t.FPSChart.OpenFolderOnDump 0'
```

`UserDir` creates an isolated profile/config/output tree for this exercise.
Keep using that tree while earning progression normally. Under this explicit
argument, `FPaths::ProjectSavedDir()` resolves to `$PERF_ROOT/user/Saved/`.
Screenshots and saves therefore do not share the normal player profile tree.

Choose and record the actual in-game quality preset, resolution scale,
dynamic-resolution choice, VSync and frame cap after settings have loaded.
The Mac detector deliberately defaults to a 30 FPS cap and limits effects on
unified-memory hardware. Do not mistake that cap for a hardware throughput
limit. For a separate uncapped diagnostic run, use console `r.VSync 0` and
`t.MaxFPS 0` after the settings service applies, then query both values and
record the override. Keep capped and uncapped runs distinct.

## Capture the real route

Use a physical display with the Metal RHI and a normal selected profile.
Record the package hash, engine/project revision and dirty state, OS/Xcode,
GPU/Mac model, RAM, power/thermal condition, input device, and settings.
Do not run another editor, build or GPU workload during measurement.

1. **Hub:** create/select a slot through the Profile screen, finish the
   opening exchange, and walk the sanctuary courtyard/training/shrine route.
   Warm the loaded game for at least 60 seconds first. Capture a repeatable
   60-second route while moving and rotating the camera; keep loading and
   menus outside this timed route.
2. **Ordinary combat room:** play CH00/CH01 normally. Before CH01 completes,
   the bamboo gate deliberately launches the tutorial realm. After
   `mq_ch01_complete`, interact with `hub.bamboo_gate` again to begin the
   ordinary `bamboo_hollow` expedition. Advance from room 0 through its door
   to room **1**, the regular combat room, and capture the encounter while
   moving, dodging and using the normal loadout. Start after the map has
   loaded and stop after the encounter ends. Record its actual duration,
   room, seed, enemies and loadout; do not pad a short encounter with idle
   time to manufacture a 60-second combat sample. Use further ordinary
   encounters if a longer combat workload is required, with each segment
   labeled separately.
3. **Foundation trial:** earn all **three Qi Refining ranks**, the Bamboo
   first-clear proof and `mq_ch02_complete` through normal play. Visit
   `hub.cultivation_shrine`, complete the CH03_S01 preparation exchange and
   its admission proof, then use `hub.trial_gate` and the normal arena
   interaction/story sequence. Capture the 90-second storm and required-kill
   phase, recording the actual included phases and duration. Warm the
   client in the hub before entry; do not wait 60 seconds inside a 90-second
   trial and then label the remaining segment a full trial. A pre-entry
   checkpoint earned by normal play can be backed up with the game closed
   and copied into a fresh isolated `UserDir` for repeat trials.

Use the Development console (default **Tilde**) to start and stop each
segment. For example, after the hub is warm:

```text
startfpschart hub-route-01
```

Close the console, perform the route, then reopen it and issue:

```text
stopfpschart
```

Use labels `combat-room1-01` and `foundation-trial-01` for the other segments.
Keep the opening/closing console intervals brief and record the exact
measurement boundary in the accompanying clip. The settings above make the
FPS chart start/stop a synchronized CSV profile too. **Do not additionally
start a second `CsvProfile START` capture.** Wait for the CSV writer to finish
and for the log's output-file message before quitting; force-killing a client
can leave a truncated file that the parser rejects.

These are normal gameplay captures. Do not pass `-QinglanTrial=`,
`-QinglanShot=`, progression-cheat flags, positional map shortcuts, or use
teleport/invulnerability/time-scale commands. `QinglanTrial` explicitly
bypasses admission and therefore is only a separate rendering diagnostic.

## Outputs and statistics

Each stopped segment writes into:

```text
$PERF_ROOT/user/Saved/Profiling/FPSChartStats/<capture-local-date-time>/
```

It contains the raw frame-table `<map>-FPS-<platform>.csv`
and synchronized `CsvProfile-<capture-date-time>-<platform>.csv`. The first
file begins with three percentile summary rows; the **raw** section begins
at `Time (sec),Frame (ms), GT (ms), RT (ms), GPU (ms),DynRes,Context`.
The CSV profile has `EVENTS` headers at both ends and a finalized metadata
footer. Its final header is authoritative because columns can be added
during capture. Manually started `CsvProfile START` captures instead default
to `Saved/Profiling/CSV/`; that is not the synchronized route used here.

Create a context JSON for each segment, using actual observed values:

```json
{
  "configuration": "Development",
  "hardware": "Record the actual Mac/GPU and RAM",
  "rhi": "Record the RHI reported by this process",
  "resolution": [1280, 720],
  "quality": "Record preset, internal resolution scale, effects, low-flash, dynamic resolution, VSync and frame cap",
  "route": "hub-route-01; record profile/checkpoint, movement and boundaries",
  "capture_seconds": 60,
  "engine_revision": "Record the actual engine commit/version",
  "project_revision": "Record the actual project commit and dirty state"
}
```

Replace the illustrative duration with the measured segment duration and add
OS/Xcode, package SHA256, power/thermal conditions and clip paths as additional
fields. Then run:

```bash
python3 tools/summarize_performance.py \
  --fps-chart '/actual/capture/map-FPS-platform.csv' \
  --csv-profile '/actual/capture/CsvProfile-file.csv' \
  --context '/actual/capture/context.json' \
  --output '/actual/capture/summary.json'
```

The tool reports arithmetic mean / nearest-rank P95 / maximum frame and GPU
milliseconds, aggregate FPS as `N / sum(frame seconds)`, and mean / P95 /
maximum frame-sampled process physical memory in **MiB**. It does not average
instantaneous FPS. Every raw frame is retained, including hitches; it does
not silently discard slow frames or resample/join the two files.

GPU values come from the FPS chart's resolved `RHIGetGPUFrameCycles` samples,
which may lag CPU frames. Zero GPU durations mean unavailable data; the tool
reports their count, excludes them from the available-sample distribution,
marks coverage incomplete and exits **2**. Resolve that missing counter with
an additional supported GPU trace before treating GPU performance as measured.
`GPU/*` CSV scope timings are exclusive scopes and must not be substituted or
summed into an invented total-frame metric.

`PhysicalUsedMB` comes from `FPlatformMemoryStats::UsedPhysical / 1024^2`.
On this Mac implementation it is **task `phys_footprint`**, not RSS, dedicated
VRAM, or total system memory. Its maximum is the peak observed during these
samples, not a claim about the lifetime peak. Shared unified memory must not
be counted again as separate VRAM. `csv.TrackMemoryUse` measures the CSV
profiler's own allocation overhead; it is not the process-memory counter.

Preserve raw files, summary/context JSON, logs and screenshots/clip under
`docs/dev/evidence/rendered/perf-*` only after collecting real data. The
synthetic parser fixtures in `tools/tests/fixtures/` are never acceptance
evidence. Keep Development/Test results and Shipping observations separately
labeled; all AUD-55 closure requirements still apply.
