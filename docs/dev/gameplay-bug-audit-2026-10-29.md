# Gameplay bug audit — 2026-10-29

This repository-wide source review prioritizes crashes, blocked progression,
lost progress, incorrect combat, and controls that stop responding. The fixes
below cover 65 related defect groups implemented in the working tree. The
latest native baseline passes the editor build, all six regeneration
commandlets, all 163 regular
registrations, both standalone campaign journeys, and Mac Shipping packaging.
The two additional UI repairs pass rebuilt Metal checks in both languages at
1280×720 and 853×554. The generated Shipping client also boots and renders its
cooked hub. The latest Shipping archive also renders readable trial warnings
at Low and High, including low-flash. The rebuilt safe-sector comparison also
confirms a visible green angular escape cue. These results cover groups 01–65.
These results do not establish that all game bugs are eliminated.

## Scope and evidence

Reviewed the runtime paths across Core, Combat, Abilities, AI, Progression,
Persistence, Narrative, UI, Platform, and VFX, together with the relevant story
data, localization, content-generation contracts, and automation fixtures.
Parallel reviews cross-checked the integration between these systems. The
current native editor builds on this host, all six content-regeneration
commandlets pass, and the current native gameplay suites pass.

Validation performed:

- `python3 tools/validate_repository.py`: passed, including story and bilingual
  localization checks. Both main locales contain 299 keys.
- `python3 -m unittest discover -s tools/tests`: 61 discovered, 58 passed,
  3 skipped. These are tooling tests, not runtime gameplay tests.
- `git diff --check`: passed.
- Source test discovery checks unique automation names and includes the new
  registrations in the acceptance runner. The exact new names are listed below.
- The first native build attempt stopped before compilation because the engine
  was absent. The owner then authorized installation. Apple Metal Toolchain
  27A266a is now installed and its compiler executes successfully. Official UE
  5.8.3 source and Mac dependencies are installed at the pinned revision.
  GitDependencies completed with exit 0; UBT build and publish completed with
  zero build warnings/errors. ShaderCompileWorker completed all 215 actions
  successfully. The first project editor build passed UHT and compiled the
  engine and most project units, but failed in four source units. The comment
  continuation, pinned live-text API, double world-clock assertion, and dynamic
  multicast subscription-query errors have been corrected. Two missing runtime
  symbol exports found by the editor-module linker have also been corrected.
  The current editor build passed all 324 incremental actions, including all
  changed gameplay and regression sources. Evidence:
  `.local/acceptance/20261029T133319.492163Z/01-build-editor.log`.
- The next baseline, `.local/acceptance/20261029T134749.591963Z`, passed the
  editor build and all six resource commandlets after restoring the pinned
  embedded Python standard library. Its regular suite executed all 162 named
  registrations: 150 reported Success (including the two documented campaign
  opt-outs), 12 failed, and none remained unrun/in progress. The runner rejected
  the baseline and did not advance to the standalone journeys or packaging.
  That failed attempt is retained as evidence of the repaired defects and
  invalid fixture prerequisites.
- Run `.local/acceptance/20261029T142024.368868Z` passed 160/162 regular
  registrations. Its remaining failures were a second invalid LocalPlayer
  Outer in the paused-frontend fixture and an input fixture demanding a scene
  choice before the active enemy wave had reached the safe dialogue boundary.
- Run `.local/acceptance/20261029T143058.323973Z` passed the editor build, all
  six regeneration commandlets, and all 162 regular registrations (127 Success,
  35 Success with Warnings, zero failed/not-run/in-progress). The two regular
  campaign registrations retain their documented opt-outs; both then ran in
  separate opted-in editor processes: **30/30 main scenes** and **24/24 side
  stages**, with zero blocked. Its packaging was superseded after the UI
  source repairs in groups 62–63.
  Remaining warnings include NullRHI presentation/viewport limits and
  deliberately rejected invalid data/save operations; they are not a
  zero-warning claim.
- Run `.local/acceptance/20261029T151005.043778Z` rebuilt groups 62–63 and
  passed regeneration, all 162 regular registrations, and both campaign
  journeys. Packaging failed at startup because a concurrent editor-owned
  AutomationTool held the engine singleton; it did not reach cook.
