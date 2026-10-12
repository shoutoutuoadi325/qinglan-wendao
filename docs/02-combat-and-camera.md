# Combat, Enemies, and Camera

All numbers are prototype defaults in Unreal units: centimeters, seconds, and degrees. Tune against a packaged build. The data examples and this document must change together.

## 1. Input map

| Action | Keyboard and mouse | Controller default |
| --- | --- | --- |
| Move | WASD | Left stick |
| Camera | Mouse | Right stick |
| Basic sword attack | Left mouse | X / Square |
| Dodge | Space | B / Circle |
| Lock target | Middle mouse | Right stick click |
| Cycle target | Mouse wheel | Right stick flick while locked |
| Technique slots 1–4 | 1, 2, 3, 4 | Hold left trigger + face button |
| Talisman slots 1–2 | Q, R | D-pad left/right |
| Interact | E | A / Cross |
| Pause | Escape | Menu / Options |

Use Enhanced Input mapping contexts for gameplay and UI. Ship complete keyboard/mouse support on both platforms; target controller support as a release gate, using a wired controller on Mac. All actions are rebindable. Detect conflicts and offer Swap, Replace, or Cancel. Never show a Windows-only key label on Mac. Controller glyphs depend on the detected controller family.

## 2. Movement and camera

Walking speed: 420 cm/s. No sprint stamina system at launch. Dodge travels approximately 450 cm over 0.45 seconds, costs 25 stamina, and has an invulnerability window from 0.10 to 0.28 seconds. It cannot pass through walls. Use capsule collision and controlled movement/root motion, not teleportation through obstacles.

Third-person camera: 350 cm spring-arm distance, 60 cm shoulder offset, 75-degree FOV. Provide FOV range 65–95. Sphere-sweep camera obstruction each frame and interpolate inward quickly, outward slowly. Tight spaces never hide the player for more than a moment; fade eligible foliage obstructors, not all geometry.

Lock-on range: 2,000 cm; require visibility at acquisition. Drop the target after death, or after 1.5 seconds continuously outside the permitted range/visibility. Use a weighted screen-distance and world-distance score. Lock-on does not force a spell through a wall; projectiles still collide. Free aiming traces from the camera to derive an aim point, then from the casting socket toward that point so shoulder-camera offsets do not cause wall penetration.

## 3. Combat state machine

`Idle / Move → AttackWindup → AttackActive → AttackRecovery → Idle`, with separate `Casting`, `Dodging`, `HitReaction`, and `Dead` states. Priority: Dead > forced HitReaction > allowed DodgeCancel > current committed action > buffered input.

Buffer one attack/dodge input for 0.15 seconds. Sword windup may be canceled by dodge before the active hit window; active hits cannot be canceled; recovery becomes dodge-cancelable after its first 0.10 seconds. Casting commits energy and cooldown at the release notify, not at button press. Interrupted windup consumes neither; an effect already released is not refunded.

Prevent reentrant ability release with a unique cast ID. Every damage application includes attacker, target, cast ID, hit ordinal, source technique, element, raw magnitude, and timestamp. Reject duplicate hit keys for each victim. Multi-tick effects deliberately increment the ordinal.

## 4. Sword combo

| Strike | Windup | Active | Recovery | Power multiplier |
| --- | --- | --- | --- | --- |
| Cut 1 | 0.14 | 0.10 | 0.22 | 0.8 |
| Cut 2 | 0.16 | 0.10 | 0.24 | 0.9 |
| Cut 3 | 0.22 | 0.12 | 0.32 | 1.3 |

Combo continuation window: 0.35 seconds after recovery begins. Sword hit checks sweep between previous and current blade sockets, with substeps when movement distance is large. Animation notifies open and close the hit window. A single swing damages a victim once. Sword hits restore 4 Spiritual Energy per unique target, capped at 8 per swing; hitting training targets cannot generate campaign resources.

## 5. Attributes and damage

Initial player values: Health 100, Spiritual Energy 100, Stamina 100, Technique Power 20, Defense 10. Energy regenerates 4/s after one second without releasing a technique. Stamina regenerates 20/s after 0.8 seconds without spending stamina. No random critical damage at launch; readable behavior is easier to tune.

```text
RawDamage = TechniquePower * Coefficient + FlatDamage
MitigatedDamage = RawDamage * 100 / (100 + max(0, Defense))
FinalDamage = round(max(1, MitigatedDamage * ReactionMultiplier * DifficultyMultiplier))
```

Prototype example: Power 20, coefficient 1.5, flat damage 5, Defense 10, no reaction → round(35 * 100 / 110) = 32. Damage rounding uses nearest integer with halves rounded away from zero in runtime code. Health clamps to [0, MaxHealth]. A damage event is resolved once; a visual emitter never applies damage.

