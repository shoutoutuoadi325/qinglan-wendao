# Narrative Runtime and Production Contract

## 1. Authority

The narrative bible defines immutable world rules. Main registries define required scenes and choices; the side-quest registry defines optional stages; lore/bark registries define repeatable presentation. Locale resources hold exact English and Simplified Chinese player copy. English prose production notes are not displayed in-game.

These are authored specifications, not an implemented narrative engine. No current resource is loaded automatically by Unreal. M0/M1 establishes a small importer or equivalent Unreal data-asset workflow before content assembly.

## 2. Required narrative state

Add a versioned `narrative` section to the future profile save: completed scenes, monotonic main chapter flags, side-quest stage IDs, choice decisions, evidence read IDs, ending-preview state, mutually exclusive confirmed ending, delivered-letter transitions, and one-time reward transaction IDs.

Keep dialogue progress separate from gameplay completion. Skipping text still applies required journal/objective updates, but cannot award an unearned boss clear or silently select a consequential choice. Main chapter completion implies all mandatory gameplay and scene requirements, not merely that a player opened a dialogue window.

## 3. Scene execution

1. Evaluate chapter/scene prerequisites and choose the appropriate safe-state room variant.
2. Move to a safe presentation anchor or defer playback until combat ends. Never freeze the player while enemies remain active.
3. Snapshot narrative state before entering a scene. Resolve already-completed scenes to their brief revisit branch.
4. Deliver ordered text cues with visible speaker/context. Preserve conditional responses exactly as authored.
5. Present dialogue choices in stable order; show missing requirements for disabled practical choices.
6. Apply the selected branch's flags once, update objectives/journal, and commit at the scene's specified safe boundary.
7. Resume gameplay using the next scene/objective, including on reload.

Transient camera movement, subtitle pagination, and line index do not create a permanent choice. An interrupted conversation restarts at a coherent exchange; committed rewards and main flags are never rolled back by replaying its text.

## 4. Choice types

An expressive dialogue option changes the response and may set a characterization flag. It cannot create an unseen combat advantage or secretly mark a good/bad score. A practical option changes an explicit quest action, allocation, or preparation. The journal shows its consequences and whether reconciliation is possible.

Final ending selection is a preview followed by a distinct confirmation. Preview creates no ending flag. Return exits to the pre-trial safe hub with story state preserved; re-entering the arena after shell victory uses the postcombat choice checkpoint, not a forced repeat fight. Confirmation performs one atomic transaction with exactly one of `ending_open`, `ending_seal`, or `ending_shared`.

`ending_shared` additionally requires all three completed agreements: `sq_spillway_verified`, `sq_allocation_agreed`, and `sq_handover_accepted`. It does not require every collectible, every side quest, a particular tone of dialogue, or a hidden virtue score. If the player chooses a different valid ending after completing these agreements, honor that decision.

## 5. Ending and replay invariants

Shen's binding is released before the final implementation decision. His present wishes are recorded before release, and no ending forcibly retains his living mind. The final Core bridge is a finite intervention; any ongoing obligation is shown explicitly in the preview.

All endings retain ordinary profile progression and access to training reconstructions. Scenes set an epilogue time label before showing a postdecision letter. A dated prerecorded Yun instruction cannot react to a future choice; only an explicitly later letter can do that.

Once confirmed, an ending is persistent for that profile. The warning explains that changing it requires a new campaign/profile, not deletion of existing progress. Do not offer an ambiguous Continue button that reruns confirmation and grants duplicate rewards.

## 6. Puzzles and clue fairness

Interactive story tasks use existing inspection, object activation, traversal, and combat interfaces. Every required operation is supported by visible environmental cues and a journal summary. Avoid invisible pixel hunts, one-attempt irreversible puzzles, real-time reading under attack, and requirements to equip a missable technique.

Optional safe-state revisits unlock from the hub after their main prerequisites, without forcing an entire random expedition for every clue. Wrong operation yields an authored explanation and reset; it never destroys the only quest item. Accessibility assists can highlight the relevant prop or replay the clue while preserving the decision itself.

## 7. Text, animation, and VFX production

Every subtitle is a localized `FText`. Use explicit text keys for inspection titles, body text, dialogue replies, objectives, journal entries, letters, ending previews, and failure messages. Capture bilingual layout at increased text scale and keep both locales on the same authored revision.

Use modest authored camera rails and animation poses. Do not silently require voice actors, facial animation, a new village map, or expensive storm fluid simulation. Boss rhetoric plays during safe phase gaps; gameplay danger indicators always take precedence. Low-flash mode preserves the dramatic information through silhouette, sound captions, and timing.

## 8. Narrative acceptance

- All registry text references resolve in both locales; no placeholder dialogue.
- Every mandatory question is answered on a direct main-only run.
- Every side-quest branch has a closure and idempotent reward; required agreements remain obtainable until confirmation.
- The final shared option cannot be confirmed with one missing prerequisite, even after save/load.
- Final ending flags are mutually exclusive and preview/return cannot commit them.
- Skip/reload before and after every choice preserves coherent objectives and transactions.
- Record dates and speaker knowledge agree with the timeline.
- English/Chinese convey the same promises, losses, quantities, and consent.
- The final shell's defeat is never described as killing Shen if he is released afterward.
- END_SEAL's replay is explicitly reconstructed rather than physically reopening its sealed intake.

Use the branch matrix and registry validation as preparation; actual narrative acceptance requires a packaged playable campaign on both platforms.

## 9. Accepted agreements versus revisions

SQ02 and SQ03 maintain separate pending and effective version slots. Selecting a new proposal replaces sibling proposal flags and invalidates any earlier reply for that proposed version. The old effective agreement remains valid until a matching delivered reply, verification, and explicit confirmation atomically replace it. Ending preview/commit reads the effective snapshot, never the latest selected draft. UI identifies both versions with complete bilingual copy. Revisions use the authored same stage exchanges and safe time transitions; they never pay a second reward or silently discard earlier consent.
