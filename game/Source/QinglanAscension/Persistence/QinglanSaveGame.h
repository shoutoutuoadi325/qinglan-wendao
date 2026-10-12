// Save game object: schema v1, mirrors data/examples/save.json.
// Stable ASCII IDs only; no asset pointers, no localized text, no absolute paths.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanSaveGame.generated.h"

/** Optional expedition checkpoint; null means "at the sanctuary". */
USTRUCT()
struct FQinglanCheckpoint
{
	GENERATED_BODY()

	UPROPERTY() int32 RealmSeed = 0;
	UPROPERTY() int32 EncounterStreamState = 0;
	UPROPERTY() int32 LootStreamState = 0;
	UPROPERTY() int32 BlessingStreamState = 0;
	UPROPERTY() int32 RoomIndex = 0;
	UPROPERTY() FName RealmId = NAME_None;
	UPROPERTY() TArray<FName> AcquiredBlessings;
	UPROPERTY() FQinglanAttributes PlayerAttributes;
	UPROPERTY() FQinglanInventory Loot;
	UPROPERTY() TArray<int32> CommittedRoomIds;
};

/** Narrative section of the profile save. */
USTRUCT()
struct FQinglanNarrativeState
{
	GENERATED_BODY()

	UPROPERTY() FName Revision = NAME_None;
	UPROPERTY() TArray<FName> CompletedScenes;
	UPROPERTY() TArray<FName> CompletedSideStages;
	UPROPERTY() TArray<FName> Flags;
	/** ChoiceId -> chosen option Id. */
	UPROPERTY() TMap<FName, FName> ChoiceDecisions;
	UPROPERTY() TArray<FName> ReadRecords;
	/** QuestId -> pending version state (proposal awaiting delivery). */
	UPROPERTY() TMap<FName, FName> PendingAgreements;
	/** QuestId -> effective version state (delivered + verified + confirmed). */
	UPROPERTY() TMap<FName, FName> EffectiveAgreements;
	UPROPERTY() EQinglanEnding ConfirmedEnding = EQinglanEnding::None;
};

/** Full profile save v1. */
UCLASS()
class UQinglanSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 SchemaVersion = 1;
	UPROPERTY() FName ContentVersion = NAME_None;
	UPROPERTY() FName ProfileId = NAME_None;
	/** Monotonic; every accepted save increments it. */
	UPROPERTY() int32 SaveSequence = 0;
	UPROPERTY() EQinglanStage CultivationStage = EQinglanStage::QiRefining;
	UPROPERTY() int32 CultivationRank = 0;
	UPROPERTY() FQinglanInventory Inventory;
	UPROPERTY() TArray<FName> UnlockedTechniques;
	UPROPERTY() TArray<FName> EquippedTechniques;
	UPROPERTY() TArray<FName> CampaignFlags;
	UPROPERTY() TArray<FName> CommittedTransactionIds;
	UPROPERTY() bool bHasCheckpoint = false;
	UPROPERTY() FQinglanCheckpoint Checkpoint;
	UPROPERTY() FQinglanNarrativeState Narrative;
	/** FNV-1a checksum over the serialized body; detects corruption, not tampering. */
	UPROPERTY() uint64 BodyChecksum = 0;
};
