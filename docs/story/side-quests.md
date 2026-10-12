# Optional Story Scripts — Work Beyond the Trial

Authoring status: eight fully scripted optional quests, twenty-four persistent stages, and 208 paired English/Simplified Chinese player-text keys. This is a production specification and data package; the quest interpreter and Unreal scenes still require implementation. The authoritative scripts are [the quest registry](../../data/story/side-quests.json), with [English resources](../../localization/story/en-US/) and [Simplified Chinese resources](../../localization/story/zh-Hans/). Each SQ01–SQ08 file contains exactly 26 keys. Development notes stay in English. Chinese names and prose exist only in the Chinese locale resources.

The stories occupy existing sanctuary, Bamboo Hollow, and Ashen Peak safe-state rooms. Keeper Lan is the only present side-story NPC. Miao, Qiao, He, Su, and Wen appear through dated records, not new humanoid characters. Mei remains deceased. Her surviving words are historical notes and cannot answer the player. Shen is never an optional-task giver or an assigned future maintenance worker.

## Reading and implementing a quest

Each registry entry declares availability, three ordered stages, exact speaker/text pairs, choice labels and responses, objective text, completion checks, checkpoint behavior, completion flags, reward grants, and ending-specific epilogue paragraphs. Its `player_text_keys` manifest includes every player-facing line, including retry and journal text. Scene scripts use the precise locale values, not ad-libbed summaries from this document.

The key pattern is `story.sqNN.s1.*`, `s2.*`, and `s3.*`, plus `title`, `summary`, and `epilogue.open|seal|shared`. The first scene includes Lan, Apprentice, and a full authored source note. The second contains Lan's interpretation, an actionable physical/document clue, and two fully authored player branches. The last has a confirmation checklist, Lan and Apprentice closure lines, the exact filed record, an explicit confirmation label, a recovery prompt, and a completion notification. This supplies dialogue, interaction text, notes, objectives, journals, outcomes, and closure rather than plot summaries alone.

`script_trigger: enter` plays once when a stage begins. A stage-three `confirmation_checks_ready` script waits until its physical tests are complete and any letter has actually arrived. Pending players see that stage's objective, journal, and retry text, never a premature line saying that the courier returned. They may inspect the proposed record while preparing the checklist; filing remains gated. `speaker: Record` opens a readable page; `Interaction` is inspect text anchored to the marked prop. Speakers are mapped to the project's existing localized speaker labels.

Stage-one and stage-two completion markers are committed only when all listed checks are satisfied, including an explicit branch selection where applicable. The third stage's confirm choice is disabled until every **non-confirmation** condition is met. Selecting confirm atomically applies the quest completion flag, its stage-three marker, the chosen record/prop state, and the one-time reward receipt. This avoids a circular objective that would demand confirmation before enabling confirmation.

All three stages are separately saveable. Read pages are kept in the journal, traversed checkpoints remain traversed, and revisiting a scene does not replay an already acknowledged line unless the player chooses Replay Record. Lines can be visually skipped without skipping an interaction or choosing a branch. A player returning to an unconfirmed choice sees both labels again. No skipped cutscene can invent an agreement, accept a duty, or complete a test.

## Availability and the ending contract

| Quest | Earliest entry | Completion flag | Purpose |
| --- | --- | --- | --- |
| SQ01 — A Light at the Spillway | `mq_ch02_complete` | `sq_spillway_verified` | Tested evacuation route and functioning lamp; required for Returning Rain |
| SQ02 — Hours for the River | `mq_ch04_complete` | `sq_allocation_agreed` | A concrete, mutually signed allocation schedule; required for Returning Rain |
| SQ03 — Names in the Empty Column | `mq_ch04_complete` | `sq_handover_accepted` | Limited named duties, consent, relief, and tested pause; required for Returning Rain |
| SQ04 — Before the Warning Bell | `mq_ch01_complete` | `sq_warning_record_filed` | Correct evacuation record; optional Mist Step training access |
| SQ05 — Borrowed Iron | `mq_ch02_complete` | `sq_iron_reconciled` | Safe repair and reconciled loan; optional Orbiting Blades training access |
| SQ06 — The Orchard That Needed Heat | `mq_ch04_complete` | `sq_orchard_calibrated` | Applied vent clue and local training preview |
| SQ07 — Thirteen Empty Spaces | `mq_ch04_complete` | `sq_archive_access_established` | Accountability without exposing unrelated personal records |
| SQ08 — A Bowl Kept for No One | `mq_ch02_complete` | `sq_meal_shared` | Mei's ordinary life and a present-day meal with Lan |

