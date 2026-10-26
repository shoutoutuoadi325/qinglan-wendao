// Story registry data assets: imported from data/story/*.json at edit time.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanStoryDataTypes.generated.h"

/** One scripted line; text resolved from the loc string table via TextKey. */
USTRUCT()
struct FQinglanRegistryLine
{
	GENERATED_BODY()
	UPROPERTY() FName Speaker = NAME_None;
	UPROPERTY() FName TextKey = NAME_None;
	UPROPERTY() FName SpeakerKey = NAME_None;
	UPROPERTY() EQinglanSourceKind SourceKind = EQinglanSourceKind::Live;
	UPROPERTY() FName SourceKindKey = NAME_None;
	/** Optional conditional trigger: line only plays when WhenAll/WhenNone match. */
	UPROPERTY() FName Event = NAME_None;
	UPROPERTY() FName Trigger = NAME_None;
	UPROPERTY() TArray<FName> WhenAll;
	UPROPERTY() TArray<FName> WhenNone;
};

USTRUCT()
struct FQinglanRegistryChoice
{
	GENERATED_BODY()
	UPROPERTY() FName ChoiceId = NAME_None;
	UPROPERTY() FName LabelKey = NAME_None;
	UPROPERTY() TArray<FName> ResponseKeys;
	UPROPERTY() TArray<FName> SetsFlags;
	/** Optional action payload (enter_trial, release_guardian, preview_ending, ...). */
	UPROPERTY() FName Action = NAME_None;
	UPROPERTY() TArray<FName> Prerequisites;
	UPROPERTY() FName PresentationGroup = NAME_None;
	UPROPERTY() FName ShowAfterTextKey = NAME_None;
};

/**
 * One machine-evaluated gameplay-proof term (AUD-11). The authored `proof`
 * payload lists these under `all_of`; kinds are event, event_count, ordered,
 * delivery, time_advance, and flag. `Kind` keeps the authored discriminator so
 * the runtime evaluator stays data-driven (unknown kinds are dropped by the
 * importer with a Warning).
 */
USTRUCT()
struct FQinglanRegistryGuardTerm
{
	GENERATED_BODY()
	UPROPERTY() FName Kind = NAME_None;
	UPROPERTY() FName Event = NAME_None;
	/** event_count threshold; authored `count` (clamped to >=1 at import). */
	UPROPERTY() int32 Count = 1;
	/** time_advance threshold in in-fiction hours. */
	UPROPERTY() double GameHours = 0.0;
	/** ordered: event names that must be fired in exactly this order. */
	UPROPERTY() TArray<FName> OrderedSteps;
	/** delivery: quest whose courier reply must no longer be pending. */
	UPROPERTY() FName Quest = NAME_None;
	/** flag: narrative flag that must be set. */
	UPROPERTY() FName Flag = NAME_None;
};

/** Conjunction of gameplay-proof terms (authored `proof` / structured `confirmation_guard`). */
USTRUCT()
struct FQinglanRegistryGuard
{
	GENERATED_BODY()
	UPROPERTY() TArray<FQinglanRegistryGuardTerm> AllOf;
};

USTRUCT()
struct FQinglanRegistryObjective
{
	GENERATED_BODY()
	UPROPERTY() FName ObjectiveId = NAME_None;
	UPROPERTY() FName TextKey = NAME_None;
	/** English-only completion contract note for QA; not player-facing. */
	UPROPERTY() FString Completion;
	/** Optional gameplay proof; empty means the legacy dialogue/proof path. */
	UPROPERTY() FQinglanRegistryGuard Guard;
};

USTRUCT()
struct FQinglanRegistryScene
{
	GENERATED_BODY()
	UPROPERTY() FName SceneId = NAME_None;
	UPROPERTY() FName ChapterId = NAME_None;
	UPROPERTY() FName Location = NAME_None;
	UPROPERTY() FName TitleKey = NAME_None;
	UPROPERTY() TArray<FName> Prerequisites;
	UPROPERTY() TArray<FQinglanRegistryLine> Script;
	UPROPERTY() TArray<FQinglanRegistryChoice> Choices;
	UPROPERTY() TArray<FQinglanRegistryObjective> Objectives;
	UPROPERTY() TArray<FName> SetsFlags;
	UPROPERTY() TArray<FName> NextSceneIds;
	/** Journal key rendered by the HUD/journal for this scene or stage (stage field journal_key). */
	UPROPERTY() FName JournalKey = NAME_None;
	UPROPERTY() FName RetryKey = NAME_None;
	UPROPERTY() FName ConfirmationGuard = NAME_None;
	/** Structured confirmation_checks_ready guard (AUD-10/AUD-11); evaluated
	 *  before confirmation dialogue begins and before the scene commits. */
	UPROPERTY() FQinglanRegistryGuard ConfirmationGuardTerms;
	/** Authored scene-level trigger (e.g. confirmation_checks_ready). */
	UPROPERTY() FName ScriptTrigger = NAME_None;
	UPROPERTY() FName AdvanceDeliveryKey = NAME_None;
	UPROPERTY() FName DeliveryTimeKey = NAME_None;
	UPROPERTY() FName DeliveryTransition = NAME_None;
	/** CH07_S04 scene-extras: quests whose ending-specific epilogue cards render
	 *  during the first-winter retrospective (optional_epilogue_quest_ids; the
	 *  keys themselves live on the quests' EpilogueKeys). */
	UPROPERTY() TArray<FName> OptionalEpilogueQuestIds;
	/** Authored data source of the optional epilogues (optional_epilogue_source). */
	UPROPERTY() FName OptionalEpilogueSource = NAME_None;
};