## 6. Techniques

| ID | Name | Element | Energy | Cooldown | Coefficient | Flat | Delivery |
| --- | --- | --- | --- | --- | --- | --- | --- |
| jade_sword_arc | Jade Sword Arc | Metal | 12 | 2.5 | 1.5 | 5 | 1,200 cm arc projectile |
| ember_lance | Ember Lance | Fire | 20 | 6.0 | 2.0 | 5 | Single projectile; applies Burn |
| flowing_ward | Flowing Ward | Water | 18 | 8.0 | 0 | 0 | 25-point barrier for 5 seconds |
| thunder_seal | Thunder Seal | Lightning | 24 | 7.0 | 2.2 | 8 | 300 cm radius ground strike |
| stone_aegis | Stone Aegis | Earth | 22 | 10.0 | 0 | 0 | 35-point barrier; no geometry blocker |
| verdant_pulse | Verdant Pulse | Wood | 25 | 12.0 | 0 | 0 | Heal 20 over 4 seconds |
| mist_step | Mist Step | Water | 15 | 9.0 | 0 | 0 | Enhanced dodge, 600 cm travel |
| orbiting_blades | Orbiting Blades | Metal | 30 | 14.0 | 0.6 | 2 | Six pulses, at most 2 targets/pulse |

Lightning is a technique category, not a sixth member of the Five Elements. The Five Elements remain Metal, Wood, Water, Fire, and Earth. Each technique has an explicit targeting rule, release time, animation, cue set, and fallback visual. Healing is bounded by maximum health. Barrier damage is absorbed before health; the stronger barrier replaces a weaker one and barriers never stack.

## 7. Status interactions

- **Wet:** 5 seconds, applied by specified Water attacks or explicit gameplay puddles; cosmetic rain does not apply it.
- **Burn:** 3 seconds; three 4-damage ticks; refresh duration without stacking; maximum three ticks after the latest refresh.
- **Root:** one second, ordinary enemies only; bosses receive a 10% slow for one second.
- **Conduct:** Thunder Seal against Wet deals 1.25× immediate damage, consumes Wet, and adds a brief readable arc cue. It cannot recursively trigger itself or jump between an unlimited number of targets.

Water removes Burn, consuming one application; Fire removes Wet before its normal damage calculation and does not get a damage bonus. No full five-by-five reaction matrix in 1.0. Define precedence centrally: immunity → barrier/health damage → reaction consumption → status application → cue. Add new interactions only when their gameplay, performance, and tutorial cost are justified.

## 8. Enemy roster

| Archetype | HP / Defense | Behavior | Telegraph and response |
| --- | --- | --- | --- |
| Spirit Wolf | 45 / 5 | Circles, then lunges | 0.65 s shoulder dip; dodge sideways |
| Corrupted Disciple | 70 / 10 | Two sword hits then retreats | Raised weapon; punish recovery |
| Hollow Archer | 40 / 5 | Fires single aimed arrows | 0.9 s bow glow; strafe or close distance |
| Stone Sentinel | 120 / 25 | Slow cone slam | 1.2 s ground wedge; move behind |
| Ember Wisp | 35 / 0 | Launches fire bolts from fixed perches | 0.8 s bright core; dodge and cast |
| Rootbound Keeper | 90 / 15 | Delayed ground roots | 1.0 s vine ring; leave marked area |

Use one shared humanoid rig where possible. Archetypes need unique combat silhouettes, not six expensive bespoke skeletons. Ordinary AI updates decisions at 5–10 Hz; attack execution and movement run normally. Stagger immunity prevents permanent stun locks. Offscreen attacks must use an audible cue and may not begin a close-range instant strike.

## 9. Bosses and tribulations

**Bamboo Guardian:** 900 HP, Defense 15; three patterns: sweeping blade, delayed roots, and two aimed spirit bolts. At 50% HP, shorten idle breaks rather than hide telegraphs. Arena diameter: 45 m. Aim for 3–4 minutes at intended progression.

**Ashen Guardian:** 1,400 HP, Defense 20; fire lance, circular shockwave with safe gaps, and a charging strike. At 50%, add intermittent embers outside marked safe lanes. No immunity phases longer than eight seconds.

**Foundation trial:** 90 seconds, three telegraphed lightning waves and a final elite construct. **Golden Core trial:** 150 seconds, three storm phases plus the Broken Guardian. Each lightning strike has a persistent ground outline and at least 0.9 seconds of warning. Winning requires surviving all phases and defeating the construct/guardian, not simply waiting out a timer.

Low quality and low-flash mode retain warning rings, safe gaps, cast audio, and hit timing. Defeat restarts the trial from preparation, with no loss of banked items or previously achieved cultivation.
