# QinglanAscension Code Contracts (for module implementers)

Authoritative conventions for `game/Source/QinglanAscension`. Read before writing code.
Spec numbers live in the design docs; every tunable constant cites its doc source
in a trailing comment, e.g. `float DodgeDuration = 0.45f; // docs/02 §2`.

## Layout

Single runtime module `QinglanAscension` (folders are physical only) plus
editor module `QinglanAscensionEditor`. Cross-folder includes are allowed;
keep them one-way where possible: Core <- Combat/Abilities/AI <- Progression/
Narrative/Persistence <- UI/Platform.

## Conventions

- UE 5.8 C++20. UCLASS/USTRUCT/UENUM via UHT; `#pragma once`; include paths
  from module root, e.g. `#include "Combat/QinglanCombatComponent.h"`.
- No STL in reflected API; `TArray/TMap/TSet/TObjectPtr/TWeakObjectPtr`.
- All player-facing text is `FText` resolved from string table
  `/Game/Qinglan/Data/ST_Game` via `FText::FromStringTable`. Never hardcode
  display strings. Stable ASCII `FName` IDs in state and saves.
- Timers use **paused game time** (`UGameplayStatics::GetGlobalTimeSubscribed`/
  `AActor::GetWorld()->GetTimeSeconds()` semantics under pause), never wall clock.
  Components tick with `TickComponent` and accumulate `DeltaTime` so pause works.
- Typed dynamic delegates only (see subsystem headers). No global event bus.
- One authoritative mutation path: UI calls subsystem `Try*` functions;
  nothing else mutates currency/flags.
- Every shipped technique keeps telegraph + impact silhouette at Low quality:
  cue selection goes through quality tiers (GraphicsSettingsService).
- Logs use `LogQinglan*` categories; never log paths, account names, raw saves.
- Pools: `ResetForPool()` clears timers, victims, owner, tags, collision, lifetime.

## Localization keys

`localization/*.json` key == string table key. Speakers: `story.speaker.*`.
UI: `ui.*`. Techniques: `technique.<id>.name`. The editor importer
(`UQinglanDataImportCommandlet`) regenerates `ST_Game` from the JSON files;
runtime code only references keys.

## Save semantics (summary)

Schema v1 fields mirror `data/examples/save.json` exactly
(`UQinglanSaveGame`). Writes follow the six-step durable protocol; a failed
write never deletes the last valid primary; 3 slots x 2 backups; migration
functions `MigrateV1_ToV2` pattern; reject newer schema.

## Combat semantics (summary)

`FinalDamage = round_half_away(max(1, Raw*100/(100+max(0,Def)) * Reaction * Difficulty))`.
Hit key = (CastId serial, victim, ordinal) — duplicates rejected. Cast commit
at release notify. Dodge i-frames [0.10, 0.28] s of a 0.45 s dodge. Status
precedence: immunity -> barrier/health -> reaction -> status -> cue.
Conduct: Thunder vs Wet = one 1.25x, consumes Wet. Burn: 3 ticks x 4 dmg/3 s,
refresh not stack. Root 1 s (bosses: 10% slow 1 s).

## Narrative semantics (summary)

Scenes from `UQinglanStoryRegistry` (imported JSON). Playback defers while
enemies are active. Choices apply flags once at the scene's safe boundary.
Endings: preview never commits; confirm requires preview + warning ack, atomic,
mutually exclusive, once per profile. `ending_shared` requires all three
effective agreements (sq_spillway_verified, sq_allocation_agreed,
sq_handover_accepted). SQ02/SQ03 keep pending vs effective version slots;
ending preview reads effective only.

## Testing hooks

Each subsystem exposes state-query functions usable by automation specs
(`QinglanAscensionEditor/Tests/…`): damage math, hit-key dedupe, rank costs,
banking multipliers, agreement versioning, ending gating. Specs must be
deterministic and headless (`-nullrhi` compatible).