- Run `.local/acceptance/20261030T013435.815037Z` passed **all six
  pipeline steps**, including Mac arm64 Shipping build/cook/stage/archive.
  The regular suite again reports 127 Success and 35 Success with Warnings;
  both opted-in journeys report 30/30 scenes and 24/24 stages, zero blocked.
  `package-artifacts.json` records the client and all five cooked containers
  with SHA-256 identities. Its client was subsequently launched from this
  exact archive into a disposable sandbox-contained UserDir; it created a
  QA profile, rendered the hub, and exited normally after its screenshot.

- Run `.local/acceptance/20261030T021616.859732Z` passes all six
  pipeline steps after group 64, including in-place regeneration of the old
  cue assets and the new ground material. The regular suite reports **163/163**:
  128 Success, 35 Success with Warnings, zero failed/not-run/in-progress.
  Both opted-in journeys pass 30/30 scenes and 24/24 stages, zero blocked.
  The exact archived Shipping client renders the repaired ground warnings at
  Low + low-flash, High + low-flash, and High with low-flash disabled, exiting
  normally after each capture. Its client SHA-256 is
  `faa58f333b2507fc1718117845bb2fc2573e26a6597d8d8b48d610a7b94196fd`.
  Package identity is recorded in this run's `package-artifacts.json`.
  The preceding `.local/acceptance/20261030T021209.917147Z` stopped at one
  newly added, invalid shading-model assertion: pinned UE returns DefaultLit
  for every deferred decal. The corrected assertion checks domain-compatible
  shading plus connected emissive/opacity inputs; no original tests were removed.

- Latest run `.local/acceptance/20261030T023816.180170Z` passes all six
  steps with group 65 included: 163/163 regular registrations (128 Success,
  35 Success with Warnings), both opted-in journeys (30/30 scenes, 24/24
  stages), and Mac arm64 Shipping build/cook/stage/archive. Zero tests failed
  or remained unrun. The geometry regression retains all original angular
  membership assertions and also checks camera-facing winding. A rebuilt
  Metal comparison of the same production cue calls changes the formerly
  invisible sector into a clearly visible green wedge over the orange fill.
  Final client SHA-256:
  `6f3b9eefae959ed464785c92d86f4be8fcd9334e98d426c148b33603c2746fb4`.

## Implemented fixes

Each row is a related defect group; some rows cover multiple failure modes of
the same mechanic. P1 denotes crashes, progression/save blockers, or materially
incorrect gameplay; P2 denotes presentation or usability failures.

