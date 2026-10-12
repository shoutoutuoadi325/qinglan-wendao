// Status component: timed statuses with the exact authored semantics of
// docs/02-combat-and-camera.md section 7.
//
// Store ownership (single authoritative store per status, no split brain):
//  - Wet / Burn live in the sibling UQinglanCombatComponent::ActiveStatuses
//    because Burn ticks deal audited damage through the damage pipeline and
//    Wet/Conduct participate in pre-damage reaction math. This component
//    bridges the combat component's status delegates and mirrors the remaining
//    time locally, so both entry points stay coherent.
//  - Root (and the boss downgrade to a 10% slow) lives here: it is a movement
//    effect, never a damage number.
//  - Conduct is not a stored status: it resolves once inside
//    UQinglanCombatComponent::ApplyDamage (1.25x, consumes Wet, cannot
//    recursively trigger). Applying it here only publishes the brief readable
//    arc cue event, per docs/02 §7.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanStatusComponent.generated.h"

class UQinglanCombatComponent;

/** Broadcast for HUD/cue listeners; RemainingSeconds is 0 for one-shot effects.
 *  (Distinct type names from the combat component's FQinglanStatusApplied /
 *  FQinglanStatusCleared, which are single-param.) */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanTimedStatusApplied, EQinglanStatus, Status, float, RemainingSeconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanTimedStatusRemoved, EQinglanStatus, Status);

/**
 * Timed statuses and bounded reactions for one actor (player or enemy).
 * All timers accumulate TickComponent DeltaTime, so everything freezes with
 * paused game time; never wall clock.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class UQinglanStatusComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanStatusComponent();

	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Applies a status with authored semantics (docs/02 §7):
	 *  - Wet: 5 s, refreshes on reapplication. Only authored Water attacks or
	 *    explicit gameplay puddles may call this; cosmetic rain never does.
	 *  - Burn: 3 s, three 4-damage ticks, refresh duration without stacking,
	 *    max three ticks after the latest refresh. Delegates to the combat
	 *    component which owns tick bookkeeping.
	 *  - Root: 1 s on ordinary enemies; bosses (tag Qinglan.Boss) receive a
	 *    10% slow for 1 s instead and are never rooted.
	 *  - Conduct: no stored state; publishes the one-shot arc cue event only.
	 *  - Water removes Burn and Fire removes Wet pre-damage are resolved inside
	 *    UQinglanCombatComponent::ApplyDamage, not here.
	 */
	UFUNCTION(BlueprintCallable) void ApplyStatus(EQinglanStatus Status, float Magnitude = 0.f);

	/** Explicit removal (Cleansing Talisman, expiry is automatic). */
	UFUNCTION(BlueprintCallable) void RemoveStatus(EQinglanStatus Status);

	/** True while Wet/Burn are held by the combat component or Root/slow is active here. */
	UFUNCTION(BlueprintPure) bool HasStatus(EQinglanStatus Status) const;

	/** Remaining paused-game-time seconds for a status; 0 when absent. */
	UFUNCTION(BlueprintPure) float GetStatusRemaining(EQinglanStatus Status) const;

	/** Localized display name from /Game/Qinglan/Data/ST_Game (keys status.wet ...). */
	UFUNCTION(BlueprintPure) FText GetStatusDisplayName(EQinglanStatus Status) const;

	// --- Movement queries (docs/02 §7 Root semantics) ---
	UFUNCTION(BlueprintPure) bool IsRooted() const;
	UFUNCTION(BlueprintPure) bool IsSlowed() const;
	/** 0 while rooted, 0.9 while boss-slowed, otherwise 1. */
	UFUNCTION(BlueprintPure) float GetMoveSpeedMultiplier() const;

	UPROPERTY(BlueprintAssignable) FQinglanTimedStatusApplied OnStatusApplied;
	UPROPERTY(BlueprintAssignable) FQinglanTimedStatusRemoved OnStatusRemoved;

	// Tunables (docs/02 §7; defaults mirror QinglanCombatTunables).
	UPROPERTY(EditDefaultsOnly) float WetDuration = 5.f;
	UPROPERTY(EditDefaultsOnly) float BurnDuration = 3.f;
	UPROPERTY(EditDefaultsOnly) float RootDuration = 1.f;
	/** Boss downgrade for Root: slow percent instead of a hard root. */
	UPROPERTY(EditDefaultsOnly) float BossSlowPercent = 10.f;
	/** Force boss semantics regardless of the Qinglan.Boss actor tag. */
	UPROPERTY(EditDefaultsOnly) bool bTreatAsBoss = false;

protected:
	virtual void BeginPlay() override;

private:
	/** Owner's combat component (Wet/Burn authority); may be null on test dummies. */
	UQinglanCombatComponent* GetOwningCombat() const;
	bool OwnerIsBoss() const;
	void BindCombatDelegates();
	void BroadcastApplied(EQinglanStatus Status, float RemainingSeconds);
	void BroadcastRemoved(EQinglanStatus Status);

	/** Bridge from the combat component so Wet/Burn applied through either
	 *  entry point fire this component's delegates as well. */
	UFUNCTION() void HandleCombatStatusApplied(EQinglanStatus Status);
	UFUNCTION() void HandleCombatStatusCleared(EQinglanStatus Status);

public:
	/**
	 * Movement-only Root sink, called by UQinglanCombatComponent (the single
	 * Root entry point for both components). Never calls back into the combat
	 * component, so the ownership chain is loop-free: any caller ->
	 * CombatComponent::ApplyStatus(Root) -> ApplyRootMovement here (and the
	 * combat-side ActiveStatuses store for non-boss owners).
	 */
	void ApplyRootMovement(float Duration, bool bBossOwner);
	/** Clears the local root/slow state without touching the combat component. */
	void ClearRootMovement();

private:

	/** Local Root effect remaining, in paused-game-time seconds. */
	float RootRemaining = 0.f;
	/** Local boss-slow remaining; movement reads GetMoveSpeedMultiplier(). */
	float SlowRemaining = 0.f;
	/** Active slow fraction (1 - BossSlowPercent/100) while slowed. */
	float ActiveSlowFraction = 1.f;
	/** Mirrored Wet/Burn countdowns; the combat component is the authority. */
	float WetRemainingMirror = 0.f;
	float BurnRemainingMirror = 0.f;
	bool bCombatDelegatesBound = false;
};
