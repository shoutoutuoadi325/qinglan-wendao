// Profile subsystem: the single authoritative mutation path for permanent state.
// UI requests changes here; nothing else subtracts currency or sets campaign flags.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanProfileSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanProfileCommitted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanInventoryChanged, FQinglanInventory, NewInventory);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanCultivationChanged, EQinglanStage, Stage, int32, Rank);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanCampaignFlagSet, FName, Flag);

/**
 * Permanent profile state: cultivation, inventory, unlocks, flags, transactions.
 * Every mutation runs through this subsystem and is journaled into the save.
 */
UCLASS()
class UQinglanProfileSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// --- Queries ---
	UFUNCTION(BlueprintPure) const FQinglanInventory& GetInventory() const;
	UFUNCTION(BlueprintPure) EQinglanStage GetCultivationStage() const;
	UFUNCTION(BlueprintPure) int32 GetCultivationRank() const;
	UFUNCTION(BlueprintPure) bool HasCampaignFlag(FName Flag) const;
	UFUNCTION(BlueprintPure) bool IsTechniqueUnlocked(FName TechniqueId) const;
	UFUNCTION(BlueprintPure) TArray<FName> GetEquippedTechniques() const;
	UFUNCTION(BlueprintPure) bool IsSideQuestStageComplete(FName StageId) const;
	UFUNCTION(BlueprintPure) bool HasReadRecord(FName RecordId) const;

	/** Requirements for the next rank, for UI preview. */
	UFUNCTION(BlueprintPure) bool GetNextRankCost(int64& OutShards, int64& OutExtraMaterialCount, FName& OutExtraMaterialId) const;

	/** Breakthrough gate status (boss cleared + rank count). */
	UFUNCTION(BlueprintPure) bool CanAttemptBreakthrough(FText& OutBlockingReason) const;

	// --- Authoritative mutations (atomic, idempotent via transaction IDs) ---
	/** Grants currency/materials. Returns false if it would overflow bounded ints. */
	UFUNCTION(BlueprintCallable) bool TryGrant(FName TransactionId, const FQinglanInventory& Grant);
	/** Spends currency; on insufficient resources returns false and changes nothing. */
	UFUNCTION(BlueprintCallable) bool TrySpend(FName TransactionId, const FQinglanInventory& Cost);
	/** Attempts the next cultivation rank up; validates boss gates and materials. */
	UFUNCTION(BlueprintCallable) bool TryAdvanceRank(FName TransactionId);
	/** Attempts a stage breakthrough trial unlock (Foundation / Golden Core). */
	UFUNCTION(BlueprintCallable) bool TryBreakthrough(FName TransactionId);
	UFUNCTION(BlueprintCallable) void SetCampaignFlag(FName Flag);
	UFUNCTION(BlueprintCallable) bool UnlockTechnique(FName TransactionId, FName TechniqueId);
	/** Loadout changes are only permitted outside expeditions. */
	UFUNCTION(BlueprintCallable) bool TrySetEquippedTechniques(const TArray<FName>& TechniqueIds);
	UFUNCTION(BlueprintCallable) void MarkRecordRead(FName RecordId);
	UFUNCTION(BlueprintCallable) void MarkSideQuestStageComplete(FName StageId);

	// --- Narrative bridge (implemented with UQinglanStorySubsystem) ---
	/** Mutates narrative state through the story subsystem's validated path. */
	UFUNCTION(BlueprintCallable) bool SetNarrativeFlag(FName Flag);
	UFUNCTION(BlueprintPure) bool HasNarrativeFlag(FName Flag) const;

	// --- Persistence bridge ---
	/** Serializes a portable snapshot for the save subsystem; never called by UI. */
	void ExportStateForSave(struct FQinglanProfileSnapshot& OutSnapshot) const;
	void ImportStateFromSave(const struct FQinglanProfileSnapshot& Snapshot);
	/** True when TransactionId was already committed (idempotency check). */
	bool WasTransactionCommitted(FName TransactionId) const;

	UPROPERTY(BlueprintAssignable) FQinglanProfileCommitted OnProfileCommitted;
	UPROPERTY(BlueprintAssignable) FQinglanInventoryChanged OnInventoryChanged;
	UPROPERTY(BlueprintAssignable) FQinglanCultivationChanged OnCultivationChanged;
	UPROPERTY(BlueprintAssignable) FQinglanCampaignFlagSet OnCampaignFlagSet;
};