| ID | Priority | Trigger and player impact | Change |
| --- | --- | --- | --- |
| 01 | P1 | Mist Step dereferences an unset actor owner; its trail also misses targets between frames or wets targets behind a wall. | Use the effect owner, sweep the actual movement segment including the final frame, and check occlusion. |
| 02 | P1 | A freshly created healing effect expires after three seconds, before all four Verdant Pulse ticks; reused effects behave differently. A hitch crossing the safety deadline can discard pending healing and projectile contact. | Give fresh and pooled effects the same safety lifetime and resolve the remaining active interval before expiry, bounded by the deadline and projectile range. |
| 03 | P1 | Orbiting Blades hits one enemy multiple times through separate collision components and consumes its two-target budget; it can hit through walls. | Deduplicate actors per pulse and require line of sight. |
| 04 | P2 | A slow frame lets player projectiles travel and hit beyond their authored range. | Clamp each movement segment to the remaining range. |
| 05 | P1 | A projectile contacting a dodging target retries under new hit ordinals until immunity ends. | Consume each target contact once, including dodged contacts. |
| 06 | P1 | Enemy projectiles pass through geometry; a pooled projectile stopped by geometry can fail to move on relaunch. | Block static/dynamic geometry, reset flight state, restore the movement component, and activate collision after placement. Simultaneous bolts ignore each other. |
| 07 | P1 | Defeated enemies are removed before delayed collection, including the final enemy; reused enemies can remain without collision. | Track retired enemies independently of encounter activity, reset abandoned actors, and restore collision during materialization. |
| 08 | P1 | Purchased cultivation ranks and breakthroughs do not change actual combat health, energy, or technique power. | Fold cultivation bonuses into player attributes on spawn and profile changes. Fresh pawns begin full at their derived maxima. |
| 09 | P1 | Draining Edge, Rooted Stance, and Second Wind do not apply their energy, damage-reduction, or health benefits. | Apply the modifiers in authoritative damage, sword-energy, and attribute paths; notify attribute refresh when blessings change. |
| 10 | P1 | Restoring an old room snapshot can discard newly acquired bonuses or make later equipment changes subtract the wrong values. | Restore checkpoint vitals while rebuilding maxima/power from the loaded profile and run; rebase complete attribute snapshots. |
| 11 | P2 | A frame crossing the regeneration delay grants regeneration for the whole frame. | Count only the portion after the delay expires. |
| 12 | P2 | Burn expiry can suppress its last tick; status display duration ignores bonuses, and dead actors can receive statuses. | Let burn tick deadlines own expiry, query authoritative remaining durations, and reject status application to dead actors. |
| 13 | P2 | Two equipped exhausted talismans cannot be removed one at a time; exhausted entries disappear from the menu. | Keep equipped exhausted items visible and allow retaining an exhausted item while removing another. |
| 14 | P1 | Garbage collection can reclaim transient enemy data still queued for staggered spawning. | Retain queued wave data in a reflected property until a spawned enemy owns it. |
| 15 | P1 | Roots and storm strikes warn around the caster instead of the recorded damage centers; the Ashen shockwave omits its safe gap. | Present warnings using the attack's recorded centers and actual radius, and render the recorded safe arc. |
| 16 | P1 | A save failure during defeat return leaves all gameplay input blocked with no retry. | Keep settlement/travel retryable through Interact and show a bilingual recovery message without duplicate banking. |
| 17 | P1 | After completing the Golden Core trial, returning to the hub for optional agreements prevents re-entering the ending platforms. | Permit story return when Golden Core and completed trial proof agree; keep actual trial replay gated. |
| 18 | P1 | A slow frame creates and fully ages a new lightning warning before the player sees it. | Age existing warnings before scheduling new ones. |
| 19 | P1 | A residual lightning strike kills the player after a victory has been queued, but the same frame still awards trial success. | Recheck life immediately after hazards in storm and required-kill/victory phases. |
| 20 | P1 | Older hub maps synthesize story stations but omit the inspection documents required for later chapters. | Record synthesized station positions for the document-spawning pass. |
| 21 | P1 | A door can use another room's entrance as a fallback while advancing the logical room index. | Require the requested room tag; allow the generic entrance only for room zero. |
| 22 | P2 | A cleared full realm still points guidance at the tutorial return station. | Resolve the current realm's actual final return station. |
| 23 | P1 | Reloading an unfinished fight rewinds enemies and vitals but retains consumed talismans. | Persist entrance talisman charges alongside replayed fights; live play and completed rooms retain actual consumption. |
| 24 | P1 | Saving in a cleared/safe room loses post-combat healing on reload; saving before world resume can sample the previous profile's pawn. | Save current safe/cleared-room vitals and preserve saved vitals while resume is pending. |
| 25 | P1 | Re-entering a completed room returns before committing its checkpoint. | Keep completed-room re-entry on the durable boundary path without respawning enemies or duplicating rewards. |
| 26 | P1 | Several transaction APIs can grow the journal beyond the save validator's limit, making later saves invalid. | Check one shared journal bound before every journaled resource/progression mutation. Existing entries remain idempotent. |
| 27 | P1 | Disk flush failure is ignored and an undurable save can report success. | Check durable flush, preserve the primary on failure, and expose flush fault injection for recovery tests. |
| 28 | P1 | An older build can rotate over a newer-format backup despite rejecting a newer primary. | Reject unsupported newer schemas/containers across primary and backups before load, save, or profile replacement. |
| 29 | P1 | Changing an answer after loading an interrupted scene leaves both mutually exclusive branch flags active. | Remove the replaced sibling answer's flags with its decision entry, retaining shared selected flags and atomic-action semantics. |
| 30 | P1 | Switching/reloading a profile retains pending ending confirmation, deferred dialogue, or ambient dialogue state. | Reset transient narrative state before importing the selected profile. |
| 31 | P1 | Garbage collection removes weakly held UI event relays, silently stopping dialogue or HUD updates. | Give each widget a reflected strong relay reference and bind/unbind with its lifecycle. |
| 32 | P2 | Changing quality presets resets low-flash, frame cap, VSync, and dynamic-resolution preferences. | Preserve these explicit player preferences when applying a new preset. |
| 33 | P2 | Selecting equipment with full slots reports success although nothing changed. | Report failure when the equipped list remains unchanged. |
| 34 | P2 | Same-state map reloads leave background music silent because the subsystem outlives its audio component. | Recreate invalid, stopped, or previous-world music components even when the requested state is unchanged. |
| 35 | P1 | Sword windup/active/recovery drops frame overshoot and expires a still-valid combo press before processing its boundary. | Advance through each phase boundary with remaining frame time, sweep crossed active windows, and check buffered input at the boundary. |
| 36 | P1 | Enemy projectiles overshoot range or retire before sweeping their final segment depending on movement tick order. | Drive one movement step bounded by remaining range and lifetime from the projectile actor. |
| 37 | P1 | Aim becomes invalid during anticipation but release still spends energy and commits cooldown. Mist Step also accepts disabled movement or vertical-only aim despite requiring a horizontal dodge. | Validate the actual release aim and dash movement readiness before committing costs and execute that exact aim. Failed dashes preserve normal walk speed. |
| 38 | P1 | Mist Step bypasses Root, including Root applied during anticipation. | Check Root at cast start and release while retaining stationary technique use. |
| 39 | P1 | Talisman slots reset on travel/load, retain another profile’s loadout, or remain empty after cold-start selection. | Store ordered slots in the profile/save, default legacy saves once, retain intentional empty loadouts, and apply expedition equipment locking. |
| 40 | P1 | Failed new-profile writes discard the live profile/run; an old file can make the UI report false success. | Write the candidate before replacing session state, expose a bool creation API, and use its actual result in the UI. |
| 41 | P1 | Exit/retreat APIs report success when banking rejects settlement; exit UI hides the real error. | Return the actual settled-run state and preserve the banking failure message. |
| 42 | P1 | Pause return leaves a live expedition in the hub, locking equipment and realm entry and allowing checkpoint corruption. | Save the checkpoint, deactivate the profile session, clear only in-memory run state, and return to profile selection. Explain room replay and reward retention; selecting the slot resumes without retreat penalties. |
| 43 | P1 | A wide rectangular safe-lane marker labels positions outside the angular exemption as safe. | Draw a steady triangle-fan sector bounded by the actual safe angle and radius, without obscuring particles. Uses built-in GeometryCore/GeometryFramework runtime modules. |
| 44 | P1 | Setting decal absolute flags after assigning world position reinterprets relative coordinates, offsetting warnings from non-origin casters. | Set world-space semantics before assigning the frozen ground transform. |
| 45 | P1 | VFX consumes a new warning’s lifetime using frame time before it was created, erasing it before its first visible frame. | Age warnings and safe sectors from creation game time, preserving pause semantics. |
| 46 | P2 | Cached FText labels remain in the old language after a live language switch. | Retain registered localization identity and finish resource refresh before rebuilding the menu. |
| 47 | P2 | Disabled ending choices omit the missing-agreement information explaining how to unlock them. | Render the authored localized requirements on the disabled choice itself. |
| 48 | P2 | Blessing, shop, and equipment choices omit effect descriptions and relic tradeoffs. | Show catalog descriptions with names/costs and add missing bilingual descriptions. |
| 49 | P2 | Mandatory blessing drafts display a close button and dismissal instructions that cannot work. | Hide the unavailable close action and display the selection-required hint. |
| 50 | P1 | A side quest's completed stage can reach the primary or backups before its reward technique; recovery then permanently loses the reward. | Defer nested scene saves until completion and technique rewards agree, and repair historically incomplete rewards only from durable full-quest proof. |
| 51 | P1 | Hidden portal or pooled enemies keep their movement component active and consume an old launch or fall through the floor. | Clear accumulated movement and disable it through portal, pool, and death; restore walking on materialization. |
| 52 | P1 | Staggered, defeated, or recycled enemies retain canceled telegraphs, montages, or queued lunges. | Cancel pending motion and presentation at the owning lifecycle boundary, including controller-owned shots when pooling. |
| 53 | P1 | A player sidestepping a charge still takes damage anywhere within its attack-start radius; a fast charge can miss a crossed contact. | Sweep the enemy's actual capsule path and consume its committed hit once. |
| 54 | P1 | Selecting another profile from an active menu skips saving the old session and retains its map, enemies, pawn, and proof caches. | Save the outgoing slot first; retain the menu on failure; queue a fresh world on success without saving the old pawn into the selected slot. |
| 55 | P1 | Pause and other modal menus disable player actions while enemies, Burn, cooldowns, and timers continue. | Acquire an actual world pause, retain paused menu input and ownership, flush held keys, and keep cold-start selection and travel responsive. |
| 56 | P1 | Ground warnings use capsule height, and the mesh fallback receives a deferred-decal material that cannot render on a mesh. Low-quality play can lose a readable danger footprint. | Project the frozen X/Y onto ground geometry, keep decal depth independent of duration, and give the mesh fallback its own compatible surface material. |
| 57 | P1 | Dodge distance changes with frame rate, terminal hitches, held input, friction, and jump apexes; finishing retains momentum, and hit/death can leave the dash active. A failed start spends stamina. | Use bounded planar root motion through CharacterMovement, correct the completed movement clock, retain floor/step/gravity collision, remove force and planar momentum on completion/cancel, expire canceled immunity, and commit stamina only after motion starts. Order Mist Step's final sample after movement and release its pooled tick prerequisite. |
| 58 | P1 | Gamepad D-pad navigation also fires technique actions treated as digit shortcuts, accidentally selecting a profile, accepting a blessing, or opening another menu. | Route physical digits and D-pad modal navigation separately; retain all four gameplay technique mappings. |
| 59 | P1 | Gamepad B cycles a combat target and then falls through to arena retreat, abandoning the trial; a mandatory choice can also fall through to retreat. | Limit B's Back route to actual modals or record dismissal, and prevent pending choices from reaching the arena-exit fallback. |
| 60 | P2 | A gamepad A press and release collected in one frame loses menu confirmation because the key is no longer held. | Confirm native overlays from the physical press event once, keeping the shared Enhanced dodge callback in gameplay and preserving Space's distinct behavior. |
| 61 | P2 | Synthetic flush releases undo the pinned engine's held-key ignore flag, allowing long-held Escape or attack keys to re-arm on repeats. Reverse release/press pairs and unprocessed holds also evade sampled-state checks. | Configure a project Enhanced input subclass that tracks the last physical digital event, retains suppression across synthetic releases, clears queued held presses, and requires a real release while preserving completed taps and analog samples. |
| 62 | P2 | Actual Metal frames show skill text overlapping the health/energy bars, with narrow talisman labels overflowing their short row. Fixed 1080p coordinates also lose HUD content at other viewport shapes. | Share responsive bottom anchors between generated and existing HUD assets, allocate separate vitals/skill/talisman sections, and give wrapped labels real font sizes and sufficient height. Verified on rebuilt Metal at 1280×720 and 853×554, and in the cooked Shipping hub. |
| 63 | P2 | Live Chinese menus split short button labels, including Back, into one-character lines despite ample row width. | Fill the button's available width while centering the text; auto-wrap then measures the full content area. Verified after live language changes in both rebuilt Metal viewport sizes; Chinese Back and settings labels remain on one line. |
| 64 | P2 | Shipping Low + low-flash trial warnings become solid white discs, washing out floor detail and skill labels; the tall decal also paints the hero’s legs and waist white. Existing material assets are skipped on regeneration, so changing the generator cannot update their graphs. | Rebuild the four code-owned cue graphs in place, clear legacy inline-constant bypass flags, retain a thin ring outline, and use a separate alpha-blended ground-fill material. Limit decal half-depth to 8 cm and give the safe sector stronger opacity and explicit overlay priority. Passed the 163-case native run and Low/High Shipping render comparisons. |
| 65 | P1 | A rendered danger circle has no visible green safe sector even though its triangles and angular bounds exist. Its XY triangle order faces away from the overhead camera, so the single-sided material culls the escape cue. | Match the pinned engine disc generator's clockwise ground-face order while keeping the same vertices, radius and angular exemption. Extend the real geometry regression to require camera-facing winding. Passed the complete native pipeline and a rebuilt Metal before/after comparison with a visible green sector. |

