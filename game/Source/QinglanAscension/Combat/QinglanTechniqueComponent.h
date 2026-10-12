// Technique component: loadout, cooldowns, cast pipeline state.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "Abilities/QinglanTechniqueTypes.h"
#include "QinglanTechniqueComponent.generated.h"

class UQinglanTechniqueDataAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanCastReleased, FName, TechniqueId, FQinglanCastId, CastId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanCooldownStarted, FName, TechniqueId, float, Seconds);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanCastInterrupted);

/**
 * Executes the fixed cast pipeline: validate alive/state/resources/target ->
 * anticipation -> release once -> commit energy+cooldown -> spawn/trace effect ->
 * resolve hits/statuses -> publish cues -> recovery. Interrupted pre-release
 * costs nothing; released techniques are never refunded.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class UQinglanTechniqueComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanTechniqueComponent();

	/** Slot 0..3. Returns false when the cast was rejected at validation. */
	UFUNCTION(BlueprintCallable) bool TryCast(int32 Slot);
	/** Force-interrupts the current cast (hit reaction, death, cutscene). */
	UFUNCTION(BlueprintCallable) void InterruptCast();
	UFUNCTION(BlueprintPure) float GetCooldownRemaining(FName TechniqueId) const;
	UFUNCTION(BlueprintPure) bool IsCasting() const { return CurrentCastId.IsValid(); }
	UFUNCTION(BlueprintPure) FQinglanCastId GetCurrentCastId() const { return CurrentCastId; }

	/** Equipment changes only outside expeditions; cooldowns are preserved. */
	UFUNCTION(BlueprintCallable) bool SetLoadout(const TArray<UQinglanTechniqueDataAsset*>& NewLoadout);
	UFUNCTION(BlueprintPure) TArray<UQinglanTechniqueDataAsset*> GetLoadout() const { return Loadout; }

	/** Time-scaled tick so cooldowns freeze with paused game time. */
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(BlueprintAssignable) FQinglanCastReleased OnCastReleased;
	UPROPERTY(BlueprintAssignable) FQinglanCooldownStarted OnCooldownStarted;
	UPROPERTY(BlueprintAssignable) FQinglanCastInterrupted OnCastInterrupted;

private:
	/** Runs release: spawns projectile/strike/barrier/heal and commits costs. */
	void ReleaseCurrentCast();
	void FailValidation();

	TArray<TObjectPtr<UQinglanTechniqueDataAsset>> Loadout;
	TMap<FName, float> CooldownEndsAt;
	FQinglanCastId CurrentCastId;
	TObjectPtr<UQinglanTechniqueDataAsset> CurrentTechnique = nullptr;
	float CastPhaseEndTime = 0.f;
	bool bInAnticipation = false;
	int32 CastSerialCounter = 0;
};
