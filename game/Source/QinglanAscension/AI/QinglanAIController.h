// AI controller: radius+LOS sensing, FSM transition logic, attack-slot
// arbitration against the encounter director, stagger immunity, portal
// warning timing, and offscreen attack audio cues.
//
// FSM states are exactly EQinglanAIState (Idle/Acquire/Approach/Windup/
// Attack/Recover/Stagger/Dead; docs/05 section 5). The authoritative state
// field lives on AQinglanEnemyCharacter (scaffold contract); this controller
// computes transitions, executes movement every tick, and owns all additional
// per-enemy runtime state. Decisions run on the 5-10 Hz accumulator the
// character drives; execution (movement, attack timing) runs every tick.
#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "AI/QinglanEnemyTypes.h"
#include "AI/QinglanEnemyCatalog.h"
#include "QinglanAIController.generated.h"

class AQinglanEncounterDirector;
class AQinglanEnemyCharacter;
class AQinglanEnemyProjectile;
class ACharacter;

DECLARE_LOG_CATEGORY_EXTERN(LogQinglanAI, Log, All);

/** Shared AI tunables; docs values cited, authored values marked. */
namespace QinglanAITunables
{
	// Decision cadence: ordinary AI updates decisions at 5-10 Hz
	// (docs/02 section 8). 0.15 s = 6.7 Hz, clamped band [0.10, 0.20].
	inline constexpr float DecisionIntervalSeconds = 0.15f;
	inline constexpr float DecisionIntervalMinSeconds = 0.10f; // 10 Hz bound
	inline constexpr float DecisionIntervalMaxSeconds = 0.20f; // 5 Hz bound

	// Sensing: radius from the archetype spec + line of sight
	// (docs/dev blueprint section 1.3 "radius+LOS sensing").
	inline constexpr float TargetMemorySeconds = 1.5f;   // authored; mirrors lock-on drop window docs/02 section 2
	inline constexpr float LeashRadiusFactor = 1.3f;     // authored; lose target beyond aggro*factor
	inline constexpr float EyeHeightCm = 60.f;           // authored trace origin offset
	inline constexpr float AcquireHoldSeconds = 0.2f;    // authored face-target beat before approach

	// Stagger: "stagger immunity prevents permanent stun locks"
	// (docs/02 section 8). Durations authored (doc gives no numbers).
	inline constexpr float StaggerSeconds = 0.45f;
	inline constexpr float StaggerImmunitySeconds = 2.0f;
	inline constexpr float StaggerMinimumDamage = 10.f;

	// Offscreen fairness: "offscreen attacks must use an audible cue"
	// (docs/02 section 8); windups always >= the authored telegraphs below.
	inline constexpr float OffscreenCueVolumeMultiplier = 1.0f; // authored; audio is never quality-stripped

	// Portal spawning: "0.8-second warning" and "may not appear within
	// 4 meters of the player" (docs/01 section 6; blueprint section 1.3).
	inline constexpr float PortalWarningSeconds = 0.8f;
	inline constexpr float PortalMinPlayerDistanceCm = 400.f;

	// Authored movement flavor.
	inline constexpr float OrbitSpeedFraction = 0.6f;    // circle-then-lunge tangent speed
	inline constexpr float MaxOrbitSeconds = 2.5f;       // authored: orbit phase must turn inward after this
	inline constexpr float RangedBackupSpeedFraction = 0.8f;
	inline constexpr int32 ProjectilePoolSize = 12;      // authored per-enemy shot pool
}

/**
 * Per-enemy behavior brain. Exactly one per AQinglanEnemyCharacter; pooled
 * together with it (both survive for the room's lifetime).
 */
UCLASS(NotBlueprintable)
class QINGLANASCENSION_API AQinglanAIController : public AAIController
{
	GENERATED_BODY()

public:
	AQinglanAIController();

	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void Tick(float DeltaSeconds) override;

