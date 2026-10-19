# Development Completion Audit and Repair TODO

Audit date: 2026-10-14 (Asia/Shanghai). Reviewed branch: `develop`.
Reviewed revision: `e94be12ee39826a6fba73f57534b06bbf0c73db8`, matching
`origin/develop` after fetch. The working tree was clean before this review.

## Repair progress log

- 2026-10-15: Repair work started on branch `fix/audit-repairs` (off `develop`
  at `de3447a`). Baseline verification before any edit: incremental editor
  build succeeded (UBT Linux Development, 28 s); working tree clean.
  A four-wave repair plan was scheduled with disjoint file ownership per wave
  (1: persistence/economy + AI lifecycle + abilities + config/bootstrap;
  2: narrative + trials/rooms + UI/localization; 3: Character integration +
  combat components + VFX/animation; 4: tests, asset regeneration, platform
  evidence, dispositions). Wave 1 agents were dispatched, interrupted by a
  transient API outage, resumed, then **halted by user request while still in
  their read/analysis phase — no code edit landed**. All 60 items below remain
  unrepaired and unchecked; no disposition is recorded yet.
- 2026-10-15 documentation defect fixed in this file: AUD-59 referenced
  `../../packaging/Windows/README.md`, but the tracked directory is
  `packaging/windows/` (lowercase), so `tools/validate_repository.py` failed on
  a broken link. The link now uses the tracked path; all three planning
  validators pass again.
- Evidence note for AUD-59 (Linux side): the untracked `packaging/Linux/`
  directory in the working tree contains a complete prior Linux Shipping
  package (Shipping binary, pak/utoc/ucas, staged engine files, manifests,
  dated 2026-10-13) produced by the previous session. It is not yet committed;
  committing it (or its manifests/checksums) belongs to the AUD-57/AUD-59
  evidence work.
- 2026-10-15 (second update): repair work resumed after the halt recorded
  above, then was stopped again by user request. This entry supersedes the
  "no code edit landed" statement: Wave 1 edits DID land and remain in the
  working tree (branch `fix/audit-repairs`, **uncommitted, unbuilt, untested**;
  24 files, +1262/−281). State by group:
  - Edits complete per repair-agent reports (statically self-consistent, not
    compiled or tested): AUD-23 geometry/KillZ authoring, AUD-24, AUD-25,
    AUD-47, AUD-53, AUD-54, AUD-57 (`game/Config/DefaultEngine.ini`,
    `QinglanContentBootstrap`, `QinglanGraphicsSettingsService`, new
    `tools/build/{build-game,package-game,regenerate-content}.sh`) and
    AUD-30 (player side), AUD-31 (player-projectile callsite), AUD-35,
    AUD-38, AUD-39 (`QinglanProjectile`, `QinglanTechniqueExecutors`,
    `QinglanTalismanComponent`).
  - Edits interrupted mid-task, expected incomplete/inconsistent: AUD-01..08
    (`QinglanSaveSubsystem`, `QinglanSaveMigration`, `QinglanSaveGame.h`,
    `QinglanRunSubsystem`, `QinglanEconomyLibrary`, `QinglanGameModeBase`;
    the agent was stopped while editing `QinglanRunSubsystem`
    room/expedition functions).
  - Edits believed complete but the final whole-file verification pass was
    interrupted: AUD-28 (enemy side), AUD-29, AUD-31 (enemy callsites),
    AUD-33, AUD-36 (AI side), AUD-40, AUD-41, AUD-46 (`AI/*` plus additive
    `QinglanCombatComponent` changes).
  - Generator-side changes (input assets, maps, string table) additionally
    require an editor-commandlet regeneration
    (`tools/build/regenerate-content.sh`) before runtime behavior changes;
    not yet run.
  - The remaining ~31 items (AUD-09..22, 26..29, 32, 34, 37, 42..45,
    48..52, 55, 56, 58..60) have no edits. No checkbox below is ticked and
    no disposition is recorded — nothing has passed compile, test, or
    acceptance, so per this document's closure rule no item may be marked
    complete. The only pushed commit on the branch is the documentation
    update `ae2ece2`. Detailed per-item repair reports from the repair
    agents are not yet persisted in this repository.
- 2026-10-15 (third update): the Wave 1 working-tree edits above were
  committed to `fix/audit-repairs` in four group commits at user request
  and pushed: `3bf6c98` (abilities: AUD-30/31/35/38/39),
  `917b3bf` (platform/config/tools: AUD-23/24/25/47/53/54/57),
  `447abee` (AI: AUD-28/29/31/33/36/40/41/46 — verification pass was
  interrupted), `160ebd1` (persistence WIP: AUD-01..08 partial, expected
  incomplete, possibly non-compiling). **The code remains unbuilt and
  untested**; commit messages carry the per-group caveats. The 1.6 GB
  untracked `packaging/Linux/` output stays out of Git on purpose; only
  manifests/checksums belong in the repository (AUD-57/59 evidence).

## Current handoff: runtime integration pass (2026-10-15)

The owner expanded the initial framework-only budget. This latest pass connects
those frameworks to production callers, adds native usable controls and real
runtime regression source, and supersedes the earlier framework-only handoff.
**All 60 acceptance checkboxes remain open:** no Unreal build or playthrough has
run on these changes. Source implementation and accepted gameplay are separate.

- Branch: `fix/audit-repairs`; baseline source revision:
  `12fa3bacb7d4e7884787f9534cc838fd66e7921e`.
- The owner requested committing and pushing these three local passes on
  2026-10-15. This handoff is included with the source/test changes in
  `fix: integrate audit repairs and runtime regression coverage`; obtain the
  repair commit hash from Git history. Six new runtime/helper/test files are
  included. Unreal compilation and runtime verification remain pending.
- Authored data changed: CH05_S04 now contains its existing localized
  `story.ch05.s04.conditions` line, matching the acknowledgment choice pin.
  English/Chinese UI JSON grew from 101 to 115 keys. CH05's bilingual reading
  edition was regenerated. No cooked assets or map binaries were regenerated.
- New runtime files: `Core/QinglanTrialAdmission.h/.cpp`,
  `Core/QinglanRoomLayout.h`, `Narrative/QinglanStoryIdentity.h`.
- New tests: `QinglanAuditFrameworkTests.cpp` (10) and
  `QinglanRunIntegrationTests.cpp` (5), in the editor module. These are authored
  test cases, **not 15 passing test results**.

### Local source/content evidence

- Source snapshot: `d2f4ce0146a1677da9201ea2f8554735586a74bcfb7f125c7e60051e1b7f6f2c` (SHA-256, 35 modified/new
  files under `game/Source`).
- Authored content snapshot: `566240af1d74363a9c126dca4b03f70e8a6d3864c79e40c8f5b37eeded69d3df` (SHA-256,
  4 modified files under `data/` and `localization/`). This is source
  data and generated reading text, not cooked `.uasset`/`.umap` evidence.
- Reproduce each digest by taking the sorted union of tracked changed paths
  (`git diff --name-only`) and untracked paths (`git ls-files --others
  --exclude-standard`), selecting the prefixes above, and hashing each UTF-8
  repository-relative path, NUL, raw bytes, NUL, in that order. No build hash
  exists for these local changes yet. After committing, obtain the file list
  with `git diff-tree --no-commit-id --name-only -r <repair-commit>` and read
  each file at that revision before applying the same prefix filters/hash.

### Per-item dispositions for the local passes

| AUD | Disposition | Current implementation and remaining acceptance |
| --- | --- | --- |
| AUD-01 | **Still open; source path connected** | Hub arrival retains active profiles. A cold boot loads the first existing slot only once (slot selection frontend remains missing). Both OpenLevel wrappers save before travel; a failed new hub departure resets its empty run so the gate can retry. Verify hub return/restart, all selected slots and save failures in-engine. |
| AUD-02 | **Still open; checkpoint resume connected** | Loaded runs route from hub to their realm before hub refill/autosave. Realm mode waits for possession, restores room entrance/vitals/status, re-arms only unfinished encounters and presents pending blessings. Interact retries a pending resume; loading is consumed only after restoration. Story scene/choice/proposal/ending/agreement boundaries now request saves. Save callbacks cannot recursively write/switch profile; Character displays save failure notices. Need real process-kill/failure tests and frontend recovery UX. |
| AUD-04 | **Still open; subsystem isolation connected** | `ResetRun` discards every run-owned field; empty checkpoint import clears the previous run. Failed loads do not publish a target slot before checking the profile subsystem. Load/create reject save/import reentrancy. Verify profile switches with world/pawn teardown when the missing profile UI is implemented. |
| AUD-05 | **Still open; signed stream callers repaired** | Removed stale two-argument RestoreStream calls left by Wave 1. Unfinished room checkpoints save the pre-wave encounter seed; rebuilding a room reproduces its wave rather than drawing the next one. New expeditions reset prior room RNG snapshots. Direct signed/zero-state and actual EnterRoom/reload comparisons are authored; run them with Unreal and check actual encounters. |
| AUD-06 | **Still open; pending offer connected** | Pending offer/owner round-trip without reroll, restore to player presentation, and save on selection. Room transitions reject leaving an unresolved draft; completed-room events fire after offer creation/save. Direct round-trip, one-time selection and replay cases are authored. Legacy saves whose offer was already lost still need a documented compatibility/recovery policy. |
| AUD-07 | **Still open; atomic settlement connected** | Base/optional first-clear delta and journal IDs commit together; allocation/profile/overflow/capacity failure retains run loot and emits a visible notice. Journal-sized ID search removes 999 cap. Exit/defeat/recall callers respect failed settlements. Reentrant EndExpedition is blocked. Test actual 999-run histories, disk failures and long-lived journal policy before acceptance. |
| AUD-08 | **Still open; payout integration connected** | Victory uses overflow-checked repeat-material top-up plus first-minus-repeat bonus only. Expected Bamboo first/repeat: 3/2 herbs + 1 crystal; Ashen: 3/2 crystals. Retreat/death use exact integer ratios floored per stack, avoiding float underpayment and int64 overflow. Boss campaign proof comes only from the actual boss-kind room. Still verify full production payout/proof paths on first/repeat/retreat/death runs. |
| AUD-09 | **Still open; runtime scoped identity implemented** | All Story objective/choice indexing uses owner-scoped keys. Production Character/Interactable/TrialDirector proof calls name exact scenes. Legacy bare APIs reject ambiguity; choice import/export migrates unique entries and logs/drops unknown, malformed or ambiguous entries. Import validation checks owner/member collisions. Lost overwritten legacy history cannot be reconstructed. Run direct scoped/runtime save tests plus the real CH04..07 journey. |
| AUD-10 | **Still open; epilogue continuation only** | CH07 completion queues the next eligible epilogue scene in the same hub visit. Foundation debrief CH03_S03 starts on hub return. Full main/side scene-to-anchor authoring remains incomplete; do not infer all mandatory scene routing was fixed. |
| AUD-12 | **Still open; runtime fix implemented** | Successful selection clears skip halt; actual SkipChoiceResume test checks no silent selection and reachable response/completion. Engine execution pending. |
| AUD-13 | **Still open; groups and controls implemented** | Only active authored group/pin options are presented/selectable; every required group must resolve. CH05 conditions pin now has a script line. Ending controls re-open their family until atomic confirmation. Native dialogue renders clickable disabled-aware buttons, scrolling/focus, and number keys 5..8; arrows/DPad/Enter and native controller button focus are wired. Clear-before-select ordering preserves synchronously presented next groups. Verify mouse/keyboard/controller and long bilingual content in-engine. |
| AUD-14 | **Still open; ending frontend connected** | Character binds preview/confirm/cancel events to a native scrollable ending widget added to viewport. Preview Continue preserves preview authorization; Cancel discards transient state. Confirmation requires explicit warning checkbox and calls actual ConfirmEnding; success requests hub travel into CH07. Modal input blocks gameplay/underlying choice shortcuts. Actual backend transaction test covers no-preview/no-ack/cancel/single commit. Verify rendered controls, every ending and failure/restart paths. Guardian release presentation/earned conditions remain AUD-11 work. |
| AUD-18 | **Still open; admission authority connected** | Hub entry checks preparation prerequisites; direct anchors/Director enforce exact trial scene/objective and combat prerequisites. Foundation: Qi rank 3 + Bamboo; Golden Core: Foundation rank 3 + Ashen. Character entry defers one tick for its scene transaction. Ashen entry also requires Foundation and CH03 debrief. Profile breakthrough now requires actual Foundation trial proof, preventing shrine bypass. First-campaign policy is implemented; replay policy and full campaign acceptance remain open. |
| AUD-19 | **Still open; return paths connected** | Foundation success commits scoped story proof, trial campaign flag and deterministic breakthrough, then saves/travels hub. Character stay action and Backspace/controller View cancellation use CancelTrialAndReturnToHub, including missing-director arena fallback. Failed save leaves the return retryable. Verify success/failure/cancel/retry interactively; no full pause/outcome frontend exists. |
| AUD-20 | **Still open; free-win fallback removed only** | Failed initialization/lost wave engine grants no proof; Director validates canonical duration/tier, living player and absence of expedition. Required elite/boss/hazard phases are still unimplemented: the existing survival timer remains the success condition. This is not an implemented authored trial. |
| AUD-21 | **Still open; room origins connected** | Shared RoomLayout constants now drive map authoring, door positioning/teleport, resume and encounter origin. Director has a root and uses current-room origin; portal positions stay on the room floor. Generator authors seven tagged PlayerStarts per realm; runtime fallback creates equivalent starts for old maps. Default spawn explicitly chooses the current room. Regenerate and verify saved maps, collision/nav and actual enemy locations. |
| AUD-51 | **Still open; narrow frontend additions** | Save/bank error notices and ending/choice controls now exist. Profiles/settings/equipment/journal/shop/outcome screens remain incomplete. Do not call native ending/dialogue work a complete menu system. |
| AUD-52 | **Still open; dialogue layout/dismiss partial** | Native responsive lower-screen dialogue has separate scrollable line/choice regions, real font sizes and wrapping. Focus scrolls options into view. Script source-kind labels now display; Backspace dismisses records when no mandatory choice is pending. Record registry lacks source classification; journal reopen and text-scale/720p/Retina/CJK visual acceptance still require work. |
| AUD-58 | **Still open; 15 regressions authored** | Tests directly execute scoped Story behavior/migration, skip/groups, ending transaction, physical/authored trial admission, atomic settlement, exact bank flooring, Run checkpoint/reset/draft/reward replay and unfinished-room RNG reconstruction. Existing rank-gate fixture now asserts Foundation trial proof. Full SaveSubsystem disk failure injection, registered profile-to-run integration, functional maps and engine execution remain missing. |

