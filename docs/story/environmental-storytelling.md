# Environmental Storytelling and Contextual Lines

## 1. Scope and source of truth

The complete authored text is in `localization/story/en-US/LORE01.json` through `LORE04.json` and matching Simplified Chinese resources. `data/story/lore.json` defines 24 records, locations, dates, availability, collection flags, and text keys. `data/story/barks.json` defines 40 contextual lines with bilingual resource values.

Every record is optional. Main scenes independently deliver the emergency duration, Shen's lack of renewed consent, Yun's role, Lan's bypass, and the viable final choices. A missing collectible cannot prevent understanding the ending.

## 2. Placement and purpose

| ID | Record | Physical presentation | Narrative use |
| --- | --- | --- | --- |
| L01 | A Repair Brief | Folded paper at dry bell | Establish diagnostic bell and Yun's practical absence |
| L02 | Supply Porter Register | Bound register in workshop | Ordinary apprenticeship and Lan's care |
| L03 | Waterwheel Tally | Board in Bamboo room 2 alcove | Measurable downstream shortage |
| L04 | The Reed Knot | Marker at safe lower stair | Resident agency and practical navigation |
| L05 | Mei's Lunch Note | Paper by spillway shelter | Everyday Mei rather than a saintly abstraction |
| L06 | A Bypass With No Number | Maintenance page at anchor | Lan's causal contribution, disclosed after confession |
| L07 | Thirty Days | Repaired reader archive panel | Corroborate the precise consent limit |
| L08 | Renewal, Year Four | Dated form in Ashen archive | Repeated institutional deferral |
| L09 | Release Valve Diagram | Diagram at cooled forge | Fair inspection-order clue |
| L10 | An Unsent Survey | Drawer beneath reports | Yun's accounting without excuses |
| L11 | A Guardian's Maintenance Request | Reader print/rubbing | Shen's attempts to prevent injury and leave |
| L12 | The Evacuation Route | Sealed packet at hub | Prepared END_SEAL alternative and real costs |
| L13 | The Room Above the Workshop | Key and note on basin | Belonging without compelled agreement |
| L14 | An Allocation Reply | Courier shelf correspondence | Bounded duty and consent |
| L15 | Ash Rather Than Ink | Wall marks, inspectable transcript | Errors in administrative assumptions |
| L16 | A Training Weight | Removable-looking sleeve prop | Care and physical combat reminder |
| L17 | Festival Cloth Invoice | Cloth bundle and invoice | Life in the valley before catastrophe |
| L18 | A Request Without a Seal | Present transcription at reader | Shen's own wish; no preservation without consent |
| L19 | Mei's Route Correction | Crooked map at spillway | Concrete usable SQ01 route clue |
| L20 | Handover Form, Unused | Form under renewal stack | Splittable, limited maintenance work |
| L21 | First Week: Open Water | Ending-specific noticeboard | Benefits and remaining burden of END_OPEN |
| L22 | First Week: The Smaller Gate | Ending-specific noticeboard | Viable daily life after END_SEAL |
| L23 | First Week: Shared Reading | Ending-specific noticeboard | Concrete accountability after END_SHARED |
| L24 | The New Apprentice Column | Updated workshop register | Close apprenticeship motif across endings |

Story props use the existing modular environments. Any physical detail described in a transcript must be represented by a modest mesh/decal/material variation or a clearly framed close-up; do not describe a new cinematic simulation the game cannot show.

## 3. Record reader interaction

Interact opens a pausing reader with title, date, full text, and Close. Read status is committed at the reader's safe close checkpoint. A journal entry preserves the full record and its date. Re-reading does not grant currency or re-trigger a plot event. Accessibility text scaling applies to the complete body; long records scroll rather than shrinking the font.

Records use distinct delivery labels: paper, register, diagram, remembered imprint, or present transcription. An imprint cannot answer a current question. L18 is explicitly present testimony transcribed after a living conversation. The player may find corroboration early when its gate allows it; later mandatory dialogue recognizes discovery without withholding its explanation from other players.

No document requires copying text manually or remembering an exact numeric code. A practical clue is summarized in the quest journal once read. Interaction icons distinguish unread, read, and currently relevant records without marking optional lore as mandatory work.

## 4. Contextual-line playback

Forty complete bilingual lines cover return visits, visible changes, scene aftermath, failure recovery, and ending-specific activity. They are subtitle-only by default; voice acting is optional future production. A speaker name and short restrained UI cue make silent delivery understandable.

Do not play ambient lines during combat, required dialogue, language changes, or an accessibility warning. Maximum one ambient line visible at a time. Minimum display duration is based on localized length with a 3–9 second range; allow subtitles to remain longer for accessibility. A line can play at most once per visit and has a 180-second cooldown. Drop obsolete queued lines when chapter/ending state changes.

Ambient lines do not contain indispensable objectives. If an important boss line is missed, its meaning belongs in the subsequent scene and journal. Priority: player warnings > required scene text > optional interaction > ambient bark. A language switch changes the visible string without restarting its gameplay trigger.

## 5. Space tells the story

At the opening, the bell is dusty, the caretaker's basin has water, and the lower gate has fresh reed knots. After Bamboo reconnection, the grove improves while the reader flickers: the apparent success and hidden cost are visible together. After Ashen restoration, soot stops falling near the cooling rack, but the central storm builds overhead.

After the ending, change only tested local props and atmospheric presets. END_OPEN adds schedule checks and a fresh thread. END_SEAL removes ceremonial abundance and replaces the gate plate with a tool notice. END_SHARED displays two disagreeing readings preserved side by side. None requires a fully simulated off-map village or an expensive new cutscene.

The post-ending realms are labeled training reconstructions; the unchanged enemy layout must not imply that the valley's repaired crisis inexplicably returns each day.