USTRUCT()
struct FQinglanRegistryChapter
{
	GENERATED_BODY()
	UPROPERTY() FName ChapterId = NAME_None;
	UPROPERTY() FName TitleKey = NAME_None;
	UPROPERTY() FName SummaryKey = NAME_None;
	UPROPERTY() TArray<FName> SceneIds;
};

USTRUCT()
struct FQinglanRegistrySideQuest
{
	GENERATED_BODY()
	UPROPERTY() FName QuestId = NAME_None;
	UPROPERTY() FName TitleKey = NAME_None;
	UPROPERTY() FName SummaryKey = NAME_None;
	UPROPERTY() TArray<FName> Availability;
	UPROPERTY() TArray<FQinglanRegistryScene> Stages;
	UPROPERTY() TArray<FName> CompletionFlags;
	/** Ending-specific epilogue text keys, authored order [open, seal, shared]
	 *  (blueprint section 2.1 CH07 "per-ending + per-side-quest closure"). */
	UPROPERTY() TArray<FName> EpilogueKeys;
	/** rewards.technique_access ids unlocked once at quest completion (currently
	 *  SQ04 mist_step + SQ05 orbiting_blades; blueprint section 2.5 unlock order). */
	UPROPERTY() TArray<FName> RewardTechniqueIds;
};

USTRUCT()
struct FQinglanRegistryRecord
{
	GENERATED_BODY()
	UPROPERTY() FName RecordId = NAME_None;
	UPROPERTY() FName Location = NAME_None;
	UPROPERTY() TArray<FName> RequiredFlags;
	UPROPERTY() FName DateLabelKey = NAME_None;
	UPROPERTY() FName TitleKey = NAME_None;
	UPROPERTY() FName BodyKey = NAME_None;
	UPROPERTY() FName CollectionFlag = NAME_None;
};

USTRUCT()
struct FQinglanRegistryBark
{
	GENERATED_BODY()
	UPROPERTY() FName BarkId = NAME_None;
	UPROPERTY() FName Speaker = NAME_None;
	UPROPERTY() FName Location = NAME_None;
	UPROPERTY() TArray<FName> RequiredFlags;
	UPROPERTY() TArray<FName> ForbiddenFlags;
	UPROPERTY() FName TextKey = NAME_None;
	UPROPERTY() float CooldownSeconds = 180.f;
	UPROPERTY() int32 MaxPerVisit = 1;
	UPROPERTY() bool bSuppressDuringCombat = true;
};

/**
 * Complete narrative registry. Populated by the editor importer from
 * the data/story JSON files and validated against both locales. Immutable at runtime.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanStoryRegistry : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY() TArray<FQinglanRegistryChapter> Chapters;
	UPROPERTY() TArray<FQinglanRegistryScene> MainScenes;
	UPROPERTY() TArray<FQinglanRegistrySideQuest> SideQuests;
	UPROPERTY() TArray<FQinglanRegistryRecord> Records;
	UPROPERTY() TArray<FQinglanRegistryBark> Barks;
	UPROPERTY() FName NarrativeRevision = NAME_None;

	const FQinglanRegistryScene* FindMainScene(FName SceneId) const;
	const FQinglanRegistryScene* FindSideStage(FName StageId, FName& OutQuestId) const;
	const FQinglanRegistrySideQuest* FindQuest(FName QuestId) const;
};

/** One realm room in the 7-room contract. */
USTRUCT()
struct FQinglanRealmRoom
{
	GENERATED_BODY()
	UPROPERTY() EQinglanRoomKind Kind = EQinglanRoomKind::Entrance;
	UPROPERTY() int32 SpiritShards = 0;
};

/** Realm (expedition template) data asset; mirrors data/examples/realm.json. */
UCLASS()
class QINGLANASCENSION_API UQinglanRealmDataAsset : public UPrimaryDataAsset // linkage-only export for editor automation specs
{
	GENERATED_BODY()

public:
	UPROPERTY() FName RealmId = NAME_None;
	UPROPERTY() FName NameTextKey = NAME_None;
	UPROPERTY() int32 DefaultSeed = 0;
	UPROPERTY() int32 MaxConcurrentEnemies = 8;
	UPROPERTY() TArray<FQinglanRealmRoom> Rooms;
	UPROPERTY() FQinglanInventory FirstClearBonus;

	EQinglanRoomKind GetRoomKind(int32 RoomIndex) const;
	int32 GetRoomShards(int32 RoomIndex) const;
	int32 GetRoomCount() const { return Rooms.Num(); }
};