All other AUD IDs retain their prior status. Remaining save limitations include
partial objective persistence, courier clock/delivery persistence (AUD-16), legacy
schema-v1 field compatibility and true disk fault injection. A bank committed in
memory but failing to save retains current memory plus the last valid disk
snapshot as restart fallback; it is not durable until a later successful save.
Scoped choice migration cannot restore decisions already overwritten by older
bare-ID saves. Normal-choice/generic anchor proof still does not implement all
authored gameplay conditions (AUD-11).

### Verification and next-agent sequence

Results on 2026-10-15 for the local changes above; all lightweight commands
exited 0. This section records their evidence:

- `python3 tools/validate_repository.py`: passed; 122 text files, **115 UI keys**
  per locale and consistent planning/data/link/packaging contracts.
- `python3 tools/validate_story.py`: passed; 30 main scenes, 24 side stages,
  24 records, 40 barks and 768 story keys per locale.
- `python3 tools/export_story_reading.py --check`: 18 bilingual editions current.
- `git diff --check`: passed. New source files were also checked for trailing
  whitespace; referenced helper types, exports, input APIs and locale keys were
  reviewed against current source.
- Independent static review found and corrected: missing ending viewport
  insertion, stale per-room RNG snapshots across expeditions, blocked-departure
  retry, pre-commit material overflow, and post-wave seed reuse on resume.

**No Unreal build, automation execution, asset regeneration, cook, package or
interactive rendering ran.** The test code is unbuilt. Native UI bypasses old
placeholder widget layouts, but new UI keys still need string-table regeneration;
the CH05 script/pins need regenerated story registry content. Tagged PlayerStarts
also need regenerated maps for source-controlled assets (runtime fallback exists).
The Chinese runtime localization pipeline remains AUD-48, not fixed by JSON parity.

1. Build this entire branch with the selected engine, including editor tests and
   UHT, before extending source. Preserve source and logs for any compiler repair.
2. Run the existing `Qinglan` suite plus all 15 tests in the two new test files.
   Attach automation output, actual source/build/content hashes and date. Do not
   treat these document validators as proof of C++ or gameplay correctness.
3. Regenerate registry/string-table/maps from current data, reload in a fresh
   engine process, and test checkpoint travel/room/vitals/wave/offer restoration;
   hub preservation; Foundation admission/success/debrief/Ashen gate; grouped
   CH05 decisions; CH06 preview/ack/confirm/cancel and same-visit CH07 continuation.
4. Continue P0 routing and earned-objective work (AUD-10/11), courier state and
   time flow (AUD-15/16/17), then real trial mechanics (AUD-20), remaining input/
   loadout/frontends/localization and asset/release acceptance. Existing acceptance
   criteria below remain authoritative; tick only with actual evidence.

## Decision

**Development is not complete.** The repository has substantial gameplay code
and authored narrative data, but the integrated player journey contains
progression blockers, save-loss paths, combat defects, and unfinished interfaces.
The current implementation does not meet the version 1.0 acceptance contract in
[the product brief](../00-product-brief.md) and
[the QA plan](../08-quality-and-testing.md).

| Backlog priority | Items |
| --- | ---: |
| P0 — progression/save blockers | 6 |
| P1 — major defects or required delivery/verification gaps | 34 |
| P2 — narrower correctness/completion issues | 20 |
| Total | 60 |

This document is a repair backlog for the development agent. The original audit
changed no game code; subsequent source work is recorded in the handoff above. Every finding below needs a recorded disposition and its acceptance checks;
compiling modules or passing the existing 12 tests does not close these items.

## Review scope and evidence limits

- Three parallel reviewers covered Combat/Abilities/AI/VFX,
  Progression/Persistence/Narrative, and Core/UI/input/trials. The main reviewer
  consolidated their findings, checked critical call chains, and reviewed
  configuration, content bootstrap, assets, localization, builds, and release gates.
- Coverage includes all 108 files under `game/Source`, their integration points,
  configuration, story JSON, generated-content references, and project requirements.
  The C++ implementation files contain approximately 21,915 lines.
- Successfully rerun locally: `python3 tools/validate_repository.py`,
  `python3 tools/validate_story.py`, and
  `python3 tools/export_story_reading.py --check`.
  Results: 121 text files; 101 UI keys per locale; 768 story keys per locale;
  30 main scenes, 24 side stages, 24 records, 40 barks; 18 reading editions current.
- The local checkout has no Unreal installation, runnable build, or packaged
  Windows/Mac artifacts. The previously reported Linux build and 12/12 automation
  result were read from [the existing report](test-report.md), **not rerun** here.
  Interactive play, actual Blueprint execution, cooking, fault injection, GPU
  rendering, and target-platform testing remain unverified by this audit.
- Binary asset checks were limited to file inventory and known ASCII/UTF-16 tokens.
  They establish missing files/keys, not valid shaders, animation skeletons,
  collision, or successful runtime loads. Engine-dependent conclusions explicitly
  require reproduction below.
- Line numbers refer to the reviewed revision. After edits, follow the named
  functions and update evidence. Related defects can mask each other: repairing
  hub rollback, for example, exposes incorrectly banked boss proof.

Priorities follow the QA plan: P0 means a progression deadlock or recoverable-save
loss path requiring immediate repair; P1 means major broken gameplay, input,
presentation, or delivery; P2 means a narrower correctness/completion issue.
“Confirmed” means demonstrated by source/data flow. “Delivery gap” means required
work/evidence is absent. “Runtime confirmation required” avoids claiming a local
playthrough or engine test that was not performed.

## Persistence and economy

### AUD-01 — P0 — Hub return overwrites current progression with the old save

- [ ] Separate initial profile loading from ordinary map arrival; save the current
  earned state before travel and retain it across hub entry.

**Confirmed.** [Hub BeginPlay](../../game/Source/QinglanAscension/Core/QinglanGameModeBase.cpp)
lines 181–187 calls `LoadProfile(0)` on every arrival, before `SaveNow` at 225.
`EndExpedition`, story choices, cultivation, and ending mutations live in the game
instance subsystems. A return to the hub imports the earlier disk snapshot over
those mutations and then saves the overwritten state. Rewards, story progress,
and completion can disappear even within one running session.

**Acceptance:** complete an expedition, buy upgrades, finish a side stage, and
return from a trial; values and flags survive each return and restart. Initial
launch loads exactly the selected profile once. Never silently reload slot 0 over
an active session.

### AUD-02 — P1 — Required safe-boundary saves and checkpoint resume are unwired

- [ ] Connect durable saves to entrance, completed-room rewards, banking, hub
  purchases, trial preparation, and final decisions; connect loaded checkpoints
  to the correct map, room, and entrance attributes. Surface failures to the player.

