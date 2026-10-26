# Mac Shipping hub performance (Metal HUD), 2026-10-21

First accepted external performance measurement of the packaged **Shipping**
client on the owner Apple Silicon Mac. It covers the hub segment only; the
runbook's ordinary-combat-room and trial segments still need a playthrough.

## Configuration

- Host: MacBook Air `Mac16,12`, Apple M4, 32 GB, macOS 27.0.1 (26A434).
- Package: `.local/perf/shipping-test/Mac/QinglanAscension-Mac-Shipping.app`
  (client `308cb978…`, pak `d61163ca…`), built from the same source as the
  final acceptance run `.local/acceptance/20261021T114346.991305Z` (client
  `24d31c23…`, pak `51dd0db1…`), which only re-cooked the identical content.
- Metal Performance HUD, Shipping, 1280x720 windowed, detected Low preset with
  a 30 FPS cap, VSync off, dynamic resolution on.
- Measurement source: `MTL_HUD_ENABLED=1 MTL_HUD_LOG_ENABLED=1`, stderr
  `metal-HUD:` per-frame records. This is external Metal telemetry, not the
  Unreal CSV/FPS chart. The HUD report HTML was not produced because the HUD
  menu was not used.

## Route

- Loaded saved slot 1; warmed ~30 s in the hub (menus/loading excluded).
- 60 s continuous hub route: WASD movement in segments plus horizontal mouse
  camera rotation.
- Raw capture: `perf-mac-shipping-hub-hud-2026-10-21-log.txt` (full stderr log;
  the measured window is 19:41:58–19:42:58).
- Screenshot: `rendered/perf-mac-shipping-hub-hud-settings-2026-10-21.png`
  (hub + HUD settings). Every reported number is also in the raw log and JSON.

## Results (nearest-rank P95 over 1,830 frames)

| Metric | Mean | P95 | Max |
|---|---:|---:|---:|
| Frame interval | 33.34 ms | 33.34 ms | 50.00 ms |
| GPU | 21.39 ms | 23.82 ms | 40.22 ms |

- The frame interval is pinned to the 30 FPS cap (33.34 ms); the 50 ms max is
  one missed display interval.
- GPU samples: no zero/unavailable values.
- HUD memory fields: 3632.89 MB (constant) and 4549.71–4833.97 MB. These are
  Apple's HUD-reported values, not RSS/dedicated VRAM.
- Context: `perf-mac-shipping-hub-context-2026-10-21.json`; summary:
  `perf-mac-shipping-hub-summary-2026-10-21.json`.

## Important defect fixed during this capture

The HUD crash (`UQinglanHUDWidget::RefreshFromPlayer`, invalid FName / bad
access) was caused by `UQinglanTechniqueComponent::Loadout` and
`CurrentTechnique` holding `TObjectPtr` values **without `UPROPERTY()`**. GC
freed the technique DataAssets while the component and HUD still used them.
Both fields are now `UPROPERTY()`; the 60 s Shipping route completed without a
crash (previously it crashed within ~10 s). `GameText` and the HUD technique
loop also guard invalid `FName`s defensively.

## Still open

- Ordinary combat-room and normal-admission trial segments (requires a
  playthrough; the QA `-QinglanTrial` hook is not valid for performance).
- The Metal HUD report HTML (menu-driven) was not generated; the per-frame log
  above is the preserved measurement.
- DX12/Windows and Win64 packaging.