Chapter completion flags are monotonic. No later chapter invalidates these entry conditions. The ending preview may show disabled Returning Rain with all three named missing conditions; it also offers a return to the pre-trial hub. Each required quest must remain finishable there even after the final shell has been defeated. Its safe rooms use the campaign's return snapshot, not destroyed or inaccessible geometry. No quest requires repeated bosses, newly spawned combat encounters, or an expiring conversation window.

Final confirmation is the only cutoff for changing ending prerequisites. After confirmation, selected records can still be read in the hub/training reconstruction, but acquiring a later training-record flag cannot retroactively change the committed ending. The main ending preview explains this. Optional quests are not a secret checklist: each required quest's summary and last journal state name its Returning Rain condition and the exact remaining tests. Main-story explanations of the original consent limit, Yun's responsibility, Lan's bypass, and Shen's wishes do not depend on these quests.

## SQ01 — A Light at the Spillway

**Dramatic purpose.** Lan has preserved a tool because it still matters, while visitors have made Mei's death into a fixed image. The apprentice brings the conversation back to routes a living person can use. The branch is a practical maintenance decision, not an attempt to reenact her death.

### SQ01.S1 — The wick still draws

Location: the existing hub lamp workbench. Stage the oil reservoir, replaceable wick, intact glass cage, Mei's note, and inspection board. Lan checks a bracket as the player arrives. Play `s1.lan`, then the Apprentice's `s1.apprentice`; the note remains inspectable before and afterward. Its historical date is three days before the White Tide. The note's admonition about carrying the lamp by its hot cage and Mei's objection to Lan claiming her survey give her everyday voice before discussing the tragedy.

Basic interactions inspect four lamp components and perform a cost-free maintenance action. No oil gathering or crafting inventory is required. Opening the note and completing the workbench checks updates `s1.journal` and enables the safe spillway visit. The player never hears or summons Mei.

### SQ01.S2 — Walk what the map describes

Location: the existing Bamboo spillway room in safe state. Reuse the east-wall gate approached from the west stair, refuge marker, upper shelf, lower stair, drain lever, and adjustable lamp bracket. Preserve the crooked detour around the hollow ledge under the second rail. Optional lore L19 corroborates this layout; the side-quest clue explicitly supplies it, so the player need not find L19.

Play Lan's practical comparison and show `s2.clue`. The choice `a` selects the north/upper shelf and raises the bracket; `b` selects the south/lower stair and secures the drain open. Both keep the lamp **before the dangerous turn**, not at the gate. A visible existing route marker and ground path show the appropriate walk. After operating the branch's device, the player traverses gate, turn, and refuge checkpoints at ordinary walking speed. The lamp is a world prop/signal, not an equipable magic barrier or new carry-physics system. The device state and walk can be resumed independently.

Choosing the alternate route before confirmation clears the previous exclusive branch flag and that route's traversal stamps, preserving repaired lamp progress. A gate closed by an interrupted interaction is reopened with the same lever. The north route remains a fallback. Neither choice damages anyone, demands a movement technique, or yields a superior moral reward.

### SQ01.S3 — Give a working route a date

Location: the workbench after refuge sightline verification. The signal must remain visible at the refuge and pass a ten-game-minute burn test. Offer an explicitly labeled immediate time-advance interaction; ten real minutes must never be required. Relight the lamp after testing, post the applicable water/drain condition, and show the exact `s3.record`. Lan rejects legend-making; the Apprentice asks for the next inspection date to be public.

The final confirm signs the checked record and grants `sq_spillway_verified` regardless of branch. Persistent flags `sq01_route_north` or `sq01_route_south` select the visible marked route. Reward: a filed inspection record and lamp-glow prop state, granted once under receipt `SQ01`; no currency/stat reward. The burn-test timer and lighting state are saved, so leaving does not restart it. Ending cards show the lamp serving an open river, ordinary alternative crossing, or shared inspection rota as appropriate.

## SQ02 — Hours for the River

**Dramatic purpose.** Vague generosity becomes a measurable agreement. Su's mill and He's reed work both have legitimate needs. The player chooses between openly stated scheduling costs, then waits for people to accept them; dispatch is not consent.

