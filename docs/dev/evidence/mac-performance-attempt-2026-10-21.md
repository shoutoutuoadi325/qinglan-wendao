# Mac Development performance-capture attempt (2026-10-21)

This records a real attempt on the packaged **Development** client after the
display was unlocked. It did **not** produce an accepted performance capture;
the reasons are below so the next attempt starts from the evidence.

## Development package

Built with:

```sh
QINGLAN_CONFIG=Development QINGLAN_ARCHIVE_DIR=$PWD/.local/perf/dev-package \
  bash tools/build/package-game.sh
```

`BuildCookRun` succeeded and the wrapper archived
`.local/perf/dev-package/Mac/QinglanAscension.app` (Development is the default
config, so the staged app name has no `-Mac-Development` suffix). The wrapper
was fixed to accept both `QinglanAscension.app` and
`QinglanAscension-<Config>.app`; the host regression test now covers the
config-less name.

Client SHA-256 (Development executable): `5528c481af04e993deba9f0f26d4fab0ca97700a3afd63b14375fec13697ed91`.

## Client run and chart start

Launched windowed at 1280x720 on Metal, loaded saved slot 1, then in the
in-game console (default `~`):

```text
startfpschart hub3
```

The engine logged `LogChartCreation: Started creating FPS charts at ...`
(2026.10.21-10.46.04). A ~41 s hub route (W/A/D held in segments, mouse
rotation) was performed. A finalized `<map>-FPS-Mac.csv` frame table was **not**
produced: the console lost input focus after the route and `stopfpschart`
could not be delivered reliably; only an earlier partial
`CsvProfile-2026.10.21-18.40.23-Mac.csv` (555 rows, no finalized footer) is in
the container `Saved/Profiling/FPSChartStats/`. No summary/P95 numbers are
claimed.

## Development crash found (and guarded)

Before the guard, the Development client crashed twice (SIGSEGV,
`EXC_BAD_ACCESS` at `0x1bbba`) with this faulting stack:

```text
FName::ToString
FQinglanLocalizationService::TryResolve(FName, FText&)
UQinglanHUDStatics::GameText(FName)
UQinglanHUDWidget::RefreshFromPlayer()
SObjectWidget::Tick -> SWidget::Paint
```

`GameText` now checks `FName::IsValid()` and returns an `<invalid text key>`
placeholder instead of calling `FName::ToString` on a corrupted name; the
Development client stayed alive through the route after this guard
(`LogQinglanUI: Warning: GameText: invalid FName key; using placeholder`).
The skill HUD then displayed `<invalid text key>` for both equipped
techniques, so the **source of the invalid FName is still unresolved** — likely
memory corruption rather than asset data. The Shipping package did not show
this crash in the interactive hub pass.

## Status

- Development packaging: works (after the wrapper fix).
- Performance capture: **not complete**. Needs a stable Development client
  (root-cause the invalid FName), and a clean start/stop with the console
  focused; then the runbook's hub/combat/trial segments and
  `tools/summarize_performance.py` can be run.
- The Shipping acceptance package and its evidence are unaffected
  (`.local/acceptance/20261021T114346.991305Z`).