	// --- Wiring ---
	/** Director + deterministic behavior seed (the room's seeded encounter stream). */
	void Initialize(AQinglanEncounterDirector* InDirector, int32 InDeterministicSeed);
	/** Called after AQinglanEnemyCharacter::InitializeFromData resolved the archetype. */
	void OnEnemyDataInitialized(UQinglanEnemyDataAsset* InData);
	/** Mirrors the enemy's pooling reset contract for this brain. */
	void ResetForPool();
	AQinglanEncounterDirector* GetDirector() const { return Director.Get(); }

	// --- FSM transition logic (state field itself lives on the character) ---
	/** True when the senses justify leaving Idle (target acquired inside radius+LOS). */
	bool HasTarget() const;
	ACharacter* GetTargetCharacter() const;
	bool HasLineOfSightToTarget() const;
	float GetDistanceToTarget() const;
	/** True when the player stands inside this archetype's release envelope. */
	bool IsInAttackRange() const;
	/** True when movement is currently suppressed (root/stagger/perch). */
	bool IsMovementSuppressed() const;

	// --- Attack arbitration against the encounter director ---
	/** Requests a melee windup slot (<= 3 ordinary enemies simultaneously, docs/05 section 5). */
	bool TryAcquireMeleeWindupSlot();
	/** Requests one unit of the separate ranged budget (docs/05 section 5). */
	bool TryAcquireRangedAttackSlot();
	/** Releases only the slots this enemy holds ("budget release on attack end/death"). */
	void ReleaseHeldSlots();
	bool HoldsMeleeWindupSlot() const { return bMeleeSlotHeld; }
	bool HoldsRangedAttackSlot() const { return bRangedSlotHeld; }

	// --- Stagger immunity ---
	bool CanBeStaggeredBy(float IncomingDamage) const;
	void NotifyStaggerBegan();

	// --- Portal warning (director-driven; enemy hidden until materialized) ---
	void BeginPortalWarning(float Duration);
	bool IsPortalPending() const { return bPortalPending; }
	void MaterializeFromPortal();

	// --- Boss helpers ---
	bool IsBoss() const;
	/** HP fraction at/below PhaseTwoAtHealthFraction (docs/02 section 9). */
	bool IsPhaseTwo() const;
	const FQinglanArchetypeSpec* GetSpec() const { return CachedSpec; }
	/** Weighted pattern pick from the deterministic stream; nullptr for non-bosses. */
	const FQinglanBossPattern* PickBossPattern(const UQinglanBossDataAsset* BossData) const;
	/** Pattern chosen for the current attack cycle (owned by the rooted data asset). */
	void SetCurrentBossPattern(const FQinglanBossPattern* Pattern) { CurrentPattern = Pattern; }
	const FQinglanBossPattern* GetCurrentBossPattern() const { return CurrentPattern; }

	// --- Attack execution support (character calls these) ---
	/** Globally-unique release identity from the combat authority (AUD-33). */
	FQinglanCastId NextAttackCastId();
	void NotifyWindupBegan(float TelegraphDuration, const FQinglanBossPattern* Pattern);
	void NotifyAttackCommitted(float LungeSpeed);
	void NotifyAttackCycleEnded();
	bool ConsumeAttackHit();
	int32 GetComboIndex() const { return ComboIndex; }
	void ResetCombo() { ComboIndex = 0; }
	FRandomStream& GetBehaviorStream() { return BehaviorStream; }
	/** Marks the ground at windup start; returns the recorded ring center. */
	FVector RecordGroundMarker(const FVector& Center) { GroundMarkerCenter = Center; return Center; }
	const FVector& GetGroundMarkerCenter() const { return GroundMarkerCenter; }
	float GetSafeArcCenterDegrees() const { return SafeArcCenterDeg; }
	/** Storm-volley strike outlines recorded at windup start (docs/02 section 9). */
	const TArray<FVector>& GetPendingStrikeCenters() const { return PendingStrikeCenters; }
	bool IsTargetInsideGroundRing(float RingRadius) const;
	/** Gap check for the Ashen ring shockwave: safe arc of SafeArcHalfWidthDeg. */
	bool IsTargetOutsideSafeArc(float SafeArcHalfWidthDeg) const;
	/** Spawns (or reuses) a pooled projectile aimed at the current target. */
	bool SpawnPooledProjectileAtTarget(float Damage, float Range, FName TechniqueId, FName CueId,
		float AimYawOffsetDeg = 0.f);
	/** Spawns (or reuses) a pooled projectile along an explicit direction (ember volley, AUD-41). */
	bool SpawnPooledProjectileInDirection(float Damage, float Range, FName TechniqueId, FName CueId,
		const FVector& Direction, float Speed);
	/** Seconds since death (world time), for the director's pool return. */
	float GetDeathAgeSeconds() const;
	void NotifyDeathBegan();
	void NotifyRetreatBegan(float Seconds) { RetreatRemaining = Seconds; }

