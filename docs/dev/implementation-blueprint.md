# QinglanAscension — Consolidated Implementation Blueprint
Repo: `/inspire/hdd/project/qianghuaxuexi/s26044/qinglan-wendao` (docs + data + tools + `game/` UE 5.8 project: runtime module `QinglanAscension`, editor module `QinglanAscensionEditor`). Engine: UE 5.8, C++ gameplay / Blueprint presentation / Niagara / Enhanced Input, **no GAS** (lightweight ability components), offline-only, EN + zh-Hans.

---

## 1. System Inventory (gameplay systems with key numbers)

### 1.1 C++ component architecture (11 components, exact lifetimes)
| Component | Lifetime | Responsibility |
|---|---|---|
| `UQinglanGameInstance` | Application | bootstrap, profile selection, travel |
| `UProfileSubsystem` | GameInstance | permanent state + transaction validation |
| `USaveSubsystem` | GameInstance | versioned saves, backups, migration |
| `URunSubsystem` | Expedition | seed, rooms, temporary loot/blessings |
| `AQinglanCharacter` | Actor | movement, input routing, animation |
| `UCombatComponent` | Actor | attributes, damage, invulnerability, barriers |
| `UTechniqueComponent` | Actor | loadout, cooldowns, cast state |
| `UStatusComponent` | Actor | timed statuses, bounded reactions |
| `UEncounterDirector` | Realm world | wave activation, completion, rewards |
| `UVFXCueComponent` | Actor/world | cosmetic cue selection + pooling |
| `UGraphicsSettingsService` | Application | capability gating, presets, persistence |

Module layout: `game/Source/QinglanAscension/{Core,Combat,Abilities,AI,Progression,Persistence,UI,Platform}`. Exactly one authoritative mutation path (UI requests, never subtracts currency); typed events only: `OnDamageResolved`, `OnCastReleased`, `OnRoomCompleted`, `OnProfileCommitted`; global untyped event bus forbidden.

### 1.2 Combat core
- **Player attributes (initial):** HP 100, Spiritual Energy 100, Stamina 100, TechniquePower 20, Defense 10. Energy regen 4/s after 1 s idle; stamina regen 20/s after 0.8 s.
- **Movement:** walk 420 cm/s (no sprint). Dodge: 450 cm over 0.45 s, 25 stamina, i-frames 0.10–0.28 s, no wall pass-through, identical time window at 30/60/120 FPS.
- **Camera:** spring arm 350 cm, shoulder offset 60 cm, FOV 75° (range 65–95); per-frame sphere-sweep obstruction (in fast / out slow); lock-on 2,000 cm with visibility requirement, drop after 1.5 s out-of-range.
- **Sword combo:** Cut1 0.14/0.10/0.22 s ×0.8; Cut2 0.16/0.10/0.24 ×0.9; Cut3 0.22/0.12/0.32 ×1.3; combo window 0.35 s; input buffer 0.15 s; dodge-cancel windup and post-0.10 s recovery; **one hit per victim per swing** (sweep with substeps, notify-driven window); example damage 32; sword hits restore 4 energy/target (cap 8/swing; training targets grant no campaign resources).
- **Damage formula:** `Raw = TechniquePower × Coefficient + Flat`; `Mitigated = Raw × 100/(100 + max(0, Defense))`; `Final = round(max(1, Mitigated × ReactionMultiplier × DifficultyMultiplier))`, halves away from zero. Every damage event carries attacker, target, cast ID, hit ordinal, technique, element, raw magnitude, timestamp; duplicate hit keys rejected; unique cast ID prevents reentrant release.
- **Cast pipeline (fixed order):** validate alive/state/resources/target → anticipation → release once → commit energy+cooldown → spawn/trace effect → resolve hits/statuses → publish cues → recovery. Interrupted pre-release = zero cost; released = exactly one cost + one cooldown.
- **Statuses:** Wet 5 s; Burn 3 s (three 4-dmg ticks, refresh not stack, max 3 ticks); Root 1 s (bosses get 10% slow 1 s instead); Conduct = Thunder Seal on Wet: exactly one 1.25× application, consumes Wet once. Water removes Burn; Fire removes Wet pre-damage (no bonus). Precedence: immunity → barrier → reaction → status → cue. Barriers absorb before health; stronger replaces weaker, never stack.
- **Stale-object safety:** pooled projectiles/portals clear timers, victims, owner, tags, collision, lifetime; destroyed doors/barriers leave no NavMesh holes; timers use paused game time, never wall clock; removing a VFX never cancels gameplay.

