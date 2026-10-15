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
class UQinglanRunSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable) bool StartExpedition(FName RealmId, int32 Seed);
	UFUNCTION(BlueprintCallable) bool EnterRoom(int32 RoomIndex);
	/** Called by the encounter director when all room enemies are dead. */
	UFUNCTION(BlueprintCallable) void CompleteCurrentRoom(FName TransactionId);
	/** Exit-shrine banking; multiplier from outcome type (1.0 / 0.7 / 0.5). */
	UFUNCTION(BlueprintCallable) void EndExpedition(bool bVictory, bool bVoluntaryRetreat);
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

	/** Checkpoint capture for the save subsystem (pre-room snapshot). */
	void ExportCheckpoint(struct FQinglanCheckpoint& Out) const;
	void ImportCheckpoint(const struct FQinglanCheckpoint& In);

	UPROPERTY(BlueprintAssignable) FQinglanRoomEntered OnRoomEntered;
	UPROPERTY(BlueprintAssignable) FQinglanRoomCompleted OnRoomCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanExpeditionEnded OnExpeditionEnded;
	/** Banking could not allocate/commit its transaction; loot stays unbanked and retryable (AUD-07). */
	UPROPERTY(BlueprintAssignable) FQinglanBankingFailed OnBankingFailed;
};
