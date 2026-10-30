// Pooled gameplay-effect actor for the techniques module. Hosts four
// tick-driven delivery modes: linear projectile (jade_sword_arc, ember_lance),
// orbiting pulse aura (orbiting_blades), heal-over-time (verdant_pulse), and
// the mist-step Wet trail that follows the owner's actual dodge sweeps.
// All hit detection is CPU-authoritative swept shape queries each Tick; the
// collision component never blocks and never generates overlaps as a damage
// source (code-contracts: "CPU-authoritative damage"; blueprint §1.2 "spawn/
// trace effect" then "resolve hits/statuses"). Pooling contract:
// ResetForPool clears timers, victims, owner, tags, collision, lifetime
// (code-contracts "Pools"; blueprint §1.2 stale-object safety).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Core/QinglanCoreTypes.h"
#include "Abilities/QinglanTechniqueExecutors.h" // AUD-42 cue context retained on pooled effects
#include "QinglanProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;
class UQinglanCombatComponent;
struct FQinglanBlessingModifiers;

/** Module-local pool: acquire/release of effect actors. Defined in
 *  QinglanProjectile.cpp; per-world free list, never destroyed mid-flight. */
struct FQinglanProjectilePool
{
	static class AQinglanProjectile* Acquire(class UWorld* World);
	static void Release(class AQinglanProjectile* Effect);
	static void ResetPoolForWorld(class UWorld* World);
};

/** Delivery behavior of one pooled effect instance. */
UENUM(BlueprintType)
enum class EQinglanEffectMode : uint8
{
	/** Straight-line projectile sweeping its path every tick (with substeps). */
	LinearProjectile,
	/** Pulses around the owning actor on a fixed interval (orbiting_blades). */
	OrbitPulses,
	/** Periodic heal ticks on the owning actor (verdant_pulse). */
	HealOverTime,
	/** Samples the owner's actual dodge path each tick, applying Wet once per
	 *  victim (mist_step); ends with the dodge. */
	WetTrail
};

/**
 * One pooled effect. Lifetime is gameplay-owned and counts down in Tick with
 * paused-game-time semantics (no wall-clock timers; Tick stops under pause so
 * the countdown freezes, matching code-contracts "Timers use paused game
 * time"). Damage resolves exclusively through each victim's
 * UQinglanCombatComponent::ApplyDamage; this actor never mutates health
 * directly and cosmetic cues never gate gameplay.
 */
UCLASS(NotBlueprintable)
class QINGLANASCENSION_API AQinglanProjectile : public AActor // linkage-only export for editor automation specs
{
	GENERATED_BODY()

public:
	AQinglanProjectile();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;

	// --- Configuration (called by the technique executors right after pool acquire) ---

	/** Configures a linear projectile. RangeCm stops the shot and returns it
	 *  to the pool (jade_sword_arc: 1200, docs/02 §6). BurnDurationSeconds is
	 *  the docs/02 §7 base 3 s plus any blessing bonus. */
	void SetupLinear(const FQinglanCastId& InCastId, FName InTechniqueId, EQinglanElement InElement,
		AActor* InOwner, const FVector& InDirection, float SpeedCmPerSec, float RangeCm,
		float InRawMagnitude, bool bInAppliesBurn, bool bInPiercing, float RawDamageMultiplier,
		float InBurnDurationSeconds = 3.f);

	/** Configures orbiting pulses around the owner (orbiting_blades: 6 pulses,
	 *  max 2 targets per pulse, docs/02 §6). */
	void SetupOrbitPulses(const FQinglanCastId& InCastId, FName InTechniqueId, EQinglanElement InElement,
		AActor* InOwner, float InRawMagnitude, int32 InPulseCount, float InPulseInterval,
		float InPulseRadiusCm, int32 InMaxTargetsPerPulse, float RawDamageMultiplier);

	/** Configures heal-over-time on the owner (verdant_pulse: heal 20 over 4 s
	 *  bounded by max health, docs/02 §6). The first tick lands after one full
	 *  interval; total magnitude divides across the authored tick count. */
	void SetupHealOverTime(const FQinglanCastId& InCastId, FName InTechniqueId,
		AActor* InOwner, float InTotalHeal, float InDurationSeconds, float HealMultiplier);

	/** Configures the Wet trail behind the owner's active dodge (mist_step):
	 *  samples the actual swept position each tick with one deduplicated
	 *  victim set and returns to the pool when the dodge ends. */
	void SetupWetTrail(const FQinglanCastId& InCastId, FName InTechniqueId,
		AActor* InOwner, float InTrailRadiusCm, float InWetDurationBonus);

	/** AUD-42: retain the release context so pooled impact cues still carry the
	 *  technique identity (and can resolve the authored impact Niagara, if any). */
	void SetCueContext(const FQinglanTechniqueCastContext& InContext);

	/** AUD-42/test seam: builds the impact cue exactly as PublishImpactCue emits
	 *  it. Public so automation can assert technique identity survives pooling. */
	FQinglanCueEvent BuildImpactCueEvent(const FVector& Location) const;

	/** Pooling contract: clears timers, victims, owner, tags, collision,
	 *  lifetime. Called on return to pool; also safe to call repeatedly. */
	void ResetForPool();

