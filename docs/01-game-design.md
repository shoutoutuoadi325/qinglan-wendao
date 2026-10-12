# Game Design Document

## 1. World and narrative

Qinglan Sanctuary's apprentice is asked to repair a damaged intake and release anchor. The apparent repair gradually exposes an emergency closed cycle that has bound Warden Shen Heng for thirteen years despite his original thirty-day consent. Keeper Lan's undocumented relief bypass helped downstream residents but strained the intake; Elder Yun signed the original design and is now downriver arranging warnings and evacuation. Their responsibility is specific and different, not collapsed into a single secret villain.

The player establishes a Foundation, uncovers the archive's uncompleted handover, and forms a Golden Core to disable the governor's defensive shell. All three final choices release Shen. Open River uses the apprentice's Core as a temporary regulator; Quiet Summit safely retires the mountain intake; Returning Rain reopens through an explicitly agreed shared maintenance network. The last option requires three transparent optional agreements rather than a hidden morality score.

The complete narrative specification begins with the [story bible](story-bible.md) and [chapter/script index](story/README.md). Exact English and Simplified Chinese player text is in paired localization resources and readable bilingual editions linked there. Scene/quest registries define prerequisites, choices, objective changes, completion flags, and safe commits.

### Main cast

| Character | Role and arc | Delivery |
| --- | --- | --- |
| The Apprentice | Adult former porter; useful obedience becomes informed responsibility; no special lineage | Short authored replies, gameplay observations, choices |
| Keeper Lan | Caretaker; fear of repeating the flood, admission of his bypass, accountable participation | Hub scenes, contextual lines, practical tasks |
| Elder Yun | Absent mentor and former surveyor; recorded complicity and concrete evacuation work | Dated letters/archive; explicitly later epilogue correspondence |
| Shen Heng / The Broken Guardian | Bound living governor; asks for a real end to the emergency and his own release | Reader conversations, shell encounter, final consent scene |
| Lan Mei | Deceased spillway guide with ordinary unfinished plans | Dated route notes and a limited recorded fragment; no resurrection |

Eight chapters run from The Dry Bell to What the River Keeps. Main dialogue/reading targets 35–50 minutes; optional stories/records add 25–40 minutes. First-clear campaign targets 4–6 hours, or 6–8 hours with all optional stories. These are pacing targets to test, not reasons to add compulsory grinding. Dialogue is skippable, with equivalent journal/objective state updates; consequential choices are never auto-selected.

## 2. Player experience pillars

1. **Cultivation changes capability.** Each realm unlocks a useful play option, with deterministic breakthrough conditions.
2. **Spells feel physical.** Energy gathers before release; attacks interact visually with air, ground, light, and sound.
3. **Preparation matters.** A limited loadout creates choices between burst, control, sustain, and protection.
4. **Fair failure.** Telegraphs are readable; expedition failure preserves permanent progress.

## 3. Core loops

```mermaid
flowchart LR
    Hub[Prepare at the Sanctuary] --> Realm[Enter a Spiritual Realm]
    Realm --> Rooms[Choose Blessings and Clear Encounters]
    Rooms --> Boss[Challenge the Realm Guardian]
    Boss --> Return[Bank Materials at the Sanctuary]
    Return --> Cultivate[Upgrade Techniques and Cultivate]
    Cultivate --> Trial[Attempt a Breakthrough Trial]
    Trial --> Hub
    Rooms -->|Fall or Retreat| Return
```

Second-to-second loop: position → recognize a telegraph → commit to a sword or technique → dodge or interrupt → recover energy → reposition. Room loop: enter → fight or interact → collect → choose one of three blessings when offered → checkpoint → move on.

An expedition owns temporary blessings, room states, earned-but-unbanked loot, and its random seed. A profile owns cultivation, unlocked techniques, upgraded relics, journal entries, banked currency, and campaign flags. This separation governs saving and failure handling.

## 4. First ten minutes