	/** One windup warning, retained for attacks outside the camera (docs/02 section 8). */
	void PlayAttackAudioCue();
	bool IsTargetOffscreenFromPlayerCamera() const;

private:
	void UpdateSensing();
	void UpdateMovement(float DeltaSeconds);
	void UpdateStaggerImmunity(float DeltaSeconds);
	void UpdatePortal(float DeltaSeconds);
	bool HasLineOfSightToTarget(ACharacter* Target) const;
	void ApproachTarget(ACharacter* Target, float AcceptanceRadius);
	AQinglanEnemyCharacter* GetEnemy() const;
	AQinglanEnemyProjectile* AcquirePooledProjectile();
	void StopMoving();

	TWeakObjectPtr<AQinglanEncounterDirector> Director;
	TWeakObjectPtr<ACharacter> TargetPawn;
	/** Weak mirror of the character's EnemyData (protected there); set via OnEnemyDataInitialized. */
	TWeakObjectPtr<UQinglanEnemyDataAsset> EnemyDataRef;

	/** Deterministic per-enemy stream seeded from the room's encounter stream
	 *  (docs/01 section 6; no FMath::RandRange anywhere in gameplay). */
	FRandomStream BehaviorStream;

	// Sensing state.
	float SensingAccumulator = 0.f;
	float SecondsSinceTargetSeen = 999.f;
	bool bHasLineOfSight = false;

	// Arbitration state.
	bool bMeleeSlotHeld = false;
	bool bRangedSlotHeld = false;

	// Stagger immunity.
	float StaggerImmunityRemaining = 0.f;

	// Portal warning.
	bool bPortalPending = false;
	float PortalElapsed = 0.f;
	float PortalDuration = QinglanAITunables::PortalWarningSeconds;

	// Attack execution.
	int32 ComboIndex = 0;
	bool bAttackHitApplied = false;
	bool bChargeLaunched = false;
	const FQinglanBossPattern* CurrentPattern = nullptr;
	FVector GroundMarkerCenter = FVector::ZeroVector;
	TArray<FVector> PendingStrikeCenters;
	float SafeArcCenterDeg = 0.f;
	float RetreatRemaining = 0.f;
	float StrafeSign = 1.f;
	bool bDirectMovement = false;

	// Bounded orbit phase (AUD-40): after MaxOrbitSeconds circling, the enemy
	// turns inward so a stationary target still reaches the attack envelope.
	bool bOrbiting = false;
	float OrbitRemaining = 0.f;

	// Death -> pool timing.
	float DeathWorldSeconds = -1.f;

	// Cached archetype spec (catalog lookup once per init).
	const FQinglanArchetypeSpec* CachedSpec = nullptr;

	// Pooled projectiles owned by this enemy.
	UPROPERTY() TArray<TWeakObjectPtr<AQinglanEnemyProjectile>> ProjectilePool;
};
