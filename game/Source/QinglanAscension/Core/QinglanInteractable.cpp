// Interactable implementation: anchor-keyed dispatch (records, scenes, gates,
// cultivation stations, the sanctuary shop, realm exits, and room doors).
//
// Primary sources:
// - docs/dev/level-layout.md (anchor table + intent per map)
// - docs/dev/implementation-blueprint.md sections 1.4 (7-room contract +
//   blessing drafts), 1.5 (economy/first-clear), 1.9 (scene payloads)
// - docs/03-progression-and-economy.md sections 1 (cultivation), 4 (passives),
//   5 (relics/talismans), 6 (banking)
// - data/story/*.json location keys (anchor matching) and localization/en-US.json
//   (string-table keys; no new keys are authored here)
//
// Every path degrades to logs: missing registry/assets/subsystems never crash
// (code-contracts.md "Null-safe degradation").
#include "Core/QinglanInteractable.h"

#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Components/SphereComponent.h"
#include "Core/QinglanTrialDirector.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Crc.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Progression/QinglanDeterminism.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"
#include "UI/QinglanHUDStatics.h"

namespace
{
	/** Interaction sphere radius (task contract ~250 cm; proximity aiming is the character's 300 cm cone). */
	constexpr float QinglanInteractionSphereRadius = 250.f;

	/** Authored anchor keys with hard-coded behavior (docs/dev/level-layout.md). */
	const FName GHubBambooGateId(TEXT("hub.bamboo_gate"));
	const FName GHubAshenGateId(TEXT("hub.ashen_gate"));
	const FName GHubTrialGateId(TEXT("hub.trial_gate"));
	const FName GCultivationShrineId(TEXT("hub.cultivation_shrine"));
	const FName GTrialPrepShrineId(TEXT("trial.preparation_shrine"));
	const FName GHubWorkbenchId(TEXT("hub.workbench"));
	const FName GBambooExitId(TEXT("bamboo.room_5_safe"));
	const FName GAshenExitId(TEXT("ashen.boss_5"));

	/** Story completion flags (data/story main-early.json sets_flags). */
	const FName GCh00CompleteFlag(TEXT("mq_ch00_complete"));
	const FName GCh01CompleteFlag(TEXT("mq_ch01_complete"));

	/**
	 * Room layout contract (QinglanContentBootstrap.cpp BootstrapMaps): seven
	 * 45 m rooms spaced 55 m apart along +X, corridor centered between rooms.
	 */
	constexpr float QinglanRoomSpacing = 5500.f;
	constexpr float QinglanRoomTeleportHeight = 150.f;

	/** PlayerStart tag convention (Core/QinglanGameModeBase.cpp GInteractableRoomEntranceTagPrefix). */
	const TCHAR* GInteractableRoomEntranceTagPrefix = TEXT("Qinglan.RoomEntrance");

	/** Cultivation transaction-id convention (deterministic, idempotent; mirrors the automation specs' "rank_qi_3"/"bt_foundation" style). */
	FString StageToken(EQinglanStage Stage)
	{
		switch (Stage)
		{
		case EQinglanStage::FoundationEstablishment: return TEXT("foundation");
		case EQinglanStage::GoldenCore: return TEXT("golden_core");
		case EQinglanStage::QiRefining:
		default: return TEXT("qi_refining");
		}
	}

	FText StageNameText(EQinglanStage Stage)
	{
		// String-table keys localization/en-US.json "cultivation.*".
		const TCHAR* Key = nullptr;
		switch (Stage)
		{
		case EQinglanStage::FoundationEstablishment: Key = TEXT("cultivation.foundation"); break;
		case EQinglanStage::GoldenCore: Key = TEXT("cultivation.golden_core"); break;
		case EQinglanStage::QiRefining:
		default: Key = TEXT("cultivation.qi_refining"); break;
		}
		return UQinglanHUDStatics::GameText(FName(Key));
	}

	/** Lowercases and strips separators so anchor suffixes match authored record locations ("hub.apprentice_quarters" == "Hub / apprentice quarters"; lore.json). */
	FString NormalizeLocationKey(const FString& Source)
	{
		FString Normalized;
		Normalized.Reserve(Source.Len());
		for (const TCHAR Char : Source)
		{
			if (Char == TEXT(' ') || Char == TEXT('.') || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('/'))
			{
				continue;
			}
			Normalized.AppendChar(FChar::ToLower(Char));
		}
		return Normalized;
	}

	/** "sq.bamboo.old_bell_safe" -> "bamboo/old_bell_safe"; other ids keep their form. */
	FName SlashFormOfAnchor(FName AnchorId)
	{
		FString Form = AnchorId.ToString();
		if (Form.StartsWith(TEXT("sq.")))
		{
			Form = Form.RightChop(3);
		}
		Form.ReplaceInline(TEXT("."), TEXT("/"), ESearchCase::CaseSensitive);
		return FName(*Form);
	}

