# Art Direction, Animation, Audio, and VFX

## 1. Visual target

The owner's target is spell presentation comparable to contemporary AAA games. Treat this as a quality aspiration for a small number of hero effects, not a claim that the whole game already matches a named production. Validate in motion at gameplay distance, in packaged Windows and Mac builds, and under combat load.

Use realistic rock, water, foliage, cloth, metal, lighting, and shadows with restrained fantasy color. Magic should have convincing motion, depth, energy concentration, material response, and impact. A bright texture alone is not sufficient. The art direction is an original Chinese-inspired mountain sanctuary, not a reconstruction of an existing game's visual identity.

### Hero effect priorities

1. **Jade Sword Arc / Orbiting Blades:** gathering energy along the blade, directional ribbon, solid inner silhouette, broken edge turbulence, sparks at contact, ground response, and restrained air distortion.
2. **Heavenly Tribulation:** storm buildup, volumetric cloud shapes, clear ground marker, stepped branching lightning, short physically plausible light response, impact crater decal, smoke and debris, and layered thunder.
3. **Ember Lance:** compact incandescent core, turbulent flame envelope, sparse embers, pressure ring at release, impact flash, lingering smoke, and scorch decal.

These three receive dedicated look-development reviews before the eight-technique roster expands. Avoid expensive character cinematics while hero spells are unfinished.

## 2. Art production rules

Build a modular kit: stone stairs, terrace, gate, shrine, pillar, cliff segment, rock cluster, bamboo cluster, bridge, and courtyard wall. Reference real construction and material scale; 1 Unreal unit = 1 cm. Use consistent texel density, PBR material inputs, roughness variation, and controlled wetness. Author separate gameplay collision and navigation geometry.

Use 2K texture sets for common props, up to 4K only for close hero surfaces. Reuse material instances and trim sheets. Dense geometry must have ordinary LODs where Nanite is unavailable or not approved. Do not assume Nanite eliminates material, shadow, animation, or memory cost. M0 tests decide whether it is enabled per platform.

One humanoid skeleton supports the player and human enemies. Prototype with original/simple meshes or properly licensed development assets. Robes use bounded cloth simulation or authored secondary motion; disable costly cloth in lower presets while preserving shape. No MetaHuman dependency or facial capture pipeline at launch.

## 3. Spell construction contract

Each technique has five stages: anticipation, release, travel/sustain, impact, and decay. Gameplay defines stage times; animations and Niagara consume them. Every cue set accepts `CastId`, `SourceTransform`, `TargetPoint`, `Element`, `Intensity`, `QualityTier`, and `AccessibilityFlags`.

Use Niagara for ribbons, mesh particles, sparks, beams, and controlled GPU simulation. Combine opaque/masked geometry with limited translucency. Smoke may use authored flipbooks or precomputed volume textures; full real-time volumetric fluid simulation is optional research, never a release dependency. Baked simulations can still look convincing when properly lit and timed.

Gameplay traces and damage remain on the CPU. GPU particles are visual. A missing GPU emitter cannot suppress a damage event or warning. Never use GPU particle collision callbacks as the only source of combat truth.

### Review checklist for each hero effect

- Recognizable silhouette at 15 m and in a busy scene.
- At least 0.2 seconds of anticipatory energy buildup for high-impact spells.
- Main direction, velocity, and scale agree with gameplay range and impact radius.
- Light pulse responds to nearby surfaces without relying on delayed emissive GI alone.
- Sound transient and impact visual align within 50 ms on the test capture.
- Decay does not obscure the next enemy telegraph.
- The same spell remains identifiable in Mac and Windows captures.
- Quality reduction preserves the core mesh/ribbon/beam and warning outline.
- No uncontrolled full-screen flashes, excessive camera shake, or motion blur requirement.

## 4. Prototype effects budget

Initial budgets are guardrails for profiling, not universal capacities. Measure the actual compiled effect at the chosen resolution.

| Budget | Performance | Balanced | High |
| --- | --- | --- | --- |
| Cosmetic GPU particles visible | 15,000 | 35,000 | 60,000 |
| Concurrent major spell systems | 4 | 6 | 8 |
| Shadowless transient effect lights | 2 | 4 | 6 |
| Distortion layers overlapping at a pixel | 0 | 1 | 2 |
| Hero effect texture size | Up to 1K | Up to 2K | Up to 2K |
| Smoke treatment | Sparse flipbooks | Layered flipbooks | Bounded precomputed volume option |

Never apply these limits to required warnings. Split critical telegraphs into lightweight components independent of cosmetic Niagara culling. Assign effect types, fixed bounds, pooling, visibility/distance culling, and explicit completion lifetimes. Drop ambient sparks first, distant cosmetic systems second, secondary impact debris third. Player attack core and enemy warnings have highest priority.

Target total VFX GPU time: <=2.0 ms within a 16.67 ms frame on the Windows reference laptop; <=3.5 ms within a 33.33 ms frame on the base Mac reference. These are goals to prove. Particle count alone is not a performance measure: translucent coverage, material complexity, lights, and bandwidth can dominate.

## 5. Lighting and rendering

Use a fixed time of day per realm; no dynamic weather calendar. High uses evaluated Lumen settings where supported. Hardware ray tracing is an optional Windows enhancement after profiling; it is not the definition of realistic effects. Mac release rendering uses approved Metal features and does not depend on experimental hardware ray tracing.

For scaled presets, preserve authored sky, fog silhouettes, reflection environment, key lights, and material response. A controlled set of temporary non-shadow-casting lights gives predictable immediate spell illumination. Do not illuminate the full scene with dozens of particle lights. Avoid reflection-heavy polished surfaces across large areas.

Reference material: [Epic's Lumen performance guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine). Its implementation guidance supports profiling and scalability decisions; the numerical budgets above are project choices.

## 6. Animation

Required player set: idle, locomotion blend, turn, three attacks, dodge in four directions, four reusable cast archetypes, interaction, hit reaction, death, meditation, and breakthrough reaction. Use animation montages and notifies for windows; root motion is limited to attacks/dodges with collision-aware behavior.

Retargeted licensed animations require provenance and shipping permission. Do not upload their editable source if redistribution is restricted. Test foot sliding, hand placement, robe clipping, sword trajectory, and animation/VFX synchronization from free camera and lock-on views.

## 7. Audio

Music: calm sanctuary motif, exploration layer, combat layer, boss variation, tribulation build, and ending resolution. Traditional-inspired timbres may be used with original or licensed recordings. No copyrighted commercial soundtrack.

Spells use launch, flight, impact, and decay layers. Thunder timing reflects distance only for ambience; gameplay cues remain immediate. Limit simultaneous similar impact voices to prevent masking. Provide separate master, music, effects, and UI volume controls, optional dynamic range reduction, and visible important audio cues. Voice acting is not required.

## 8. Art acceptance and evidence

Create `VFXBenchmark` in M0 with courtyard lighting, dark-rock background, rain/fog, close and distant cameras, and worst-case overlapping spells. Record fixed camera paths with the same content version on both platforms. Review 30-second real-time clips, screenshots, Shader Complexity/Quad Overdraw views, GPU captures, and texture memory.

Visual claims in the store use real packaged gameplay. Offline-rendered promotional shots may be labeled artwork but cannot substitute for in-game proof of frame rate or functionality.
