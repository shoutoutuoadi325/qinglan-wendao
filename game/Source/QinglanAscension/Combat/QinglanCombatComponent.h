// Combat component: attributes, damage resolution, barriers, hit audit.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanCombatComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanDamageResolved, const FQinglanDamageEvent&, Event);
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

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// --- Attributes ---
	UFUNCTION(BlueprintPure) float GetHealth() const { return Attributes.Health; }
	UFUNCTION(BlueprintPure) float GetMaxHealth() const { return Attributes.MaxHealth; }
	UFUNCTION(BlueprintPure) float GetTechniquePower() const { return Attributes.TechniquePower; }
	UFUNCTION(BlueprintPure) float GetDefense() const { return Attributes.Defense; }
	UFUNCTION(BlueprintPure) const FQinglanAttributes& GetAttributes() const { return Attributes; }
	/** Re-applies derived values after rank/breakthrough/equipment changes. */
	UFUNCTION(BlueprintCallable) void SetDerivedAttributes(const FQinglanAttributes& NewAttributes);

	// --- Damage (single entry point) ---
	/** Applies one audited damage event; returns true when health/barrier actually changed. */
	bool ApplyDamage(const FQinglanDamageEvent& Request);
	bool IsAlive() const { return Attributes.Health > 0.f; }
	/** True while inside the dodge i-frame window (paused-game-time based). */
	UFUNCTION(BlueprintPure) bool IsInvulnerable() const;

	// --- Barriers (absorb before health; stronger replaces weaker; never stack) ---
	UFUNCTION(BlueprintCallable) void ApplyBarrier(EQinglanElement Element, float Amount, float Duration);
	UFUNCTION(BlueprintPure) float GetBarrierAmount() const { return BarrierAmount; }

	// --- Resources ---
	UFUNCTION(BlueprintCallable) bool TrySpendStamina(float Amount);
	UFUNCTION(BlueprintCallable) bool TrySpendEnergy(float Amount);
	UFUNCTION(BlueprintCallable) void GrantEnergy(float Amount);
	/**
	 * Authoritative heal: clamps to max health, broadcasts OnHealthChanged.
	 * The caller folds healing-percent modifiers (docs/03 section 4
	 * Herbal Knowledge, Verdant Seed relic) into Amount.
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
	/** Water removes Burn; Fire removes Wet pre-damage. */
	void RemoveStatus(EQinglanStatus Status);

	UPROPERTY(BlueprintAssignable) FQinglanDamageResolved OnDamageResolved;
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
	/** Damage-side Conduct resolution: Thunder vs Wet -> 1.25x once, consumes Wet. */
	float ResolveReactionMultiplier(EQinglanElement IncomingElement);

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

	// Burn bookkeeping: three 4-damage ticks across 3 s.
	float BurnNextTickAt = -1.f;
	int32 BurnTicksRemaining = 0;
};