| Time | Player action | Intended learning |
| --- | --- | --- |
| 0:00–1:00 | Choose a profile; move through a calm courtyard | Camera and movement |
| 1:00–3:00 | Strike training targets; dodge a slow construct | Basic combo, stamina, telegraphs |
| 3:00–5:00 | Cast Jade Sword Arc and use a shrine | Spiritual energy and recovery |
| 5:00–7:00 | Enter the first guided realm room | Lock-on and elemental setup |
| 7:00–9:00 | Clear a mixed encounter and select a blessing | Build choice |
| 9:00–10:00 | Return through a shrine and cultivate | Banking and permanent growth |

Tutorial prompts never block an ongoing enemy attack. The first excursion uses a short fixed route and a safe return shrine; the full expedition unlocks afterward. Tutorial skip becomes available on later profiles and preserves mandatory initial unlocks.

## 5. Realm design

No runtime generation of cliffs, temples, or collision meshes. Use hand-authored connected rooms with seeded encounter and reward variation. This keeps lighting, navigation, traversal, composition, and performance testable.

### Bamboo Hollow

Wet stone paths, bamboo, broken mountain gates, morning mist, shallow water, and jade-colored spiritual veins. Introduces melee, ranged pressure, Wet status, and lightning interaction. Modular room footprints: 35–55 meters across, at least two unobstructed dodge lanes, no damaging bottomless pits.

### Ashen Peak

Charred pines, dark volcanic stone, scorched shrines, warm embers, and wind-driven ash. Introduces Burn, protective Earth barriers, and more aggressive encounter timing. Reuses rock and shrine structures with new materials and layouts; it is not a full second environment pipeline.

### Seven-room layout

| Index | Type | Rules |
| --- | --- | --- |
| 0 | Entrance | No enemies; loadout confirmation; suspend allowed |
| 1 | Combat | One basic archetype; 4–6 enemies total |
| 2 | Combat | Two archetypes; first blessing choice |
| 3 | Shrine or event | Free heal; optional lore or exchange |
| 4 | Elite | Mixed wave; guaranteed material; second blessing |
| 5 | Boss | Arena closes; defeat grants campaign flag |
| 6 | Exit shrine | Banking preview; return confirmation |

An authored optional alcove may be attached to room 2 or 3 but does not change the count. Navigation validates entrance-to-exit paths before release. Doors are explicit interactables; after all living encounter enemies are defeated, unlock the door once. Ambient decorative creatures do not count.

## 6. Encounter selection

Use weighted, versioned encounter lists. Each room defines archetype eligibility, maximum concurrent enemies, spawn markers, reward table, and expected duration. A seeded `FRandomStream` selects the encounter and blessing offers. Use separate streams for encounters, loot, and blessings so adding a cosmetic effect does not change rewards.

At most eight active ordinary enemies, or one boss with at most two adds. Introduce no more than one new behavior per tutorial encounter. Enemy spawns occur behind an obvious portal with a 0.8-second warning and may not appear within 4 meters of the player. The same seed reproduces content selections; identical physics or frame timing across platforms is not promised.

## 7. Outcomes and replay

Victory banks all earned loot. Voluntary retreat banks 70% of unbanked Spirit Shards and common materials, rounded down per stack. Defeat banks 50%. Guaranteed quest proof from a defeated boss is never lost. Permanent upgrades and already banked inventory cannot be lost.

Boss clear flags commit immediately at a safe reward screen so a later failure or crash cannot erase a completed story objective. First-clear rewards are keyed by boss and profile and can be awarded only once. Repeat runs award ordinary materials and a different blessing combination.

After the ending, unlock Ascended Challenge: optional stronger encounters with no exclusive ending or paid unlock. It is a stretch feature and may be omitted from 1.0. Normal replay remains available.

## 8. Accessibility and interaction

Pause freezes combat and timers. Focus loss pauses single-player play. Dialogue pauses world interaction in safe areas. Provide camera shake strength, motion blur toggle, flashes reduction, independent music/effects volumes, subtitles for meaningful nonverbal cues, text scale, invert camera axes, aim sensitivity, and hold/toggle lock-on.

Use shape plus color to distinguish threats. Decorative glyphs cannot replace localized instructions. All actionable text is available in English and Simplified Chinese. A brief low-flash mode preserves attack outlines while removing bright full-screen lightning and post-process pulses.
