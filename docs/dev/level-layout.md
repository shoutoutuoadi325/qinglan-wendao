# Level Layout Contract (map-building input)

Derived from `data/story/*.json` locations and docs/01/02. The map bootstrap
commandlet consumes this to place interaction anchors and dressing. All story
`location` keys must resolve to a named anchor in one of the four maps.

## Maps

| Map | File | Purpose |
| --- | --- | --- |
| Sanctuary hub | L_Hub | Safe zone: courtyard, training, cultivation, workbenches, tables, post-ending dressing |
| Bamboo Hollow | L_BambooHollow | 7-room realm (metal/wood/water mood) |
| Ashen Verge | L_AshenVerge | 7-room realm (fire/earth/ash mood) |
| Tribulation arena | L_TribulationArena | Foundation/Golden Core trials + ending decision platform |

## L_Hub anchors (story keys -> station)

- hub.courtyard: dry bell prop, CH00 opening stage
- hub.training_yard: training mat + practice rack (training target actors, tagged Qinglan.TrainingTarget)
- hub.cultivation_shrine: rank-up/breakthrough station
- hub.shrine: free recovery shrine
- hub.workbench + hub/lamp_workbench: repair benches (SQ01 lamp, SQ05 iron)
- hub.bamboo_gate: expedition departure gate (realm selection)
- hub/handover_table: SQ03 names ledger
- hub/archive_shelf + hub/archive_table + hub/archive_drawer (record station, SQ07)
- hub/courier_board + hub/courier_shelf: dispatch board (SQ04)
- hub/inspection_board: SQ06
- hub/water_table: SQ08 bowl kept for no one
- hub/stonewright_crate: SQ05 borrowed iron
- hub/old_table: misc stage
- hub/training_mat: technique practice (mist_step/orbiting_blades access)
- apprentice quarters (L03 record), workshop, post-ending noticeboard + workshop
- governor reader (Hub), sealed dispatch packet (records)
- Winter dressing variants for CH07 (snow decals, dry foliage) toggled by flags

## L_BambooHollow rooms (indices per realm.json)

0 entrance (bamboo gate arrival, loadout shrine)
1 combat (bamboo thicket, 1 archetype)
2 combat + alcove (room 2 alcove record; blessing draft after clear)
3 shrine (spillway shelter: old bell safe, spillway map board, picnic shelf, wheel, free heal, lore exchange)
4 elite (lower stair, reader archive, anchor maintenance panel, abandoned supply chest; blessing draft)
5 boss (Bamboo Guardian arena, 45 m diameter)
6 exit shrine (bank preview + confirm)
- SQ safe revisits: bamboo/old_bell_safe, bamboo/picnic_shelf_safe, bamboo/spillway_safe, bamboo/wheel_safe
- Rooms 35-55 m across, >= 2 unobstructed dodge lanes, no damaging pits

## L_AshenVerge rooms

0 entrance (ashen approach corridor)
1 combat (Ashen Peak approach), 2 combat (approach cont.), 3 archive shrine
(archive drawer/table, outer archive, governor reader; SQ07 archive_safe)
4 elite vent room (vent machinery, forge station cooled, shelter wall; orchard_safe alcove)
5 boss (Ashen Guardian arena; release antechamber beyond)
6 exit shrine

## L_TribulationArena

- foundation_arena: 90 s, three lightning waves + elite construct
- Golden Core arena (same map, storm dressing): 150 s, three storm phases + Broken Guardian
- decision platform (ending preview/confirm), governor platform (quiet)
- Persistent ground outlines + >=0.9 s warnings on every strike; safe lanes marked

## Dressing kit

Environment kit modules (10): courtyard floor, wooden building shell, roof set,
wall set, shrine, workbench, table set, lantern set, path stones, gate.
Winter/ash/bamboo variants swap materials only, not meshes.