## Regression coverage

The new native tests use real subsystem/actor paths, disposable save directories,
fault injection, collision queries, or garbage collection where appropriate.
The native attempts are recorded above; failures exposed invalid fixture
prerequisites as well as production dodge-cancellation and held-input edge
defects. The latest regular suite and isolated journeys pass with the original
behavior assertions retained. Source discovery alone remains registration
validation.

63 new native tests; 163 unique source registrations in the full current suite.

- `Qinglan.AI.ChargeRequiresSweptCapsuleContact`
- `Qinglan.AI.DeathAndReuseClearPendingPresentation`
- `Qinglan.AI.GroundWarningsMatchDamageShapes`
- `Qinglan.AI.PortalAndPoolFreezeCharacterMovement`
- `Qinglan.AI.QueuedEnemyDataSurvivesGC`
- `Qinglan.Combat.CultivationBlessingsAndCheckpointAttributes`
- `Qinglan.Combat.DepletedTalismanSlotsCanBeRemoved`
- `Qinglan.Combat.DodgeCancelsOnDamageDeathAndTeardown`
- `Qinglan.Combat.DodgeDistanceAcrossFrameRatesAndTerminalHitch`
- `Qinglan.Combat.DodgePauseAndFinalMistStepSample`
- `Qinglan.Combat.DodgeRetainsWallsStepsAndGravity`
- `Qinglan.Combat.EncounterCollectsAndRearmsLastEnemy`
- `Qinglan.Combat.EnemyProjectileMovesOnlyItsRemainingRange`
- `Qinglan.Combat.EnemyProjectileWallsAndReuse`
- `Qinglan.Combat.FreshHealCompletesAllFourTicks`
- `Qinglan.Combat.MistStepTrailOwnerAndOcclusion`
- `Qinglan.Combat.OrbitPulseCountsUniqueVisibleVictims`
- `Qinglan.Combat.ProjectileRangeAndDodgedContact`
- `Qinglan.Combat.StatusDurationAndRegenBoundaries`
- `Qinglan.Combat.SwordBufferUsesRecoveryBoundary`
- `Qinglan.Combat.SwordCarriesTimeAcrossPhaseBoundaries`
- `Qinglan.Combat.TechniquesRevalidateTargetAndRootBeforeRelease`
- `Qinglan.Graphics.PresetPreservesAccessibilityAndFramePacing`
- `Qinglan.Input.DPadNavigatesPausedMenusWithoutSelecting`
- `Qinglan.Input.DPadRetainsAllFourGameplayTechniqueSlots`
- `Qinglan.Input.FlushDropsPendingHoldsAndPreservesTapsAndAxes`
- `Qinglan.Input.GamepadAndKeyboardDodgeRetainGameplay`
- `Qinglan.Input.GamepadBCyclesTargetsWithoutAbandoningTrial`
- `Qinglan.Input.HeldKeysRequireReleaseAfterModalFlush`
- `Qinglan.Input.SubframeGamepadAConfirmsExactlyOnce`
- `Qinglan.Localization.CachedWidgetTextChangesLanguage`
- `Qinglan.Narrative.CompletedRealmGuidesToItsReturnStation`
- `Qinglan.Narrative.InterruptedChoiceReplacesBranchFlags`
- `Qinglan.Narrative.LegacyHubSynthesizesRequiredDocuments`
- `Qinglan.Narrative.ReloadDiscardsPendingEndingConfirmation`
- `Qinglan.Persistence.ActiveMenuProfileCreationFailureIsRetryable`
- `Qinglan.Persistence.ActiveMenuProfileSwitchSavesAndRebuildsWorld`
- `Qinglan.Persistence.CheckpointConsumableRewind`
- `Qinglan.Persistence.CompletedRoomVitals`
- `Qinglan.Persistence.DefeatedPlayerCanRetryFailedReturn`
- `Qinglan.Persistence.FutureBackupProtection`
- `Qinglan.Persistence.PauseSuspendsWithoutStrandingRun`
- `Qinglan.Persistence.ProfileCreationFailurePreservesSession`
- `Qinglan.Persistence.ProfileJournalCapacity`
- `Qinglan.Persistence.QuestCompletionSavesItsTechniqueReward`
- `Qinglan.Persistence.SaveFlushFailure`
- `Qinglan.Persistence.TalismanLegacyTaggedSaveRoundTrip`
- `Qinglan.Persistence.TalismanLoadoutLegacyDefaultsAndValidation`
- `Qinglan.Persistence.TalismanLoadoutSurvivesTravelAndProfileSwitch`
- `Qinglan.Progression.DoorCannotTeleportToAnotherRoomsEntrance`
- `Qinglan.Progression.ExitRequestsReportBankingFailure`
- `Qinglan.Progression.GoldenCoreCanReturnToEndingPlatforms`
- `Qinglan.Trial.LethalStrikeCannotAwardVictory`
- `Qinglan.Trial.WarningSurvivesSlowFrame`
- `Qinglan.UI.DialogueSubscriptionsSurviveGarbageCollection`
- `Qinglan.UI.LockedEndingChoicesExplainMissingAgreements`
- `Qinglan.UI.OfferEffectsAndTradeoffsAreVisible`
- `Qinglan.UI.PauseExplainsCheckpointSuspension`
- `Qinglan.UI.PauseStopsDamageAndTimersWhileMenuKeysStillWork`
- `Qinglan.UI.PausedFrontendStillAcceptsProfileSelection`
- `Qinglan.VFX.AuthoredCueMaterialsHaveConnectedGraphs`
- `Qinglan.VFX.GroundWarningFallbackUsesSurfaceMaterialAndFloor`
- `Qinglan.VFX.WarningsSurviveCreationFrame`

