# Technical Architecture

## 1. Stack and implementation strategy

Unreal Engine 5.8 release family, C++ gameplay foundations, Blueprint assembly and presentation, Niagara effects, Enhanced Input, UMG/Common UI if useful, and native engine packaging. Pin the exact installed patch and compatible toolchain after the two-platform M0 spike; do not upgrade mid-milestone without a separate branch and regression results.

No mandatory paid engine license, paid-only plugin, external backend, online account, CUDA dependency, or separate renderer. Unreal's RHI selects the platform graphics backend. Keep third-party plugins minimal and require both Win64 and native Mac arm64 support.

The current repository contains planning, examples, and validation tools only. Proposed Unreal paths below are implementation instructions, not existing components.

## 2. Proposed project layout

```text
game/QinglanAscension.uproject
game/Source/QinglanAscension/
  Core/ Combat/ Abilities/ AI/ Progression/ Persistence/ UI/ Platform/
game/Config/
game/Content/Qinglan/
  Characters/ Environments/ Maps/ VFX/ Audio/ UI/ Data/
data/examples/
docs/
packaging/windows/
tools/
```

Original binary Unreal assets use Git LFS. Engine and restricted third-party sources remain outside the public repository. Document an acquisition/import manifest if a contributor needs assets that cannot be redistributed. A public clone must not depend on undocumented private asset paths: provide original placeholder alternatives for development.

## 3. Ownership and components

| Component | Responsibility | Lifetime |
| --- | --- | --- |
| `UQinglanGameInstance` | Bootstrap services, profile selection, travel coordination | Application |
| `UProfileSubsystem` | Permanent state and transaction validation | Game instance |
| `USaveSubsystem` | Versioned saves, backup rotation, migration | Game instance |
| `URunSubsystem` | Expedition seed, rooms, temporary loot/blessings | Expedition |
| `AQinglanCharacter` | Movement, input routing, animation | Actor |
| `UCombatComponent` | Attributes, damage, invulnerability, barriers | Actor |
| `UTechniqueComponent` | Loadout, cooldowns, cast state | Actor |
| `UStatusComponent` | Timed statuses and bounded reactions | Actor |
| `UEncounterDirector` | Wave activation, completion, rewards | Realm world |
| `UVFXCueComponent` | Cosmetic cue selection and pooling | Actor/world |
| `UGraphicsSettingsService` | Capability gating, presets, settings persistence | Application |

One authoritative mutation path updates profile state. UI requests a purchase but never subtracts currency itself. Presentation subscribes to typed events such as `OnDamageResolved`, `OnCastReleased`, `OnRoomCompleted`, and `OnProfileCommitted`. Avoid a global untyped event bus.

## 4. Ability implementation

Use lightweight actor components and data assets for the eight-technique first release. Gameplay Ability System is a possible later migration if complexity justifies it; do not require a large multiplayer framework for an offline prototype.

Technique metadata includes ID, display text key, element, energy cost, cooldown, cast phases, targeting, magnitude, animation soft reference, cue soft reference, and unlock rule. Validate all references during cook. Data changes require a `content_version` increment when selection/reward outcomes change.

Execution order: validate alive/state/resources/target → begin anticipation → release once → commit energy/cooldown → spawn or trace gameplay effect → resolve hits/statuses → publish cues → finish recovery. Use timers based on paused game time, not wall-clock date. Removing a visual effect never cancels its gameplay effect.

## 5. Navigation and AI

Author NavMesh bounds for each realm and certify every route. Use Behavior Trees or a small explicit state machine: Idle, Acquire, Approach, Windup, Attack, Recover, Stagger, Dead. Share sensing and attack arbitration. Permit no more than three ordinary enemies to begin melee windup simultaneously; ranged attacks have their own budget.

Enemy projectiles and spawn portals are pooled with reset contracts. An actor returned to a pool clears timers, victims hit, owner, tags, collision, and remaining lifetime. No stale hit can damage a new expedition. Destroyed doors and temporary barriers do not leave permanent NavMesh holes; Stone Aegis is a character barrier, not a runtime wall.

## 6. Save contract

Use engine `Saved` locations resolved by platform APIs, not the installation directory. Windows and Mac path behavior must be verified in packaged builds, including the Store package. Provide a Show Save Folder action only through the supported desktop platform API. Keep one stable application save-directory identifier even if the display name changes.

Save schema version 1 has `profile_id`, `schema_version`, `content_version`, monotonic `save_sequence`, permanent cultivation/inventory/unlocks/flags, transaction IDs, and an optional expedition checkpoint. A checkpoint includes seed, stream states, room index, acquired blessings, player attributes, loot, and committed-room IDs. Graphics settings are platform-local and never transferred with gameplay progression.

Safe checkpoint boundaries: entrance, completed room reward screen, exit, hub, and trial preparation. There is no mid-animation combat save. On crash in an active room, resume its entrance with the pre-room snapshot; do not respawn already committed rewards. Pausing offers Save and Quit only at a safe boundary; during combat offer Quit to Last Checkpoint with a clear explanation.

### Durable write protocol

1. Snapshot game-thread state and validate invariant bounds.
2. Serialize to a temporary file in the same save directory.
3. Flush, re-open, parse, and verify checksum/sequence. A checksum detects corruption; it is not an anti-cheat guarantee.
4. Preserve the last known valid save as a backup.
5. Replace the primary using platform-supported atomic replacement where available; retain recovery metadata until success.
6. Report completion on the game thread. A failed write must not delete the last valid primary.

Keep three profile slots and two validated backups per profile. Use migration functions `v1_to_v2`, etc., and never overwrite an unsupported newer save. On corrupt primary, offer the latest valid backup with timestamp and progress preview. On disk full or permission failure, show an actionable localized message and allow retry. Do not silently pretend success.

## 7. Portability and data integrity

Runtime state stores stable ASCII IDs, not asset pointers, localized text, absolute paths, or binary memory layouts. Use Unreal serialization with an explicit portable exchange format for manual save transfer. Automated transfer between Windows and Mac is out of scope; a later import/export flow must validate size, schema, ranges, and identifiers before applying data.

Examples in `data/examples` describe the intended contracts, not production Unreal serialization. Validate nonnegative bounded currency, unique technique IDs, referenced rewards, optional stream states, and rank limits. Reject unknown mandatory fields or versions with an explicit migration result; do not silently discard them.

## 8. Build configurations

Development builds enable safe debugging and benchmark maps. Shipping builds remove debug menus, developer commands, test unlocks, verbose logs, and uncooked development content. Use soft references and explicit cook lists for production maps. No dynamic remote code download or custom self-updater.

Windows and Mac build from the same tagged commit. Record engine patch, SDK/toolchain, content version, target architecture, configuration, and checksum. Build on Windows for Windows/GDK and on macOS for Mac signing and native testing; a Mac editor session is not proof of Windows compatibility.

## 9. Security and privacy by design

No login, telemetry network endpoint, ads, anti-cheat driver, or unnecessary OS permission. Logs should not include account names, home-directory paths, raw saves, signing credentials, or store keys. A voluntary support export previews and redacts its contents before the user shares it; there is no automatic upload.

Test third-party default analytics/crash upload settings and disable unsolicited networking in Shipping. Store and OS services have separate data practices; disclose them in the final privacy statement. Keep signing material and GDK package keys outside public Git and public CI artifacts.
