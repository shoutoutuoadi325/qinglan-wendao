// Run subsystem: one expedition's seed, rooms, temporary loot and blessings.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanRunSubsystem.generated.h"

class UQinglanRealmDataAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanRoomEntered, int32, RoomIndex);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanRoomCompleted, int32, RoomIndex, int64, ShardsAwarded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanExpeditionEnded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanBankingFailed);

/** One drafted temporary blessing. */
USTRUCT(BlueprintType)
struct FQinglanBlessing
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName BlessingId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FText DisplayName;
	UPROPERTY(BlueprintReadOnly) FText Description;
	UPROPERTY(BlueprintReadOnly) FName GroupId = NAME_None;
};

/**
 * Expedition lifetime state. Owns the three seeded FRandomStream instances
 * (encounters / loot / blessings) and the room pipeline. Banking rules:
 * victory 100%, voluntary retreat 70% floored per stack, defeat 50%.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanRunSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable) bool StartExpedition(FName RealmId, int32 Seed);
	UFUNCTION(BlueprintCallable) bool EnterRoom(int32 RoomIndex);
	/** Called by the encounter director when all room enemies are dead. */
	UFUNCTION(BlueprintCallable) void CompleteCurrentRoom(FName TransactionId);
	/**
	 * Exit-shrine banking; multiplier from outcome type (1.0 / 0.7 / 0.5).
	 * A victory is authoritative only when CanClaimVictory() passes (the run is
	 * standing in the realm's last room and the boss room was completed); any
	 * other victory request is rejected with OnBankingFailed.
	 */
	UFUNCTION(BlueprintCallable) void EndExpedition(bool bVictory, bool bVoluntaryRetreat);

	/** Index of the realm's Boss room; INDEX_NONE when no realm is loaded (AUD-22). */
	UFUNCTION(BlueprintPure) int32 GetBossRoomIndex() const;
	/** True when this expedition committed its realm's boss room (AUD-22). */
	UFUNCTION(BlueprintPure) bool IsBossRoomCompleted() const;
	/**
	 * The authoritative victory gate (AUD-22): current room is the last room
	 * (the safe exit) AND the boss room is committed. Victory banking, first-
	 * clear rewards, and boss proof are all keyed off this.
	 */
	UFUNCTION(BlueprintPure) bool CanClaimVictory() const;
	/**
	 * Settlement policy query: boss proof/campaign flags are written only for
	 * a victory whose boss room was actually completed. Defeat and retreat are
	 * always false, even if the boss was cleared before the run ended.
	 */
	UFUNCTION(BlueprintPure) bool ShouldWriteBossProof(bool bVictory) const;
	/**
	 * Safe-room exit request. Validates the victory gate, then banks at 100%.
	 * Returns false (and leaves the expedition open) when the gate fails.
	 * True means the settlement was accepted; a banking failure still leaves
	 * IsInExpedition() true and loot retryable (AUD-07).
	 */
	UFUNCTION(BlueprintCallable) bool RequestExit();
	/**
	 * Distinct voluntary retreat: banks the current run loot at the 70%
	 * retreat ratio and never writes boss proof/campaign flags (AUD-22).
	 * Returns true when the retreat settlement was accepted.
	 */
	UFUNCTION(BlueprintCallable) bool RequestRetreat();
	UFUNCTION(BlueprintPure) bool IsInExpedition() const;
	UFUNCTION(BlueprintPure) int32 GetCurrentRoomIndex() const;
	UFUNCTION(BlueprintPure) EQinglanRoomKind GetCurrentRoomKind() const;
	UFUNCTION(BlueprintPure) const UQinglanRealmDataAsset* GetCurrentRealm() const;

	/** Blessing draft: offers exactly 3 mutually exclusive options. */
	UFUNCTION(BlueprintCallable) bool GetBlessingOffer(TArray<FQinglanBlessing>& OutOffers);
	UFUNCTION(BlueprintCallable) bool AcceptBlessing(FName BlessingId);

	/** Loot kept until banking; visible in the pause HUD. */
	UFUNCTION(BlueprintPure) FQinglanInventory GetUnbankedLoot() const;

	/** Clears every expedition-owned state (profile switch with no checkpoint; AUD-04). */
	UFUNCTION(BlueprintCallable) void ResetRun();
	/** Realm of the open checkpoint resume target; NAME_None when at the sanctuary (AUD-02). */
	UFUNCTION(BlueprintPure) FName GetPendingCheckpointRealm() const;
	/** Loaded checkpoints are restored once after the matching realm world/pawn exists. */
	UFUNCTION(BlueprintPure) bool NeedsCheckpointResume() const;
	/** Cooked map for the loaded realm; NAME_None when absent or unsupported. */
	UFUNCTION(BlueprintPure) FName GetPendingCheckpointMap() const;
	void MarkCheckpointResumed();

	/** Checkpoint capture for the save subsystem (pre-room snapshot). */
	void ExportCheckpoint(struct FQinglanCheckpoint& Out) const;
	void ImportCheckpoint(const struct FQinglanCheckpoint& In);

	UPROPERTY(BlueprintAssignable) FQinglanRoomEntered OnRoomEntered;
	UPROPERTY(BlueprintAssignable) FQinglanRoomCompleted OnRoomCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanExpeditionEnded OnExpeditionEnded;
	/** Banking could not allocate/commit its transaction; loot stays unbanked and retryable (AUD-07). */
	UPROPERTY(BlueprintAssignable) FQinglanBankingFailed OnBankingFailed;
};
