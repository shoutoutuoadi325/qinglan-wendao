// Combat component: attributes, damage resolution, barriers, hit audit.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanCombatComponent.generated.h"

/**
 * One accepted damage resolution, published after barrier/health were applied
 * (AUD-28). Dodged, duplicate, and dead-victim requests return from
 * ApplyDamage without broadcasting; subscribers may treat every notification
 * as an accepted hit. AppliedMagnitude is the post-mitigation/reaction/rounding
 * number the victim's barrier+health actually absorbed.
 */
USTRUCT(BlueprintType)
struct FQinglanResolvedDamage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<AActor> Attacker;
	UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<AActor> Victim;
	UPROPERTY(BlueprintReadOnly) FQinglanCastId CastId;
	/** Per-victim hit ordinal of the resolved event. */
	UPROPERTY(BlueprintReadOnly) int32 HitOrdinal = 0;
	UPROPERTY(BlueprintReadOnly) float AppliedMagnitude = 0.f;
	/** Always true on this delegate; kept explicit so rejection paths can join later without a new signature. */
	UPROPERTY(BlueprintReadOnly) bool bAccepted = true;
	UPROPERTY(BlueprintReadOnly) bool bBlockedByBarrier = false;
	/** Health reached zero on this hit (Burn ticks included). */
	UPROPERTY(BlueprintReadOnly) bool bLethal = false;
	/** Dedupe key of the resolved hit; 0 when the request carried no cast id.
	 * Blueprint has no uint64, so the 64-bit key is reinterpreted as int64;
	 * equality is bit-exact, only the display sign differs. */
	UPROPERTY(BlueprintReadOnly) int64 HitKey = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanDamageResolved, const FQinglanDamageEvent&, Event);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanDamageResultResolved, const FQinglanResolvedDamage&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanBarrierChanged, float, Current, float, Maximum);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanHealthChanged, float, Current, float, Maximum);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanEnergyChanged, float, Current, float, Maximum);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStaminaChanged, float, Current, float, Maximum);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanStatusApplied, EQinglanStatus, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanStatusCleared, EQinglanStatus, Status);

/**
 * Authoritative attributes + damage pipeline.
 * FinalDamage = round(max(1, Mitigated * Reaction * Difficulty)), halves away
 * from zero. Precedence: immunity -> barrier/health -> reaction consumption ->
 * status application -> cue. Duplicate (CastId, victim, ordinal) rejected.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class QINGLANASCENSION_API UQinglanCombatComponent : public UActorComponent // QINGLANASCENSION_API: linkage-only export so the editor automation specs can drive ApplyDamage/ApplyStatus/ApplyBarrier from the modular editor build; public API unchanged.
{
	GENERATED_BODY()

public:
	UQinglanCombatComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// --- Attributes ---
	UFUNCTION(BlueprintPure) float GetHealth() const { return Attributes.Health; }
	UFUNCTION(BlueprintPure) float GetMaxHealth() const { return Attributes.MaxHealth; }
	UFUNCTION(BlueprintPure) float GetTechniquePower() const { return Attributes.TechniquePower; }
	UFUNCTION(BlueprintPure) float GetDefense() const { return Attributes.Defense; }
	UFUNCTION(BlueprintPure) const FQinglanAttributes& GetAttributes() const { return Attributes; }
	/** Restores a complete derived snapshot, including already applied bonuses. */
	UFUNCTION(BlueprintCallable) void SetDerivedAttributes(const FQinglanAttributes& NewAttributes);
	/** Restores checkpoint vitals and rebuilds player maxima/power from the current profile and blessings. */
	void RestoreCheckpointAttributes(const FQinglanAttributes& Snapshot);

	// --- Damage (single entry point) ---
	/** Applies one audited damage event; returns true when health/barrier actually changed. */
	bool ApplyDamage(const FQinglanDamageEvent& Request);
	/**
	 * Globally-unique, frame-stable cast identity for one attack release
	 * (AUD-33). The serial comes from one process-wide counter, so two
	 * attackers can never mint colliding ids; FrameNumber is the release
	 * frame and must travel unchanged through projectiles (never be
	 * reconstructed at the impact frame).
	 */
	static FQinglanCastId NextCastId();
	bool IsAlive() const { return Attributes.Health > 0.f; }
	/** True while inside the dodge i-frame window (paused-game-time based). */
	UFUNCTION(BlueprintPure) bool IsInvulnerable() const;
	/** True when TimeSeconds lies inside [0.10, 0.28] s of the current dodge.
	 *  World-free seam so automation can assert consecutive-dodge clocks. */
	UFUNCTION(BlueprintPure) bool IsInvulnerableAtTime(float TimeSeconds) const;
	/** AUD-34: explicitly re-anchor the immunity clock at a dodge's start.
	 *  Every dodge start (dodge component notification or direct call) resets
	 *  the window; no frame-observation lag and no carry-over from a previous
	 *  dodge. Clears any prior dodge bookkeeping. */
	UFUNCTION(BlueprintCallable) void NotifyDodgeStarted(float StartTimeSeconds);
	UFUNCTION(BlueprintPure) float GetDodgeStartTime() const { return DodgeStartTime; }

	// --- Barriers (absorb before health; stronger replaces weaker; never stack) ---
	UFUNCTION(BlueprintCallable) void ApplyBarrier(EQinglanElement Element, float Amount, float Duration);
	UFUNCTION(BlueprintPure) float GetBarrierAmount() const { return BarrierAmount; }

	// --- Resources ---
	UFUNCTION(BlueprintCallable) bool TrySpendStamina(float Amount);
	UFUNCTION(BlueprintCallable) bool TrySpendEnergy(float Amount);
	UFUNCTION(BlueprintCallable) void GrantEnergy(float Amount);
	/**
	 * Authoritative heal: clamps to max health, broadcasts OnHealthChanged.
	 * For the player pawn the permanent healing percent (docs/03 section 4
	 * Herbal Knowledge, Verdant Seed relic) is folded here exactly once;
	 * callers pass the authored raw magnitude, never a pre-scaled amount.
	 */
	UFUNCTION(BlueprintCallable) void Heal(float Amount);
	/** Sword-hit energy restore: 4 per unique target, capped 8 per swing. */
	void NotifySwordHitTarget(AActor* Victim);
	/**
	 * Swing-boundary reset for the sword energy ledger: clears the per-swing
	 * victim list and the 8-energy cap so every swing starts clean
	 * (docs/02 section 4 "one damage/energy share per victim per swing").
	 */
	void NotifySwordSwingStarted();
	/** Clears pooled-actor state (hit keys, swing ledger) for reuse. */
	void ResetForPool();

	// --- Statuses ---
	UFUNCTION(BlueprintCallable) void ApplyStatus(EQinglanStatus Status, float Magnitude = 0.f);
	UFUNCTION(BlueprintPure) bool HasStatus(EQinglanStatus Status) const;
	UFUNCTION(BlueprintPure) float GetStatusRemaining(EQinglanStatus Status) const;
	/** Water removes Burn; Fire removes Wet pre-damage. */
	void RemoveStatus(EQinglanStatus Status);

	UPROPERTY(BlueprintAssignable) FQinglanDamageResolved OnDamageResolved;
	/** Accepted-hit resolution results (AUD-28); fires once per applied hit, Burn ticks included. */
	UPROPERTY(BlueprintAssignable) FQinglanDamageResultResolved OnDamageResultResolved;
	UPROPERTY(BlueprintAssignable) FQinglanBarrierChanged OnBarrierChanged;
	UPROPERTY(BlueprintAssignable) FQinglanHealthChanged OnHealthChanged;
	UPROPERTY(BlueprintAssignable) FQinglanEnergyChanged OnEnergyChanged;
	UPROPERTY(BlueprintAssignable) FQinglanStaminaChanged OnStaminaChanged;
	UPROPERTY(BlueprintAssignable) FQinglanStatusApplied OnStatusApplied;
	UPROPERTY(BlueprintAssignable) FQinglanStatusCleared OnStatusCleared;

	// Tunables
	UPROPERTY(EditDefaultsOnly) float EnergyRegenPerSecond = 4.f;
	UPROPERTY(EditDefaultsOnly) float EnergyRegenDelay = 1.f;
	UPROPERTY(EditDefaultsOnly) float StaminaRegenPerSecond = 20.f;
	UPROPERTY(EditDefaultsOnly) float StaminaRegenDelay = 0.8f;
	UPROPERTY(EditDefaultsOnly) float InvulnerableWindowStart = 0.10f;
	UPROPERTY(EditDefaultsOnly) float InvulnerableWindowEnd = 0.28f;