### SQ02.S1 — Enough for whom?

Location: the existing hub courier board. Use two incoming request pages and the water-table gauge diagram. Both pages are stamped as received this morning. Su asks for four steady milling hours; He asks for two rinse hours. Both explicitly protect household drinking water. Su voluntarily offers two evening shifts when the sluice is lit. Read both requests and inspect the blue drinking-reserve mark and red workshop-stop mark. Lan admits how little his former phrase “give the valley enough” specified. No off-map worker appears as a model or live omniscient voice.

### SQ02.S2 — Send the actual hours

Location: the existing water table. Choice `a`: mill 06:00–10:00, rinse 14:00–16:00. Choice `b`: rinse 06:00–08:00, mill 08:00–12:00; on days two and five of the seven-day cycle, mill 16:00–20:00 instead. Day one is posting day, so “market days” is not an undefined calendar rule. Both protect the cistern's marked drinking reserve, stop both workshops below the red work mark, issue a same-day notice, and review seven days after posting.

These are **downstream workshop withdrawal** slots supplied by the buffered cistern. They do not require nonstop mountain intake recharge and do not contradict an ending's separate recharge intervals. The proposal grants no every-night community maintenance promise. L14's refusal of undefined night coverage remains valid.

The player seals and dispatches the selected written proposal. Save `sq02_schedule_a` or `sq02_schedule_b`, the proposal version, and the current in-game timestamp. The immediate branch response says the packet was sealed, not accepted. Only after dispatch plus eighteen game hours may the returned agreement spawn. The hub rest/delivery transition immediately advances those hours with an explicit elapsed-time label; normal play can also advance the clock. There is no real-time delay, login requirement, random mail roll, or repeated expedition.

### SQ02.S3 — Two signatures

Location: the courier board. When delivery is due, stamp the outgoing dispatch date and incoming arrival date as localized calendar fields, then play Lan's returned-courier line. Attach the actual selected timetable, not merely its letter label. The shared returned text states Su and He accepted that enclosed timetable without amendments, repeats the reserve and stop conditions, limits gauge-reading work to the two signatories’ own work slots, leaves physical upkeep to the separate maintenance charter, and gives the first review date.

The player reads both signature fields, verifies the enclosed hours, and confirms posting. Only that action grants `sq_allocation_agreed`; unacknowledged letters do not. Reward: the retained schedule record, receipt `SQ02`. A pending proposal can be replaced, restarting only its dispatch timestamp. Revising an already accepted agreement uses a fresh eighteen-hour round; the old confirmed version stays in force until a new one is signed and posted. No schedule choice has a concealed ending advantage. The ending card describes the review under the actual selected infrastructure, including renegotiation during relocation rather than magical constant supply.

## SQ03 — Names in the Empty Column

**Dramatic purpose.** The original institution placed a title in a box and failed to name a successor. This quest prevents the player from solving that error by binding another person indefinitely. It also treats a right to stop safely as part of responsibility, rather than cowardice.

### SQ03.S1 — The thirty-day box

Location: the safe Ashen archive. Put the original governor agreement beside four present duty-offer cards. The original consent and term are already main-story facts. These copies show the empty successor and witness fields. Lan offers one season of lamp and spillway inspection under his own name. Qiao offers gauge/stonework and one-shift relief coordination; He offers the allocation board; Su offers weekly cross-checks with He. Every card is read separately and states it is an offer pending acceptance of the selected charter, not consent manufactured by the player.

### SQ03.S2 — Where review happens

Location: the hub handover table. Choice `a` reviews weekly at the sanctuary board with public minutes. Choice `b` sends duplicate logs to the lower-valley board weekly. Both have the same physical duties, one-season terms, explicit-consent renewal, a named relief rule, and a safe-pause fallback. Duplicate writing is not an inspection. Copies reflect missed checks rather than covering them up.

The charter exempts released Shen from all duties. It limits the Apprentice's start-up bridge to **at most three days**, followed by scheduled seasonal checks. It assigns no daily/nightly indefinite bodily regulation to the Apprentice. Lan's daily checks and all off-map duties are daytime work; accepting community water use does not silently recruit a night worker. If further night coverage is ever proposed, it needs its own named, limited consent and relief entry.

Dispatch the selected exact charter to the four signatories. Save the version and timestamp; consent requires a twelve-game-hour round trip. Use the same immediate hub time-advance system as SQ02. Both pending quests advance under the same clock and can be completed in one rest transition; neither requires a second artificial wait.