	/** Finds the lore record whose location matches the "record.<map>.<place>" anchor suffix; NAME_None when the registry is absent. */
	FName RecordIdForAnchor(const UQinglanStorySubsystem* Story, FName AnchorId)
	{
		const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
		if (Registry == nullptr)
		{
			return NAME_None; // degrade silently (registry not imported yet)
		}
		FString Suffix = AnchorId.ToString();
		if (Suffix.StartsWith(TEXT("record.")))
		{
			Suffix = Suffix.RightChop(7);
		}
		const FString NormalizedSuffix = NormalizeLocationKey(Suffix);
		for (const FQinglanRegistryRecord& Record : Registry->Records)
		{
			if (NormalizeLocationKey(Record.Location.ToString()) == NormalizedSuffix)
			{
				return Record.RecordId;
			}
		}
		return NAME_None;
	}

	/** True when any encounter director in this world reports an active fight. */
	bool IsAnyEncounterActive(UWorld* World)
	{
		if (World == nullptr)
		{
			return false;
		}
		for (TActorIterator<AQinglanEncounterDirector> It(World); It; ++It)
		{
			if (It->IsEncounterActive())
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Best room-entrance PlayerStart for a room (Core/QinglanGameModeBase.cpp
	 * FindRoomEntranceStart convention: exact "Qinglan.RoomEntrance.<N>" tag,
	 * then any entrance-tagged start). Maps authored by the bootstrap carry no
	 * tagged starts, so callers fall back to the layout-contract position.
	 */
	APlayerStart* FindRoomEntranceStart(UWorld* World, int32 RoomIndex)
	{
		if (World == nullptr)
		{
			return nullptr;
		}
		const FString Prefix(GInteractableRoomEntranceTagPrefix);
		APlayerStart* AnyEntrance = nullptr;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			APlayerStart* Start = *It;
			if (!Start)
			{
				continue;
			}
			if (Start->PlayerStartTag == FName(*FString::Printf(TEXT("%s.%d"), *Prefix, RoomIndex)))
			{
				return Start;
			}
			if (!AnyEntrance && Start->PlayerStartTag.ToString().StartsWith(Prefix))
			{
				AnyEntrance = Start;
			}
		}
		return AnyEntrance;
	}

	/** Teleports the player to a room entrance: authored PlayerStart first, layout-contract position otherwise. */
	void TeleportToRoomEntrance(AQinglanCharacter* Player, int32 RoomIndex)
	{
		if (Player == nullptr)
		{
			return;
		}
		const FVector LayoutPosition(RoomIndex * QinglanRoomSpacing, 0.f, QinglanRoomTeleportHeight);
		if (APlayerStart* Entrance = FindRoomEntranceStart(Player->GetWorld(), RoomIndex))
		{
			Player->SetActorLocationAndRotation(
				Entrance->GetActorLocation(), Entrance->GetActorRotation(), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
			return;
		}
		Player->SetActorLocation(LayoutPosition, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	}

	/** Bamboo proof for ashen access: the same campaign/story pair the Foundation breakthrough gate reads (Progression/QinglanProgressionTables.h). */
	bool IsBambooCleared(const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
	{
		const bool bCampaign = Profile != nullptr
			&& Profile->HasCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag));
		const bool bStory = Story != nullptr
			&& Story->HasFlag(FName(QinglanProgression::BambooClearedStoryFlag));
		return bCampaign || bStory;
	}

	/** Ashen proof for trial access: the pair the Golden Core breakthrough gate reads. */
	bool IsAshenCleared(const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
	{
		const bool bCampaign = Profile != nullptr
			&& Profile->HasCampaignFlag(FName(QinglanProgression::AshenClearedCampaignFlag));
		const bool bStory = Story != nullptr
			&& Story->HasFlag(FName(QinglanProgression::AshenClearedStoryFlag));
		return bCampaign || bStory;
	}

	// --- Timed trial proof (CH03_S02 foundation / CH06_S02 golden core) ---

	/** Trial scenes: their choice-free objective is earned by surviving the timed wave trial. */
	const FName GTrialFoundationSceneId(TEXT("CH03_S02"));
	const FName GTrialGoldenCoreSceneId(TEXT("CH06_S02"));

	bool IsTrialProofScene(FName SceneId)
	{
		return SceneId == GTrialFoundationSceneId || SceneId == GTrialGoldenCoreSceneId;
	}

	/**
	 * Trial clock per scene. Foundation: docs/story/CH03.md S02 "Use the
	 * existing 90-second Foundation trial" (data/story/main-early.json CH03_S02
	 * authors no number). Golden core: docs/story/CH06.md S02 "three 50-second
	 * storm phases for the established 150-second trial"; data/story/main-late.json
	 * CH06_S02 encodes the same clock in its script triggers (storm_time_0,
	 * storm_time_50, storm_time_100, storm_survived_150). Tier is a seed/balance
	 * hook (1 = foundation, 2 = golden core).
	 */
	float TrialDurationSecondsFor(FName SceneId, int32& OutTier)
	{
		OutTier = 0;
		if (SceneId == GTrialFoundationSceneId)
		{
			OutTier = 1;
			return 90.f;
		}
		if (SceneId == GTrialGoldenCoreSceneId)
		{
			OutTier = 2;
			return 150.f;
		}
		return 0.f;
	}

	AQinglanTrialDirector* FindTrialDirector(UWorld* World)
	{
		return World
			? Cast<AQinglanTrialDirector>(UGameplayStatics::GetActorOfClass(World, AQinglanTrialDirector::StaticClass()))
			: nullptr;
	}

	/**
	 * True when every authored line of the scene's script is gameplay-gated
	 * (CH06_S02 storm beats: storm_time_0/50/100, storm_survived_150, ...).
	 * Such a script never delivers a line through dialogue alone, so dialogue
	 * can never "exhaust" it the way WasSceneScriptExhausted requires - the
	 * trial itself is the script's gameplay moment, and its stopped playback
	 * counts as played out.
	 */
	bool SceneScriptIsEventDriven(const UQinglanStorySubsystem* Story, FName SceneId)
	{
		const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
		const FQinglanRegistryScene* Main = Registry ? Registry->FindMainScene(SceneId) : nullptr;
		if (Main == nullptr || Main->Script.IsEmpty())
		{
			return false;
		}
		for (const FQinglanRegistryLine& Line : Main->Script)
		{
			if (Line.Trigger == NAME_None && Line.Event == NAME_None)
			{
				return false;
			}
		}
		return true;
	}

	/** Trial proof gate: the script played out (or is gameplay-gated throughout). */
	bool TrialProofReady(const AQinglanCharacter* Player, const UQinglanStorySubsystem* Story, FName ProofScene)
	{
		if (Player != nullptr && Player->WasSceneScriptExhausted(ProofScene))
		{
			return true;
		}
		return SceneScriptIsEventDriven(Story, ProofScene);
	}

	/**
	 * Routes an exhausted trial scene into the timed trial system: finds (or
	 * spawns, once per world) the trial director at this anchor, refuses while
	 * a trial already runs, and keeps the v1 direct-complete fallback when no
	 * director can exist. Returns true when the press was consumed.
	 */
	bool TryBeginTrialAtAnchor(AActor* Anchor, AQinglanCharacter* Player,
		UQinglanStorySubsystem* Story, FName ProofScene, FName ProofObjective)
	{
		if (Story == nullptr || Player == nullptr || Anchor == nullptr)
		{
			return false;
		}
		int32 Tier = 0;
		const float DurationSeconds = TrialDurationSecondsFor(ProofScene, Tier);
		if (DurationSeconds <= 0.f)
		{
			return false; // not a trial scene: caller keeps the generic proof
		}

		// One trial director per arena world, positioned at the trial anchor
		// (the storm platform center; both trial stages anchor there).
		UWorld* World = Anchor->GetWorld();
		AQinglanTrialDirector* Director = FindTrialDirector(World);
		if (Director == nullptr && World != nullptr)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.Name = TEXT("QinglanTrialDirector");
			Director = World->SpawnActor<AQinglanTrialDirector>(
				AQinglanTrialDirector::StaticClass(), Anchor->GetActorLocation(), FRotator::ZeroRotator, Params);
			if (Director)
			{
				UE_LOG(LogQinglan, Log, TEXT("Trial anchor spawned trial director for %s"), *ProofScene.ToString());
			}
		}

		if (Director != nullptr && Director->IsTrialActive())
		{
			// One trial at a time; after a failure the defeat flow returns the
			// player to the hub, and the next arena press restarts clean.
			// FIXME-CONTRACT: no string-table key for the in-progress refusal.
			Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Trial.InProgress", "The trial is already underway."));
			return true;
		}
		if (Director != nullptr && Director->BeginTrial(ProofScene, ProofObjective, DurationSeconds, Tier))
		{
			return true;
		}

		// Null-safe fallback (no director / wave engine unavailable): the
		// pre-trial stand-in proof, so the objective never dead-ends.
		if (Director == nullptr)
		{
			UE_LOG(LogQinglan, Warning, TEXT("Trial anchor: no trial director; committing %s directly"),
				*ProofObjective.ToString());
		}
		Story->NotifyObjectiveCompleted(ProofObjective);
		return true;
	}
} // namespace

// ---------------------------------------------------------------------------
// QinglanInteraction helpers
// ---------------------------------------------------------------------------

void QinglanInteraction::TravelToMap(UObject* WorldContext, const TCHAR* MapPath)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::LogAndReturnNull)
		: nullptr;
	if (!World)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TravelToMap: no world context; travel to %s dropped"), MapPath);
		return;
	}
	UGameplayStatics::OpenLevel(World, FName(MapPath), /*bAbsolute=*/true, TEXT(""));
}

