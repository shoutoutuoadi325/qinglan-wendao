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
	/** Un-accepted draft offer restored without rerolling; INDEX_NONE when no draft is pending. */
	UPROPERTY() TArray<FName> PendingBlessingOffer;
	/** Owning room of the pending draft (the room whose completion rolled it). */
	UPROPERTY() int32 PendingBlessingRoom = INDEX_NONE;
	UPROPERTY() FQinglanAttributes PlayerAttributes;
	UPROPERTY() FQinglanInventory Loot;
	UPROPERTY() TArray<int32> CommittedRoomIds;
};

/** One quest's pending courier delivery (SQ02/SQ03 proposal round trip);
 *  timestamps are story-clock seconds advanced only by safe hub rests. */
USTRUCT()
struct FQinglanNarrativeDelivery
{
	GENERATED_BODY()

	/** Pending version this delivery was dispatched for; NAME_None = none. */
	UPROPERTY() FName Version = NAME_None;
	/** Story-clock second the courier was dispatched (the selection moment). */
	UPROPERTY() double DispatchedAtSeconds = 0.0;
	/** Story-clock second at which the matching reply arrives. */
	UPROPERTY() double ReadyAtStorySeconds = 0.0;
	/** True once the safe hub time advance delivered the matching reply. */
	UPROPERTY() bool bDelivered = false;
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
	/** Hub story clock in seconds; advanced only by safe hub time advances. */
	UPROPERTY() double StoryClockSeconds = 0.0;
	/** QuestId -> pending courier delivery (SQ02/SQ03 round trips). */
	UPROPERTY() TMap<FName, FQinglanNarrativeDelivery> Deliveries;
	UPROPERTY() EQinglanEnding ConfirmedEnding = EQinglanEnding::None;
};

/** Full profile save v1. */
UCLASS()
class QINGLANASCENSION_API UQinglanSaveGame : public USaveGame // QINGLANASCENSION_API: linkage-only export for the editor automation specs (NewObject in a transient package); public API unchanged.
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
	/** Permanent relics bought at the sanctuary (docs/03 section 5); equip at most one. */
	UPROPERTY() TArray<FName> OwnedRelics;
	UPROPERTY() FName EquippedRelic = NAME_None;
	/** Passive manuals studied (docs/03 section 4); equip at most two. */
	UPROPERTY() TArray<FName> OwnedPassives;
	UPROPERTY() TArray<FName> EquippedPassives;
	/** Talisman id -> remaining expedition charges; refilled at the sanctuary. */
	UPROPERTY() TMap<FName, int32> TalismanCharges;
	UPROPERTY() TArray<FName> CampaignFlags;
	UPROPERTY() TArray<FName> CommittedTransactionIds;
	UPROPERTY() bool bHasCheckpoint = false;
	UPROPERTY() FQinglanCheckpoint Checkpoint;
	UPROPERTY() FQinglanNarrativeState Narrative;
	/** FNV-1a checksum over the serialized body; detects corruption, not tampering. */
	UPROPERTY() uint64 BodyChecksum = 0;
};