private:
	void RegenResources(float DeltaTime);
	/** Damage-side Conduct resolution: Thunder vs Wet -> 1.25x once, consumes
	 *  Wet. The attacker's blessing/permanent ElementalInsight-style
	 *  ConductDamagePercent raises the multiplier once. */
	float ResolveReactionMultiplier(EQinglanElement IncomingElement, AActor* Attacker);
	/** AUD-34: dodge component start notification (frame-exact re-anchor). */
	UFUNCTION() void HandleDodgeStarted(float Duration);
	/** AUD-37: Grounded Steps / Steady Foundation / Mountain Token fold onto
	 *  the player-owned derived maxima exactly once (delta-tracked). */
	void RefreshEquipmentAttributeBonuses(bool bSnapshotIncludesBonuses = false);
	UFUNCTION() void HandleProfileCommitted();

	FQinglanAttributes Attributes;
	/** Active statuses with remaining paused-game-time. */
	TMap<EQinglanStatus, float> ActiveStatuses;
	TSet<uint64> AppliedHitKeys;
	float BarrierAmount = 0.f;
	float BarrierRemaining = 0.f;
	EQinglanElement BarrierElement = EQinglanElement::None;
	float TimeSinceEnergySpend = 999.f;
	float TimeSinceStaminaSpend = 999.f;
	float DodgeStartTime = -1000.f;
	TArray<AActor*> SwordSwingVictims;
	int32 SwordSwingEnergyGranted = 0;

	// Equipment-derived maxima already folded into Attributes (delta tracking
	// keeps recomputation idempotent across profile commits and checkpoint
	// restores).
	float AppliedEquipmentMaxHealthBonus = 0.f;
	float AppliedEquipmentMaxStaminaBonus = 0.f;
	float AppliedCultivationMaxEnergyBonus = 0.f;
	float AppliedCultivationPowerBonus = 0.f;

	// Burn bookkeeping: three 4-damage ticks across the requested duration.
	// The requested duration is recorded at application time; the tick cadence
	// is Duration/3 so a BurnDurationBonus extends the window without ever
	// exceeding the maximum-three-ticks-after-refresh contract.
	float BurnNextTickAt = -1.f;
	float BurnTickInterval = 1.f;
	int32 BurnTicksRemaining = 0;
};