### SQ03.S3 — Accept, and know how to pause

Location: the courier board and existing hub control prop. Deliver four matching signed copies after twelve game hours. Inspect each living signatory's acceptance, the Apprentice's three-day/seasonal terms, and the no-Shen-duty clause. Before confirmation, run a safe-state drill: return handle to neutral, close work release, keep reservoir relief open, verify the settled indicator, then restart the training loop. This uses deterministic existing control animations and indicators, not real-time fluid simulation or a dangerous live flood test. Retry text supplies the exact sequence; wrong input harmlessly resets the drill. Lore L20 is corroboration, not a required pickup.

The Apprentice's confirmation is their own explicit acceptance of limited duties. Only verified signatures **plus** tested shutdown **plus** that confirmation grant `sq_handover_accepted`. Reward: the consenting charter record, receipt `SQ03`. Change of rota requires fresh named consent; an accepted version stays valid while another is pending. Lack of SQ02 agreement does not block reading or accepting maintenance duties, but this quest never asserts that water-allocation consent exists. The three final prerequisites are independently visible. Ending cards adapt duties to open regulation, retired infrastructure, or the shared network; the shared card includes Qiao declining renewal and obtaining a willing replacement.

## SQ04 — Before the Warning Bell

**Dramatic purpose.** An institutional report attributed survival to its bell, while residents had begun acting earlier. Neither a single savior nor a malicious lying witness explains the discrepancy. The quest retains the uncertain details and recovers distributed competence.

### SQ04.S1 — What did Miao remember?

Location: the old Bamboo bell in safe state. Inspect the bell ledger and Miao's already archived testimony from last winter. Miao was twelve during the White Tide; Wen noticed backwards-rising water and asked them to knock on doors. They remember knocking, not every door. The third bell sounded halfway up the slope. Lan admits his “warning heard” line expressed hope rather than direct observation. No current courier round trip is implied by this historical testimony.

### SQ04.S2 — Two accounts can remain

Location: the hub archive/courier surface. Place three existing ledger tabs in dated order using the footbridge watermark. The clue establishes timing, not an exhaustive household list. Choice `a` publishes a joined timeline crediting local warning and bell. Choice `b` preserves both original accounts with an obvious cross-reference to the discrepancy. Both keep uncertain door coverage and immutable originals. Neither marks Miao a perfect prophet, calls Lan a villain, or penalizes the player for believing a survivor.

### SQ04.S3 — A usable sketch

Location: the existing training mat. File the selected annotation and inspect Miao's three-step dry-edge sketch. Lan describes dry-foot practice; the player need not equip or use Mist Step to solve the record puzzle. Confirming grants `sq_warning_record_filed` and unlocks optional `mist_step` **training access**, receipt `SQ04`. Actual training/equipping remains subject to the established progression system. If the technique is already available, the grant is a no-op, without substitute currency or duplicated mastery. Incorrect tab order resets only ordering and repeats the physical timing clue. Ending cards retain resident warnings under all infrastructure outcomes.

## SQ05 — Borrowed Iron

**Dramatic purpose.** A crate inventory labeled a still-serving repair part “missing.” Resolving the loan requires looking at a structure before demanding a fitting back. The choice then gives a scarce useful part two safe, concrete purposes without fabricating a life-or-death moral test.

### SQ05.S1 — Read beneath the lid

Location: the hub stonewright crate. Read the item list, Qiao's two-season-old loan note, and existing return receipt. Qiao left the brace supporting a split grain-wheel socket and warned against pulling it before placing the stone wedge. Lan pauses his assumption of carelessness. No accusation mechanically lowers approval, and no forgiveness prompt replaces inspection.

### SQ05.S2 — Transfer the load first

Location: the safe Bamboo wheel room. Use the pre-existing wedge, load arrow, plumb line, brace latch, and housing/plaque props. Seat the wedge until arrow and plumb line align. Only then does the latch release. Choice `a` uses the freed fitting to quiet/reinforce the wheel housing; choice `b` stabilizes the public board's rusting plaque stand. Both already ensure axle safety. The unchosen plaque can rest readable and flat; the unchosen housing can safely await later iron. No inscription is destroyed and no grain supply is sacrificed for a memorial.

### SQ05.S3 — A location, not a verdict

