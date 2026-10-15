// Technique effect executors: one entry point per EQinglanTargeting delivery.
// The executors implement the "execute by targeting -> resolve hits/statuses
// through the owner/victim UQinglanCombatComponent -> publish cues" tail of
// the cast pipeline owned by UQinglanTechniqueComponent (blueprint §1.2 cast
// pipeline; docs/02 §3, §6). Executors never mutate resources and never start
// cooldowns - the technique component commits energy+cooldown exactly once at
// release (docs/02 §3 "commits energy and cooldown at the release notify").
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Core/QinglanCoreTypes.h"
#include "Abilities/QinglanBlessingLibrary.h"
#include "Abilities/QinglanTechniqueTypes.h"
#include "QinglanTechniqueExecutors.generated.h"

class AQinglanCharacter;
class AQinglanProjectile;
class UQinglanCombatComponent;
class UNiagaraSystem;

/**
 * Cosmetic cue description (docs/04 §"Each technique has five stages ... Every
 * cue set accepts CastId, SourceTransform, TargetPoint, Element, Intensity,
 * QualityTier, AccessibilityFlags"). Cues are publish-only: a missing or
 * culled emitter never suppresses damage or warnings (blueprint §1.7).
 */
USTRUCT(BlueprintType)
struct FQinglanCueEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FQinglanCastId CastId;
	UPROPERTY(BlueprintReadOnly) FTransform SourceTransform = FTransform::Identity;
	UPROPERTY(BlueprintReadOnly) FVector TargetPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) EQinglanElement Element = EQinglanElement::None;
	UPROPERTY(BlueprintReadOnly) float Intensity = 1.f;
	/** Stage tag: anticipation/release/travel/impact/decay (docs/04 stages). */
	UPROPERTY(BlueprintReadOnly) FName StageId = NAME_None;
	/** Quality gate id resolved through UQinglanGraphicsSettingsService. */
	UPROPERTY(BlueprintReadOnly) FName FeatureId = NAME_None;
	/** True when low-flash mode must retain a readable silhouette. */
	UPROPERTY(BlueprintReadOnly) bool bWarningRelevant = false;
};

/** Everything one release needs; built by the technique component. */
USTRUCT(BlueprintType)
struct FQinglanTechniqueCastContext
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<AActor> Caster;
	/** Treated as read-only by every executor; non-const because UHT does not
	 *  support TObjectPtr<const T> in reflected properties. */
	UPROPERTY(BlueprintReadOnly) TObjectPtr<UQinglanTechniqueDataAsset> Technique = nullptr;
	UPROPERTY(BlueprintReadOnly) FQinglanCastId CastId;
	/** Free-aim point from the camera, re-traced from the cast socket
	 *  (docs/02 §2 "Free aiming traces from the camera to derive an aim
	 *  point, then from the casting socket toward that point"). */
	UPROPERTY(BlueprintReadOnly) FVector AimPoint = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) FVector AimDirection = FVector::ForwardVector;
	/** Aggregated expedition blessing modifiers (docs/03 §5). */
	UPROPERTY(BlueprintReadOnly) FQinglanBlessingModifiers Blessings;
	/** Tier-2 upgrade multiplier on raw damage/heal/barrier magnitude
	 *  (docs/03 §3: +15%; 1.0 at tier 1). */
	UPROPERTY(BlueprintReadOnly) float TierMultiplier = 1.f;
};

/**
 * Static executor library. Deterministic: no FMath::RandRange - any spread or
 * selection uses fixed ordering; seeds belong to the run subsystem only.
 */
