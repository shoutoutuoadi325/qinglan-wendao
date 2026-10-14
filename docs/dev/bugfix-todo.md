# Gameplay Bugfix TODO and Integration Acceptance

Owner-requested repair backlog, prepared on 2026-10-14 (Asia/Shanghai).

Audit baseline: `origin/develop` at `98aa878c746a3c151aae61513de5c590fcd376ae`.
At that baseline, `main` contains the planning/narrative package, while
`develop` contains the Unreal implementation. Recheck the latest code before
acting: file locations below identify investigated paths, not immutable line
numbers or proof that a later revision still contains each defect.

**Objective:** fix the gameplay defects and incomplete player-facing flows,
verify the integrated result, then merge the game implementation and fixes into
`main` through the repository's normal workflow. A successful bugfix merge does
not establish release readiness for Windows or macOS.

## 0. Working rules

- [ ] Read applicable `AGENTS.md`, [CONTRIBUTING](../../CONTRIBUTING.md),
  [code contracts](code-contracts.md), and the current implementation.
- [ ] Inspect local changes and preserve work that belongs to others.
- [ ] Fetch current remote branches. Start a repair branch from the latest
  `develop`, for example `codex/fix-gameplay-progression`.
- [ ] Reproduce or trace each reported defect against that revision. Record
  whether it is still present, already fixed, or requires runtime confirmation.
- [ ] Use subagents if useful for narrative, encounter, persistence, and UI
  reviews; verify the final combined implementation after integration.
- [ ] Fix root causes and add meaningful regression coverage. Do not weaken
  assertions, remove tests, bypass prerequisites, or pre-grant progression flags
  to make a broken path appear complete.
- [ ] Keep development documentation, comments, and commits in English. Put
  player-facing text into English and Simplified Chinese localization resources.
- [ ] Keep source, JSON, generated Unreal assets, and saved-state contracts
  consistent. Do not commit engine sources, credentials, caches, or private SDKs.

## 1. Boss-clear proof is granted after defeat

**Confirmed at the audit baseline.**

Investigate:

- [RunSubsystem](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp):
  `EndExpedition`, campaign flag banking (baseline lines 535-541).
- [Character](../../game/Source/QinglanAscension/Combat/QinglanCharacter.cpp):
  defeat handler calls `EndExpedition(false, false)` (baseline lines 1308-1310).

The clear-flag block selects Bamboo/Ashen proof from the realm's boss ID without
checking victory. Death and other non-victory exits can therefore satisfy boss
gates. The nearby first-clear payout already checks victory, but that does not
protect the separate campaign flags.

- [ ] Grant boss-clear proof only from a verified boss victory. Audit every
  `EndExpedition` caller so a misleading victory argument cannot bypass this.
- [ ] Prevent death, retreat, and pre-boss exits from granting first-clear proof
  or first-clear bonuses.
- [ ] Preserve previously earned proof after subsequent failures.
- [ ] Preserve the documented banking percentages and idempotent rewards.
- [ ] Determine whether existing saves contain incorrectly granted flags and
  document a conservative compatibility policy; do not invent valid completion.

Acceptance:

- [ ] New-profile death and retreat grant no boss-clear proof.
- [ ] Clearing ordinary rooms without defeating the boss grants no proof.
- [ ] A valid boss victory grants proof and the intended first-clear bonus once.
- [ ] Repeating settlement, restarting, and later failing do not duplicate
  rewards or remove legitimately earned proof.

## 2. Late-campaign objective IDs collide

**Confirmed at the audit baseline.**

Investigate:

- [Late main-story data](../../data/story/main-late.json): CH04-CH07 reuse
  `s01.objective`, `s02.objective`, `s03.objective`, and `s04.objective`.
- [Data importer](../../game/Source/QinglanAscensionEditor/QinglanDataImportCommandlet.cpp):
  `FillObjective` imports IDs unchanged.
- [Story subsystem](../../game/Source/QinglanAscension/Narrative/QinglanStorySubsystem.cpp):
  `FindSceneByObjective` returns the first match; `NotifyObjectiveCompleted`
  uses that scene as the owner.

Consequently a later boss or trial completion can resolve to a CH04 objective.
Completed-objective bookkeeping also uses the unscoped ID. The existing
repository validator passed despite these collisions.

- [ ] Define one identity contract: globally unique objective IDs or an explicit
  scene/objective composite identity. Apply it consistently.
- [ ] Update story data, importer, runtime state, event callers, and save
  serialization. Audit main scenes and side stages for similar ambiguity.
- [ ] Make completion resolve to the intended scene and respect prerequisites.
- [ ] Reject duplicate identities during data validation/import.
- [ ] Handle old saves. Reconstruct scoped progress from trustworthy completed
  scenes where possible. Never expand one ambiguous legacy objective into
  completion of every chapter that shared it.
- [ ] Document unresolved legacy ambiguity and any required recovery behavior.

Acceptance:

- [ ] Same-numbered objectives in CH04, CH05, CH06, and CH07 remain isolated.
- [ ] CH05 boss completion advances the correct scene.
- [ ] CH06 trial completion commits its actual scene and sets the intended
  flags, including `mq_governor_shell_disabled` when its conditions are met.