int32 QinglanInteraction::MakeExpeditionSeed(FName RealmId, const UObject* WorldContext)
{
	// Deterministic mix of realm + fiction clock (safe hub time only advances
	// through QinglanStoryRuntime::AdvanceHubTime, so a given campaign moment
	// always seeds identical rooms/loot/blessings streams).
	UGameInstance* Instance = nullptr;
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	Instance = World ? World->GetGameInstance() : nullptr;
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	const double StoryClock = Story ? QinglanStoryRuntime::GetStoryClockSeconds(Story) : 0.0;
	const int32 Mixed = static_cast<int32>(FCrc::StrCrc32(
		*FString::Printf(TEXT("%s.%d"), *RealmId.ToString(), static_cast<int32>(StoryClock))));
	return QinglanDeterminism::CanonicalSeed(Mixed);
}

bool QinglanInteraction::BeginSceneAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, bool bAllowRevisit)
{
	if (Story == nullptr)
	{
		return false;
	}
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (Registry == nullptr || AnchorId == NAME_None)
	{
		return false;
	}
	const FName SlashForm = SlashFormOfAnchor(AnchorId);

	// Pass 1: uncompleted scenes in authored order (the player's next beat);
	// Pass 2 (opt-in): completed scenes replay their brief revisit branch.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bRequireUncompleted = (Pass == 0) || !bAllowRevisit;
		for (const FQinglanRegistryScene& Scene : Registry->MainScenes)
		{
			if ((Scene.Location == AnchorId || Scene.Location == SlashForm)
				&& (!bRequireUncompleted || !Story->IsSceneCompleted(Scene.SceneId)))
			{
				if (Story->BeginScene(Scene.SceneId))
				{
					return true;
				}
				// Unmet prerequisites: try the next scene at this anchor.
			}
		}
		for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
		{
			for (const FQinglanRegistryScene& Stage : Quest.Stages)
			{
				if ((Stage.Location == AnchorId || Stage.Location == SlashForm)
					&& (!bRequireUncompleted || !Story->IsSceneCompleted(Stage.SceneId)))
				{
					if (Story->BeginScene(Stage.SceneId))
					{
						return true;
					}
				}
			}
		}
	}
	UE_LOG(LogQinglanNarrative, Verbose, TEXT("No playable scene at anchor %s"), *AnchorId.ToString());
	return false;
}