UCLASS()
class UQinglanTechniqueExecutors : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Dispatches on Technique->Targeting; returns false when the technique
	 *  asset is missing or the mode is unsupported. */
	UFUNCTION(BlueprintCallable) static bool Execute(const FQinglanTechniqueCastContext& Context);

	// --- Individual executors (docs/02 §6 table) ---
	/** jade_sword_arc: 1,200 cm arc projectile (pierce, arc half-angle). */
	static void ExecuteSelfArc(const FQinglanTechniqueCastContext& Context);
	/** ember_lance: single projectile, applies Burn on hit (docs/02 §7). */
	static void ExecuteProjectile(const FQinglanTechniqueCastContext& Context);
	/** flowing_ward 25 pt / 5 s; stone_aegis 35 pt, no geometry blocker. */
	static void ExecuteSelfBarrier(const FQinglanTechniqueCastContext& Context);
	/** verdant_pulse: heal over time, bounded by max health. */
	static void ExecuteSelfHeal(const FQinglanTechniqueCastContext& Context);
	/** thunder_seal: 300 cm radius ground strike at the aim point; Conduct is
	 *  resolved inside the damage pipeline (Thunder vs Wet, docs/02 §7). */
	static void ExecuteGroundStrike(const FQinglanTechniqueCastContext& Context);
	/** mist_step: enhanced dodge, 600 cm travel (docs/02 §6). */
	static void ExecuteSelfDash(const FQinglanTechniqueCastContext& Context);
	/** orbiting_blades: 6 pulses, max 2 targets/pulse (docs/02 §6). */
	static void ExecuteSelfPulses(const FQinglanTechniqueCastContext& Context);

	// --- Shared helpers ---
	/** Raw damage expression pre-mitigation: Raw = Power * Coef + Flat, then
	 *  tier and blessing raw-damage percent applied once (docs/02 §5,
	 *  docs/03 §3-4). Mitigation happens in the victim combat component. */
	static float ComputeRawDamage(const FQinglanTechniqueCastContext& Context);

	/** Effective magnitude (barrier points / heal / dash distance) with tier
	 *  and blessing bonuses applied once (docs/03 §3 tier-2 +15%). */
	static float ComputeMagnitude(const FQinglanTechniqueCastContext& Context);

	/** Gathers valid enemy victims inside a sphere; deterministic order
	 *  (distance then name). Owner is always excluded. */
	static void GatherVictims(const FQinglanTechniqueCastContext& Context, const FVector& Center,
		float RadiusCm, TArray<AActor*>& OutVictims);

	/** Applies one audited damage event through the victim's combat
	 *  component; returns false when there is no authoritative target. */
	static bool ApplyDamageToVictim(const FQinglanTechniqueCastContext& Context, AActor* Victim,
		float RawMagnitude, int32 HitOrdinal);

	/** Publishes a cosmetic cue event: records it and, when the technique's
	 *  soft cue reference resolves and quality allows, spawns the Niagara
	 *  system. Cosmetic only (docs/04; blueprint §1.7). WorldContextObject
	 *  lets cue-only callers (e.g. pooled effect actors) publish without a
	 *  full cast context. */
	static void PublishCue(const FQinglanTechniqueCastContext& Context, const FQinglanCueEvent& Cue, const UObject* WorldContextObject = nullptr);

	// --- Tunables (named constants, doc-cited; all overridable via config in
	//     a future data pass, kept here so no magic numbers hide in code) ---
	static constexpr float ArcRangeCm = 1200.f;          // docs/02 §6 jade_sword_arc
	/** Cosmetic blade-sweep width; the authoritative hit volume is the pierced
	 *  projectile path. FIXME-CONTRACT: arc width unspecified in docs. */
	static constexpr float ArcHalfAngleDeg = 60.f;
	static constexpr float ArcSpeedCmPerSec = 2400.f;    // FIXME-CONTRACT: unspecified; tunable
	static constexpr float ProjectileSpeedCmPerSec = 3000.f; // FIXME-CONTRACT: ember_lance speed unspecified; tunable
	static constexpr float ProjectileMaxRangeCm = 2000.f;    // FIXME-CONTRACT: unspecified; tunable
	static constexpr float BarrierDurationFlowingWard = 5.f; // docs/02 §6 flowing_ward "5 seconds"
	static constexpr float BarrierDurationStoneAegis = 6.f;  // FIXME-CONTRACT: stone_aegis duration unspecified; tunable
	static constexpr float GroundStrikeRadiusCm = 300.f;     // docs/02 §6 thunder_seal
	static constexpr float DashDistanceCm = 600.f;           // docs/02 §6 mist_step travel
	static constexpr float DashDurationSeconds = 0.45f;      // docs/02 §2 dodge duration
	static constexpr float DashHeightVelocityCmPerSec = 50.f;// FIXME-CONTRACT: unspecified minor lift, tunable
	static constexpr float DashWetRadiusCm = 220.f;          // FIXME-CONTRACT: capsule-side Wet reach, tunable
	static constexpr int32 OrbitPulseCount = 6;              // docs/02 §6 orbiting_blades
	static constexpr float OrbitPulseInterval = 0.5f;        // FIXME-CONTRACT: cadence unspecified; tunable
	static constexpr float OrbitPulseRadiusCm = 350.f;       // FIXME-CONTRACT: unspecified; tunable
	static constexpr float EffectLifetimeSafety = 6.f;       // pool safety expiry, beyond max effect duration
};