## Additional verification-pipeline fix

An arbitrary older Windows editor cache could execute stale gameplay, test, or
importer DLLs while unchanged test names still produced a green report. Cache
builds now seal source/build-input and archive/artifact fingerprints; restore
checks them before extraction, and packaging rechecks current inputs, module
BuildIds, and the installed files before imports, tests, or cook. Matching
caches continue to skip rebuilding. Legacy, edited, or damaged caches fail
with an explicit rebuild instruction. Eleven fixture/CLI tooling tests were
added (one PowerShell check is skipped on this Mac). Windows native execution
of this pipeline remains outstanding.

The native acceptance wrapper also selected its editor path before the first
build. On a fresh Mac source checkout it retained a nonexistent standalone
commandlet path after UBT created the app bundle, preventing the suite from
starting. The runner now resolves the host's built editor immediately before
each test process, retains explicit overrides, checks executable availability,
and records the selected path. A stub-process regression builds the previously
absent bundle and verifies all three test processes select it.

Synchronous world-tick fixtures now advance the engine frame counter after
each frame and restore it after EndPlay. UWorld::Tick alone does not advance
the scheduler frame, so the old menu-pause fixture could stop updating
components after its first frame. The existing standing-animation fixture
already advanced frames but leaked them into the enclosing editor frame.
The corrected menu, standing, and new dodge fixtures follow the pinned
engine test-world wrapper. They were included in the first complete native run.

