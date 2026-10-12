# Quality Assurance and Release Acceptance

## 1. Testing layers

Planning validation checks links, example contracts, English development text, bilingual localization parity, and packaging template rules. It is not a gameplay test suite. Add runtime tests as the corresponding gameplay is implemented.

Use unit/automation tests for damage calculations, rank transactions, duplicate rewards, save migration, and checkpoint serialization. Use functional maps for collision, camera, input, room progression, and technique cues. Use human tests for readability, animation, feel, bilingual phrasing, and visual fidelity. Use actual packaged builds for platform, memory, GPU, installation, and distribution testing.

## 2. Gameplay acceptance cases

| Area | Test | Pass condition |
| --- | --- | --- |
| Damage | Repeated sword sweep contacts same enemy | Exactly one hit per strike; expected example damage 32 |
| Cast transaction | Interrupt windup then cast normally | No cost before release; one cost/cooldown afterward |
| Dodge | Run at 30/60/120 FPS | Same time-based window; no tunneling through wall |
| Reaction | Wet + Thunder Seal | One 1.25× damage application; Wet consumed once |
| Barrier | Receive damage with two barrier casts | Stronger replaces weaker; no multiplicative stacking |
| Death | Health reaches zero during a cast | No further input or duplicate reward; safe outcome screen |
| Room | Last wave killed simultaneously | Door and reward open once; no deadlock |
| Boss | Defeat then quit/restart at reward screen | Story proof persists; first-clear bonus not repeated |
| Economy | Attempt unaffordable upgrade | No state changes; localized missing-resource message |
| Campaign | Complete mandatory path with basic upgrades | Both trials and ending reachable |

## 3. Persistence failure injection

Kill the process before temp write, during write, after backup preservation, and before/after replacement. Fill the save volume, make a save unwritable, truncate a primary, change a checksum, and load a future schema. All cases must retain a known-valid recoverable state or explicitly report that none exists.

Create three profiles and ensure they remain isolated. Verify that restarting cannot duplicate room grants, first-clear bonuses, or talisman charges. Resume checkpoints with every eligible blessing combination. Manual exchange-format tests move a save from Windows to Mac and back in development tools; this does not imply a shipped cross-device synchronization feature.

## 4. Platform matrix

- Windows 11 x64 gaming laptop with RTX 4050-class GPU; hybrid integrated/discrete switching.
- Windows 11 x64 with RTX 4060-class or stronger GPU; high-quality visual reference.
- Windows 11 x64 AMD gaming GPU/laptop; verify vendor independence.
- Apple Silicon base M2/M3-class, 16 GB; prolonged thermal/load test.
- Apple Silicon Pro-class, 18 GB+; 60 FPS target evaluation.
- Recent 8 GB Apple Silicon Mac; explicitly classify supported settings or publish its limitation.
- Supported macOS baseline and current stable macOS on real hardware; do not certify against a beta OS alone.

Device requirements and timings are provisional until evidence exists. See the [platform contract](06-platform-and-performance.md) for the numerical goals. Test cold shader/PSO initialization and repeated camera turns, not only a warm stationary scene.

## 5. Bilingual and accessibility QA

Switch English ↔ Simplified Chinese from the title screen, settings, paused play, dialogue, inventory, and reward screen. Persist preference across restart. Test missing-key fallback, placeholders, number formatting, line wrapping, long English words, 100/125/150% text scales, CJK glyphs, punctuation, and controller key labels.

Capture the same screen in both languages at 1280×720, 1920×1080, ultrawide, and a Retina Mac window. No clipping, unreadable text, untranslated player messages, hardcoded English technique names in Chinese UI, or developer log strings exposed to players. All essential information remains visible when flashes, shake, or motion blur are disabled.

## 6. VFX quality gates

The three hero effects pass the stage and silhouette checklist in the art document. Review real-time recordings with multiple effects overlapping, dark and bright backgrounds, camera movement, and controller play. Observe no missing GPU emitters, compile errors, sorting artifacts, extreme bloom, smoke covering safe gaps, or resolution-dependent targeting.

Record p95/p99 frame times and memory; average FPS alone does not pass. Benchmark 15 minutes of representative gameplay and 30 minutes on a fanless Mac. Performance/Low-flash settings preserve dangerous attack markers. High rendering may improve light/shadow detail but cannot add hidden gameplay indicators.

## 7. Distribution tests

Windows: install through the test Store/GDK workflow, launch from expected shell entry points, run offline, save, quit, restart, update over the prior version, uninstall, and reinstall according to intended save retention. Verify there is no accidental Xbox sign-in dependency.

Mac: test a signed and notarized download on a clean account with quarantine/Gatekeeper present; launch from Finder, verify arm64 binary, save and update without resetting profiles, and test controlled shutdown. Do not disable Gatekeeper as the release workaround.

For both: scan actual shipped files, ensure debug/private content is absent, validate credits/licenses, inspect attempted network traffic, and compare store descriptions with enabled functionality.

## 8. Release blockers

P0: crash, progression deadlock, unrecoverable save loss, secret/restricted-source exposure, installation failure, or fundamental platform failure. P1: recurring combat hitch, unreadable telegraph, major bilingual UI failure, broken input action, duplicate currency, or material gameplay disparity between platforms.

No unresolved P0/P1 in the release candidate. Minor P2 visual imperfections can ship only with a recorded rationale and no misleading marketing claim. A passing documentation CI job does not authorize a gameplay release.

## 9. Evidence template

Each verification record includes build hash, date, tester, OS, hardware/driver, preset/resolution, language, steps, expected and actual behavior, duration, timings/memory if relevant, and artifact links. Redact personal paths and user data before posting a public issue. Store restricted certificates and package keys privately.
