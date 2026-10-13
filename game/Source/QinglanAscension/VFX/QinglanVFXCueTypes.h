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
	// Sword / melee
	const FName SwordArc = "vfx.sword_arc";
	const FName SwordImpact = "vfx.sword_impact";
	// Techniques
	const FName JadeArcRelease = "vfx.jade_sword_arc";
	const FName EmberLanceRelease = "vfx.ember_lance";
	const FName EmberTrail = "vfx.ember_trail";
	const FName FlowingWardBarrier = "vfx.flowing_ward";
	const FName ThunderSealStrike = "vfx.thunder_seal";
	const FName ConductArc = "vfx.conduct_arc";
	const FName StoneAegisBarrier = "vfx.stone_aegis";
	const FName VerdantHeal = "vfx.verdant_pulse";
	const FName MistStepBurst = "vfx.mist_step";
	const FName OrbitingBlades = "vfx.orbiting_blades";
	// World / enemies
	const FName Portal = "vfx.portal";
	const FName TelegraphRing = "vfx.telegraph_ring";
	const FName BossGroundMarker = "vfx.boss_ground_marker";
	const FName DeathDissolve = "vfx.death_dissolve";
}