	/** Deactivates and hands the actor back to the module pool. */
	void ReturnToPool();

	bool IsActive() const { return bActive; }

protected:
	// --- Tick drivers ---
	void TickLinear(float DeltaSeconds);
	void TickOrbitPulses(float DeltaSeconds);
	void TickHealOverTime(float DeltaSeconds);
	void TickWetTrail(float DeltaSeconds);

	/** Sweeps from PreviousLocation to ProposedLocation in substeps; resolves
	 *  damage through victim combat components with per-victim hit ordinals
	 *  (docs/02 §3: duplicate hit keys rejected; ordinals increment
	 *  deliberately). Returns false when the shot is consumed. */
	bool SweepProjectilePath(const FVector& ProposedLocation);

	/** One orbit pulse: nearest-N valid victims inside the pulse radius,
	 *  deterministic (distance, then name) ordering; no random rolls. */
	void ExecuteOrbitPulse();

	/** One heal tick, clamped to MaxHealth (docs/02 §6 "Healing is bounded by
	 *  maximum health"). */
	void ExecuteHealTick(float TickAmount);

	/** Builds and applies one audited damage event; returns false when the
	 *  victim had no combat component. */
	bool ApplyHitToVictim(AActor* Victim, float RawMagnitude, int32 HitOrdinal);

	/** Finds the victim's authoritative combat component. */
	UQinglanCombatComponent* ResolveVictimCombat(AActor* Victim) const;

	/** Cosmetic impact marker; never gates or cancels gameplay. */
	void PublishImpactCue(const FVector& Location, AActor* Victim);

	/** AUD-42: creates the engine-shape placeholder body once (BeginPlay for
	 *  spawned actors, defensively on first launch) and colours it per element. */
	void EnsureVisualPlaceholders();
	void RefreshVisualPresentation();

private:
	// --- Shared identity ---
	FQinglanCastId CastId;
	FName TechniqueId = NAME_None;
	EQinglanElement Element = EQinglanElement::None;
	TWeakObjectPtr<AActor> EffectOwner;
	EQinglanEffectMode Mode = EQinglanEffectMode::LinearProjectile;
	bool bActive = false;

	// --- Linear projectile state ---
	FVector VelocityCmPerSec = FVector::ZeroVector;
	float RemainingRangeCm = 0.f;
	bool bPiercing = false;
	bool bAppliesBurn = false;
	/** docs/02 §7 Burn: 3 s; blessing can extend. */
	float BurnDurationSeconds = 3.f;

	// --- Orbit pulse state ---
	int32 PulsesRemaining = 0;
	float PulseInterval = 0.5f;        // authored default; orbiting cadence unspecified in docs/02 §6 (FIXME-CONTRACT: tunable)
	float PulseRadiusCm = 350.f;       // FIXME-CONTRACT: orbit radius unspecified in docs; tunable default
	int32 MaxTargetsPerPulse = 2;      // docs/02 §6: "at most 2 targets/pulse"
	float PulseTimer = 0.f;

	// --- Heal-over-time state ---
	float HealRemaining = 0.f;
	float HealTickInterval = 1.f;      // 4 ticks over 4 s (docs/02 §6: heal 20 over 4 s)
	float HealPerTick = 0.f;           // total / tick count; final tick carries the remainder
	float HealTimer = 0.f;             // 0 at setup: no heal at cast time

	// --- Wet-trail state (mist_step) ---
	float WetTrailRadiusCm = 220.f;    // capsule-side reach along the dodge path; FIXME-CONTRACT: tunable
	float WetTrailDurationBonus = 0.f; // blessing WetDurationBonus (docs/03 §4)
	TSet<TWeakObjectPtr<AActor>> WetTrailVictims;
	TWeakObjectPtr<UActorComponent> TrailTickPrerequisite;

	// --- Lifetime (paused-game-time via Tick DeltaSeconds) ---
	float LifetimeRemaining = UQinglanTechniqueExecutors::EffectLifetimeSafety;

	// --- Damage bookkeeping ---
	float RawMagnitude = 0.f;
	float DamageMultiplier = 1.f;      // tier-2 (+15% raw, docs/03 §3) + blessing raw percent
	/** Per-victim hit ordinal counter for this cast; duplicate (CastId, victim,
	 *  ordinal) keys are rejected downstream. */
	TMap<TWeakObjectPtr<AActor>, int32> VictimHitOrdinals;
	TSet<TWeakObjectPtr<AActor>> ProcessedVictims;

	UPROPERTY(VisibleAnywhere, Category = "Qinglan")
	TObjectPtr<USphereComponent> CollisionSphere = nullptr;

	/** AUD-42 placeholder body/glow for the otherwise invisible linear shot.
	 *  Engine basic shapes only; no collision, never a gameplay source. */
	UPROPERTY(VisibleAnywhere, Category = "Qinglan|VFX")
	TObjectPtr<UStaticMeshComponent> VisualMesh = nullptr;
	UPROPERTY(VisibleAnywhere, Category = "Qinglan|VFX")
	TObjectPtr<UPointLightComponent> VisualLight = nullptr;

	/** Complete release identity for impact cues (AUD-42). Purely cosmetic. */
	UPROPERTY()
	FQinglanTechniqueCastContext CueContext;

	friend struct FQinglanProjectilePool;
};