### 1.3 AI and encounters
- FSM states exactly: Idle, Acquire, Approach, Windup, Attack, Recover, Stagger, Dead (Behavior Tree or explicit FSM). Decision tick 5–10 Hz; shared sensing/attack arbitration; ≤3 ordinary enemies in simultaneous melee windup; ranged has a separate (unnumbered) budget; stagger immunity prevents stun-lock; offscreen attacks need audio cue, no offscreen close-range instant strikes.
- Seeded `FRandomStream` ×3 (encounters / loot / blessings), same-seed reproducibility; enemy cap ≤8 ordinary or 1 boss + ≤2 adds; spawns behind portal with 0.8 s warning, never within 4 m of player; ≤1 new behavior per tutorial encounter; room encounter 45–90 s; expedition 12–18 min.

### 1.4 Realm/expedition system
- 7-room contract: 0 entrance (no enemies, loadout confirm, suspend OK) / 1 combat (1 archetype, 4–6 enemies) / 2 combat (2 archetypes, blessing #1) / 3 shrine (free heal, lore/exchange) / 4 elite (mixed wave, guaranteed material, blessing #2) / 5 boss (arena closes, campaign flag) / 6 exit shrine (bank preview, confirm). Optional authored alcove on rooms 2–3 allowed. Rooms 35–55 m across, ≥2 unobstructed dodge lanes, no damaging pits. Doors = explicit interactables, unlock exactly once when all living encounter enemies die (decorative creatures excluded); simultaneous last-kill must open door + reward exactly once (no deadlock).
- Bamboo room shard allocation: 0/30/40/0/50/60/0 = 180 base.

### 1.5 Progression / economy
- Ranks: Qi Refining 80/120/180 Shards + 1 Spirit Herb each; Foundation 220/300/400 + 1 Vein Crystal each; total 1,300 Shards pre-final. Per rank +5 MaxHealth / +2 TechniquePower (6 ranks = +30 / +12). Foundation breakthrough +15 HP / +20 Energy + tier-2 unlock; Golden Core +20 HP + post-ending replay. Breakthrough gates: Qi→ Bamboo Guardian cleared + 3 ranks; Foundation→ Ashen Guardian cleared + 3 ranks; Golden Core→ Golden Core trial passed; trial entry free; purchases atomic (insufficient = no state change).
- Payouts: tutorial 80 + 1 Herb; Bamboo first 180 + 120 bonus + 3 Herb + 1 Crystal, repeat 180 + 2 Herb + 1 Crystal; Ashen first 300 + 200 + 3 Crystal, repeat 300 + 2 Crystal. First-clear bonus idempotent, keyed by boss+profile; base payout is expedition total incl. room rewards.
- Banking: victory 100%, voluntary retreat 70% (floored per stack), defeat 50%; boss proof never lost; boss flags commit at safe reward screen.
- Loadout: 4 techniques / 2 passives / 2 talismans / 1 relic; changes only at sanctuary/entrance shrine, free, not mid-combat, cooldown-preserving. Tier-2: 100 Shards + 1 Crystal, +15% (damage/heal pre-mitigation raw expression, or barrier amount), no tier 3.
- Bonus stacking: add within category, apply once; energy cost min 1 rounded up; reaction modifiers affect Conduct only; no blessing/passive hidden stacking.
- Inventory: 64-bit bounded ints, reject negatives/overflow; stable transaction IDs = profile+expedition+room+category; idempotent banking; failed saves retry without double payout; transaction preview UI mandatory.

### 1.6 Save system
- Schema v1 (see §3). 3 profiles × 2 validated backups; 6-step durable write (snapshot+validate → temp file same dir → flush/reopen/parse/verify checksum+sequence → preserve backup → atomic replace → game-thread report); failed write never deletes last valid primary. Must survive full failure matrix (kill before/during/after writes, disk full, unwritable, truncated, bad checksum, future schema). Safe checkpoints: entrance, completed-room reward screen, exit, hub, trial prep only. Crash in active room → resume room entrance from pre-room snapshot without respawn of committed rewards. Portable ASCII IDs only; graphics settings platform-local.

### 1.7 VFX/rendering/audio contracts
- Five-stage spell contract (anticipation/release/travel/impact/decay, gameplay-defined timings) + cue params `CastId, SourceTransform, TargetPoint, Element, Intensity, QualityTier, AccessibilityFlags`. CPU-authoritative damage; missing GPU emitter never suppresses damage/warnings; budgets never apply to warning telegraphs (lightweight, culling-immune components). Degradation order: ambient sparks → distant cosmetics → secondary debris; player attack core + enemy warnings highest priority.
- Budgets (Performance/Balanced/High): GPU particles 15k/35k/60k; major spell systems 4/6/8; transient lights 2/4/6; distortion layers 0/1/2; hero textures 1K/2K/2K. VFX GPU: ≤2.0 ms @16.67 ms (Win reference), ≤3.5 ms @33.33 ms (base Mac).
- Hero-effect checklist (9 items incl. 15 m silhouette, ≥0.2 s buildup, ≤50 ms audio/visual alignment, decay never occludes telegraphs, Low quality keeps silhouette+outline).
- Audio: 4 layers per spell (launch/flight/impact/decay); 6 music pieces; master/music/effects/UI volumes + dynamic range reduction + visible cue representation.
- Performance gates: Win baseline (RTX 4050-class, 1080p): GT ≤5 ms, RT ≤5 ms, GPU ≤14 ms; Mac base (M2/M3, 900p–1080p): GPU ≤28 ms, GT ≤8 ms; p95 ≤18.5 ms / p99 ≤25 ms (60 FPS tier), p95 ≤35 / p99 ≤45 (30 FPS tier); no combat spikes >50 ms; resident ≤6 GB (16 GB Mac); texture residency ≤3.5 GB (6 GB Win GPU). 15-min benchmark route + 30-min fanless-Mac sustained run, cold+warm.

### 1.8 Localization/UI/accessibility
- Exactly 2 locales (en-US, zh-Hans); dotted keys, FText/String Tables, no fragment concatenation, named-arg FText::Format, plural/number-aware; first launch auto-zh-Hans on OS match; live switching without restart; 100/125/150% text scale; no tofu glyphs; profile name ≤24 displayed chars, never a filename. 10-term canonical glossary (Technique never Skill/Spell/Art; Foundation Establishment never "Foundation Building"; etc.). Mandatory settings: camera shake slider, motion blur toggle, flash reduction, music/effects volumes, nonverbal-cue subtitles, text scale, axis inversion, sensitivity, hold/toggle lock-on; low-flash keeps outlines/warning rings; threats shape+color.

### 1.9 Narrative runtime
- 30 main scenes + 24 side stages + 24 records + 40 barks; scene = ordered transaction (flag + journal + objective + unlock + choice atomically); crash-before-commit resumes scene, crash-after never re-awards; skip applies mandatory journal facts then pauses at unselected choice; choices mutually exclusive attitude flags, no hidden virtue score; input freeze while choice pending; dispatcher binds gameplay events → `mq_*` flags; side-quest registry can never overwrite `mq_*` flags; courier deliveries use safe hub time advance (SQ02 18 h, SQ03 12 h, zero real-time wait); `mq_shen_released` saved only after pattern dissolution; endings commit once, mutually exclusive, New Game explained before confirmation.

---

## 2. Content Inventory (exact counts and names)

### 2.1 Chapters (8, 30 scenes) — `data/story/main-early.json` (CH00–03) / `main-late.json` (CH04–07)
| Chapter | Title | Scenes | Flag chain highlights |
|---|---|---|---|
| CH00 | The Dry Bell | 4 | bell_examined → training_complete → route_read → ch00_complete |
| CH01 | Paths Left Open | 3 | watermarks_read → short_route_clear → ch01_complete (Ember Lance + guided bank at S03) |
| CH02 | The First Weight | 4 | pressure_baseline → bamboo_defeated → intake_reconnected → ch02_complete (full 7-room Bamboo; boss banks once at S04 return) |
| CH03 | A Foundation That Listens | 3 | prepared → trial_passed → ch03_complete (90 s Foundation trial; unlocks Ashen access) |
| CH04 | The Ash Ledger | 4 | 30-day consent + empty handover revelations; S03 physical dials (no fluid sim) |
| CH05 | The Unfinished Handover | 4 | storm onset; S01 boss beats (arena_entry, telegraphs, 50% HP, defeat); S04 live question + Shen consent |
| CH06 | The Borrowed Sky | 4 | 150 s storm (beats t=0/50/100/150), shell disable, release sequence, ending platform |
| CH07 | What the River Keeps | 4 | first-winter epilogue, per-ending + per-side-quest closure |

### 2.2 Side quests (8 × 3 stages = 24)
SQ01 A Light at the Spillway (after ch02 → `sq_spillway_verified`); SQ02 Hours for the River (ch04 → `sq_allocation_agreed`); SQ03 Names in the Empty Column (ch04 → `sq_handover_accepted`); SQ04 Before the Warning Bell (ch01 → `sq_warning_record_filed`, mist_step access); SQ05 Borrowed Iron (ch02 → `sq_iron_reconciled`, orbiting_blades access); SQ06 The Orchard That Needed Heat (ch04 → `sq_orchard_calibrated`); SQ07 Thirteen Empty Spaces (ch04 → `sq_archive_access_established`); SQ08 A Bowl Kept for No One (ch02 → `sq_meal_shared`).

### 2.3 Endings (3, one-time commit)
END_OPEN "Open River" (always), END_SEAL "Quiet Summit" (always), END_SHARED "Returning Rain" (requires `sq_spillway_verified` AND `sq_allocation_agreed` AND `sq_handover_accepted`, journal shows unlock conditions once proposal known). All require `mq_shen_released`; all release Shen; same final combat; mutually exclusive flags, commit_once.

### 2.4 Enemies (6 archetypes + 2 bosses)
Spirit Wolf 45/5 (0.65 s dip); Corrupted Disciple 70/10; Hollow Archer 40/5 (0.9 s glow); Stone Sentinel 120/25 (1.2 s wedge); Ember Wisp 35/0 (0.8 s core, fixed perch); Rootbound Keeper 90/15 (1.0 s vine ring). Bamboo Guardian 900 HP/15 Def, 45 m arena, 3 patterns, 3–4 min. Ashen Guardian 1,400 HP/20 Def, 3 patterns, ≤8 s immunity phases. Trials: Foundation 90 s (3 lightning waves + elite construct); Golden Core 150 s (3 storm phases + Broken Guardian), strikes ≥0.9 s warning + persistent outline.

### 2.5 Techniques (8; vertical slice 3: jade_sword_arc, flowing_ward, ember_lance)
| ID | Element | Energy | CD | Coef | Flat | Delivery |
|---|---|---|---|---|---|---|
| jade_sword_arc | Metal | 12 | 2.5 | 1.5 | 5 | 1,200 cm arc projectile |
| ember_lance | Fire | 20 | 6.0 | 2.0 | 5 | projectile, applies Burn |
| flowing_ward | Water | 18 | 8.0 | 0 | 0 | 25-pt barrier, 5 s |
| thunder_seal | Lightning | 24 | 7.0 | 2.2 | 8 | 300 cm ground strike |
| stone_aegis | Earth | 22 | 10.0 | 0 | 0 | 35-pt barrier, no geometry |
| verdant_pulse | Wood | 25 | 12.0 | 0 | 0 | heal 20 over 4 s |
| mist_step | Water | 15 | 9.0 | 0 | 0 | enhanced dodge, 600 cm |
| orbiting_blades | Metal | 30 | 14.0 | 0.6 | 2 | 6 pulses, ≤2 targets/pulse |

Unlock order: Jade Sword Arc + Flowing Ward (tutorial, CH00_S02), Ember Lance (post-guided, CH01_S03), Thunder Seal (Bamboo first clear), Stone Aegis + Verdant Pulse (Foundation), Mist Step (SQ04) + Orbiting Blades (SQ05) via training access + sanctuary practice.

### 2.6 Items/economy content
- Passives (8, equip 2): Energy Recovery +1 energy/s; Steady Foundation +10 HP; Sword Discipline +10% sword; Quiet Mind −10% tech cost; Grounded Steps +10 stamina; Herbal Knowledge +10% healing; Elemental Insight +10% Conduct; Measured Breath +10% stamina recovery.
- Talismans (6, expedition charges): Healing 25 HP ×2; Cleansing (Burn/Root) ×2; Protective 20 barrier ×2; Focus 25 energy ×2; Shock (interrupt 300 cm) ×1; Recall (retreat from cleared room) ×1.
- Relics (6, equip 1, names only): Jade Pendant, Ember Bead, River Pearl, Thunder Charm, Mountain Token, Verdant Seed.
- Blessings (12 = 4 groups × 3: sword offense, casting economy, protection, elemental interactions); draft 1-of-3 after rooms 2 and 4; one-stack-cap never re-offered; expire at expedition end.
- Resources: Spirit Shards, Spirit Herb, Vein Crystal only. Records L01–L24; barks B01–B40 (cooldown 180 s, ≤1/visit, combat-suppressed); cast 5 (Apprentice, Keeper Lan, Elder Yun, Shen Heng, Lan Mei-deceased); 2 robe cosmetics; 10-module environment kit; 6 music pieces; player animation set (18 named clips incl. 4-direction dodge, 4 cast archetypes).

---

## 3. Data Contracts (JSON the C++ must parse)

All files exist in-repo under `data/examples/` and `data/story/` (schema_version 1 everywhere). `data/examples` are contract illustrations, not production serialization.

**`data/examples/techniques.json`** — `{schema_version, content_version, techniques[]}`; technique = `{id, name_key, element, energy_cost, cooldown_seconds, power_coefficient, flat_damage}`. C++ data asset additionally carries: display text key, cast phases, targeting, magnitude, animation soft ref, cue soft ref, unlock rule; all refs validated during cook; selection/reward-affecting changes require `content_version` bump.

**`data/examples/save.json`** — profile save v1: `{schema_version, content_version, profile_id, save_sequence, cultivation{stage,rank}, inventory{spirit_shards,spirit_herb,vein_crystal}, unlocked_techniques[], equipped_techniques[], campaign_flags[], committed_transaction_ids[], checkpoint|null, narrative{revision, completed_scenes[], completed_side_stages[], flags[], choice_decisions{}, read_records[], pending_agreements{}, effective_agreements{}, confirmed_ending}}`. Checkpoint payload: `{seed, stream states, room index, acquired blessings, player attributes, loot, committed-room IDs}`; null = sanctuary.

**`data/examples/realm.json`** — `{schema_version, id, name_key, seed, max_concurrent_enemies, rooms[]{index, kind, spirit_shards}, first_clear_bonus{spirit_shards, spirit_herb, vein_crystal}}`; `kind` enum: entrance|combat|shrine|elite|boss|exit.

**`data/story/main-early.json` / `main-late.json`** — `{schema_version, chapters[]{id, title_key, summary_key, scene_ids[]}, scenes[]}`; scene = `{id, chapter, location, title_key, prerequisites[], script[]{speaker, text_key, speaker_key, source_kind, source_kind_key, [event, trigger, when_all[], when_none[]]}, choices[]{id, label_key, response_keys[], sets_flags[], [action, prerequisites[], presentation_group, show_after_text_key]}, objectives[]{id, text_key, completion}, sets_flags[], next_scene_ids[], player_text_keys[]}` + scene-extras `interaction_flow[]` (CH05_S04, CH06_S03), `physical_reveal` (CH04_S03), `optional_epilogue_*` (CH07_S04). `source_kind`: live|paper|letter|system|memory|recorded. `action` payloads: `enter_trial{requires_progression}`, `stay_at_hub`, `release_guardian{idempotency_key, required_before_any_ending, completion_sequence[], sets_only_after_dissolution, requires_live_consent}`, `preview_ending`, `confirm_ending{requires_preview, requires_warning_acknowledgment, mutually_exclusive_flags, commit_once}`, `cancel_confirmation`, `return_to_hub{preserve_flags, resume_scene_id}`, `complete_campaign{credits_skippable, idempotency_key}`.

**`data/story/side-quests.json`** — `{schema_version, quests[]{id, title_key, summary_key, availability[], stages[], completion_flags[], epilogue_keys[3], implementation{decision, checkpoints[], timing?, fallback?}, rewards{}, player_text_keys[]}}`; stage = `{id, location, prerequisites[], script[], choices[], objectives[], sets_flags[], player_text_keys[], checkpoint, script_trigger(enter|confirmation_checks_ready), journal_key, [retry_key, confirmation_guard, advance_delivery_key, delivery_time_key, delivery_transition]}`; SQ02/SQ03 add `pending_status_key` + `proposal_state` (pending/effective version slots).

**`data/story/lore.json`** — records L01–L24: `{id, location, required_flags[], date_label, clue, title_key, body_key, date_key, inspect_key, collection_flag, player_text_keys[]}`.
**`data/story/barks.json`** — B01–B40: `{id, speaker, location, required_flags[], forbidden_flags[], text_key, cooldown_seconds:180, max_per_visit:1, suppress_during_combat:true, player_text_keys[], speaker_key, source_kind, source_kind_key}`.
**Localization** — `localization/en-US.json`, `localization/zh-Hans.json`, `localization/story/{en-US,zh-Hans}/{CH00–CH07, SQ01–SQ08, LORE01–04, BARKS, SPEAKERS}.json`: flat key→string maps; identical keys/placeholders both locales; imported into String Tables via documented workflow (mechanism undecided). `assets/asset-manifest.csv`: exactly 13 fields (`asset_id,category,source_url,author,license_id,license_url,acquired_version,free_use_verified,game_distribution_allowed,source_redistribution_allowed,attribution,public_path,notes`), enums yes/no/pending.
Validation gate: `python3 tools/export_story_reading.py` → `python3 tools/validate_repository.py` → `export_story_reading.py --check` (checks keys, placeholders, transitions, chapter scope, ending confirmation, every string referenced).

---

## 4. Implementation Order Proposal (M0/M1)

**M0 — Feasibility (2–3 wks), gate: both packaged builds run + one hero spell meets provisional VFX budget (≤2 ms Win) + store route confirmed:**
1. ENG-001 (P0): pin UE 5.8 exact patch + toolchain on a branch; record license path; no paid-only deps.
2. PLAT-001 (P0, dep ENG-001): create `game/QinglanAscension.uproject` + 8 source modules (skeleton only); package empty Win64 (GDK plugin path) and native Mac arm64; launch/quit/input outside editor both OSes.
3. Parallel after PLAT-001: VFX-001 (VFXBenchmark map + Jade Sword Arc hero effect, Metal validation, p95/p99 + memory capture harness); STORE-001 (GDK Game identity, MSIXVC spike, private key handling); LOC-001 (String Table pipeline, language switch on all test widgets); SAVE-001 (schema v1 read/write, durable-write protocol, 3×2 profiles); MOVE-001 (character, camera obstruction, lock-on, rebindable Enhanced Input).
4. In parallel on Linux headless (no engine needed): freeze data contracts with JSON Schemas generated from §3; wire `tools/` validators into CI; author all content registries + locale parity checks; generate String Table import manifests.

**M1 — Combat foundations (4–6 wks), gate: fair 10-minute encounter, bilingual settings, correct hit windows:**
1. COMBAT-001 (dep MOVE-001): `UCombatComponent` attributes, sword sweep single-hit, substepped traces, damage formula, 30/60/120 FPS dodge automation tests, duplicate-hit test.
2. SPELL-001 (dep COMBAT-001): `UTechnineComponent` + `UStatusComponent`; cast pipeline transaction; 3 slice techniques (jade_sword_arc, flowing_ward, ember_lance) from data assets; Conduct + barrier-replacement rules.
3. AI-001 (dep COMBAT-001): 3 archetypes (Spirit Wolf, Corrupted Disciple, Hollow Archer) + `UEncounterDirector`, spawn portals (0.8 s/4 m), ≤3 windup arbitration, wave completion, door unlock single-trigger.
4. Continue SAVE-001 → failure-injection matrix automation; begin PROG-001 (rank purchases, atomic checks, first-clear banking) and STORY-001 (dispatcher, importer, scene transaction commits) since deps (SAVE+LOC) land in M0/M1.
5. Exit M1 with CH00 tutorial scene playable end-to-end (technique grants, training reset safety, skip/choice freeze semantics).

**Sequencing into M2+ (context):** STORY-002 registries → STORY-QA-001 branch matrix → SLICE-001 (hub→boss→breakthrough loop, 4 rooms, Foundation trial) → M3 content (14 more rooms, 8 techniques, CH04–07, SQ04–08, endings) → M4 presets/bilingual layout on reference hardware → M5 RC/store drafts → M6 matched release.

---

## 5. Cross-Document Contradictions and Gaps

**Resolved by repo docs not in the extraction set** (`docs/02-combat-and-camera.md`, `docs/06-platform-and-performance.md` exist and fill former "open questions"): full camera specs, movement/dodge numbers, damage formula, enemy/boss/trial stats, input map, and all numerical performance budgets (§1.7).

**True contradictions / ambiguities:**
1. "Realm" collision: cultivation realms (Qi Refining/Foundation/Golden Core) vs expedition realms (Bamboo Hollow/Ashen Peak, UI "Spiritual Realm"). Canonical term table needed per surface.
2. "Two realms and three cultivation stages" (core design) vs docs/03: three stages where only two have purchasable ranks; Golden Core is a trial-gated achievement — implementation must treat Golden Core as rankless.
3. Save schema v1 has BOTH top-level `campaign_flags[]` and `narrative.flags[]` — ownership of `mq_*`/`sq_*`/`lore_*` flags between the two stores is undefined (risk of split-brain flag reads).
4. Save schema omits equipped passives/talisman/relic, talisman charge counts, and audio/accessibility settings persistence. Talisman charges are expedition-owned but absent from the checkpoint payload field list ("loot" is not explicit enough).
5. Damage formula includes `DifficultyMultiplier` — no difficulty system is specified anywhere in the doc set (ship as 1.0 and document).
6. VFX cue contract `Element` enum: techniques.json uses `Lightning`, which is explicitly "not a sixth member of the Five Elements" — enum must be 6-valued (Metal/Wood/Water/Fire/Earth/Lightning) or element+category pair; Intensity/QualityTier/CastId formats still unspecified (QualityTier implied Performance/Balanced/High).
7. Location identifiers inconsistent across registries: `hub.courtyard` (dotted) vs `hub/lamp_workbench` (slash) vs free text ("Ashen Peak boss arena, safe state") — canonical location ID table required before C++ parsing.
8. Objective `completion` fields are English prose, not machine predicates; a dispatcher convention mapping prose → gameplay events is unspecified.
9. Story timing consistency risk: CH02 uses the full 7-room Bamboo instance while the M2 slice has 4 rooms — scene data and slice scope must not be conflated.

**Genuine gaps (unauthored content):**
- 12 blessings: no names/effects/stack-cap list exists anywhere (only 4×3 grouping + cap rule); slice's 4 blessings also unnamed.
- Relic bonuses/tradeoffs: 6 names only; which 2 are in the slice unspecified.
- Which 3 of 8 passives are slice passives; which 4 of 12 slice blessings.
- Ranged-attack concurrency budget: "its own budget" with no number.
- Stream-state serialization shape (3 streams, field format) undefined.
- `content_version` increment trigger is qualitative; needs a review checklist.
- Tooltip data contract for xianxia term explainers (mandatory accessibility feature) is undefined.
- Localization import mechanism undecided (direct String Table import vs documented script).
- Bark `required_barks_this_visit` appears on 1 of 40 barks; semantics unconfirmed.
- MSIXVC2 (Oct 2026 GDK) adoption pending; store identity/signing/support contact/privacy URL all owner-pending; 8 GB Mac classification pending evidence.

---

## 6. Top Risks for a Linux-Headless Dev Environment

1. **Zero local build/run capability (highest):** targets are Win64-GDK + macOS-arm64 only; UE cannot cross-compile either from Linux (Win64 needs MSVC/Windows, Mac needs Xcode/macOS), and GDK/Metal are unavailable. Every M0/M1 gate (packaged builds, VFXBenchmark, hit-window/dodge FPS automation, save failure injection on-device) requires remote Windows + macOS machines or CI runners — procure these before writing gameplay code, or all acceptance evidence stalls.
2. **Engine not acquired:** no Epic EULA accepted, engine fetched via owner account, kept out of repo (~40+ GB). Nothing engine-dependent can even begin locally; on Linux only docs/data/tooling work is real.
3. **Unverifiable contract drift:** C++ parsers and JSON registries authored without runtime import validation invite schema rot. Mitigation: author JSON Schemas for all §3 contracts now, run `tools/validate_repository.py` + `tools/validate_story.py` + locale key/placeholder parity + asset-manifest lint as required CI on Linux; generate String Table fixture manifests from locale JSONs to freeze key formats (`story.chNN.sMM.line_XX`, `technique.<id>.name`).
4. **All GPU/VFX/performance evidence unobtainable:** p95/p99 frame-time, Metal captures, Shader Complexity/Quad Overdraw, hero-effect checklist, 15-min benchmark and fanless-Mac sustained runs need actual GPUs on supported OSes; editor-FPS is explicitly invalid evidence. Budget remote hardware time early; never let headless-passing docs CI be mistaken for release authorization.
5. **Signing/store pipelines blocked:** GDK packaging, Partner Center identity, MSIXVC, Apple Developer ID, notarization/stapling, and clean-machine quarantine tests all require Windows/macOS + owner credentials; unsigned Mac builds must never be labeled final.
6. **Automation-test assumptions:** UE automation tests require an editor/game process on a supported OS (often a graphics context); plan which tests (damage math, rank transactions, save migration, checkpoint serialization) can eventually run headless via commandlet on the Windows/macOS CI vs. which need rendering.
7. **Binary asset workflow partially available:** Blender, fonttools (CJK glyph coverage), and audio tooling run on Linux, but FBX retarget/interoperability, animation notify, and LFS-tracked .uasset verification all need editor sessions; keep original placeholder assets manifest-verified from Linux.
8. **Locale/formatting tests deferred:** 100/125/150% layout, Retina DPI, live language switch, and CJK rendering cannot be screenshot-verified headless — schedule bilingual capture sessions on real hardware as a standing M1/M4 task rather than a trailing one.