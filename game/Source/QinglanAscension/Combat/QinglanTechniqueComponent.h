// Technique component: loadout, cooldowns, cast pipeline state.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "Abilities/QinglanTechniqueTypes.h"
#include "QinglanTechniqueComponent.generated.h"

class UQinglanTechniqueDataAsset;
class UQinglanProfileSubsystem;

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
class QINGLANASCENSION_API UQinglanTechniqueComponent : public UActorComponent // linkage-only export for editor automation specs
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
	/** True during the cancellable, zero-cost anticipation phase (AUD-32). */
	UFUNCTION(BlueprintPure) bool IsInAnticipation() const { return CurrentCastId.IsValid() && bInAnticipation; }
	/** True after the release notify (release/recovery); normal attacks may not cancel it (AUD-32). */
	UFUNCTION(BlueprintPure) bool IsInRecovery() const { return CurrentCastId.IsValid() && !bInAnticipation; }
	UFUNCTION(BlueprintPure) FQinglanCastId GetCurrentCastId() const { return CurrentCastId; }

	/**
	 * AUD-27: imports the active profile's EquippedTechniques into runtime
	 * slots 0-3. Safe to call on possession/BeginPlay, after map travel (a new
	 * pawn), and whenever OnProfileCommitted fires. Missing DA_Tech assets are
	 * logged and leave their slot empty; cooldowns are never overwritten.
	 * ProfileOverride is a test seam for worlds without a game instance.
	 * Returns false when no profile is available, a cast is active, or a
	 * changed slot still has an unexpired cooldown.
	 */
	bool SyncLoadoutFromProfile(UQinglanProfileSubsystem* ProfileOverride = nullptr);
	/**
	 * Applies an explicit equipped-id list (the profile's authoritative order).
	 * Missing/locked/duplicate ids leave the slot empty rather than shifting
	 * later entries; the four-slot and cooldown-preservation rules apply.
	 */
	bool ApplyEquippedTechniqueIds(const TArray<FName>& TechniqueIds, const UQinglanProfileSubsystem* Profile = nullptr);
	/** Resolves /Game/Qinglan/Data/Techniques/DA_Tech_<id> (test catalog first). */
	UQinglanTechniqueDataAsset* ResolveTechniqueById(FName TechniqueId) const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test seam: inject transient technique assets (empty array clears). */
	static void SetTechniqueCatalogForTesting(const TArray<UQinglanTechniqueDataAsset*>& Assets);
#endif

	/** Equipment changes only outside expeditions; cooldowns are preserved. */
	UFUNCTION(BlueprintCallable) bool SetLoadout(const TArray<UQinglanTechniqueDataAsset*>& NewLoadout);
	// Direct-initialization: TArray's TArray<OtherElement> converting constructor
	// is explicit in UE 5.x, so a plain copy-return from the TObjectPtr store
	// would not compile. Public signature unchanged.
	UFUNCTION(BlueprintPure) TArray<UQinglanTechniqueDataAsset*> GetLoadout() const { return TArray<UQinglanTechniqueDataAsset*>(Loadout); }

	/** Time-scaled tick so cooldowns freeze with paused game time. */
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(BlueprintAssignable) FQinglanCastReleased OnCastReleased;
	UPROPERTY(BlueprintAssignable) FQinglanCooldownStarted OnCooldownStarted;
	UPROPERTY(BlueprintAssignable) FQinglanCastInterrupted OnCastInterrupted;

private:
	/** Runs release: spawns projectile/strike/barrier/heal and commits costs. */
	void ReleaseCurrentCast();
	void FailValidation();

	// UPROPERTY keeps the DataAssets reachable from GC. Without it the HUD
	// could read freed memory after a collection (invalid FNames / crash in
	// UQinglanHUDWidget::RefreshFromPlayer).
	UPROPERTY()
	TArray<TObjectPtr<UQinglanTechniqueDataAsset>> Loadout;
	TMap<FName, float> CooldownEndsAt;
	FQinglanCastId CurrentCastId;
	UPROPERTY()
	TObjectPtr<UQinglanTechniqueDataAsset> CurrentTechnique = nullptr;
	float CastPhaseEndTime = 0.f;
	bool bInAnticipation = false;
	int32 CastSerialCounter = 0;
};