**Confirmed integration gap.** The only ordinary production `SaveNow` call is
the [hub call](../../game/Source/QinglanAscension/Core/QinglanGameModeBase.cpp) at 225;
profile creation is the other internal writer. There are no save calls on room
completion, story transactions, rank purchases, or trial/ending commits.
[Run checkpoint export/import](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
at 654–702 exists, but loading remains in the default hub rather than restoring a
playable expedition checkpoint. Quitting at a promised safe boundary loses progress.

**Acceptance:** quit or kill the process at each listed boundary, restart through
the normal frontend, and resume exactly the earned state without reward replay.
A failed save shows a localized notice and retains the last recoverable state.

### AUD-03 — P0 — Backup rotation destroys the last valid recovery copy

- [ ] Rotate only validated saves and retain the selected last-valid fallback
  until the new primary is committed; make failed replacement recoverable.

**Confirmed failure path.** [LoadProfile/SaveNow](../../game/Source/QinglanAscension/Persistence/QinglanSaveSubsystem.cpp)
568–607 can recover from `bak2` when primary and `bak1` are corrupt. At 792,
the next save deletes `bak2` unconditionally, then moves unvalidated `bak1` into
its place. Primary preservation at 802 runs only for a valid primary. A crash
before the final move at 818, or a failed move followed by temp deletion at 826,
leaves no valid primary or backup. Successful replacement also leaves invalid backups.

**Acceptance:** corrupt primary and `bak1`, recover `bak2`, then inject crashes
and write/move failures at every rotation/replacement step. At least the original
valid save must remain loadable; a successful write retains a validated backup.

### AUD-04 — P1 — Loading or creating another profile retains the previous run

- [ ] Atomically replace all profile/session state, including an explicit empty
  run when the chosen profile has no checkpoint.

**Confirmed API defect.** [Save subsystem](../../game/Source/QinglanAscension/Persistence/QinglanSaveSubsystem.cpp)
637–642 only imports a run when a checkpoint exists; 865–917 creates a profile
without clearing the old run. [ImportCheckpoint](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
680–683 returns for an empty realm rather than resetting. Loading hub profile B
while A has an expedition leaves A's loot/blessings/run active; later banking can
credit B. This becomes player-visible when profile selection is implemented.

**Acceptance:** alternate A/B profiles with and without checkpoints and create a
new profile during an existing session. No realm, loot, blessings, pending offers,
or transaction state crosses profile ownership.

### AUD-05 — P2 — Valid signed RNG states are replaced on restore

- [ ] Restore the exact serialized stream state; use schema/presence information
  for missing fields instead of treating nonpositive states as invalid.

**Confirmed structural mismatch; engine regression required.**
[RestoreStream](../../game/Source/QinglanAscension/Progression/QinglanDeterminism.h)
83–89 replaces any state `<= 0` with the derived seed.
[Run export/import](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
667–669 and 687–689 serialize `FRandomStream::GetCurrentSeed()`.
[Save validation](../../game/Source/QinglanAscension/Persistence/QinglanSaveMigration.cpp)
367–368 correctly permits every `int32` state. The positive-only Lehmer assumption
is not a valid contract for Unreal's signed stream state.

**Acceptance:** use real engine streams with negative and zero states; uninterrupted
and resumed streams must produce identical subsequent draws, encounter choices,
loot, and blessings. [Epic's FRandomStream API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FRandomStream)
documents the signed seed interface.

### AUD-06 — P2 — Unaccepted blessing offers vanish after checkpoint restore

- [ ] Persist the pending offer and owning room, or commit the reward checkpoint
  only after selection; restore the same choices without rerolling.

**Confirmed checkpoint API defect.** [Run subsystem](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
476–480 creates a pending draft; 654–675 exports no pending offer, while 695–699
clears it on import. The room is already committed, so replaying completion at
449–451 cannot regenerate the lost draft. This must be repaired with AUD-02.

**Acceptance:** save/crash immediately before and after choosing in rooms 2/4.
The same offer returns when unchosen; a chosen blessing is granted once.

### AUD-07 — P2 — Expedition 1000 can erase loot without banking it

- [ ] Remove the arbitrary transaction-ID limit and preserve the run on every
  failure to allocate or commit a banking transaction.

**Confirmed.** [Banking ID allocation](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
325–335 only searches counters 1–999. `NAME_None` bypasses the grant at 501 but
still reaches state/loot clearing at 547–553.

**Acceptance:** prepopulate 999 committed IDs for a realm, complete another run,
and verify the full payout exactly once. Allocation/commit failure leaves loot
available for retry and displays an error.

### AUD-08 — P2 — First and repeat material payouts differ from the economy contract

- [ ] Allocate guaranteed expedition materials and first-clear bonus separately;
  verify the integrated payout rather than only the economy lookup table.

**Confirmed.** [Run rewards](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
460–469 grant one elite material, then 525–527 add the entire first-clear material
total again. Repeat runs never apply the repeat material total. Actual/required:
Bamboo first `4 Herbs + 1 Crystal` / `3 + 1`; repeat `1 + 0` / `2 + 1`;
Ashen first `4 Crystals` / `3`; repeat `1` / `2`.
[The economy table](../../game/Source/QinglanAscension/Progression/QinglanEconomyLibrary.cpp)
119–132 contains the intended values.

**Acceptance:** complete first/repeat expeditions through production callers;
check total materials, first-clear bonus once, per-stack retreat/death flooring,
and save/reload idempotency.

## Narrative and campaign

### AUD-09 — P0 — Objective IDs collide across chapters; choice history is also unscoped

- [ ] Make objectives globally unique or pass explicit scene ownership; scope
  choice history by scene. Reject invalid identity collisions during import and
  provide a compatibility policy for already saved IDs.

**Confirmed.** [Late-story data](../../data/story/main-late.json) reuses
`s01.objective` through `s04.objective` in every CH04–CH07 chapter (119/227/362/450;
550/651/767/906; 1003/1109/1242/1445; 1577/1708/1818/1926).
[FindSceneByObjective/NotifyObjectiveCompleted](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
157–193 and 1437–1482 choose the first registry match. After CH04 completes,
Ashen-clear, Golden Core trial, and epilogue events resolve to already completed
CH04 scenes and return at 1473. The global `Session.ChoiceDecisions` map at 1017–1023 also
uses bare IDs such as `a`, `b`, and `confirm`, overwriting choices from other stages.

**Acceptance:** each CH04–CH07 objective advances only its owning scene; all
30 scenes remain completable. Choices from multiple quests round-trip independently.
Import a deliberately duplicated objective and ensure validation fails.

### AUD-10 — P0 — Required late-story scenes have no usable scene/anchor route

- [ ] Author explicit stable interaction IDs and implement validated transitions
  for every mandatory scene, including the full epilogue in one visit.

**Confirmed routing mismatch.** [Interaction spawner](../../game/Source/QinglanAscension/Core/QinglanInteractionSpawner.cpp)
32–40 excludes Ashen room 0/approach/archive/vent/release anchors except `ashen.boss_5`.
[BeginSceneAtAnchor](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
and `FindObjectiveProofAtAnchor` at 519–533 match exact dot/slash IDs, while
[CH04–CH07](../../data/story/main-late.json) locations are prose descriptions.
The hub's automatic scene pass is CH07 only. After acknowledging CH07_S01,
[HandleSceneCompleted](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1161–1164 clears the last scene; no transition starts CH07_S02 in the same visit.
Completing an unrelated scene does not make these dispatch routes appear.

**Acceptance:** produce a scene-to-map/anchor/event table and play all 30 main
scenes and 24 side stages from a new profile, with no console commands, hub
reload trick, pre-granted flags, or developer-only scene calls.

### AUD-11 — P1 — Dialogue completion substitutes for actual gameplay proof

- [ ] Implement the authored tutorial, puzzle, traversal, verification, and
  consent conditions; let dialogue finish independently from earned objectives.

**Confirmed missing behavior.** [Generic anchor proof](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
689–705 marks a choice-free objective complete after reading and pressing again.
[Narrative completion](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
455–460 marks all objectives complete, and 560–575 completes ordinary-choice
scenes without evaluating their gameplay guard. `ConfirmationGuard` is imported
as text but has no runtime evaluator. Examples include CH00_S02 combat training,
CH01_S02 encounter/blessing/shrine practice, SQ01 route and lamp test, SQ04/SQ07
document order, SQ05 wedge/latch, SQ06 cooling sequence, and SQ08 returns.
[Boss proof dispatch](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1219–1225 chooses the first eligible choice-free chapter objective instead of
the exact completed encounter objective.

**Acceptance:** reading/skipping/confirming without the task cannot grant progress
or rewards. Actual action events prove the exact scene objective. Wrong puzzle
actions have feedback and recovery; every legitimate route remains completable
and saved. Tutorial acceptance uses real movement/combat/talisman actions.

### AUD-12 — P1 — Skip followed by a choice leaves dialogue permanently halted

- [ ] Clear the skip halt on successful selection and restore the correct
  response/continuation state without making a silent choice.

**Confirmed.** [Story subsystem](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
1093–1101 sets `bAdvanceHalted` at an unresolved choice. Selection at 1036–1040
never clears it; `AdvanceLine` at 971–973 subsequently always returns.

**Acceptance:** in each choice scene, test skip-before-choice and skip-after-choice.
Responses and scene completion remain reachable and rewards commit once.

### AUD-13 — P1 — Choice groups are flattened and options after number four are unreachable

- [ ] Present only the active authored choice group, track each required group,
  and provide keyboard/controller/mouse navigation for every displayed option.

**Confirmed.** [PresentChoices/SelectChoice](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
516–527 exposes every choice at the first pin; 1037 uses one scene-wide resolved
boolean. CH05_S04's live question and later acknowledgment are combined, so asking
one question can commit consent without the required acknowledgment.
CH06_S04 has eight choices; [character menu input](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
592–598 and 1435–1457 only exposes slots 0–3.
[Dialogue choices](../../game/Source/QinglanAscension/UI/QinglanUIWidgets.cpp)
169–198 are text blocks with no clickable selection handlers. Seal/shared
confirmation, cancel, and return choices at positions 5–8 cannot be selected.

**Acceptance:** both CH05 live-question branches require the later acknowledgment.
All ending previews, confirmations, cancel, and return can be operated on keyboard
and controller, including options beyond four. Skip/load never bypasses a group.

### AUD-14 — P0 — Ending preview, acknowledgment, and confirmation have no frontend caller

- [ ] Implement the ending frontend and bind preview, warning acknowledgment,
  confirm, cancel, and return events to the atomic ending transaction.

**Confirmed.** [Character delegate binding](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1346–1350 omits `OnEndingPreviewed`, `OnConfirmEndingRequested`,
`OnCancelConfirmation`, and `OnReleaseGuardian`. The corresponding choice action
at [StorySubsystem 773–781](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
only broadcasts. `ConfirmEnding` has no production caller. The ending widget in
[UIWidgets.h](../../game/Source/QinglanAscension/UI/QinglanUIWidgets.h) 112–117 is
empty, its generated tree is static, and it is never created for the player.

**Acceptance:** earn and complete each of the three endings through ordinary input,
including required warning acknowledgment. Preview/cancel leaves ending flags
unchanged; confirm saves once and continues through CH07.

### AUD-15 — P1 — Courier time never advances during ordinary play

- [ ] Add the authored sanctuary rest/delivery flow, advance the quest clock,
  and show the correctly signed reply for the selected proposal version.

**Confirmed.** [Story delivery lifecycle](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
1353–1356 creates an undelivered reply; 1391–1394 rejects it until delivered.
`AdvanceHubTime` at 1668–1688 is the only writer of `bDelivered=true` and has no
production caller. SQ02/SQ03 can never make their agreements effective through
normal interaction, so the Shared ending remains unavailable.

**Acceptance:** show and honor the authored 18h/12h waits without requiring real
wall-clock waiting. Early confirmation fails visibly; the correct arrived reply
can be verified and accepted, making the Shared ending reachable.

### AUD-16 — P1 — Saving a pending agreement loses the delivery needed to finish it

- [ ] Version and persist the story clock and delivery version/timing/state;
  migrate old pending agreements into a recoverable flow.

**Confirmed.** [Narrative export/import](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
1718–1719 and 1746–1747 preserve pending/effective maps, but 1783 clears deliveries.
[SaveGame](../../game/Source/QinglanAscension/Persistence/QinglanSaveGame.h) 34–45
contains no delivery state. The completed proposal stage replays without choices
at StorySubsystem 634–659/947, so it cannot recreate the missing delivery.

**Acceptance:** save/load before dispatch, after dispatch, before/after arrival,
and before/after verification. The matching proposal can still complete; previously
effective agreements and one-time rewards remain intact.

### AUD-17 — P2 — Effective agreements cannot be revised through the normal quest flow

- [ ] Implement proposal revisions independently from one-time quest completion,
  retaining the old effective agreement until the new verified reply is accepted.

**Confirmed.** [Available quests and revisits](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp)
861–875 excludes completed quests; 634–659/947 makes completed stages choice-free.
[Side-quest interaction](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
420–449 therefore offers only recap. This contradicts the revision contract in
[narrative implementation](../story/narrative-implementation.md).

**Acceptance:** make agreement A effective, draft B, revise B while waiting, and
confirm its matching reply. Ending evaluation reads A until B is accepted;
revisions and restart neither erase A nor duplicate rewards.

## Trials, rooms, traversal, and input

### AUD-18 — P0 — Trial admission requires the wrong chapter proof

- [ ] Separate Foundation and Golden Core admission, enforcing the corresponding
  cultivation, boss, and story prerequisites at every entry path.

**Confirmed.** [Hub gates](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
859–872 require Ashen proof for the only trial gate even though CH03 Foundation
precedes CH04 Ashen. The Ashen gate at 839–854 checks only Bamboo proof, permitting
out-of-order access. [Enter-trial choice handler](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1247–1269 checks Ashen proof but not Foundation rank 3 for Golden Core.

**Acceptance:** Bamboo clear + Qi rank 3 admits Foundation before any Ashen clear;
Golden Core requires Foundation rank 3 + Ashen proof. All shortcuts/scene actions
use the same authority, with a localized explanation for rejection.

### AUD-19 — P1 — Foundation success and trial cancellation have no safe return route

- [ ] Implement explicit preparation/cancel/stay/return/success transitions and
  place the post-Foundation scene in its intended hub location.

**Confirmed.** [Trial success](../../game/Source/QinglanAscension/Core/QinglanTrialDirector.cpp)
286–294 only completes an objective. No arena hub-return gate exists; the
[trial interaction branch](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
944–991 starts scenes/trials. `OnStayAtHub` is not bound. The chapter scan may play
CH03_S03 remotely but never returns to the hub. The later CH06 return choice is
not an available Foundation exit. Defeat or restart should not be the escape route.

**Acceptance:** cancel preparation, succeed, fail, and retry either trial using
ordinary controls. Foundation success reaches CH03_S03 at the hub with earned
progress preserved; no unwanted expedition remains open.

### AUD-20 — P1 — Timed ordinary waves replace the required trial mechanics

- [ ] Implement the authored trial phases, hazards, required elite/boss kills,
  and gameplay-triggered narrative events; reject initialization failures.

**Confirmed completion gap.** [TrialDirector](../../game/Source/QinglanAscension/Core/QinglanTrialDirector.cpp)
137–140 grants success solely when time expires; 203–248 cycles ordinary
archetypes, and success removes outstanding enemies. Foundation's elite construct,
Golden Core's required guardian, and storm/lightning phases are not implemented.
[TryBeginTrialAtAnchor](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
358–369 can directly complete the objective if trial initialization fails.
`NotifyGameplayEvent` has no production event producers for the authored trial beats.

**Acceptance:** timer expiry with the required enemy alive is not success; early
enemy defeat alone is not success. All required phases, warnings, safe zones, and
narrative beats occur. Failed spawning leaves a visible retryable failure and
grants no trial proof.

### AUD-21 — P1 — Encounter spawning remains at the first room's origin

- [ ] Pass an explicit current-room spawn volume/origin to the director or move
  a rooted director at room entry; unify the authored room layout constants.

**Confirmed placement mismatch; exact runtime behavior needs a functional map.**
[Realm BeginPlay](../../game/Source/QinglanAscension/Core/QinglanGameModeBase.cpp)
278–291 creates one director at room 0. [EnterRoom](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
397–419 updates the index but not the spawn origin.
[Portal placement](../../game/Source/QinglanAscension/AI/QinglanEncounterDirector.cpp)
281–282 still uses the director's `GetActorLocation()`; its constructor has no root.
Door/map coordinates use 5,500 cm pitch, while
[RoomLayout](../../game/Source/QinglanAscension/Progression/QinglanRoomLayout.h)
62/85–88 uses 6,000 cm. Enemies for later rooms are not tied to their room's floor.

**Acceptance:** portals/enemies for every room including each boss are within that
room's walkable bounds and at least 4 m from the player. Completion unlocks only
the correct next room. Validate nav/collision and failed-spawn recovery.

### AUD-22 — P1 — Open doors and unchecked exits grant victory without a boss fight

- [ ] Prevent physical room bypass and validate real boss/room completion before
  victory, proof, or first-clear payouts. Provide a distinct legitimate retreat flow.

**Confirmed.** [Interactable collision and exits](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp)
658–665 creates invisible `NoCollision` spheres; synthesized doors do not block
the connected slabs. At 787–801 the exit checks only that no encounter is active,
then calls `EndExpedition(true, true)` regardless of current room or boss completion.
A player can walk down the corridor without entering rooms and request victory.
[EndExpedition](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
535–542 also sets boss-clear flags on **defeat or retreat**, merely from realm ID.
The first-clear bonus trusts the caller's victory argument at 512.

**Acceptance:** walking past stations, leaving early, dying in room 1, or retreating
grants no unearned boss proof/bonus. Actual boss defeat commits proof once and
preserves it through later failures. Victory/retreat/defeat banking stays correct.

### AUD-23 — P1 — Some generated stations lack connected, safe traversal

- [ ] Connect all mandatory areas, relocate off-floor anchors, resolve overlapping
  trial targets by campaign context, and add safe boundary/fall recovery.

**Confirmed geometry; cooked traversal and collision adjustment need verification.**
[Arena bootstrap](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp)
861–869 leaves an 800 cm gap between the main and decision platforms without a
bridge or authored travel route. There is no jump input. Foundation and Golden
Core trial anchors occupy the same `(0,0,100)`, while aim selection chooses only
the strictly better dot score, making equal-position targets ambiguous.
The Ashen shelter-wall record at `(19500,1700,100)` (844) is between room floors
and outside the corridor's 400 cm half-width. Hub anchors/PlayerStart at Z=500
sit below the courtyard's Z=600 surface (705/718–750) and require spawn/focus QA.
No project `FellOutOfWorld`/KillZ recovery routes default destruction through
the health-driven defeat flow. Bamboo records at `(9500,±1900)` are inside room 2
and are **not** an off-floor defect.

**Acceptance:** ordinary movement reaches all scene/lore/decision stations;
each trial stage targets correctly. Walk/dodge off exposed edges and recover
with a usable pawn and correct checkpoint/banking. Test hub start/anchor heights
on cooked maps rather than assuming engine collision adjustment will repair them.

### AUD-24 — P1 — WASD axes are rotated and the input value contract disagrees

- [ ] Use a consistent Axis2D mapping and consumer; regenerate committed input assets.

**Confirmed mapping defect.** [Input bootstrap](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp)
133 declares `IA_Move` Axis3D; 159–162 maps W/S to X and D/A to Y.
[InputMove](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp) 619–625
reads `FVector2D`, mapping Y forward and X right. W therefore strafes right and
D moves forward. The type mismatch must also be corrected, but this audit does
not claim it necessarily crashes without the target engine's checks.
The mappings at bootstrap 159–186 are keyboard/mouse only; there are no gamepad
bindings, so controller support still requires implementation.

**Acceptance:** W/S camera-relative forward/back, A/D left/right, diagonals and
dodge direction correct in Development and Shipping. Controller movement, camera,
combat, interaction, pause, and menu bindings must all work, using the same movement
convention. [Epic's value API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/EnhancedInput/FInputActionValue/Get)
defines the typed access contract.

### AUD-25 — P1 — Escape pauses the world but no input can resume it

- [ ] Enable intended paused input and implement a usable pause/resume menu.

**Confirmed configuration gap.** [Pause handler](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
753–762 toggles world pause. [IA_Pause creation](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp)
140/176 never enables `bTriggerWhenPaused`, and no paused-menu unpause path exists.
[Epic's UInputAction API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/EnhancedInput/UInputAction)
documents that property.

**Acceptance:** repeatedly pause/resume combat, trials, and dialogue through
keyboard/controller. Gameplay timers/resources freeze; resume/quit/settings
remain usable and do not accidentally execute combat inputs.

### AUD-26 — P2 — Interaction focus uses camera distance without proximity, visibility, or hints

- [ ] Use player proximity plus camera focus/line of sight, scene-aware priority,
  visible station presentation, and a localized prompt for the selected target.

**Confirmed.** [Aimed interaction selection](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
196–226 checks distance from the camera (300 cm) with only dot ranking. The
camera arm is 350 cm, so a nearby player station can be outside the camera range.
Occlusion/player-distance checks are absent. The spawner sets `DisplayNameKey`
at 136 but no presenter consumes it; stations are invisible spheres.

**Acceptance:** adjacent stations work at natural camera angles; walls block
focus, overlapping stations choose the correct scene, and the prompt matches
the action actually dispatched.

## Combat, abilities, AI, animation, and effects

### AUD-27 — P1 — Equipped techniques never populate the player's runtime loadout

- [ ] Resolve profile technique IDs into assets and synchronize the pawn on
  possession, unlock/equip, profile load, and map travel.

**Confirmed.** [Technique component](../../game/Source/QinglanAscension/Combat/QinglanTechniqueComponent.cpp)
starts with an empty loadout at 87–93 and rejects absent slots at 139–142.
`SetLoadout` has only its declaration/implementation and no production caller;
`Character::BeginPlay` never imports the equipped profile techniques. Permanent
unlocks therefore do not make keys 1–4 usable or fill the skill HUD.

**Acceptance:** tutorial grants immediately become castable. Save/load/travel
retains the four assigned slots; every one of the eight techniques can be equipped
and used, with permitted swap/cooldown semantics preserved.

### AUD-28 — P1 — Ordinary damage bypasses enemy death and stagger lifecycle

- [ ] Drive enemy damage reactions/death from the combat authority's resolved
  result; stop actions, disable collision, release budgets, and emit death once.

**Confirmed.** [Sword damage](../../game/Source/QinglanAscension/Combat/QinglanSwordComponent.cpp)
478, [projectile damage](../../game/Source/QinglanAscension/Abilities/QinglanProjectile.cpp)
520, and [ground damage](../../game/Source/QinglanAscension/Abilities/QinglanTechniqueExecutors.cpp)
432 call `Combat::ApplyDamage` directly. Enemy stagger/death is only in
[EnemyCharacter::TakeDamage](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
236–289, with no binding from combat delegates. Tick at 177–194 checks AI Dead,
not health, so zero-health enemies can continue attacking/colliding without `OnDied`.
The director polls `IsAlive`, so this finding does **not** assert that missing
`OnDied` necessarily blocks room completion.

**Acceptance:** sword, projectile, area, and Burn kills during windup/attack stop
all damage/movement, remove collision correctly, release slots, and produce one
death/reward event. Nonfatal accepted hits apply the intended stagger rules.

### AUD-29 — P1 — Generic active-phase damage bypasses real enemy attack shapes

- [ ] Resolve each attack through its actual projectile/area/collision authority;
  limit active-phase proximity hits to the shapes that require them.

**Confirmed.** [Enemy attack update](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
686–692 runs a distance hit for every shape. Projectile commits do not consume
the hit flag, so ranged attacks can immediately damage within attack range and
then hit again with the projectile. Ground-circle, cone, and safe-gap attacks
that missed at commit can later hit through this generic range branch.

**Acceptance:** evading a projectile causes no damage; one projectile produces
one hit. Leaving a marked circle, staying in a safe gap, or moving behind a cone
avoids damage throughout the active window.

### AUD-30 — P1 — Player and enemy projectiles ignore walls

- [ ] Include world blockers in projectile collision and validate the actual
  casting/muzzle origin; allow piercing targets only where authored.

**Confirmed.** [Player sweep](../../game/Source/QinglanAscension/Abilities/QinglanProjectile.cpp)
309–320 only queries `ECC_Pawn`; [enemy collision](../../game/Source/QinglanAscension/AI/QinglanEnemyProjectile.cpp)
30–31 ignores all channels except Pawn overlap. `SetupLinear` at 169–172 places
the player projectile 60 cm forward without checking the intervening wall and
does not share the casting-socket origin contract.

**Acceptance:** both sides' projectiles stop at static/dynamic walls under free
aim, lock-on, shoulder-camera offsets, and point-blank casting. Jade piercing
does not pass through level geometry; spawn placement cannot cross a wall.

### AUD-31 — P1 — Dodged or rejected hits still apply Burn/Root

- [ ] Apply statuses only after an accepted authoritative hit and preserve the
  distinction between immunity, duplicate rejection, and accepted barrier hits.

**Confirmed.** [Enemy direct hits](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
139–149, [enemy projectile hits](../../game/Source/QinglanAscension/AI/QinglanEnemyProjectile.cpp)
175–182, and [player projectiles](../../game/Source/QinglanAscension/Abilities/QinglanProjectile.cpp)
520–525 ignore the damage result before applying status. A successful dodge can
still suffer Burn/Root, and a rejected duplicate can refresh it.

**Acceptance:** attacks inside valid i-frames change neither health nor status;
duplicate hit keys never refresh status. Accepted damage/barrier cases follow
the documented status precedence.

### AUD-32 — P1 — Combat actions and consequential-choice input lack a common state gate

- [ ] Implement shared action/cancel permissions, forced reactions, dodge
  buffering, and a complete menu input mode while a consequential choice is pending.

**Confirmed.** [CanStartSwing](../../game/Source/QinglanAscension/Combat/QinglanSwordComponent.cpp)
104–111 only checks alive/cuts, allowing sword attacks during casts/dodges.
[Character dodge/input](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
770–841 does not enforce post-release cast recovery, and forced HitReaction is
explicitly unwired at 782/1052. Failed early dodge input is dropped rather than
buffered. Movement/sword/dodge/lock/talismans at 610–698 also remain available
while `PendingSceneChoices` is nonempty; only technique number keys are intercepted.
The narrative freeze at 971–974 halts line advancement, not gameplay input.

**Acceptance:** exercise attack/cast/dodge for each state; no parallel committed
actions, no prohibited active/recovery cancellation, and allowed windup cancel
has no resource cost. The 0.15 s buffer works. Pending choices block gameplay and
resource consumption; resolve/cancel/death/travel reliably restores normal control.

### AUD-33 — P1 — Cast identities collide between independent attackers

- [ ] Allocate stable unique cast identities per attack and carry the complete
  release identity through projectiles; include attacker ownership in deduplication.

**Confirmed.** [AI controller](../../game/Source/QinglanAscension/AI/QinglanAIController.h)
130/195 has a local serial counter per controller.
[Direct damage](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
129–132 and [projectile damage](../../game/Source/QinglanAscension/AI/QinglanEnemyProjectile.cpp)
168–170 combine that local serial with the current frame. Two enemies' Nth
same-frame attack can match. [ApplyDamage dedupe](../../game/Source/QinglanAscension/Combat/QinglanCombatComponent.cpp)
286–301 keys only cast/victim/ordinal, so the second legitimate hit is rejected.
Projectile impact reconstructs the frame rather than retaining its release identity.

**Acceptance:** independent simultaneous attacks both resolve, while each attack's
duplicate contacts resolve once. Results do not change with projectile impact
frame, pooling, or attacker ordering.

### AUD-34 — P2 — Consecutive dodges reset their immunity clock late

- [ ] Record immunity start explicitly on each dodge, or query the dodge component's
  actual elapsed window instead of periodically observing the character state.

**Confirmed.** [Combat dodge tracking](../../game/Source/QinglanAscension/Combat/QinglanCombatComponent.cpp)
56–60 only rearms when `Now - DodgeStartTime > 0.5`. The dodge lasts 0.45 s,
so an immediate second dodge reuses the old clock until partway through the
new action, shifting the 0.10–0.28 immunity window.

**Acceptance:** at 30/60/120 FPS, first and consecutive dodges each grant immunity
only at the intended elapsed start/end boundaries, independent of component tick order.

### AUD-35 — P2 — Mist Step lacks bounded enhanced-dodge behavior and loses Wet targets

- [ ] Implement a bounded collision-aware dash and its immunity; apply Wet along
  the actual swept path with a deduplicated accumulated victim set.

**Confirmed.** [SelfDash](../../game/Source/QinglanAscension/Abilities/QinglanTechniqueExecutors.cpp)
274–294 uses only `LaunchCharacter`, explicitly omitting dodge immunity and a
bounded 600 cm movement controller. Four Wet samples at 312–315 reuse the same
array, but `GatherVictims` resets it at 378, leaving only the final sample. Wet
is applied before movement using the theoretical path, including wall-separated targets.

**Acceptance:** repeatable 600 cm travel, wall stops, authored immunity, and Wet
for targets at the path's start/middle/end; no Wet beyond actual movement/blockers.

### AUD-36 — P2 — Root/Slow and boss identity do not affect movement correctly

- [ ] Use one authoritative boss identity and connect movement status to player/AI
  movement and relevant actions; apply Shock's boss exclusion through that identity.

**Confirmed.** [Root authority](../../game/Source/QinglanAscension/Combat/QinglanCombatComponent.cpp)
560–566 relies on `Qinglan.Boss`, which no production code adds to boss actors.
[Status movement](../../game/Source/QinglanAscension/Combat/QinglanStatusComponent.cpp)
109–126 only updates counters; `GetMoveSpeedMultiplier` at 315 has no consumer.
Player movement continues while rooted, and the intended 10% boss slow has no
speed effect. Ordinary AI already has a Root suppression check in
[AIController::IsMovementSuppressed](../../game/Source/QinglanAscension/AI/QinglanAIController.cpp)
238–253; preserve that existing behavior while repairing player/slow integration.
[Shock talisman](../../game/Source/QinglanAscension/Combat/QinglanTalismanComponent.cpp)
241 uses the same missing tag and can incorrectly affect bosses.

**Acceptance:** Root halts ordinary targets; bosses receive the authored temporary
slow and recover their speed; affected player inputs obey the state. Shock does
not damage excluded bosses. Reapply/expiry/pool reset do not leave stale movement.

### AUD-37 — P2 — Technique upgrades and several equipped bonuses are inert

- [ ] Implement persistent tier-2 purchase/state and fold all authored damage,
  healing, barrier, reaction, and duration categories into the real combat authority.

**Confirmed delivery/correctness gap.** [Technique release](../../game/Source/QinglanAscension/Combat/QinglanTechniqueComponent.cpp)
429–432 hardcodes tier multiplier 1. Profile/save APIs have no technique-tier
mutation/state, while tier-2 cost/multiplier helpers have no production consumer.
ConductDamagePercent and WetDurationBonus are accumulated but not consumed;
[Conduct/Burn authority](../../game/Source/QinglanAscension/Combat/QinglanCombatComponent.cpp)
243–255 fixes Conduct at 1.25, and 540–546 discards the requested Burn duration.
Verdant Pulse does not fold the permanent healing bonus that the healing talisman uses.
Grounded Steps' `MaxStaminaBonus` has no consumer, and
[AppendPermanentRegen](../../game/Source/QinglanAscension/Progression/QinglanEconomyLibrary.cpp)
417–426 folds passive regeneration but omits Herbal Knowledge's `HealingPercent`;
neither the healing talisman nor Verdant Pulse receives that passive's +10% healing.

**Acceptance:** buy/save/reload tier 2 at the intended stage/cost; actual technique
damage/heal/barrier rises 15%. Each equipment/blessing category changes the measured
result once. Resolve and document duration-extension semantics alongside the
maximum-three-ticks-after-refresh contract rather than silently ignoring a bonus.
Explicitly verify Grounded Steps changes maximum stamina and Herbal Knowledge
increases both healing paths, including equip/unequip and save/load.

### AUD-38 — P2 — Verdant Pulse heals immediately and upgrades distort its duration

- [ ] Start the timer at zero and divide total healing across the authored tick
  count/duration, preserving total magnitude and the final remainder.

**Confirmed.** [Healing projectile](../../game/Source/QinglanAscension/Abilities/QinglanProjectile.cpp)
214 initializes `HealTimer` to a full interval, so the next tick heals immediately;
458–468 hardcodes at most 5 health each tick. Baseline 20 healing finishes near
0/1/2/3 s instead of 1/2/3/4 s. Increasing magnitude adds ticks and duration rather
than increasing the same four-second treatment.

**Acceptance:** baseline has no tick before one second, then 5 at seconds 1/2/3/4.
Tier/equipment bonuses increase total healing within the same four seconds;
health caps, interruption/expiry, and pooled reuse remain correct.

### AUD-39 — P2 — Recall teleports within combat instead of requesting a cleared-room retreat

- [ ] Route Recall through the authoritative cleared-room retreat/banking/hub
  transition; rejected requests must not consume charges.

**Confirmed.** [Recall implementation](../../game/Source/QinglanAscension/Combat/QinglanTalismanComponent.cpp)
254–280 teleports to the current room entrance without checking expedition or
cleared-room state and consumes a charge after returning success. It never ends
the expedition or performs the authored retreat.

**Acceptance:** using Recall in an uncleared room is refused without charge loss;
from a cleared room it banks the correct retreat amount, closes the run, returns
to the hub, and handles duplicate input once.

### AUD-40 — P2 — Wolf orbiting has no bounded transition to an attack

- [ ] Give the orbit phase a finite duration/inward transition before a telegraphed lunge.

**Confirmed control-flow issue; behavior needs a functional AI test.**
[Wolf decision](../../game/Source/QinglanAscension/AI/QinglanAIController.cpp)
735–743 moves purely tangentially between attack range and orbit radius, but only
begins attacking inside attack range. There is no orbit timer/phase exit.
A stationary player can leave the wolf circling without the authored lunge.

**Acceptance:** stationary and moving targets receive a lunge within a bounded
time; collision/nav avoidance does not create permanent circling or stuck movement.

### AUD-41 — P2 — Ashen Guardian's phase-two attack mechanism is absent

- [ ] Implement the authored phase-two embers outside the readable safe lane.

**Confirmed content gap.** [Enemy phase-two use](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
around 101 only shortens the idle break. The
[catalog](../../game/Source/QinglanAscension/AI/QinglanEnemyCatalog.cpp) describes
the 50%-health ember mechanic, but no corresponding phase-two hazard producer exists.

**Acceptance:** crossing 50% health adds the specified mechanic, with visible
safe lanes and unchanged minimum warnings, under Low and Metal as well as High.

### AUD-42 — P1 — Critical telegraphs and technique/projectile effects have no runtime presenter

- [ ] Bind actual attack/cast/impact/sustain events to complete cue lifecycles;
  provide warning-preserving fallback visuals and carry the full cast context.

**Confirmed.** [VFXCueComponent](../../game/Source/QinglanAscension/VFX/QinglanVFXCueComponent.cpp)
has no instantiation or call site outside its implementation.
[Enemy telegraph](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
443 broadcasts to no subscriber. Ground centers and safe arcs are recorded but
not displayed. Projectile classes have collision spheres without visible bodies.
[Technique cue publishing](../../game/Source/QinglanAscension/Abilities/QinglanTechniqueExecutors.cpp)
454–460 requires a non-null Niagara asset without a fallback. The
[projectile impact](../../game/Source/QinglanAscension/Abilities/QinglanProjectile.cpp)
around 542 sends an empty context, so it cannot resolve the technique's impact cue.
Imported cue materials alone do not display these events.

**Acceptance:** every shipped enemy/technique has visible anticipation, projectile
or area, impact, sustain, and decay as applicable. Warning shapes/safe gaps match
real damage regions. Low/low-flash keeps essential markers. Capture overlapping
effects on both target graphics backends.

### AUD-43 — P2 — Cue lights are omitted from lifecycle storage and ignore actual quality

- [ ] Store every created light before adding the cue instance; honor the actual
  quality/accessibility configuration and a bounded light budget.

**Confirmed latent defect.** [PlayCue](../../game/Source/QinglanAscension/VFX/QinglanVFXCueComponent.cpp)
199 copies the instance into `ActiveCues` before setting local `Inst.Light` at 211.
Decay/reset cannot destroy the real light. Line 202 always checks Balanced rather
than the player's setting. Low-flash flags are not read. The component is currently
unwired (AUD-42); fixing its wiring without this repair exposes the leak.

**Acceptance:** after 100 cues and decay/pool reset, component/light counts return
to baseline. Low suppresses forbidden cosmetic lights; low-flash changes effects
while retaining their warning silhouette.

### AUD-44 — P1 — Combat animation paths and action-state integration are incomplete

- [ ] Correct montage references, connect locomotion/action animation states and
  slots, play cast montages, and author/verify combat sockets and blade trajectories.

**Confirmed path/integration defects; skeleton/socket playback needs engine inspection.**
[Sword montage paths](../../game/Source/QinglanAscension/Combat/QinglanSwordComponent.cpp)
44–47 load `AM_SwordCut_1/2/3`, but committed assets and
[AnimBootstrap](../../game/Source/QinglanAscensionEditor/QinglanAnimBootstrap.cpp)
255 use `AM_Human_SwordCut_1/2/3`. Player/enemy meshes are assigned, but
[Character 459–460](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
and [EnemyCharacter 317–319](../../game/Source/QinglanAscension/AI/QinglanEnemyCharacter.cpp)
select looping Idle with `AnimationSingleNode`; no animation class/state-machine
is connected. `CastMontage` is assigned to techniques but never played.
Movement, directional dodge, interaction, hit reaction, and death animation flows
are unfinished. Sword sweep at 409–413 skips unmoving sockets, so a timer alone
does not supply the missing blade trajectory.

**Acceptance:** standing three-hit combos physically hit through moving blade
sockets; movement/dodge/cast/interaction/impact/death animate and obey gameplay
windows. Validate DefaultSlot, compatible skeletons, `sword_base`, `sword_tip`,
and `cast_source` in the saved/cooked assets after editor restart.

### AUD-45 — P2 — Animation bootstrap packages do not match their saved montage paths

- [ ] Create a separate full package path for each montage and explicitly assign
  the correct skeleton; verify regenerated assets from a fresh process.

**Confirmed source structure; current binary package/skeleton integrity needs UE inspection.**
[BuildComboMontage](../../game/Source/QinglanAscensionEditor/QinglanAnimBootstrap.cpp)
101 creates the directory `OutPath` as a package, while multiple differently
named montages at 106 are saved to `OutPath / MontageName` at 141. Object package
identity and on-disk path do not follow the same contract; the function also does
not assign the montage's skeleton. The Monsters group includes multiple rigs.

**Acceptance:** every montage's package/object path and skeleton match its file
and intended character. Fresh editor loads, cooking, and actual playback resolve
all assets without relying on objects left resident by the bootstrap process.

### AUD-46 — P2 — Each encounter creates permanently rooted transient catalog assets

- [ ] Cache a bounded catalog or use GC-visible owners instead of rooting each
  newly generated asset forever.

**Confirmed.** [CreateDataAssetFromSpec](../../game/Source/QinglanAscension/AI/QinglanEnemyCatalog.cpp)
419–421 calls `AddToRoot` with no matching release.
[Room wave construction](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp)
225 and [trial catalog creation](../../game/Source/QinglanAscension/Core/QinglanTrialDirector.cpp)
211–215 repeatedly generate new instances. Map destruction cannot collect them.

**Acceptance:** repeat rooms, expeditions, trials, and hub returns; catalog instance
and rooted-object counts remain bounded and referenced assets remain valid after GC.

## Bootstrap, localization, UI, graphics, and delivery

### AUD-47 — P1 — The custom game instance is configured in the wrong INI section

- [ ] Put `GameInstanceClass` in `/Script/EngineSettings.GameMapsSettings` and
  verify startup using a fresh Saved/Config directory.

**Confirmed configuration defect.** [DefaultEngine.ini](../../game/Config/DefaultEngine.ini)
31–34 places this property under `/Script/Engine.Engine` instead of the maps
settings section at 1. [Epic's UGameMapsSettings API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/EngineSettings/UGameMapsSettings)
identifies the property owner. A fresh default launch therefore does not select
`UQinglanGameInstance` through this configuration, skipping custom early graphics,
locale initialization, and registry warm load. Hub-side subsystem logic can still
run with the base game instance, so a headless hub log does not prove this setup.

**Acceptance:** assert the runtime game-instance class and its Init log in clean
Development and Shipping launches. Apply selected/conservative graphics before
the first heavy map; initialize locale and registry through the actual bootstrap.

### AUD-48 — P1 — Chinese translation sources are never compiled or registered for runtime

- [ ] Build a complete gather/import/compile/stage localization pipeline or an
  equivalent registered runtime translation implementation for both locales.

**Confirmed delivery gap.** [ImportStringTable](../../game/Source/QinglanAscensionEditor/QinglanDataImportCommandlet.cpp)
1563–1586 reads only `en-US` and inserts its source strings. Chinese JSON is used
for parity validation but never emitted as translations. There are no `.locres`
files, localization target/configuration, polyglot registration, or runtime locale
JSON loader in `game/`. `ActiveLocales` in DefaultEngine.ini alone does not create
translations or the packaged ICU/culture data.
[Runtime text resolution](../../game/Source/QinglanAscension/UI/QinglanHUDStatics.cpp)
95–106 loads the one string-table asset. See
[Epic's localization tooling](https://dev.epicgames.com/documentation/en-us/unreal-engine/localization-tools-in-unreal-engine)
and [localization packaging](https://dev.epicgames.com/documentation/unreal-engine/localization-overview?application_version=4.27)
for the resource/staging contracts; use the selected 5.8 engine's actual pipeline.

**Acceptance:** a clean offline Shipping install switches every UI/story/notice
English ↔ Simplified Chinese, including placeholders and CJK glyphs, and persists
the choice. Both resources are in the package, without access to repository JSON.

### AUD-49 — P1 — First-launch locale detection runs after first-launch state is removed

- [ ] Capture first-launch state before profile creation; choose/persist culture
  from that state and provide a normal language preference flow.

**Confirmed independent ordering bug.** [GameInstance Init](../../game/Source/QinglanAscension/Core/QinglanGameInstance.cpp)
185–187 creates profile 0 before `ApplyFirstLaunchLocale` at 207. The latter calls
`IsFirstLaunch` at 135, which now sees an existing profile and returns without
selecting Chinese. This remains a bug after AUD-47 is repaired. There is no other
language-selection/persistence caller in production code.

**Acceptance:** a truly new profile on a Chinese OS starts in the intended locale;
English OS starts in English. Explicit player preference wins on every later
launch and survives deleting/changing profiles according to the chosen settings policy.

### AUD-50 — P1 — The committed string table is stale relative to its consumers

- [ ] Regenerate and commit the current data assets, and automatically verify
  that all runtime text keys exist in the generated table.

**Confirmed artifact drift.** JSON has 101 UI keys, while the committed
[ST_Game](../../game/Content/Qinglan/Data/ST_Game.uasset) does not contain
`hud.barrier`, `hud.skill_slot`, or `dialogue.choice_entry` in ASCII or UTF-16.
[UI runtime](../../game/Source/QinglanAscension/UI/QinglanUIWidgets.cpp) 63–65,
287–290 and the choice formatter require them. This is a concrete missing-key
case, independent of the missing Chinese resource. Check the other newly added
key and all scene/asset IDs by loading the real generated registry.

**Acceptance:** reload regenerated assets in a fresh engine process; every runtime
key resolves without missing-string fallback. Importing current JSON and then
checking generated assets is part of the data-change workflow.

### AUD-51 — P1 — Required player menus and live HUD information are unfinished

- [ ] Implement usable profile, settings/accessibility, equipment/talisman,
  journal, pause/quit, shop, blessing, and outcome frontends with live state.

**Confirmed completion gap.** [UI bootstrap](../../game/Source/QinglanAscensionEditor/QinglanUIBootstrap.cpp)
177–239 only creates HUD/Dialogue/Journal/Ending trees. Journal is an empty class,
blank body, and has no open caller. [HUD refresh](../../game/Source/QinglanAscension/UI/QinglanUIWidgets.cpp)
272–319 updates vitals/barrier/skill text but not the objective, talisman charge,
or bark placeholders. The trial-remaining-time helper has no presenter caller.
[Shop/blessing offers](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1387–1416/1419–1432 use a single dialogue line or engine debug-message fallback,
without a complete persistent menu/dismiss flow. Three save slots and equipment
APIs exist without normal player selection/swap screens.

**Acceptance:** use every screen in Shipping on keyboard/mouse/controller;
display complete costs, inventory/banking previews, objectives, charges, cooldowns,
trial time, records, and agreement state. Offers persist until select/dismiss;
closing a menu clears stale number-key capture. Save/recovery errors are visible.

### AUD-52 — P1 — Long dialogue/records need complete layout and dismiss behavior

- [ ] Implement wrapping/scrolling, proper font/text-scale settings, record
  dismissal/journal reopening, and live/paper/memory source labels.

**Confirmed missing configuration/lifecycle; clipping requires visual QA.**
[AddText/dialogue layout](../../game/Source/QinglanAscensionEditor/QinglanUIBootstrap.cpp)
67–79 uses render scaling instead of a real font-size setting and has no wrapping
or scrolling; 208–215 gives fixed-size body and choice regions. Long multi-paragraph
lore can exceed that presentation. [Record opening](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp)
1230–1243 has no close action, while only scene completion hides dialogue.
[Source-kind field](../../game/Source/QinglanAscension/UI/QinglanUIWidgets.cpp)
145–150 is explicitly cleared instead of populated. Fixed coordinates do not
alone prove failure at 720p: the engine DPI curve also participates.

**Acceptance:** render longest EN/zh-Hans text and every choice group at 100/125/150%
scale, 720p/1080p/ultrawide/Retina; no clipping, missing CJK, or hidden controls.
Records open/scroll/close/reopen and show the intended source classification.

### AUD-53 — P1 — The cooking contract does not include assets loaded only by runtime strings

- [ ] Make runtime-loaded registries, widgets, techniques, meshes, montages,
  effects, and audio explicit cook dependencies through references, asset labels,
  or `DirectoriesToAlwaysCook`; validate the resulting package inventory.

**Confirmed configuration gap; packaged missing-asset behavior requires reproduction.**
[Packaging settings](../../game/Config/DefaultEngine.ini) 45–53 enumerate maps and
only add audio with `DirectoriesToAlwaysStageAsUFS=(Path="/Game/Qinglan/Audio")`.
This is a raw/non-asset staging setting with paths relative to Content, not a
request to cook `.uasset` dependencies. UI/story/audio and several character
resources are resolved through runtime string paths in
[HUDStatics](../../game/Source/QinglanAscension/UI/QinglanHUDStatics.cpp),
[GameInstance](../../game/Source/QinglanAscension/Core/QinglanGameInstance.cpp),
and [AudioSubsystem](../../game/Source/QinglanAscension/Core/QinglanAudioSubsystem.cpp).
There are no authored asset labels/always-cook rules establishing a complete
runtime asset graph in the repository. Editor load success does not establish
that a clean Shipping package contains these assets.
[Epic's packaging settings API](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DeveloperToolSettings/Settings/UProjectPackagingSettings?application_version=5.5)
distinguishes asset cooking from raw-file staging; verify the selected engine's
configuration and redirects rather than relying on an external cook-all script.

**Acceptance:** clean cook from a fresh checkout using the committed runbook;
inspect package inventory and launch offline without loose editor assets.
Every dynamic path resolves, with working UI/story/techniques/characters/audio.

### AUD-54 — P2 — Graphics settings do not apply their advertised feature policy

- [ ] Use the correct dynamic-resolution API/CVar and apply GI/reflection/Nanite
  and other preset decisions to actual rendering, with capability-aware reasons.

**Confirmed.** [ApplyResolutionScaleAndDynamicRes](../../game/Source/QinglanAscension/Platform/QinglanGraphicsSettingsService.cpp)
103–108 looks up `r.DynamicRes`, but the supported operation CVar is
`r.DynamicRes.OperationMode` and user settings must enable the corresponding
engine state. The null lookup merely logs a warning. See
[Epic's dynamic resolution documentation](https://dev.epicgames.com/documentation/unreal-engine/dynamic-resolution-in-unreal-engine?lang=en-US).
`ApplyToEngine` at 118–124 sets texture/shadow/effects/view-distance but not
GI/reflections/Nanite. `IsFeatureAllowed` at 492–501 returns false for Low without
applying that policy; [DefaultEngine.ini](../../game/Config/DefaultEngine.ini)
10–15 still enables those expensive rendering features. Detection at 345–375
largely considers system memory/platform, rather than GPU capability/memory.

**Acceptance:** inspect actual rendering settings/status after each preset and
restart. Low applies the promised reduced feature set, dynamic resolution responds
within user bounds on supported backends, and unavailable options show reasons.
Do not call Windows/Metal paths validated until measured on those devices.

### AUD-55 — P1 — Realistic environment/material and effect content is still a blockout

- [ ] Deliver the required scene dressing, valid textured surfaces, and authored
  hero effects; inspect the saved maps/materials rather than counting generated assets.

**Confirmed content gap; final visual quality needs rendered review.**
[Map bootstrap](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp)
701–713 and 764–779 builds slabs, box walls, and cylinder groves; imported nature
meshes are not used for a finished environment pass. No project Texture2D source
assets are present under `game/Content`, although the master at 233–242 uses
texture parameters and the surface importer at 531–548 intends to create them.
The committed small material instances alone do not deliver the intended
PolyHaven texture sets. No Niagara systems are committed, and the cue wiring is
also missing (AUD-42). This does not satisfy the promised realistic 3D surfaces
and scalable hero-effect presentation.

**Acceptance:** fresh-process shader/material validation with no missing texture
or default-error materials; rendered walkthroughs of the hub, both realms,
trials, and each hero effect at Low/High and both locales. Verify visual warning
clarity, collision/nav, and performance after the actual art pass.

### AUD-56 — P1 — The asset provenance ledger contains no imported assets

- [ ] Record every imported pack/model/material/texture/animation/music/SFX/font,
  exact source/version, redistribution rights, and required credits/notices.

**Confirmed delivery gap.** [Asset manifest](../../assets/asset-manifest.csv)
has only its header and zero records, while the repository includes third-party
character/weapon/nature/audio assets and importers cite external packs.
The requirements in [asset policy](../10-licensing-and-assets.md) demand provenance
even for CC0 assets. Source comments labeling a pack CC0 do not identify the
specific acquired file/license, particularly for the audio tracks.
This finding does not assert infringement; the required evidence is missing.

**Acceptance:** all committed and cooked third-party assets map to documented
approved sources and license files/credits. Check source redistribution separately
from permission to ship cooked content; resolve any unknown source before release.

**Resolution (2026-10-19).** The eight unknown-source audio files were replaced
with project-original WAVs generated by
`assets-raw/audio/original/generate_original_audio.py` (standard-library
synthesis; 4 seamless loops + 4 one-shot SFX). The manifest rows now record
`Project original (all rights reserved)`, and `QinglanAudioBootstrap.cpp`
imports the new `original/` sources while keeping the `SW_*` target names and
runtime paths unchanged. `regenerate-content.sh` fails when any of the eight
sources or imported audio assets are missing, and
`Qinglan.Content.AudioSourceContract` pins the source/target mapping. Remaining
acceptance: run regeneration/cook so the committed `SW_*` uassets carry the new
audio, then perform human listening QA.

### AUD-57 — P2 — Build and content regeneration depend on one uncommitted environment

- [ ] Commit portable build/test/cook/bootstrap entry points and version locks;
  parameterize external source roots and fail when mandatory outputs are missing.

**Confirmed reproducibility gap.** [Build/run guide](build-and-run.md) invokes
`/inspire/.../build-game.sh` and packaged output from `package-game.sh`, but neither
script is in the repository. Engine association specifies only `5.8`, without
the tested source revision/toolchain lock. Raw input roots are hardcoded in
[MeshImport](../../game/Source/QinglanAscensionEditor/QinglanMeshImport.cpp) 18,
[AudioBootstrap](../../game/Source/QinglanAscensionEditor/QinglanAudioBootstrap.cpp)
14, and [ContentBootstrap](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp)
50. Missing skeletal files are logged at MeshImport 61–64 without failing;
`bOk` can remain true with no sources. AnimBootstrap can report success with
zero paired techniques. Such success does not prove the intended outputs exist.

**Acceptance:** a contributor on a second machine builds and regenerates using
documented engine/source acquisition and configurable paths, without private
scripts. Missing mandatory source/output causes a nonzero result. Record command,
toolchain, engine commit, source hash, and artifact hash for each tested build.

### AUD-58 — P1 — The test suite does not cover the player journey or persistence failures

- [ ] Add meaningful integrated/functional regression coverage for the above
  root causes, and record results from the actual repaired revision.

**Confirmed coverage gap.** [Automation tests](../../game/Source/QinglanAscensionEditor/QinglanAutomationTests.cpp)
cover damage math, selected component cases, economy tables, mirror ending rules,
container serialization, and isolated transactions. Ending-gating fixtures at
623–658 do not execute real scene playback/courier/confirmation flows.
[CI](../../.github/workflows/validate.yml) runs only planning validation and reading
edition checks. Neither proves profile→pawn loadout, room spawn placement,
travel→bank→save, scene identity routing, skip/choice recovery, or a new-profile
campaign. The previous 12/12 report must not be extrapolated to these paths.

**Acceptance:** reproduce each code defect with its actual caller before closing
it. Cover save rotation failure injection; exact once rewards; first/repeat/death/
retreat runs; scene ownership/groups/courier/ending; loadout/input; enemy lifecycle;
and functional room/trial maps. Run the repaired suite, retain outputs, and perform
a complete keyboard/controller new-profile playthrough without debug grants.

### AUD-59 — P1 — Required target-platform and release acceptance has not been demonstrated

- [ ] Build and test matched Windows x64 DX12 and native Apple Silicon arm64 Metal
  packages, then record gameplay/performance/install/update acceptance.

**Delivery/verification gap, not a claim of a platform-specific crash.**
[The existing test report](test-report.md) explicitly leaves Windows/Metal
builds and interactive play pending. Linux compilation/headless tests and a
swapchain attempt cannot establish target compatibility. The
[Windows packaging directory](../../packaging/windows/README.md) contains an
incomplete template rather than a proven package/runbook; no completed Mac
signing/notarization/install record exists. Release certification/account actions
are separate owner-controlled steps; this audit request authorizes a TODO and
Git push, not publishing or purchasing services.

**Acceptance:** same gameplay/content hash on both targets; real-device full
campaign, offline persistence, keyboard/controller and bilingual UI; 15-minute
p95/p99/GPU/memory captures and 30-minute fanless Mac test against
[the platform budgets](../06-platform-and-performance.md); cold/warm startup and
vendor/hybrid-GPU coverage. Validate install/update/save retention; final Windows
packaging and Mac trusted-download tests follow [distribution acceptance](../09-distribution.md).
Keep private identities, credentials, and package keys out of public Git.

### AUD-60 — P2 — Repository status documents contradict the implementation and evidence

- [ ] Rewrite status/build/test documentation around current behavior and
  attached evidence, separating implemented modules, playable acceptance, and release readiness.

**Confirmed documentation defect.** [README](../../README.md) says gameplay modules
exist but later says no playable scene has been implemented and implementation
starts at M0. [CONTRIBUTING](../../CONTRIBUTING.md) says there is no `.uproject`.
[Test report](test-report.md) retains obsolete shell/trial statements alongside
newer implementation claims, and reports 97 UI keys while current JSON has 101.
[Build guide](build-and-run.md) mixes superseded rendering limitations with later
attempts. These statements make a developer's completion claim difficult to assess.

**Acceptance:** every passing claim includes build hash/date/environment and
accessible result artifacts; historical results are labeled as historical. The
status explicitly lists unresolved audit IDs, actual supported builds, and
remaining release gates. Never equate planning CI or asset generation with playability.

## Required handoff and closure

Use the priority order below while retaining the cross-item dependencies:

1. Repair save preservation/isolation and campaign identity/routing/admission/
   confirmation blockers. Add regressions before broad content polishing.
2. Restore functioning input/loadouts, room ownership/exit authority, enemy
   lifecycle/shapes/immunity, animation, and critical warning presentation.
3. Complete earned objectives, courier agreements, real trials, essential UI,
   translation/cook pipelines, and balance/status correctness.
4. Regenerate source-controlled assets from current data, verify a fresh checkout,
   complete rendered/packaged tests, and attach target-platform evidence.

For **each AUD ID**, append a disposition: fixed / still open / approved scope
change / not reproducible, with the repair commit, exact test steps/results,
source/content/build hash, and evidence location. A rejected finding needs a
concrete counterexample from the reviewed/repaired code or a runnable reproduction,
not only a statement that the modules compile. Explicit owner approval is needed
to reduce the product's agreed scope; do not silently mark missing features complete.

The development-completion gate is a normal new-profile journey from CH00 through
CH07, both realms/trials, all side stages and records, and each ending branch on
separate eligible saves, with no developer console or pre-granted proof. Include
save/restart at safe boundaries, defeat/retreat/retry, all equipment/techniques,
keyboard/controller, both languages, and working pause/settings/profile flows.

The release-ready gate additionally requires the target-device, performance,
asset-rights, and distribution evidence in AUD-53 through AUD-60. Development
completion and storefront publication are separate decisions. No unresolved P0/P1
may be hidden behind a passing documentation validator.

## Repair dispositions (2026-10-19)

This section records the disposition of every AUD ID after the 2026-10-19
repair waves. It does **not** declare development complete: the closure rule
above still governs. "Fixed" below means the source defect was removed and
automated coverage exercises the repaired path; "partially fixed" means the
source work landed but acceptance still needs in-engine, rendered, or
platform evidence; "open" and "blocked" are recorded as such.

Environment for every automated result: Linux container, UE 5.8.3 source
build (`tools/build/versions.lock`), headless `-nullrhi`. Evidence logs are in
`docs/dev/evidence/`. Repair commits: `1c7e3a0` (build repairs, 27 tests),
`ed02e88` (courier/trials/routing data, 35 tests), `16b1a7b` (proof guard
runtime, combat/AI/VFX, bilingual runtime, content, 44 tests), `f481f03`
(exit authority, focus, loadout, action gates, animation/VFX, 49 tests).
Auxiliary: `ac56e83` (AUD-56 ledger), `368445d` (packaging evidence). Later
waves: `f438ee1` (player menus/journal, textured surfaces, scene dressing,
53 tests), `40213bd` (authored Niagara cue systems, Human weapon/cast sockets,
66 tests), `36e16c5` (enemy presentation object paths, per-rig ABPs, hit-react
montages, 67 tests). Final wave 36 verification: clean content regeneration
`rc_all=0`, 67/67 automation, main journey 30/30, side quests 24/24, and a
Linux Shipping package with `NS_*`/`ABP_*` inventory verified. Wave 37 added
the cold-start profile frontend (no silent slot-0 creation), verified with
real keyboard input in the packaged build, and rebuilt the regenerated
967-key string table. Packaging evidence:
`docs/dev/evidence/package-linux-2026-10-19-wave37.log` and
`packaging/evidence/linux-shipping-2026-10-19-wave37.txt` (earlier:
`package-linux-2026-10-19-wave36.log`, `linux-shipping-2026-10-19-wave36.txt`,
`linux-shipping-2026-10-19.txt`).

| AUD | Disposition | Implementation and coverage | Remaining acceptance |
| --- | --- | --- | --- |
| AUD-01 | Partially fixed | Hub arrival no longer re-imports slot 0; save-before-travel wrappers; profile replacement clears run state. `ProcessRestartResume` simulates a process exit (new GameInstance/subsystems) and reloads the persisted checkpoint. Selected-slot frontend landed: `UQinglanGameInstance::Init` no longer creates slot 0 on first launch, `AQinglanHubGameMode` defers all arrival work (checkpoint resume, talisman refill, autosave, epilogue entry) until a profile session exists, and `AQinglanCharacter` opens the 3-slot Profile screen (local player controllers only) at cold start; a successful load/create closes it and runs the deferred arrival. The Profile screen cannot be backed out of without a session, and the load/create failures surface `menu.notice.load_failed`/`create_failed`. Verified in the packaged Shipping build with real keyboard input: empty slots render, Return creates slot 1 and enters the hub, Down+Return creates slot 2 — `docs/dev/evidence/rendered/profile-frontend-cold-start-2026-10-19.png`, `profile-frontend-selected-slot1-2026-10-19.png`, `profile-frontend-slot-navigation-2026-10-19.png`; save files for both slots are written. | Interactive hub return/restart within a full playthrough. |
| AUD-02 | Partially fixed | All promised boundaries request durable saves; realm resume restores room/vitals/encounters/blessings. `SaveRotationFaultInjection`, `SaveTruncatedTempRejected`, and `ProcessRestartResume` (new subsystem instance reload) cover the failure and restart paths. The cold-start frontend surfaces load/create failures as localized notices (`menu.notice.load_failed`/`create_failed`/`profile_required`) and keeps the selector open for retry; hub arrival work runs only after a successful selection. | Interactive kill-and-resume at each promised boundary on a display machine. |
| AUD-03 | Fixed (source + real-file fault injection) | Rotation only replaces validated back-ups and never deletes the last valid copy before the new primary commits; a failed dead-backup delete now aborts fail-safe. Tests: `SaveBackupRecoveryChain` (primary+bak1 corrupt, bak2 recovered), `SaveRotationFaultInjection` (one-shot failure at each rotation Move/Delete; last commit stays loadable, retry leaves a validated backup), `SaveTruncatedTempRejected` (torn temp write leaves the old primary byte-identical). Uses test-only save-directory override + one-shot Move/Delete/Write hooks. | Real process-kill / OS-level I/O failure on target hardware. |
| AUD-04 | Fixed (source) | Profile load/create atomically replaces profile+run, including an explicit empty run; reentrancy rejected. | Profile-switch UI with pawn/world teardown. |
| AUD-05 | Fixed | Real signed stream state restored; room-wave rebuild reproduces the saved wave. Tests: `RunSignedStreamResume`, `RunUnfinishedRoomResume`. | Engine stream comparison across save/reload on cooked maps. |
| AUD-06 | Fixed | Pending offer + owning room persisted; restore without reroll; selection saves once. Tests: `RunPendingDraftRoundtrip`, `RunDraftRoomReplay`. | Audio/UI verification of the restored offer. |
| AUD-07 | Fixed | Journal-sized transaction id search; atomic settlement; allocation/profile/overflow failures retain loot with a notice. Tests: `ExpeditionBankingAtomicity`, `TransactionIdempotency`, `ExactBankingFloor`. | Real 999-entry histories and disk failures. |
| AUD-08 | Fixed | First/repeat material payout contract and integer retreat/death flooring in production callers. Tests: `BankingMultipliers`, `ExactBankingFloor`. | Full first/repeat/retreat/death runs on cooked content. |
| AUD-09 | Fixed | Owner-scoped objective/choice identity with ambiguity rejection and legacy migration. Tests: `StoryIdentityResolution`, `ScopedObjectiveRuntime`, `ChoiceSaveScopeMigration`. | CH04–CH07 journey. |
| AUD-10 | Fixed | 16 late-story locations canonicalized; ledger/correspondence anchors authored and synthesized; trial gate/order and hub chapter scans fixed. `AuthoredCampaignRouting` validates the registry, `HubMapInteractionSpawner` the committed map. The opt-in new-profile journeys complete **30/30 main scenes and 24/24 side stages** through production APIs (`docs/dev/evidence/automation-2026-10-19-{campaign-30of30,sidequests-24of24}.log`). | Interactive keyboard/controller walkthrough on a stable display machine. |
| AUD-11 | Fixed (guard runtime) | Guard term import + evaluator + ordered reset + anchor/choice gates + production producers for all major authored beats; the 30-scene campaign verifies every authored guard is satisfied by its production action (or the trial director's settlement). Tests: `GuardBlocksDialogueOnlyProgress`, `OrderedGuardReset`, `ConfirmationGuardGate`, `AuthoredCampaignRouting`, campaign 30/30. | The authored device mini-games (shutter/wedge/cooling) are represented by their anchor interactions; a dedicated mini-game pass would deepen them. |
| AUD-12 | Fixed | Successful selection clears the skip halt. Test: `SkipChoiceResume`. | In-engine skip-before/after-choice QA. |
| AUD-13 | Fixed | Authored choice groups, required groups, clickable/disabled-aware native choices, keyboard/controller/mouse navigation beyond four. Test: `GroupedChoiceAcknowledgment`. | Rendered input QA. |
| AUD-14 | Fixed | Ending preview/acknowledge/confirm/cancel bound to the atomic transaction; modal input blocking; warning acknowledgement required. Test: `EndingConfirmationTransaction`. | All three endings rendered. |
| AUD-15 | Fixed | Sanctuary rest advances fiction time (18 h/12 h), premature accept shows the authored retry line, arrivals broadcast. Test: `CourierDeliveryFlow`. | Interactive verification on cooked content. |
| AUD-16 | Fixed | Story clock + delivery state persisted (schema v2), v1 migration synthesizes a pending reply, invariants enforce consistency. Tests: `DeliverySaveRoundtrip`, `SaveV1DeliveryMigration`. | Process-kill at each courier boundary. |
| AUD-17 | Fixed | Completed agreement stages replay as revisions; effective A stays until B's reply is confirmed; rewards idempotent. Test: `AgreementRevisionFlow`. | Interactive revision on cooked content. |
| AUD-18 | Fixed | Foundation/Golden Core admission authorities at every entry path. Tests: `AuthoredTrialAdmission`, `TrialAdmissionPolicy`. | In-engine gate verification. |
| AUD-19 | Fixed (source) | Cancel/stay/return/success paths; success commits proof + breakthrough + hub travel; failed saves stay retryable. | In-engine success/failure/cancel/retry QA. |
| AUD-20 | Fixed | Two-phase trials: storm schedule with ≥0.9 s warnings and safe spacing, then a required elite/shell kill; no free win. Tests: `TrialCatalogSpecs`, `HazardPlacementDeterministic`, `TimerExpiryRequiresKill`, `GoldenCoreEventOrder`. | Rendered hazard clarity and full trial QA. |
| AUD-21 | Fixed (source+content) | Shared room layout drives authoring/doors/resume; tagged PlayerStarts regenerated; maps regenerated. Anchor object-name suffixes are stripped so interaction ids match authored keys. Test: `HubMapInteractionSpawner` loads the committed L_Hub and verifies all authored anchors. | Cooked collision/nav and enemy placement QA. |
| AUD-22 | Fixed | Victory requires the final room with the boss room committed; defeat/retreat never write boss proof; distinct retreat. Test: `ExitRequiresBossClear`. | Full victory/retreat/defeat runs. |
| AUD-23 | Fixed (source+content) | Arena gaps, off-floor anchors, overlapping trial targets, and boundary recovery addressed; hub anchors/start heights corrected; maps regenerated. | Cooked traversal QA. |
| AUD-24 | Fixed (source+content) | Consistent Axis2D convention, gamepad bindings, regenerated input assets. | Development/Shipping input QA on both devices. |
| AUD-25 | Fixed (source+content) | Pause action triggers while paused; pause/resume menu path. | In-engine pause/resume QA. |
| AUD-26 | Fixed | Proximity + camera + line-of-sight focus with deterministic priority and a localized prompt. | Rendered prompt QA. |
| AUD-27 | Fixed | Profile loadout resolves to technique assets and syncs on possess/BeginPlay/commit/travel. Test: `LoadoutSync`. | All eight techniques equipped/used in-engine. |
| AUD-28 | Fixed (source) | Accepted-hit delegate drives enemy stagger/death; zero-health enemies stop. | Kills during windup/attack across delivery layers. |
| AUD-29 | Fixed (source) | Attack shapes resolve through their own authorities; generic proximity limited. | Evasion QA per shape. |
| AUD-30 | Fixed (source) | Projectiles sweep world blockers; spawn origin validated; piercing limited. | Wall/cover QA both sides. |
| AUD-31 | Fixed (source) | Statuses apply only after an accepted authoritative hit. | I-frame/duplicate QA. |
| AUD-32 | Fixed | Shared action gate, dodge buffering, forced reaction, choice input lock. Test: `ActionStateGate`. | Full input-state matrix QA. |
| AUD-33 | Fixed | Process-wide cast identity + attacker in the dedupe key. Test: `HitKeyDedupe`. | Independent simultaneous attacks in-engine. |
| AUD-34 | Fixed | Immunity clock re-anchors on every dodge start. Test: `ConsecutiveDodgeImmunity`. | Frame-rate matrix QA. |
| AUD-35 | Fixed (source) | Bounded collision-aware Mist Step dash; Wet applied along the swept path with a deduplicated victim set. | In-engine dash QA. |
| AUD-36 | Fixed | Root/slow drive movement through one authority with baseline restore; boss identity shared with Shock exclusion. Test: `StatusMoveMultiplier`. | Reapply/expiry/pool QA. |
| AUD-37 | Fixed | Tier-2 purchase/state/persistence, 1.15 multiplier, Conduct/Burn/Wet bonuses, Herbal Knowledge in both heal paths, Grounded Steps stamina, equipment deltas. Test: `TechniqueTierBonus`. | Equip/unequip/save/load QA. |
| AUD-38 | Fixed | Verdant Pulse timer starts at zero, total healing spread across the authored ticks. Test: `BurnThreeTickRefresh` (status cadence) covers the shared timing contract. | In-engine heal timing QA. |
| AUD-39 | Fixed | Recall routes through the authoritative cleared-room retreat and never charges on rejection. | In-engine recall QA. |
| AUD-40 | Fixed (source) | Wolf orbit has a finite budget and inward transition before the telegraph. | Functional AI test. |
| AUD-41 | Fixed | Ashen embers gated to the boss at 50% with warnings and a safe-lane broadcast. | Rendered Low/Metal QA. |
| AUD-42 | Partially fixed | Cue component instantiated on enemies, telegraph/ember/impact context carried, ground-center/ring/safe-lane placeholders, visible projectile bodies, no-Niagara fallback. `BootstrapNiagaraEffects` now authors `/Game/Qinglan/VFX/NS_{Impact,Telegraph,Cast}` from engine emitter templates (1 emitter handle each, post-save load verified; deletes the package on verification failure); the cue component resolves/caches the category system, spawns it for impact/telegraph/safe-lane with a bounded per-component Niagara budget, suppresses Niagara (placeholder markers remain) under Low quality / low-flash, and tears instances down on cue decay and pool reset. `Qinglan/VFX` is listed in `DirectoriesToAlwaysCook` because the systems are loaded by runtime path (no hard reference). Evidence: `content-bootstrap-2026-10-19-niagara-sockets.log`. Test: `Qinglan.VFX.NiagaraCueContract`. Enemy presentation paths were also package-only (`FSoftObjectPath` without `.Object`), so no archetype ever loaded its mesh, idle, or montage; the catalog now builds real object paths, addresses the Bat/Dragon meshes by their imported names, and resolves the sibling slot graph per rig. Test: `Qinglan.Content.EnemyPresentationAssets` (asserts every archetype mesh/idle/attack/hit-react asset resolves and its slot graph targets the mesh skeleton). | Capture overlapping effects on the Metal/Low backends (GPU environment blocked); VFX audio cues still unbound. |
| AUD-43 | Fixed | Lights stored before registration, bounded, destroyed on decay/reset, honor preset + low-flash. Test: `CueLightBookkeeping`. | Rendered light-budget QA. |
| AUD-44 | Partially fixed | Montage paths corrected, per-cut playback, cast montages, stationary-socket sampling with fallbacks, and a native animation instance (`UQinglanCharacterAnimInstance`) with a speed-driven Idle/Walk/Run/Airborne state machine plus action states wired on player and enemies. The bootstrap generates `SK_HumanHuman_Armature_Walk` (1.0 s) / `_Run` (0.6 s) via the engine data-controller recipe and a compiled `ABP_Human` (parent = the native anim instance, Root←Slot "DefaultSlot", 0 compile errors), so montage slots now have a pose graph; the player and Human-skeleton enemies select it at BeginPlay/InitializeFromData with a native-class fallback. Tests: `MontagePathContract`, `NativeStateMachineExists`. The bootstrap now authors `sword_base`, `sword_tip`, and `cast_source` on `SK_Human_Skeleton` at `RightHand` (saved with the skeleton, re-loaded and verified on every run; meshes inherit skeleton sockets), so `AQinglanSwordComponent`'s blade sweep and `AQinglanCharacter::GetAimPoint`'s cast socket resolve on the rig instead of falling back. Test: `Qinglan.Animation.SkeletonSocketContract`. Non-Human rigs now get their own compiled slot graphs beside the mesh (`ABP_Knight`, `ABP_Slime`, `ABP_Skeleton`, `ABP_Bat`, `ABP_Dragon`; 0 compile errors, target skeleton verified, re-loaded after save), and `AQinglanEnemyCharacter` derives the sibling graph from the mesh package and verifies its target skeleton before use (native-class fallback otherwise). `AM_Human_HitReact` / `AM_Knight_HitReact` are cloned from the dodge/attack take so every catalog hit-react path resolves; the catalog also addresses the Bat/Dragon meshes by their real imported names (`Monsters/Bat`, `Monsters/Dragon`) instead of nonexistent `SK_` paths. Test: `Qinglan.Content.EnemyPresentationAssets`. | Visual tuning of the procedural cycles (rendered QA). |
| AUD-45 | Fixed | Independent package per montage with explicit skeleton; post-build contract verification. Test: `MontagePathContract`; regeneration verified 8 montages. | Cooking/playback from a fresh process. |
| AUD-46 | Fixed (source) | Bounded catalog asset cache replaces permanent rooting. | GC/count telemetry across rooms/trials. |
| AUD-47 | Fixed (config) | `GameInstanceClass` moved to the maps-settings section. | Clean Saved/Config launch asserting the instance and Init log. |
| AUD-48 | Fixed | Generated `DT_Localization` (890 bilingual rows), registered runtime text source for every `FText::FromStringTable` consumer, HUD prefers the service. Test: `BilingualKeyParityAndResolve`. Offline Shipping switch verified: with `[QinglanLocalization] PreferredCulture=zh-Hans`, every HUD string resolves in Chinese and CJK glyphs render (no tofu) — `docs/dev/evidence/rendered/hub-packaged-shipping-zh-Hans-2026-10-19.png`. | — |
| AUD-49 | Fixed | First-launch state captured before profile creation; explicit culture preference persisted outside the profile. Test: `FirstLaunchOrdering`. | New-profile QA on a Chinese OS. |
| AUD-50 | Fixed | `ST_Game` regenerated and asserted at import time (966 keys written; failure if the table truncates); the object-path table id bug that made every packaged lookup return the missing marker is fixed and covered by `Qinglan.Localization.StringTableRuntimeKeys`. | Fresh-process asset-load QA (now covered by the rendered packaged boot). |
| AUD-51 | Fixed (source+tests) | Native pause, profile-slot, settings/accessibility, equipment/talisman, journal, shop/offer, and outcome screens with keyboard/controller/mouse navigation; HUD shows objective, talisman charges, trial time, barks. Tests: `JournalOpenCloseReopen`, `MenuStateMachine`, `OfferPersistence`. Rendered hub HUD confirmed in the packaged Shipping build (`docs/dev/evidence/rendered/`). | Full menu interaction QA on a display machine; outcome summary is limited to returned loot until Run exposes a settlement summary. |
| AUD-52 | Fixed (source+tests) | Responsive native dialogue/record widgets with real font-size settings (not render scale), wrapping, scrolling, source-kind labels, record close/reopen. Test: `JournalOpenCloseReopen`. First rendered HUD pass shows correctly spaced skill slots and resolved labels. | Text-scale/720p/Retina/CJK visual QA remains a rendered acceptance step. |
| AUD-53 | Fixed (Linux) | Packaging settings corrected to the config=Game section: all runtime-string content directories and all four maps cook. Package inventory verified in the IoStore table of contents (maps, DT_Localization, ST_Game, registry, widgets, montages, character/monster meshes, audio, materials); `package-game.sh` rc=0 and mandatory-output checks fixed. Evidence: `packaging/evidence/linux-shipping-2026-10-19.txt`. | Offline launch on a display machine; Windows/macOS cook inventory. |
| AUD-54 | Fixed (source) | Correct dynamic-resolution CVar/user setting; GI/reflection/Nanite policy applied per preset; capability reasons. | Measured rendering on supported backends. |
| AUD-55 | Partially fixed | 81 Poly Haven Texture2D assets imported with correct sRGB/compression; textured + solid MICs per surface set; master material with tiling/samplers; hub/trial surfaces textured; deterministic collision-free Quaternius nature dressing on all four maps; sky atmosphere + atmosphere-driven sun added to every map; hero skin texture imported and bound through `MIC_HumanCharacter`. Test: `TextureSetContract`. Rendered packaged screenshots show the textured world and a lit blue sky (`docs/dev/evidence/rendered/`), including a Low-preset + low-flash + `zh-Hans` boot (`hub-packaged-shipping-low-lowflash-zh-2026-10-19.png`). Hero cue effects now use the authored Niagara systems. | Hand art/UV mapping for the palette skin and target-hardware performance captures remain open (performance numbers need a stable display machine). |
| AUD-56 | Fixed (source+ledger) | Ledger populated with 42 records. The eight untraceable audio sources were replaced 2026-10-19 with project-original WAVs (generator script in `assets-raw/audio/original/`), the bootstrap imports the new `original/` sources under unchanged `SW_*` paths, `regenerate-content.sh` validates the eight sources and imported assets, and `Qinglan.Content.AudioSourceContract` pins the contract. No unknown or pending ledger rows remain. | Regeneration/cook re-imports the new audio into the committed `SW_*` uassets; human listening QA (loop seams, mix, SFX transients). |
| AUD-57 | Fixed | Portable build/regenerate scripts, engine revision lock, nonzero on missing inputs, mandatory-output validation, regeneration rc_all=0. | Second-machine reproduction. |
| AUD-58 | Partially fixed | 53 prior automation cases plus three real-file save fault-injection specs (`SaveBackupRecoveryChain`, `SaveRotationFaultInjection`, `SaveTruncatedTempRejected`) cover the repaired persistence/narrative/trial/combat/localization/UI/content paths. | Functional maps, complete keyboard/controller playthrough, process-kill at promised boundaries. |
| AUD-59 | Blocked | Linux Shipping package rebuilt after the repair waves (rc=0) with a verified IoStore inventory (including the Niagara cue systems and per-rig slot graphs) and artifact hashes: `packaging/evidence/linux-shipping-2026-10-19-wave37.txt` (supersedes `linux-shipping-2026-10-19.txt`). | Windows DX12 and Apple Silicon Metal builds/devices are unavailable in this environment; owner action required. |
| AUD-60 | Fixed | README/CONTRIBUTING/test-report/build guide rewritten around recorded evidence with historical results labeled. | — |

### Remaining acceptance gates (not satisfied by this repair work)

1. **Full campaign playthrough** from a new profile through CH00–CH07, both
   realms and trials, all side stages/records, and each ending branch, with
   save/restart, defeat/retreat/retry, keyboard/controller and both locales.
   This requires a display-capable machine and is the development-completion
   gate from the closure rule above.
2. **Fault injection** for save rotation, disk failures, and process kills at
   every promised boundary.
3. **Rendered and cooked evidence**: collision/nav in regenerated maps, input
   QA on both device classes, hazard clarity at Low/low-flash, CJK glyphs in
   Shipping, and the target-platform performance captures. (Wave 36 closed the
   Shipping CJK-glyph check and a Low + low-flash boot render; hazards still
   need an in-combat capture, and input QA/perf remain.)
4. **Asset provenance** (resolved at source level 2026-10-19): the eight audio
   assets are project-original and the ledger has no unknown or pending rows.
   Regeneration/cook and listening QA remain before release.
5. **Target platforms**: Windows x64 DX12 and native Apple Silicon Metal
   builds and device acceptance (AUD-59) are outside this environment.

No checkboxes above are ticked: per the closure rule, ticking requires the
in-engine/platform evidence listed in each item's acceptance section.
