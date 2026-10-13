// VFX cue identifiers and quality tiers.
#pragma once

#include "CoreMinimal.h"
#include "QinglanVFXCueTypes.generated.h"

/** Cosmetic quality tiers mirror the graphics presets (docs/06 section 3). */
UENUM(BlueprintType)
enum class EQinglanVFXQuality : uint8
{
	Low,      // silhouette + minimal sprites, no dynamic lights from cues
	Balanced, // sprites + lights, halved densities
	High,     // full density, lights, trails
	Ultra     // reserved
};

namespace QinglanVFXCues
{
	// Extern declarations (UE builds error on namespace-scope FName global
	// constructors in headers); definitions live in QinglanVFXCueComponent.cpp.
	// Sword / melee
	extern const FName SwordArc;           // "vfx.sword_arc"
	extern const FName SwordImpact;        // "vfx.sword_impact"
	// Techniques
	extern const FName JadeArcRelease;     // "vfx.jade_sword_arc"
	extern const FName EmberLanceRelease;  // "vfx.ember_lance"
	extern const FName EmberTrail;         // "vfx.ember_trail"
	extern const FName FlowingWardBarrier; // "vfx.flowing_ward"
	extern const FName ThunderSealStrike;  // "vfx.thunder_seal"
	extern const FName ConductArc;         // "vfx.conduct_arc"
	extern const FName StoneAegisBarrier;  // "vfx.stone_aegis"
	extern const FName VerdantHeal;        // "vfx.verdant_pulse"
	extern const FName MistStepBurst;      // "vfx.mist_step"
	extern const FName OrbitingBlades;     // "vfx.orbiting_blades"
	// World / enemies
	extern const FName Portal;             // "vfx.portal"
	extern const FName TelegraphRing;      // "vfx.telegraph_ring"
	extern const FName BossGroundMarker;   // "vfx.boss_ground_marker"
	extern const FName DeathDissolve;      // "vfx.death_dissolve"
}