- [ ] Duplicate notifications cannot duplicate rewards or progression.
- [ ] Save/load preserves correct scene ownership; legacy fixtures do not
  acquire progress that was never earned.
- [ ] An intentionally duplicated-ID fixture fails validation.

## 3. Encounter spawning does not follow the current room

**Placement mismatch confirmed statically; exact gameplay blockage needs a
runtime reproduction.**

Investigate:

- [Encounter director](../../game/Source/QinglanAscension/AI/QinglanEncounterDirector.cpp):
  constructor, `BeginEncounter`, and portal placement in `Tick`.
- [Realm game mode](../../game/Source/QinglanAscension/Core/QinglanGameModeBase.cpp):
  initial director creation near the first entrance.
- [Run subsystem](../../game/Source/QinglanAscension/Progression/QinglanRunSubsystem.cpp):
  `EnterRoom` starts an encounter without updating its spatial anchor.
- [Interactable](../../game/Source/QinglanAscension/Core/QinglanInteractable.cpp):
  door progression teleports the player to the next room.
- [Room layout](../../game/Source/QinglanAscension/Progression/QinglanRoomLayout.h)
  and [content bootstrap](../../game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp).

Portal placement uses the director actor location while the player moves between
rooms. The director constructor also needs review for its missing root component.

- [ ] Resolve an explicit current-room spawn region/anchor for every encounter.
- [ ] Unify room bounds, entrances, doors, and spawn coordinates; check existing
  hardcoded room spacing for disagreement with the layout contract.
- [ ] If actor transforms represent the anchor, provide a valid root component.
- [ ] Validate ground height, collision, room bounds, and player safety distance.
- [ ] Cover ordinary enemies, elites, bosses, adds, and pooled actors.
- [ ] Preserve cleared-room idempotency and correct placement on checkpoint
  restore and room re-entry.

Acceptance:

- [ ] All Bamboo and Ashen combat rooms spawn enemies inside the current room.
- [ ] Enemies can engage the player, with no enemies stranded at the first room.
- [ ] Clearing each encounter opens the correct door exactly once.
- [ ] Advancing and restoring rooms starts the correct wave without duplicate
  rewards or previously cleared waves.

## 4. Story scenes lack reliable interaction routes

**Static mismatch found; verify each route in the current implementation.**

Investigate late-story `location` values, bootstrap anchors, interactable anchor
matching, chapter entry, and `CompleteSceneById`. Some locations are natural
language descriptions such as "Ashen Peak archive, existing room 3", while the
map uses an ID such as `ashen.archive_3`. Existing raw-name/slash-form matching
does not connect those representations. Completing a scene does not itself
prove the next scene is reachable.

- [ ] Use stable machine identifiers to bind scenes to map interactions.
  Keep human-readable location descriptions separate where needed.
- [ ] Inventory every main and side scene's entry trigger: map entry, NPC,
  investigation, encounter completion, trial completion, or another event.
- [ ] Close the chain between prerequisites, dialogue completion, gameplay
  proof, `next_scene_ids`, and the next valid trigger.
- [ ] Do not require repeated map exits/re-entry to progress normally.
- [ ] Do not unconditionally autoplay successors that require combat,
  investigation, travel, or a player decision.
- [ ] Validate missing anchors, missing trigger routes, and unreachable scenes.

Acceptance:

- [ ] A new profile can progress through all eight main chapters normally.
- [ ] Specifically verify CH04_S01 to CH04_S02, CH05 boss aftermath, and CH06
  trial through ending selection and epilogue.
- [ ] Side stages remain reachable with their intended prerequisites.
- [ ] Reloading or revisiting a map neither skips prerequisites nor repeats
  committed scene rewards.

## 5. Close the runtime localization pipeline

**Tracked delivery gap at the baseline; check for external generated artifacts
before deciding the exact repair.**

`ImportStringTable` populates `ST_Game` from en-US. Chinese JSON is validated,
but the audit found no tracked localization resource, gather/compile
configuration, or alternate runtime Chinese loader. Culture selection and
matching JSON keys are insufficient evidence that Chinese appears in-game.

- [ ] Implement and document a reproducible English/Simplified Chinese runtime
  localization pipeline, including generated resources and packaging.
- [ ] Verify first-launch selection, a usable language switch, and persistence
  after restart according to the product requirements.
- [ ] Cover main/side dialogue, lore, skills, items, choices, interactions,
  defeat/error notices, and endings.
- [ ] Remove hardcoded English and placeholder text from the Chinese player UI.
- [ ] Verify CJK fonts, wrapping, supported text scales, and missing-key fallback.
- [ ] Capture actual English and Chinese runtime screens. JSON parity alone
  cannot close this task.

## 6. Finish essential player UI connections

The current HUD/Dialogue already has real named-element bindings. Do not treat
the old test report's "empty shells" statement as current evidence or rewrite
working widgets without a reason.

- [ ] Connect talisman names, charges/status, and current objectives to the HUD.
  Remove baseline `[Item 1]`, `[Item 2]`, and `Objective` placeholders.