bool QinglanInteraction::SceneHasChoices(UQinglanStorySubsystem* Story, FName SceneId)
{
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (Registry == nullptr)
	{
		return false;
	}
	FName QuestId = NAME_None;
	if (const FQinglanRegistryScene* Main = Registry->FindMainScene(SceneId))
	{
		return Main->Choices.Num() > 0;
	}
	if (const FQinglanRegistryScene* Stage = Registry->FindSideStage(SceneId, QuestId))
	{
		return Stage->Choices.Num() > 0;
	}
	return false;
}

FName QinglanInteraction::ChapterIdOfScene(UQinglanStorySubsystem* Story, FName SceneId)
{
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (const FQinglanRegistryScene* Main = Registry ? Registry->FindMainScene(SceneId) : nullptr)
	{
		return Main->ChapterId;
	}
	return NAME_None;
}

namespace
{
	/** Prerequisite check identical to UQinglanStorySubsystem::BeginScene's gate. */
	bool ScenePrerequisitesMet(const UQinglanStorySubsystem* Story, const FQinglanRegistryScene& Scene)
	{
		for (const FName Prerequisite : Scene.Prerequisites)
		{
			if (!Story->HasFlag(Prerequisite))
			{
				return false;
			}
		}
		return true;
	}

	/** Shared proof-target predicate: choice-free, uncompleted, prerequisites met, objective authored. */
	bool IsObjectiveProofTarget(const UQinglanStorySubsystem* Story, const FQinglanRegistryScene& Scene)
	{
		return Scene.Choices.Num() == 0
			&& Scene.Objectives.Num() > 0
			&& !Story->IsSceneCompleted(Scene.SceneId)
			&& ScenePrerequisitesMet(Story, Scene);
	}
} // namespace

bool QinglanInteraction::FindObjectiveProofAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, FName& OutSceneId, FName& OutObjectiveId)
{
	OutSceneId = NAME_None;
	OutObjectiveId = NAME_None;
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (Registry == nullptr || AnchorId == NAME_None)
	{
		return false;
	}
	const FName SlashForm = SlashFormOfAnchor(AnchorId);
	for (const FQinglanRegistryScene& Scene : Registry->MainScenes)
	{
		if ((Scene.Location == AnchorId || Scene.Location == SlashForm) && IsObjectiveProofTarget(Story, Scene))
		{
			OutSceneId = Scene.SceneId;
			OutObjectiveId = Scene.Objectives[0].ObjectiveId;
			return true;
		}
	}
	for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
	{
		for (const FQinglanRegistryScene& Stage : Quest.Stages)
		{
			if ((Stage.Location == AnchorId || Stage.Location == SlashForm) && IsObjectiveProofTarget(Story, Stage))
			{
				OutSceneId = Stage.SceneId;
				OutObjectiveId = Stage.Objectives[0].ObjectiveId;
				return true;
			}
		}
	}
	return false;
}

