// C++ constant specs for the six ordinary archetypes and three bosses.
// Numeric contracts from docs/02-combat-and-camera.md sections 8-9 and
// docs/dev/implementation-blueprint.md section 2.4; every authored (doc
// unspecified) value is marked. Specs are plain C++ constants: they are the
// behavioral authority at runtime; UQinglanEnemyDataAsset instances carry the
// tunables that the data pipeline needs (and are filled from these specs).
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"
#include "AI/QinglanEnemyTypes.h"

/**
 * How one attack finds its victims at the release notify. Derived from the
 * behavior descriptions in docs/02 section 8 (archetypes) and section 9
 * (bosses); shapes keep the same telegraph + impact silhouette at Low quality
 * (docs/06 section 6) because they are logical checks, not visual effects.
 */
enum class EQinglanAttackShape : uint8
{
	/** Sword swing / wolf lunge in a frontal arc around the attacker. */
	MeleeArc,
	/** Slow frontal cone (Stone Sentinel wedge slam, boss sweeping blade). */
	MeleeCone,
	/** Single aimed projectile (Hollow Archer arrow, spirit bolt, fire lance). */
	AimedProjectile,
	/** Delayed ground ring marked at windup start (Rootbound Keeper vines). */
	GroundRing,
	/** Expanding ring with deterministic safe gaps (Ashen Guardian shockwave). */
	GroundRingGaps,
	/** Attacker dashes; proximity damage during the active window (charge strike). */
	ChargeDash,
	/** Several delayed ground strikes, each >= 0.9 s warning (Broken Guardian storm). */
	MultiGroundStrikes
};

/** One archetype or boss: full constant spec with doc citations. */
struct FQinglanArchetypeSpec
{
	/** Stable ASCII id used by state, saves, and cue tables. */
	FName EnemyId;

	// --- Core attributes (docs/02 section 8 roster table; section 9 bosses) ---
	float Health = 45.f;
	float Defense = 5.f;
	float MoveSpeed = 300.f;       // authored where doc gives no speed
	float AttackRange = 150.f;     // cm, release-check radius
	float AttackDamage = 10.f;     // flat part of the raw magnitude
	float AttackCoefficient = 0.5f; // scales the attacker's TechniquePower

	// --- Attack cycle: windup -> active -> recover (scaffold defaults) ---
	float AttackWindup = 0.65f;
	float AttackActive = 0.2f;
	float AttackRecover = 0.5f;

	// --- Telegraph (duration = authored roster telegraph; docs/02 section 8) ---
	float TelegraphDuration = 0.65f;
	FName TelegraphCueId = NAME_None;

	bool bRanged = false;
	bool bRootImmune = false;      // Root downgrades to 10% slow on bosses; docs/02 section 7
	float AggroRadius = 1500.f;    // sensing radius; docs/dev blueprint section 1.3

	/** Element and status carried by this archetype's hit (Conduct/Burn rules in docs/02 section 7). */
	EQinglanElement Element = EQinglanElement::None;
	EQinglanStatus StatusOnHit = EQinglanStatus::None;

	EQinglanAttackShape AttackShape = EQinglanAttackShape::MeleeArc;

	// --- Behavior flags from the roster descriptions (docs/02 section 8) ---
	bool bCircleBeforeAttack = false; // Spirit Wolf "circles, then lunges"
	float OrbitRadius = 0.f;          // cm; circle at this range before committing
	int32 ComboCount = 1;             // Corrupted Disciple "two sword hits"
	bool bRetreatAfterCombo = false;  // "...then retreats"
	float RetreatSeconds = 0.f;       // authored retreat duration
	bool bFixedPerch = false;         // Ember Wisp "from fixed perches": never moves
	float PreferredRange = 0.f;       // ranged comfort distance (cm)
	float MinComfortRange = 0.f;      // ranged enemies back away inside this (cm)
	float LungeSpeed = 0.f;           // >0: launch toward the target at release
	float GroundRingRadius = 0.f;     // delayed-root / shockwave / strike radius (cm)
	float ProjectileSpeed = 0.f;      // authored: cm/s for AimedProjectile shapes

	// --- Boss-only (docs/02 section 9; blueprint section 2.4) ---
	bool bBoss = false;
	float PhaseTwoAtHealthFraction = 0.5f; // "at 50% HP": shorter idles, same telegraphs
	float ArenaDiameter = 4500.f;          // cm; Bamboo Guardian 45 m arena
	/** Longest allowed invulnerable/untargetable window; Ashen "no immunity phase over 8 s". */
	float MaxImmunityPhaseSeconds = 0.f;
	/** Scripted pattern list; weighted pick per cycle from the run's seeded stream. */
	TArray<FQinglanBossPattern> Patterns;
};

/** Stable pattern ids shared by catalog specs and the AI controller. */
// (kept as inline constexpr FName builders because FName is not constexpr-constructible)
namespace QinglanPatternIds
{
	QINGLANASCENSION_API FName SweepingBlade();    // "sweeping_blade"
	QINGLANASCENSION_API FName DelayedRoots();     // "delayed_roots"
	QINGLANASCENSION_API FName SpiritBolts();      // "spirit_bolts"
	QINGLANASCENSION_API FName FireLance();        // "fire_lance"
	QINGLANASCENSION_API FName RingShockwave();    // "ring_shockwave"
	QINGLANASCENSION_API FName ChargingStrike();   // "charging_strike"
	QINGLANASCENSION_API FName StormBoltVolley();  // "storm_bolt_volley"
	QINGLANASCENSION_API FName SpiritRend();       // "spirit_rend"
}

/**
 * Static catalog of every enemy spec in the game. Pure C++; no instances are
 * created until a data asset is requested, so automation tests can assert
 * values headlessly.
 */
namespace QinglanEnemyCatalog
{
	/** All six ordinary archetypes (docs/02 section 8). */
	QINGLANASCENSION_API const TArray<FQinglanArchetypeSpec>& GetAllArchetypes();

	/** The three bosses (docs/02 section 9; Broken Guardian for the Golden Core trial). */
	QINGLANASCENSION_API const TArray<FQinglanArchetypeSpec>& GetAllBosses();

	/** Finds an archetype or boss by id; nullptr when unknown. */
	QINGLANASCENSION_API const FQinglanArchetypeSpec* FindSpec(FName EnemyId);

	/**
	 * Creates a transient UQinglanEnemyDataAsset (UQinglanBossDataAsset when
	 * the spec carries patterns) filled from the constant spec, rooted so it
	 * survives GC for the session. Returns nullptr on invalid input.
	 */
	QINGLANASCENSION_API UQinglanEnemyDataAsset* CreateDataAssetFromSpec(const FQinglanArchetypeSpec& Spec, UObject* Outer);

	/** Resolves the logical attack shape of one boss pattern step. */
	QINGLANASCENSION_API EQinglanAttackShape ShapeOfPattern(const FQinglanBossPattern& Pattern);
}