The new input-routing fixture evaluates the configured project Enhanced input
class, production mapping context, and both Enhanced and native bindings with a
local player and actual world pause. It checks that navigation cannot transact,
gameplay B cannot abandon a trial, same-frame confirmation acts once, and
gameplay dodge/technique bindings remain usable. Held-key cases include repeated
flushes, release/re-press ordering, pending input before a flush, completed taps,
and paired movement/look axes. All seven now pass in the complete native
suite. The mandatory-choice case first verifies real encounter deferral, then
defeats the wave through portal materialization and authoritative combat damage;
completion resumes playback while the trial remains active between waves. This
preserves its B-routing and no-retreat assertions without bypassing the story's
safe-boundary rule. This does not establish that a physical controller was
exercised.

## Reproduction and validation limits

With a supported licensed Unreal installation, run the native-host pipeline:

```sh
QINGLAN_ENGINE_ROOT=/path/to/UnrealEngine bash tools/build/run-acceptance.sh
```

This builds the editor, regenerates content, runs the regular automation suite
and both campaign journeys, then packages the game. Regeneration is required
for the new defeat, suspension, and item-description keys to reach `ST_Game`; JSON edits alone
do not update the shipped string-table asset.

Then verify the affected paths interactively in both languages at increased
text scale, especially defeat retry, equipment menus, full-realm return, and
hub-to-arena ending continuation. Ground warnings and safe arcs still need
complete device coverage, especially Windows/DX12. Check same-state map reloads
with audible output. The captured hub and menu checks do not establish a
complete interactive playthrough, physical-controller operation, audio listening,
or sustained combat/trial performance.

