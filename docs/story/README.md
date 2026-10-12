# Complete Narrative Package

Status: fully authored first-release story specification. Contains the complete plot and ending spoilers. English production documents describe staging and logic; bilingual reading editions show every authored player line in both languages. No gameplay runtime or voiced cinematic is included.

## Read the complete story

Start with the [narrative bible](../story-bible.md), then read the chapters in order. The reading editions include dialogue, choices and responses, objectives, journals, inspection copy, retry messages, and ending previews. They are generated from authoritative resources and should not be edited directly.

| Chapter | Title | Production script | Complete bilingual text | Scenes |
| --- | --- | --- | --- | --- |
| CH00 | The Dry Bell | [Staging and logic](CH00.md) | [English / Simplified Chinese](../../localization/story/reading/CH00.md) | 4 |
| CH01 | Paths Left Open | [Staging and logic](CH01.md) | [English / Simplified Chinese](../../localization/story/reading/CH01.md) | 3 |
| CH02 | The First Weight | [Staging and logic](CH02.md) | [English / Simplified Chinese](../../localization/story/reading/CH02.md) | 4 |
| CH03 | A Foundation That Listens | [Staging and logic](CH03.md) | [English / Simplified Chinese](../../localization/story/reading/CH03.md) | 3 |
| CH04 | The Ash Ledger | [Staging and logic](CH04.md) | [English / Simplified Chinese](../../localization/story/reading/CH04.md) | 4 |
| CH05 | The Unfinished Handover | [Staging and logic](CH05.md) | [English / Simplified Chinese](../../localization/story/reading/CH05.md) | 4 |
| CH06 | The Borrowed Sky | [Staging and logic](CH06.md) | [English / Simplified Chinese](../../localization/story/reading/CH06.md) | 4 |
| CH07 | What the River Keeps | [Staging and logic](CH07.md) | [English / Simplified Chinese](../../localization/story/reading/CH07.md) | 4 |

## Optional stories

All eight have complete staged objectives, choices, recovery behavior, and ending-specific closure. The first three provide transparent shared-maintenance requirements; the other five enrich the record and characters without hiding main explanations.

| Quest | Title | Complete bilingual text | Completion |
| --- | --- | --- | --- |
| SQ01 | A Light at the Spillway | [Full script](../../localization/story/reading/SQ01.md) | `sq_spillway_verified` |
| SQ02 | Hours for the River | [Full script](../../localization/story/reading/SQ02.md) | `sq_allocation_agreed` |
| SQ03 | Names in the Empty Column | [Full script](../../localization/story/reading/SQ03.md) | `sq_handover_accepted` |
| SQ04 | Before the Warning Bell | [Full script](../../localization/story/reading/SQ04.md) | `sq_warning_record_filed` |
| SQ05 | Borrowed Iron | [Full script](../../localization/story/reading/SQ05.md) | `sq_iron_reconciled` |
| SQ06 | The Orchard That Needed Heat | [Full script](../../localization/story/reading/SQ06.md) | `sq_orchard_calibrated` |
| SQ07 | Thirteen Empty Spaces | [Full script](../../localization/story/reading/SQ07.md) | `sq_archive_access_established` |
| SQ08 | A Bowl Kept for No One | [Full script](../../localization/story/reading/SQ08.md) | `sq_meal_shared` |

Read the [side-quest production specification](side-quests.md) for physical tasks, choices, signed agreement versions, checkpoints, rewards, and fallback paths.

## World and dialogue details

- [24 full environmental records in both languages](../../localization/story/reading/LORE.md).
- [40 contextual lines in both languages](../../localization/story/reading/BARKS.md).
- [Record placement and ambient-line rules](environmental-storytelling.md).
- [Narrative execution, save, consent, and ending contracts](narrative-implementation.md).
- [Branch and continuity acceptance matrix](narrative-qa.md).

## Data and editing

Authoritative scene registries: [early main](../../data/story/main-early.json), [late main](../../data/story/main-late.json), [side quests](../../data/story/side-quests.json), [lore](../../data/story/lore.json), and [ambient lines](../../data/story/barks.json). Both locale folders contain matching text IDs. Localized speaker/source labels distinguish a current voice from an archived record.

After editing copy or scene order, run:

```sh
python3 tools/export_story_reading.py
python3 tools/validate_repository.py
python3 tools/export_story_reading.py --check
```

Every authored story string must be referenced. Registry validation checks keys, placeholders, scene transitions, chapter scope, and explicit ending confirmation requirements. Playable timing, staging, interface layout, and branch execution still require implementation tests.

## Narrative scope

Eight main chapters, 30 main scenes, eight side quests with 24 stages, three final outcomes, 24 environmental records, and 40 contextual lines. The 18 generated bilingual books make the entire text reviewable without navigating raw JSON. Total text counts and word counts are in the QA report.
