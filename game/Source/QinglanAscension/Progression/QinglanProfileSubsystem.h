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
 * Permanent profile state: cultivation, inventory, unlocks, permanent
 * equipment (relics, passives, talisman charges), flags, transactions.
 * Every mutation runs through this subsystem and is journaled into the save.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanProfileSubsystem : public UGameInstanceSubsystem // QINGLANASCENSION_API: linkage-only export for the editor automation specs (transaction idempotency, breakthrough gates); public API unchanged.
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

	/** AUD-37: purchased technique tier (1 = base, 2 = upgraded; no tier 3). */
	UFUNCTION(BlueprintPure) int32 GetTechniqueTier(FName TechniqueId) const;
	/** Multiplier for the profile's current tier of TechniqueId (1.0 / 1.15). */
	UFUNCTION(BlueprintPure) float GetTechniqueTierMultiplier(FName TechniqueId) const;

	// --- Permanent equipment queries (docs/03 sections 4-5) ---
	UFUNCTION(BlueprintPure) TArray<FName> GetOwnedRelics() const;
	UFUNCTION(BlueprintPure) FName GetEquippedRelic() const;
	UFUNCTION(BlueprintPure) TArray<FName> GetOwnedPassives() const;
	UFUNCTION(BlueprintPure) TArray<FName> GetEquippedPassives() const;

	/** Requirements for the next rank, for UI preview. */
	UFUNCTION(BlueprintPure) bool GetNextRankCost(int64& OutShards, int64& OutExtraMaterialCount, FName& OutExtraMaterialId) const;

	/** Breakthrough gate status (boss cleared + rank count). */
	UFUNCTION(BlueprintPure) bool CanAttemptBreakthrough(FText& OutBlockingReason) const;

	// --- Authoritative mutations (atomic, idempotent via transaction IDs) ---
	/** Grants currency/materials. Returns false if it would overflow bounded ints. */
	UFUNCTION(BlueprintCallable) bool TryGrant(FName TransactionId, const FQinglanInventory& Grant);
	/** AUD-07/08: commit run payout and optional first-clear delta together.
	 * Replayed banking IDs are no-ops; overflow/journal-capacity failure changes
	 * neither inventory nor transaction history. The run owner saves afterward.
	 */
	bool TryBankExpedition(FName BankingId, const FQinglanInventory& Grant,
		FName FirstClearId, const FQinglanInventory& FirstClearBonus);
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

	// --- Permanent equipment mutations (atomic, idempotent via transaction IDs) ---
	/** Buys one relic for its VeinCrystalCost (docs/03 section 5); rejects unknown/owned ids. */
	UFUNCTION(BlueprintCallable) bool TryPurchaseRelic(FName TransactionId, FName RelicId);
	/** Equips one owned relic (limit 1); NAME_None unequips; never mid-expedition. */
	UFUNCTION(BlueprintCallable) bool TryEquipRelic(FName RelicId);
	/** Equips up to two owned passives (docs/03 section 4); never mid-expedition. */
	UFUNCTION(BlueprintCallable) bool TrySetEquippedPassives(const TArray<FName>& PassiveIds);
	/** Studies one passive manual (docs/03 section 4); rejects unknown/owned ids. */
	UFUNCTION(BlueprintCallable) bool TryPurchasePassive(FName TransactionId, FName PassiveId);
	/**
	 * AUD-37: buys the optional tier-2 upgrade (100 Shards + 1 Vein Crystal,
	 * Foundation Establishment onward; docs/03 sections 1 and 3). The stable
	 * transaction id makes replays no-ops; an already-upgraded technique
	 * commits the id without charging again.
	 */
	UFUNCTION(BlueprintCallable) bool TryPurchaseTechniqueTier(FName TransactionId, FName TechniqueId);
	/** Authored study cost (docs/03 section 4 lists no prices): 120 Shards + 2 Spirit Herbs. */
	static constexpr int64 PassiveStudyCostShards = 120;
	static constexpr int64 PassiveStudyCostHerbs = 2;

	// --- Talisman charges (docs/03 section 5: expedition charges, free refill at the sanctuary) ---
	UFUNCTION(BlueprintPure) void GetTalismanCharges(TMap<FName, int32>& OutCharges) const;
	UFUNCTION(BlueprintPure) int32 GetTalismanChargeCount(FName TalismanId) const;
	/** Consumes one charge; false (and no change) when none remain. */
	UFUNCTION(BlueprintCallable) bool TryConsumeTalismanCharge(FName TalismanId);
	/** Resets every catalog talisman to its authored charge count (sanctuary arrival). */
	UFUNCTION(BlueprintCallable) void RefillTalismans();

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

private:
	// --- Implementation-only additions; the public API above is unchanged. ---
	// State is deliberately not reflected: serialization flows exclusively
	// through Export/ImportStateForSave so the save subsystem stays the single
	// owner of on-disk schema (code-contracts.md "One authoritative mutation path").

	/** Seeds the fresh-profile defaults (docs/03 section 3; data/examples/save.json). */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	void ResetToFreshProfile();
	/** Shared gate validator for TryBreakthrough and CanAttemptBreakthrough. */
	bool CheckBreakthroughGate(class FText& OutBlockingReason) const;
	/** Boss/trial proofs accept either the campaign proof flag or the story flag. */
	bool IsBambooCleared() const;
	bool IsAshenCleared() const;
	bool IsGoldenCoreTrialWon() const;
	/** Adds a proof flag without broadcasting; true when newly added. */
	bool SetCampaignFlagInternal(FName Flag);
	/** Loadout gate shared by equipment swaps: sanctuary/entrance shrine only (docs/03 sections 3-5). */
	bool IsLoadoutLockedByExpedition() const;

	FQinglanInventory Inventory;
	EQinglanStage Stage = EQinglanStage::QiRefining;
	int32 Rank = 0; // 0..3 purchased ranks within the current stage; Golden Core stays 0 (docs/03 section 1)
	TSet<FName> CampaignFlags;
	TSet<FName> UnlockedTechniques;
	TArray<FName> EquippedTechniques;
	/** AUD-37: technique id -> purchased tier (absent or 1 = base). */
	TMap<FName, int32> TechniqueTiers;
	TSet<FName> OwnedRelics;
	FName EquippedRelic = NAME_None;
	TSet<FName> OwnedPassives;
	TArray<FName> EquippedPassives;
	TMap<FName, int32> TalismanCharges;
	TSet<FName> CommittedTransactionIds;
	TSet<FName> ReadRecords;
	TSet<FName> CompletedSideStages;
};