bool QinglanInteraction::FindObjectiveProofInChapters(UQinglanStorySubsystem* Story, const TArray<FName>& ChapterIds, FName& OutSceneId, FName& OutObjectiveId)
{
	OutSceneId = NAME_None;
	OutObjectiveId = NAME_None;
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (Registry == nullptr)
	{
		return false;
	}
	for (const FName ChapterId : ChapterIds)
	{
		for (const FQinglanRegistryScene& Scene : Registry->MainScenes)
		{
			if (Scene.ChapterId == ChapterId && IsObjectiveProofTarget(Story, Scene))
			{
				OutSceneId = Scene.SceneId;
				OutObjectiveId = Scene.Objectives[0].ObjectiveId;
				return true;
			}
		}
	}
	return false;
}

bool QinglanInteraction::BeginFirstSceneOfChapters(UQinglanStorySubsystem* Story, const TArray<FName>& ChapterIds)
{
	if (Story == nullptr)
	{
		return false;
	}
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (Registry == nullptr)
	{
		return false;
	}
	for (const FName ChapterId : ChapterIds)
	{
		const FQinglanRegistryChapter* Chapter = nullptr;
		for (const FQinglanRegistryChapter& Candidate : Registry->Chapters)
		{
			if (Candidate.ChapterId == ChapterId)
			{
				Chapter = &Candidate;
				break;
			}
		}
		if (Chapter == nullptr)
		{
			continue;
		}
		for (const FName SceneId : Chapter->SceneIds)
		{
			if (Story->IsSceneCompleted(SceneId))
			{
				continue;
			}
			// BeginScene false = unmet prerequisites (the chapter chain is not
			// here yet) or another scene playing; both are non-error states.
			if (Story->BeginScene(SceneId))
			{
				return true;
			}
		}
	}
	return false;
}

TArray<FName> QinglanInteraction::ChaptersForRealm(FName RealmId)
{
	// Entering a realm starts its current chapter scene (task contract;
	// data/story chapter ids). The guided tutorial (CH01) is the bamboo map's
	// first visit; free bamboo expeditions pick up CH02/CH03.
	TArray<FName> Chapters;
	if (RealmId == FName(UQinglanEconomyLibrary::TutorialRealmIdText))
	{
		Chapters.Add(FName(TEXT("CH01")));
	}
	else if (RealmId == FName(UQinglanEconomyLibrary::BambooRealmIdText))
	{
		Chapters.Add(FName(TEXT("CH02")));
		Chapters.Add(FName(TEXT("CH03")));
	}
	else if (RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
	{
		Chapters.Add(FName(TEXT("CH04")));
		Chapters.Add(FName(TEXT("CH05")));
	}
	return Chapters;
}

TArray<FName> QinglanInteraction::ChaptersForMapName(const FString& MapName)
{
	// Map fallbacks cover trial/epilogue travel without an open expedition:
	// the arena hosts the CH03 foundation trial as well as the CH06 decision.
	TArray<FName> Chapters;
	if (MapName.Contains(TEXT("Bamboo")))
	{
		Chapters = { FName(TEXT("CH02")), FName(TEXT("CH03")) };
	}
	else if (MapName.Contains(TEXT("Ashen")))
	{
		Chapters = { FName(TEXT("CH04")), FName(TEXT("CH05")) };
	}
	else if (MapName.Contains(TEXT("Tribulation")) || MapName.Contains(TEXT("Arena")))
	{
		Chapters = { FName(TEXT("CH03")), FName(TEXT("CH06")) };
	}
	return Chapters;
}

// ---------------------------------------------------------------------------
// AQinglanInteractable
// ---------------------------------------------------------------------------

AQinglanInteractable::AQinglanInteractable()
{
	// Authoring marker only: aiming is a camera-cone distance check on the
	// character side, so the shape never blocks movement or queries.
	USphereComponent* Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("InteractionSphere"));
	Sphere->InitSphereRadius(QinglanInteractionSphereRadius);
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetRootComponent(Sphere);
	InteractionSphere = Sphere;

	PrimaryActorTick.bCanEverTick = false;
}