Location: the crate/workbench. Inspect the installed fitting and close the loan with its actual current placement. Read Qiao's expressly reusable balance sketch, whose arrows inspired an optional three-blade formation. Confirming grants `sq_iron_reconciled` and optional `orbiting_blades` training access, receipt `SQ05`. Existing access receives no duplicate reward. All operations use basic interaction and load-state animation, not a lifting ability or new inventory minigame. Branch changes safely undo the prior non-load-bearing placement before fitting the other site; the axle wedge stays seated. Ending cards preserve or relocate the placement ledger rather than declaring parts lost after closure.

## SQ06 — The Orchard That Needed Heat

**Dramatic purpose.** Wen retained burned and frozen branches to document changing her mind. Fire is neither inherently corrupt nor an unlimited cure. The player applies a specific local result and records its limited scope.

### SQ06.S1 — Two kinds of damage

Location: the safe Ashen orchard room. Inspect the scorched/frost-split samples and the trial card written three winters ago. It records a sealed pipe's burned bark, a fully blocked cold vent's frost, and a successful short pulse through an open outlet. The crossed-out first claim stays legible. The scene does not grow a tree instantly or summon its former keeper.

### SQ06.S2 — The outlet comes first

Location: the adjacent existing vent room, safe state with an empty calibration bed. Operate manual controls: open outlet, short pulse, cooling count one, cooling count two. No Fire technique is necessary. A red mark returns the handle to neutral without harming plants, people, or inventory. Two cooling beats have deterministic visual/text equivalents and an immediate accessible completion interaction.

After a successful test, choice `a` installs an insulated sleeve on the service handle; `b` installs a wide ash guard with a cleaning gap. Both keep the outlet visible and open. The choice trades handling safety emphasis for ash-cleaning emphasis; both are viable, and neither licenses skipping inspections.

### SQ06.S3 — Keep the failed sentence

Location: the hub board. Place original/corrected observations together, post the successful sequence and chosen fitting, and date today's test. Confirming grants `sq_orchard_calibrated`, the local calibration record, and a default-off `ashen_local_vent_pulse_preview` training hint, receipt `SQ06`. This preview only annotates the room's existing practice telegraph; it changes no boss strength, drops, general world balance, or ending condition. Readable enemy telegraphs remain a base accessibility feature for all players. Epilogue growth is explicitly one season later; the seal ending retests old local settings at a new small garden.

## SQ07 — Thirteen Empty Spaces

**Dramatic purpose.** Three representative renewals reveal how an emergency became ordinary: promises, inertia, then expense. Public accountability needs access to decision evidence, not the unrelated addresses of households listed in attachments.

### SQ07.S1 — Authorization is not consent

Location: the safe Ashen archive. Read Year 1, Year 6, and Year 12 renewal extracts. Preserve their approval stamps and original reasons, then inspect each separate blank warden-consent and successor field. Lan distinguishes understandable fear from permission to bind another person. These documents expand institutional context; the main story already gives the mandatory thirteen-year and thirty-day facts.

### SQ07.S2 — Access without collateral exposure

Location: the hub archive table. Order the three records and mark missing consent boxes. Choice `a` posts summaries with decision roles/dates/missing consent and an index to originals. Choice `b` makes full decision-page facsimiles available on-site, preserving controlled access to personal attachments. Both retain originals and withhold unrelated addresses from public display. The choice changes how people inspect the evidence, not whether they are allowed to know it.

### SQ07.S3 — Include Lan's own record

Location: the existing archive shelf. Compare the access notice with all sources: governor agreement, renewal extracts, Yun's calculations, and Lan's dated bypass ledger. Lan explicitly asks not to exempt his own ledger. Include append-only dated corrections, original storage locations, and the consent distinction. Confirming grants `sq_archive_access_established` and the renewal/access codex record, receipt `SQ07`. Incorrect ordering or unchecked fields are recoverable through the same table; no expedition or litigation minigame is added. Ending cards keep the archive open, including after retirement, and recognize that a new charter can also fail.

## SQ08 — A Bowl Kept for No One

**Dramatic purpose.** A deceased character is allowed to be impatient, funny, and ordinary. Lan's meal does not cure grief, award an approval point, or serve as proof that the Apprentice has said the correct comforting sentence.

### SQ08.S1 — This was a plan, not a farewell