The first current Metal game-mode check used an isolated UserDir and real
keyboard events. Enter durably created the QA slot; Escape, Down and Enter
opened Pause and Settings; language switching refreshed the menu and HUD.
The engine log confirmed low-flash remained enabled after switching from
Balanced to Low. Captures in `docs/dev/evidence/rendered/` include
`gameplay-audit-2026-10-29-hud-before.jpg` and
`gameplay-audit-2026-10-29-settings-zh-before.jpg`. They record the defects in
groups 62–63 before repair. The fresh October 30 captures record their corrected
behavior on the current native baseline:

- `gameplay-audit-2026-10-30-hud-zh-after.jpg` and
  `gameplay-audit-2026-10-30-hud-en-after.jpg`: separated vitals, wrapped
  techniques and talisman labels at 1280×720.
- `gameplay-audit-2026-10-30-settings-zh-after.jpg` and
  `gameplay-audit-2026-10-30-settings-en-after.jpg`: actual keyboard
  pause/settings navigation, live language switching, and full-width labels.
- `gameplay-audit-2026-10-30-hud-en-narrow-after.jpg` and
  `gameplay-audit-2026-10-30-settings-zh-narrow-after.jpg`: 853×554 repeats
  the original failing viewport shape; labels wrap within their own columns
  and Chinese menu labels retain full row width.