void AQinglanInteractable::ExecuteInteraction(AQinglanCharacter* Player)
{
	if (InteractionId == NAME_None)
	{
		return;
	}
	UGameInstance* Instance = GetGameInstance();
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	const FString Id = InteractionId.ToString();

	// Objective-proof pass (blueprint section 1.9 "the dispatcher binds
	// gameplay events -> mq_* flags"): choice-free scenes commit only through
	// NotifyObjectiveCompleted, so a re-press at the anchor after the script
	// played out IS the gameplay proof for training/examination/preparation
	// objectives. Runs before every scene-beginning branch below.
	const bool bAnchorProofConsumed = [&]()
	{
		FName ProofScene = NAME_None;
		FName ProofObjective = NAME_None;
		if (Story && Player
			&& QinglanInteraction::FindObjectiveProofAtAnchor(Story, InteractionId, ProofScene, ProofObjective)
			&& Player->WasSceneScriptExhausted(ProofScene))
		{
			if (IsTrialProofScene(ProofScene))
			{
				// Trial scenes (CH03_S02 sits at trial.foundation_arena): the
				// proof is surviving the timed wave trial, not the re-press.
				return TryBeginTrialAtAnchor(this, Player, Story, ProofScene, ProofObjective);
			}
			Story->NotifyObjectiveCompleted(ProofObjective);
			return true;
		}
		return false;
	}();
	if (bAnchorProofConsumed)
	{
		return;
	}

	// --- Room doors (synthesized by the spawner; ids "door.<from>_<to>") ---
	if (Id.StartsWith(TEXT("door.")))
	{
		int32 FromRoom = INDEX_NONE;
		int32 ToRoom = INDEX_NONE;
		FString Rest = Id.RightChop(5);
		TArray<FString> Parts;
		Rest.ParseIntoArray(Parts, TEXT("_"));
		if (Parts.Num() == 2)
		{
			FromRoom = FCString::Atoi(*Parts[0]);
			ToRoom = FCString::Atoi(*Parts[1]);
		}
		if (Run == nullptr || !Run->IsInExpedition())
		{
			UE_LOG(LogQinglan, Verbose, TEXT("Door %s: no active expedition"), *Id);
			return;
		}
		if (IsAnyEncounterActive(GetWorld()))
		{
			// Never advance rooms mid-fight (blueprint 1.4: doors open on clear).
			UE_LOG(LogQinglan, Warning, TEXT("Door %s locked: encounter active"), *Id);
			if (Player)
			{
				// FIXME-CONTRACT: no string-table key for the locked door state.
				Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Door.Locked", "Clear the room first."));
			}
			return;
		}
		if (Run->GetCurrentRoomIndex() != FromRoom)
		{
			UE_LOG(LogQinglan, Verbose, TEXT("Door %s: room %d is not the current room"), *Id, Run->GetCurrentRoomIndex());
			return;
		}
		// Blessing draft first (docs/03 section 5: drafts after rooms 2 and 4
		// pause the pipeline until one of three options is accepted).
		TArray<FQinglanBlessing> Offer;
		if (Run->GetBlessingOffer(Offer))
		{
			if (Player)
			{
				Player->PresentBlessingOffer(Offer);
			}
			return;
		}
		if (Run->EnterRoom(ToRoom))
		{
			TeleportToRoomEntrance(Player, ToRoom);
			UE_LOG(LogQinglan, Log, TEXT("Door %s: entered room %d"), *Id, ToRoom);
		}
		return;
	}

	// --- Lore records ("record.<map>.<place>") ---
	if (Id.StartsWith(TEXT("record.")))
	{
		const FName RecordId = RecordIdForAnchor(Story, InteractionId);
		if (RecordId == NAME_None)
		{
			UE_LOG(LogQinglanNarrative, Verbose, TEXT("Record anchor %s: registry has no matching record"), *Id);
			return;
		}
		// TryInspectRecord gates on RequiredFlags, journals the read through
		// the profile subsystem, and broadcasts OnRecordOpened (display).
		if (Story != nullptr && QinglanStoryRuntime::TryInspectRecord(Story, RecordId))
		{
			return;
		}
		UE_LOG(LogQinglanNarrative, Verbose, TEXT("Record %s not inspectable (requirements unmet or no story subsystem)"), *RecordId.ToString());
		return;
	}

	// --- Realm exits: safe-room scene first, banked retreat second ---
	if (InteractionId == GBambooExitId || InteractionId == GAshenExitId)
	{
		if (IsAnyEncounterActive(GetWorld()))
		{
			// Never strand the player inside a live encounter (task contract).
			UE_LOG(LogQinglan, Warning, TEXT("Exit %s refused: encounter active"), *Id);
			return;
		}
		if (QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId, /*bAllowRevisit=*/false))
		{
			return; // e.g. CH02_S03 intake reconnect plays before leaving
		}
		if (Run != nullptr && Run->IsInExpedition())
		{
			Run->EndExpedition(/*bVictory=*/true, /*bVoluntaryRetreat=*/true); // 100% banking, docs/03 section 6
		}
		QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
		return;
	}

	// --- Hub gates: story-driven departure -> tutorial -> free expeditions ---
	if (InteractionId == GHubBambooGateId)
	{
		if (Story == nullptr || Run == nullptr)
		{
			UE_LOG(LogQinglan, Warning, TEXT("Bamboo gate: subsystems unavailable"));
			return;
		}
		const bool bCh00Done = Story->HasFlag(GCh00CompleteFlag);
		const bool bCh01Done = Story->HasFlag(GCh01CompleteFlag);
		if (!bCh00Done)
		{
			// First departure: the CH00_S04 gate scene runs instead of travel.
			Story->BeginScene(FName(TEXT("CH00_S04")));
			return;
		}
		if (!bCh01Done)
		{
			// Guided tutorial expedition (CH01 scenes live in the bamboo map).
			if (Run->StartExpedition(FName(UQinglanEconomyLibrary::TutorialRealmIdText),
					QinglanInteraction::MakeExpeditionSeed(FName(UQinglanEconomyLibrary::TutorialRealmIdText), this)))
			{
				QinglanInteraction::TravelToMap(this, QinglanInteraction::BambooMapPath);
			}
			return;
		}
		if (Run->StartExpedition(FName(UQinglanEconomyLibrary::BambooRealmIdText),
				QinglanInteraction::MakeExpeditionSeed(FName(UQinglanEconomyLibrary::BambooRealmIdText), this)))
		{
			QinglanInteraction::TravelToMap(this, QinglanInteraction::BambooMapPath);
		}
		return;
	}

	if (InteractionId == GHubAshenGateId)
	{
		if (!IsBambooCleared(Profile, Story))
		{
			// Same proof the Foundation breakthrough gate reads (docs/03 section 1).
			UE_LOG(LogQinglan, Log, TEXT("Ashen gate locked: bamboo guardian not cleared"));
			if (Player)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("cultivation.blocked.boss_required"))));
			}
			return;
		}
		if (Run != nullptr && Run->StartExpedition(FName(UQinglanEconomyLibrary::AshenRealmIdText),
				QinglanInteraction::MakeExpeditionSeed(FName(UQinglanEconomyLibrary::AshenRealmIdText), this)))
		{
			QinglanInteraction::TravelToMap(this, QinglanInteraction::AshenMapPath);
		}
		return;
	}

	if (InteractionId == GHubTrialGateId)
	{
		if (!IsAshenCleared(Profile, Story))
		{
			// Same proof the Golden Core breakthrough gate reads (docs/03 section 1).
			UE_LOG(LogQinglan, Log, TEXT("Trial gate locked: ashen guardian not cleared"));
			if (Player)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("cultivation.blocked.boss_required"))));
			}
			return;
		}
		// Trials are scene-driven (CH03_S02/CH06_S02), not 7-room expeditions.
		QinglanInteraction::TravelToMap(this, QinglanInteraction::ArenaMapPath);
		return;
	}

	// --- Cultivation stations: rank purchase, then breakthrough ---
	if (InteractionId == GCultivationShrineId || InteractionId == GTrialPrepShrineId)
	{
		if (Profile == nullptr)
		{
			UE_LOG(LogQinglan, Warning, TEXT("Cultivation shrine: profile subsystem unavailable"));
			return;
		}
		FText BlockingReason;
		const bool bBreakthroughReady = Profile->CanAttemptBreakthrough(BlockingReason);
		bool bActed = false;
		if (bBreakthroughReady)
		{
			const FName TransactionId(*FString::Printf(TEXT("breakthrough.%s"), *StageToken(Profile->GetCultivationStage())));
			if (Profile->TryBreakthrough(TransactionId))
			{
				bActed = true;
				if (Player)
				{
					Player->ShowGameMessage(StageNameText(Profile->GetCultivationStage()));
				}
			}
			else if (Player)
			{
				Player->ShowGameMessage(BlockingReason);
			}
		}
		else
		{
			// Purchasable rank next (materials only; docs/03 section 1).
			FQinglanRankCost Cost;
			if (QinglanProgression::GetRankCostFor(Profile->GetCultivationStage(), Profile->GetCultivationRank(), Cost))
			{
				const FName TransactionId(*FString::Printf(TEXT("rank.%s.%d"),
					*StageToken(Profile->GetCultivationStage()), Profile->GetCultivationRank() + 1));
				if (Profile->TryAdvanceRank(TransactionId))
				{
					bActed = true;
					if (Player)
					{
						Player->ShowGameMessage(UQinglanHUDStatics::FormatGameTextWithNumber(
							FName(TEXT("cultivation.rank")), FName(TEXT("rank")), Profile->GetCultivationRank()));
					}
				}
				else if (Player)
				{
					// FIXME-CONTRACT: no string-table key exists for "insufficient
					// banked resources"; scaffold literal until ST_Game regenerates.
					Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Cultivation.Insufficient", "Not enough banked resources."));
				}
			}
			else if (Player)
			{
				// Stage maxed or rankless: surface the authoritative reason.
				Player->ShowGameMessage(BlockingReason);
			}
		}
		// The shrine anchors a chapter scene too (CH03_S01 preparation); it
		// plays once the cultivation transaction above is settled (no revisit
		// replay: the station stays a cultivation station afterwards).
		QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId, /*bAllowRevisit=*/false);
		if (!bActed)
		{
			UE_LOG(LogQinglan, Verbose, TEXT("Cultivation shrine %s: no transaction this visit"), *Id);
		}
		return;
	}

	// --- Arena trial stages: chapter scan (CH03 foundation / CH06 core) ---
	if (Id.StartsWith(TEXT("trial.")))
	{
		TArray<FName> Chapters;
		if (InteractionId == FName(TEXT("trial.foundation_arena")))
		{
			Chapters.Add(FName(TEXT("CH03")));
		}
		else
		{
			Chapters.Add(FName(TEXT("CH06"))); // golden core arena, governor + decision platforms
		}
		// Any trial-stage press while a trial runs is a refusal, before the
		// scene-begin path below could queue a deferred scene that a wave
		// boundary would then play mid-trial.
		if (AQinglanTrialDirector* Director = FindTrialDirector(GetWorld()))
		{
			if (Director->IsTrialActive())
			{
				if (Player)
				{
					// FIXME-CONTRACT: no string-table key for the in-progress refusal.
					Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Trial.InProgress", "The trial is already underway."));
				}
				return;
			}
		}
		// Trial proof first, BEFORE the scene replay below would restart an
		// already-exhausted script: the trial scenes' objectives (CH03_S02
		// 90 s foundation, CH06_S02 150 s golden core) are earned by surviving
		// the timed wave trial. The chapter scan covers the CH06 scene too,
		// whose authored location is prose (no anchor match above).
		FName ProofScene = NAME_None;
		FName ProofObjective = NAME_None;
		if (Player
			&& QinglanInteraction::FindObjectiveProofInChapters(Story, Chapters, ProofScene, ProofObjective)
			&& IsTrialProofScene(ProofScene)
			&& TrialProofReady(Player, Story, ProofScene))
		{
			TryBeginTrialAtAnchor(this, Player, Story, ProofScene, ProofObjective);
			return;
		}
		if (QinglanInteraction::BeginFirstSceneOfChapters(Story, Chapters))
		{
			return;
		}
		UE_LOG(LogQinglanNarrative, Verbose, TEXT("Trial anchor %s: no playable trial scene"), *Id);
		return;
	}

	// --- Sanctuary shop at the workbench (scene first, shop fallback) ---
	if (InteractionId == GHubWorkbenchId)
	{
		if (QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId, /*bAllowRevisit=*/false))
		{
			return; // e.g. CH02_S04 lantern repair scene
		}
		if (Profile != nullptr && Player != nullptr)
		{
			TArray<FQinglanShopOfferEntry> Entries;
			// Relics: skip owned, keep affordable (docs/03 section 5 prices).
			for (const FQinglanRelicDef& Relic : UQinglanEconomyLibrary::GetRelicDefs())
			{
				if (Entries.Num() >= 4)
				{
					break;
				}
				if (Profile->GetOwnedRelics().Contains(Relic.RelicId)
					|| Profile->GetInventory().VeinCrystal < Relic.VeinCrystalCost)
				{
					continue;
				}
				FQinglanShopOfferEntry Entry;
				Entry.ItemId = Relic.RelicId;
				Entry.bRelic = true;
				Entry.Title = UQinglanHUDStatics::HasGameText(Relic.NameTextKey)
					? UQinglanHUDStatics::GameText(Relic.NameTextKey)
					: FText::FromString(Relic.RelicId.ToString());
				Entry.CostSummary = UQinglanHUDStatics::FormatNumber64(Relic.VeinCrystalCost);
				Entries.Add(MoveTemp(Entry));
			}
			// Passives: study cost from the profile header constants (docs/03 section 4).
			for (const FQinglanPassiveCatalogEntry& Passive : UQinglanEconomyLibrary::GetPassiveCatalog())
			{
				if (Entries.Num() >= 4)
				{
					break;
				}
				if (Profile->GetOwnedPassives().Contains(Passive.PassiveId)
					|| Profile->GetInventory().SpiritShards < UQinglanProfileSubsystem::PassiveStudyCostShards
					|| Profile->GetInventory().SpiritHerb < UQinglanProfileSubsystem::PassiveStudyCostHerbs)
				{
					continue;
				}
				FQinglanShopOfferEntry Entry;
				Entry.ItemId = Passive.PassiveId;
				Entry.bRelic = false;
				Entry.Title = UQinglanHUDStatics::HasGameText(Passive.NameTextKey)
					? UQinglanHUDStatics::GameText(Passive.NameTextKey)
					: FText::FromString(Passive.PassiveId.ToString());
				Entry.CostSummary = UQinglanHUDStatics::FormatNumber64(UQinglanProfileSubsystem::PassiveStudyCostShards);
				Entries.Add(MoveTemp(Entry));
			}
			if (Entries.Num() > 0)
			{
				Player->PresentShopOffer(Entries);
			}
			else
			{
				// FIXME-CONTRACT: no string-table key for the empty shop state.
				Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Shop.Empty", "Nothing affordable today."));
			}
		}
		return;
	}

	// --- Scene anchors ("hub.*", "sq.*", "bamboo.*"): BeginScene or Verbose ---
	if (!QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId))
	{
		UE_LOG(LogQinglanNarrative, Verbose, TEXT("Interact %s: prerequisites unmet or no scene"), *Id);
	}
}