- [ ] Provide creation, selection, and continuation for the three profile slots;
  verify slot isolation and recovery behavior.
- [ ] Provide equipped-talisman selection and reflect it in the Q/R slots.
- [ ] Verify Journal and Ending creation, opening, closing, and live contents;
  author their missing flows if still absent.
- [ ] Verify pause/resume, dialogue choices, focus, and keyboard/controller
  navigation, including recovery from every modal screen.
- [ ] Keep essential information and choices visible in Shipping. Developer
  on-screen messages cannot be the sole player-facing presentation path.
- [ ] Exercise supported resolutions and text scales, including 1280x720, for
  clipping and inaccessible controls.

## 7. Assets, reproducibility, and accurate status

- [ ] Regenerate affected data/UI assets and commit required LFS assets so they
  match their source contracts.
- [ ] Inspect asset dependencies and cook/staging rules. Verify a clean checkout
  obtains all necessary content without undocumented local files.
- [ ] Populate [asset-manifest.csv](../../assets/asset-manifest.csv) with sources,
  versions, licenses, distribution rights, and attribution for imported models,
  materials, music, and SFX. At baseline it contains only the header; this alone
  does not establish a licensing violation.
- [ ] Supply repository-owned build/import scripts or reproducible instructions;
  remove dependency on uncommitted machine-specific absolute-path scripts.
- [ ] Update [README](../../README.md), [build instructions](build-and-run.md),
  and [test report](test-report.md) to reflect the final code.
- [ ] Distinguish implemented behavior, checks actually passed, checks not run,
  environmental blockers, and remaining work. Do not imply tests were executed
  when only test code was authored.
- [ ] Track remaining Niagara/art/audio polish, platform performance, Store
  identity/package configuration, Mac signing/notarization, and installation
  acceptance separately. Do not claim these are complete without evidence.

## 8. Required verification

- [ ] Run `python3 tools/validate_repository.py`.
- [ ] Run `python3 tools/export_story_reading.py --check`.
- [ ] Compile affected Unreal runtime and editor modules.
- [ ] Run the existing Unreal automation suite and the new regression coverage.
- [ ] Run world/functional tests for encounter positioning, scene progression,
  death, retreat, settlement, and save/recovery; independent formula tests do
  not cover these integration defects.
- [ ] Verify a new-profile campaign, both trials, and each ending route with
  appropriate prerequisites. Record interactive playthrough evidence when the
  environment supports it.
- [ ] Build/cook a Shipping configuration and inspect runtime content/UI;
  check for editor-only APIs and soft-reference asset omissions.
- [ ] Build and test Windows DX12 and Apple Silicon Metal when suitable target
  environments are available. Otherwise explicitly leave them unverified.
- [ ] Apply the relevant acceptance cases in [QA](../08-quality-and-testing.md)
  and [distribution](../09-distribution.md), without substituting documentation
  CI for gameplay validation.

Record revision, commands, configuration, platform/hardware, expected/actual
results, and artifact locations. The baseline reported 12/12 Unreal automation
tests, but those were not independently rerun by the audit. Its independently
executed checks were the two Python commands above, both passing. They did not
detect the objective-ID collision and do not establish complete gameplay.

## 9. Merge gate and delivery

- [ ] Confirm all core progression defects are fixed and relevant automated
  and functional checks pass. Do not merge known main-story blockers or failed
  required checks.
- [ ] If an environment prevents verification of a critical fix, record "changed,
  verification pending" and the precise blocked checks. Do not mark it accepted
  or silently reduce the merge gate.
- [ ] Keep outstanding Store publication, signing, and broader release-matrix
  tasks explicit. They are separate from acceptance of the gameplay bugfix;
  merging must not be described as a completed commercial-quality release.
- [ ] Fetch again before integration, reconcile other agents' changes, and
  rerun checks affected by conflicts or new changes.
- [ ] Integrate the validated repair branch into `develop`, then merge the
  latest integrated `develop` into `main`, preserving existing gameplay work.
- [ ] Follow branch protection and PR requirements. Do not force-push or bypass
  protections. If a PR is required, create it with evidence and report any
  remaining merge requirement.
- [ ] Check the final merged result and push the authorized branches normally.
- [ ] Confirm remote `main` contains both the game project and the fixes.

Use a final verification record for each task:

| Task | Root cause and fix | Commit | Verification performed | Result / remaining limitation |
| --- | --- | --- | --- | --- |
| Boss proof | Pending | Pending | Pending | Pending |
| Objective identity | Pending | Pending | Pending | Pending |
| Encounter placement | Pending | Pending | Pending | Pending |
| Story routes | Pending | Pending | Pending | Pending |
| Runtime localization | Pending | Pending | Pending | Pending |
| Player UI | Pending | Pending | Pending | Pending |
| Assets/build/status | Pending | Pending | Pending | Pending |

Final handoff must identify changed files, checks actually run and their results,
campaign/target-platform verification, unresolved items, fix commits, final
`main` revision, and PR URLs if used. Leave unchecked tasks visible. Do not stop
at a plan or a documentation-only change when assigned to execute this backlog.