Location: the hub old table. Inspect a folded recipe and the small waiting bowl. The page is dated one week before the White Tide: less salt, a wide bowl, sour-greens instructions, two spoons because Lan forgets, a proposed walk after lunch, and a crossed-out boast. Lan says plainly that he knows Mei will not return. The Apprentice offers to preserve the page without preserving an indefinitely waiting seat.

### SQ08.S2 — Who uses the copy?

Location: the safe Bamboo picnic shelf. Inspect the existing rail and dry marker, then the living-week margin note about checking the rail before bringing children. This small inspection is not SQ01's full gate-to-refuge route test and cannot grant `sq_spillway_verified`. Choice `a` places a copy at the communal table; `b` places it in the field workers' meal book. Both retain the original with Lan, the crossed-out joke, and the function of the spillway lamp. This quest never relocates or extinguishes that lamp.

### SQ08.S3 — Thin the broth

Location: the hub table. Place the selected recipe copy, return the original, and inspect a meal already available from the pantry. No ingredient hunt, cooking minigame, or currency payment is required. Lan repeats the salt mistake; the Apprentice offers water and time, not a diagnosis of his grief. An explicit sit/share confirmation advances twenty **game** minutes immediately and updates pending courier clocks. It stores the small bowl, never breaks it, and grants `sq_meal_shared`, a recipe codex page, and the wide-bowl prop, receipt `SQ08`.

Skipping the sitting animation completes the same consented meal and journal state. Leaving beforehand consumes nothing. No answering ghost/echo appears. Each ending card shows Lan still remembering Mei in an ordinary way. The shared card's forgotten spoons reprise the recipe's joke without implying she returned.

## Branches, rewards, and persistence

All eight paired branch flags are exclusive within their quest. Before final confirmation, a player can reconsider a branch by returning to that stage's interaction. Clear only its sibling flag and invalidated branch-specific checklist stamps. Preserve unrelated evidence reads, completed lamp/wedge work, and main-story progression. The latest proposed letter version and branch flag are not automatically the effective confirmed version; store each separately. Epilogues, retained boards, and final commissioning use the accepted effective version, not an unacknowledged proposal. For SQ02 and SQ03, preserve the old signed version until the new one returns and is explicitly confirmed. No branch change grants an additional reward or removes a earned training unlock.

Receipts `SQ01` through `SQ08` are save-persistent. The atomic completion transaction checks the receipt before grant, preventing duplicated cosmetic states, codex cards, or training access after reload. Grant retries may reconcile a missing receipt after interrupted persistence but cannot award a substitute currency payout. There is no side-story experience farming, hidden virtue score, punishment for compassion, or paid bypass.

The added quest state fits the existing generic quest/save system: current stage, per-prop checklist booleans, selected branch, proposed/effective version, dispatch/delivery game timestamps where needed, and one reward receipt. These are data fields, not additional player-facing subsystems. Do not serialize current physical object pointers; restore room props from persistent quest state on loading.

Closure epilogue selection: for each completed quest, choose exactly one `epilogue_keys` entry matching the committed `END_OPEN`, `END_SEAL`, or `END_SHARED`. Incomplete quests do not emit a falsely completed outcome. Their original notes remain inspectable. Stage flags and rewards never unlock a different ending except the three expressly named shared-network flags.

## Required implementation checks

1. Both SQ01 branches preserve the hollow-ledge detour and grant the same prerequisite only after tested lamp/route conditions.
2. SQ02/SQ03 never set their completion flags on dispatch, on rest alone, or by skipping text; signed delivery and explicit checklist confirmation are required.
3. Saving during pending correspondence retains timestamp, proposal, and returned version; one immediate hub rest advances all clocks consistently.
4. Twelve/eighteen game hours are shown as travel context and advanced immediately on request, with no real-time timers or grind gates.
5. All three shared prerequisites remain achievable when returning from the final preview; no post-boss geometry or dialogue cutoff strands a stage.
6. Safe-pause drill has a cost-free recovery and never conscripts Shen or an unnamed community into continuing work.
7. Mist Step/Orbiting Blades are rewards for training access, never prerequisites for their own quest interactions.
8. Ending cards respect retirement/relocation and Shen/Mei's status; none promises perfect institutions or immediate regrowth.
9. Locale key parity is exact. Every branch label, journal, authored note, retry prompt, objective, closure line, and epilogue has both languages.
10. Visually skipped scenes retain identical objective/record state, but consequential choices always await explicit input.
