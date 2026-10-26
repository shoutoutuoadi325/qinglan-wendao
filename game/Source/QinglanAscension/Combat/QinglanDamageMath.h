// Pure damage/status math shared by UQinglanCombatComponent, UQinglanStatusComponent,
// and the automation specs. No state, no world access, fully deterministic.
//
// Authoritative source: docs/02-combat-and-camera.md sections 5 and 7, plus
// docs/08-quality-and-testing.md section 2 (acceptance expectations).
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"

/** Named tunables cited from the design docs; kept in one place for review. */
namespace QinglanCombatTunables
{
	/**
	 * DifficultyMultiplier ships as 1.0: no difficulty system exists in the doc
	 * set. docs/dev/implementation-blueprint.md "Resolved open questions" item:
	 * "Damage formula includes DifficultyMultiplier - no difficulty system is
	 * specified anywhere in the doc set (ship as 1.0 and document)".
	 */
	constexpr float DefaultDifficultyMultiplier = 1.0f;

	/** Conduct: Thunder vs Wet deals one 1.25x application and consumes Wet. docs/02 §7. */
	constexpr float ConductMultiplier = 1.25f;

	/** Wet duration 5 s. docs/02 §7. */
	constexpr float WetDurationSeconds = 5.f;

	/** Burn: 3 s total, three ticks. docs/02 §7. */
	constexpr float BurnDurationSeconds = 3.f;
	/** Burn tick damage: 4 per tick. docs/02 §7. */
	constexpr float BurnTickDamage = 4.f;
	/** Burn tick count: exactly three, refresh does not add ticks. docs/02 §7. */
	constexpr int32 BurnTickCount = 3;
	/** Burn cadence: one tick per second (3 ticks across 3 s). docs/02 §7. */
	constexpr float BurnTickIntervalSeconds = 1.f;

	/** Root: 1 s on ordinary enemies. docs/02 §7. */
	constexpr float RootDurationSeconds = 1.f;
	/** Bosses are never rooted; they take a 10% slow for the Root duration. docs/02 §7. */
	constexpr float BossSlowPercent = 10.f;

	/** Sword-hit energy restore per unique target. docs/02 §4. */
	constexpr float SwordEnergyPerTarget = 4.f;
	/** Sword-hit energy restore cap per swing. docs/02 §4. */
	constexpr float SwordEnergyCapPerSwing = 8.f;

	/** Actor tag marking training targets: hits on them grant no campaign resources. docs/02 §4. */
	constexpr ANSICHAR QinglanTrainingTargetTag[] = "Qinglan.TrainingTarget";
	/** Actor tag marking bosses (Root downgrades to a slow). docs/02 §7. */
	constexpr ANSICHAR QinglanBossTag[] = "Qinglan.Boss";
}

/** Pure functions; safe to call from any thread, usable by headless automation specs. */
namespace QinglanDamageMath
{
	/**
	 * RawDamage = TechniquePower * Coefficient + FlatDamage.
	 * docs/02 §5.
	 */
	QINGLANASCENSION_API float ComputeRawDamage(float TechniquePower, float Coefficient, float FlatDamage);

	/**
	 * MitigatedDamage = RawDamage * 100 / (100 + max(0, Defense)).
	 * Negative defense is clamped to zero so it never becomes a damage amplifier.
	 * docs/02 §5.
	 */
	QINGLANASCENSION_API float ComputeMitigatedDamage(float RawDamage, float Defense);

	/**
	 * FinalDamage = round_half_away_from_zero(max(1, Mitigated * ReactionMultiplier * DifficultyMultiplier)).
	 * Minimum one point of damage on any accepted hit. docs/02 §5.
	 * Prototype check (docs/02 §5): Power 20, coef 1.5, flat 5, Defense 10, no
	 * reaction -> round(35 * 100 / 110) = 32.
	 */
	QINGLANASCENSION_API float ComputeFinalDamage(float MitigatedDamage, float ReactionMultiplier, float DifficultyMultiplier);

	/**
	 * Nearest integer with halves rounded away from zero (runtime damage rounding
	 * rule, docs/02 §5). FMath::RoundToInt is banker's rounding on some
	 * platforms, so the rule is implemented explicitly.
	 */
	QINGLANASCENSION_API float RoundHalfAwayFromZero(float Value);

	/**
	 * Hit audit key: (CastId, victim, ordinal) -> uint64. The victim is encoded
	 * through VictimKeyId (the resolved owner's ObjectKey or FObjectKey hash);
	 * duplicates of the same triple must be rejected. docs/02 §3,
	 * docs/05-technical-architecture.md section "Execution order".
	 */
	QINGLANASCENSION_API uint64 MakeHitKey(const FQinglanCastId& CastId, uint32 VictimKeyId, int32 HitOrdinal);
}
