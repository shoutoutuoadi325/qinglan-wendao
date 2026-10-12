# Production Roadmap and Backlog

## 1. Estimation assumptions

One developer experienced with Unreal, approximately 30–35 productive hours per week, original modular environments, carefully licensed free supporting assets, and no paid outsourcing dependency. High-end VFX, animation, two-platform optimization, and store packaging are substantial work.

Expanded narrative estimate: 40–58 working weeks, plus approximately 15% contingency, yielding roughly 46–67 weeks. This replaces the earlier short-story scope; the added effort is for scene/quest integration, bilingual presentation, choice persistence, and branch QA rather than additional maps. This is a planning range, not a delivery commitment. Part-time work, learning Unreal from scratch, missing test hardware, or bespoke character production can extend it substantially. Re-estimate after M0 and the vertical slice using measured task throughput.

## 2. Milestones and gates

| Milestone | Effort range | Deliverable | Exit gate |
| --- | --- | --- | --- |
| M0: feasibility | 2–3 weeks | Win64 and Mac arm64 prototypes; VFXBenchmark; Windows packaging spike | Both packaged builds run; one hero spell meets provisional budget; Store route confirmed |
| M1: combat foundations | 4–6 weeks | Character, camera, sword, dodge, three techniques, three enemies | Fair 10-minute encounter; bilingual settings; correct hit windows |
| M2: vertical slice | 7–10 weeks | Hub, four rooms, Bamboo boss, Foundation trial, cultivation and durable saves | Complete loop on both platforms; external playtest; visual direction approved |
| M3: content production | 13–18 weeks | Two seven-room realms, eight techniques, eight story chapters, eight side quests, three endings | Feature complete; all content IDs/localization keys validated |
| M4: optimization and polish | 6–8 weeks | Final animation/VFX/audio, scalable presets, bilingual layout | Sustained frame-time and memory gates pass on reference hardware |
| M5: beta and release candidate | 6–9 weeks | Clean installs, upgrade tests, trusted Mac build, Store draft | No release blockers; legal notices and listing complete |
| M6: submission and stabilization | 2–4 weeks | Store certification fixes, matched Windows/Mac release | Certified Windows listing; tested Mac artifact; recovery plan |

Store review time and signing enrollment are external dependencies. They do not have guaranteed completion dates. M0 must discover identity, SDK, and account issues before art production expands.

## 3. First implementation tasks

| ID | Priority | Task | Depends on | Acceptance |
| --- | --- | --- | --- | --- |
| ENG-001 | P0 | Lock Unreal patch and free-license eligibility | None | Recorded versions and license path; no paid-only dependency |
| PLAT-001 | P0 | Package empty Win64 and native Mac arm64 project | ENG-001 | Launch/quit and input work outside editor on both OSes |
| VFX-001 | P0 | Build Jade Sword Arc hero benchmark | PLAT-001 | Real-time clip, timings, scalable core, Metal validation |
| STORE-001 | P0 | Confirm GDK Game setup and packaging spike | PLAT-001 | Test package installs; identity and key handling documented privately |
| LOC-001 | P0 | Build English/Simplified Chinese UI pipeline | PLAT-001 | All test widgets switch language; glyph coverage and fallback pass |
| MOVE-001 | P0 | Movement, camera obstruction, target lock | PLAT-001 | No wall clipping or trapped camera; controls rebindable |
| COMBAT-001 | P0 | Sword hit sweep, damage, dodge windows | MOVE-001 | Duplicate-hit and frame-rate tests pass |
| SPELL-001 | P0 | Release/cooldown/energy transaction | COMBAT-001 | Interrupted pre-release cast consumes nothing; released cast commits once |
| AI-001 | P1 | Three archetypes and encounter director | COMBAT-001 | Wave completion and safe spawn behavior |
| SAVE-001 | P0 | Profiles, durable write, backups, checkpoint | PLAT-001 | Corruption/disk-full tests preserve last valid state |
| PROG-001 | P1 | Cultivation ranks and first-clear banking | SAVE-001 | No duplicate rewards; ending achievable without optional upgrades |
| STORY-001 | P1 | Narrative importer, trigger/choice/save pipeline | SAVE-001, LOC-001 | Bilingual scenes, safe skip/reload, explicit ending gates |
| STORY-002 | P1 | Implement chapter/side-quest registries and staging | STORY-001, AI-001 | Complete main-only and optional-story paths, no unresolved text |
| STORY-QA-001 | P0 | Execute ending/skip/reload branch matrix | STORY-002 | All ending conditions, side resolutions, and chronology pass |
| SLICE-001 | P1 | Hub-to-boss-to-breakthrough loop | Above foundations | End-to-end packaged slice on both platforms |

P0 means feasibility/release-critical; P1 means required content. Optional cosmetic variants and extra lore are P2. The initial GitHub issues should correspond to the foundations above, not imply that gameplay already exists.

## 4. Production discipline

Each task defines behavior, content affected, supported platforms, localization keys, performance cost where relevant, and evidence. Finish a thin working feature before adding variants. Store binary source assets once through LFS; avoid duplicated packs. Review gameplay effects and VFX together, then profile in a packaged build.

Weekly cycle: select a small complete outcome, implement, test Windows and Mac affected behavior, gather a short gameplay capture, update task status, and record any changed design decision. New functionality should not accumulate in unreviewed level Blueprint graphs.

## 5. Cut order and risk register

| Risk | Early signal | Response |
| --- | --- | --- |
| Hero VFX too expensive on Mac | M0 GPU budget missed | Reduce overlapping translucency and use precomputed smoke; preserve core silhouette |
| Unreal/GDK workflow mismatch | Test package fails to launch | Resolve in M0 with official Win64 plugin path; do not postpone to final submission |
| Scope exceeds solo capacity | Slice exceeds estimate by >30% | Cut cosmetic robes, optional relic variants, optional alcoves; preserve both story anchors; any map cut requires an explicit narrative redesign |
| Restricted asset leaks into public Git | Missing redistribution entry | Use original placeholder; exclude restricted source; fix before publication |
| 8 GB recent Mac stalls | Memory pressure during warmup | Reduce residency and streaming; disclose verified minimum if still below gate |
| Chinese text breaks layouts | Clipping at 150% scale | Flexible widgets, CJK font, localized layout tests |
| Save/reward duplication | Restart changes banked totals | Stable transaction IDs, primary/backup protocol, failure injection |
| No trusted Mac signing access | Gatekeeper refuses test distribution | Arrange owner-controlled credentials; retain local development build until ready |

Never cut Windows/Mac support, bilingual core UI, free access, warning readability, save recovery, or essential performance checks to preserve optional content. If a scope cut changes the published design baseline, update README, content tables, estimates, and store copy in one change.

## 6. Versioning and deliverables

Use semantic gameplay versions (`0.1.0` slice, `0.5.0` feature complete, `0.9.0` release candidate, `1.0.0` public release) and separate monotonic package versions required by the store. A release record includes source commit, engine patch, content version, both artifacts, checksums, benchmark report, license manifest, known issues, and save compatibility.

The current documentation commit is not `1.0.0` and must not be presented as a playable release.