- `gameplay-audit-2026-10-30-shipping-hub.png`: the exact archived Shipping
  client, cooked assets, 1280×720 Metal, isolated QA slot creation.

Group 64 evidence uses the latest Shipping archive:

- `gameplay-audit-2026-10-30-trial-low-before.png`: the preceding archive's
  solid white disc also whitens the hero's lower body and reduces HUD contrast.
- `gameplay-audit-2026-10-30-trial-low-after.png` and
  `gameplay-audit-2026-10-30-trial-high-after.png`: Low/High with low-flash;
  the complete outline remains visible, the interior retains terrain detail,
  and the hero and HUD remain readable.
- `gameplay-audit-2026-10-30-trial-high-standard-after.png`: High with
  low-flash disabled, with the same readability outcome.

These trial frames use the existing `-QinglanTrial=90` QA admission bypass;
they verify rendered warning presentation, not ordinary progression admission
or successful trial completion. The previously captured Shipping hub remains
provenance for the preceding `20261030T013435.815037Z` archive.

Group 65 uses `gameplay-audit-2026-10-30-safe-sector-before.png` and
`gameplay-audit-2026-10-30-safe-sector-after.png`. A standalone native Editor
game process calls the production `PlayTelegraph` and `PlaySafeLane` methods
from an isolated Python preview: a 350 cm fire disc, a 30-degree-half-width
green sector, centered 500 cm ahead of the pawn, yaw 270 degrees. The preview
component has a long display lifetime and does not execute an AI attack.
The screenshots establish actual rasterized winding, shader visibility and
safe/danger overlay readability; gameplay membership and warning lifetimes
are covered by the real registered native fixtures. See
`evidence/vfx-warning-render-verification-2026-10-30.md` for provenance.

The overnight audio queue growth aligns with Mac clamshell sleep and dark
wakes. A source and system-power-log review found no repeated menu music
submission and identified incomplete device recovery in the pinned CoreAudio
backend. See `evidence/audio-sleep-recovery-2026-10-30.md`. Timeout logging
stopped after full wake; this does not prove that audible playback recovered.

The journal limit remains 8192 entries: the fix prevents an invalid save but
does not introduce journal compaction. Long-lived-profile compaction needs a
separate versioned design that preserves transaction replay safety.
