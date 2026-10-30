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
#include "Combat/QinglanCombatComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Core/QinglanTrialDirector.h"
#include "Core/QinglanTrialAdmission.h"
#include "Core/QinglanRoomLayout.h"
#include "Core/QinglanSceneInspections.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Crc.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveSubsystem.h"
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
	const FName GHubShrineId(TEXT("hub.shrine"));
	const FName GCultivationShrineId(TEXT("hub.cultivation_shrine"));
	const FName GTrialPrepShrineId(TEXT("trial.preparation_shrine"));
	const FName GHubWorkbenchId(TEXT("hub.workbench"));
	const FName GHubRestStationId(TEXT("hub.rest_station"));
	const FName GBambooExitId(TEXT("bamboo.room_5_safe"));
	const FName GAshenExitId(TEXT("ashen.boss_5"));

	/** Story completion flags (data/story main-early.json sets_flags). */
	const FName GCh00CompleteFlag(TEXT("mq_ch00_complete"));
	const FName GCh01CompleteFlag(TEXT("mq_ch01_complete"));

	/**
	 * Room layout contract (QinglanContentBootstrap.cpp BootstrapMaps): seven
	 * 45 m rooms spaced 55 m apart along +X, corridor centered between rooms.
	 */

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
	 * FindInteractableRoomEntranceStart convention: exact "Qinglan.RoomEntrance.<N>" tag,
	 * then any entrance-tagged start). Maps authored by the bootstrap carry no
	 * tagged starts, so callers fall back to the layout-contract position.
	 */
	static APlayerStart* FindInteractableRoomEntranceStart(UWorld* World, int32 RoomIndex)
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
			if (!AnyEntrance && Start->PlayerStartTag == FName(*Prefix))
			{
				AnyEntrance = Start;
			}
		}
		return RoomIndex == 0 ? AnyEntrance : nullptr;
	}

	/** Teleports the player to a room entrance: authored PlayerStart first, layout-contract position otherwise. */
	void TeleportToRoomEntrance(AQinglanCharacter* Player, int32 RoomIndex)
	{
		if (Player == nullptr)
		{
			return;
		}
		const FVector LayoutPosition = QinglanRoomLayout::RoomEntrance(RoomIndex);
		if (APlayerStart* Entrance = FindInteractableRoomEntranceStart(Player->GetWorld(), RoomIndex))
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
	 * a trial already runs. Initialization failure leaves the objective pending
	 * so the player can retry or return safely. Returns true when the press was consumed.
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
		const UGameInstance* Instance = Anchor->GetGameInstance();
		const UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		const QinglanTrialAdmission::FResult Admission = QinglanTrialAdmission::EvaluateScene(
			ProofScene, ProofObjective, Profile, Story);
		if (!Admission.IsAllowed())
		{
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(Admission.ReasonKey));
			return true;
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
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.in_progress"))));
			return true;
		}
		if (Director != nullptr && Director->BeginTrial(ProofScene, ProofObjective, DurationSeconds, Tier))
		{
			// AUD-11: authored preparation proof term (CH03_S01).
			QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("trial.admission_confirmed")), {});
			return true;
		}

		UE_LOG(LogQinglan, Warning, TEXT("Trial %s could not start; objective remains pending"), *ProofScene.ToString());
		Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.unavailable"))));
		return true;
	}

	// --- Agreement revision entry (AUD-17) ---

	/** True when the stage carries a protocol negotiation action; only such
	 *  completed stages may replay with live choices instead of the brief
	 *  recap (side-quests.json select_proposal / accept_verified_proposal). */
	bool IsAgreementRevisionStage(const FQinglanRegistryScene& Stage)
	{
		for (const FQinglanRegistryChoice& Choice : Stage.Choices)
		{
			if (Choice.Action == FName(TEXT("select_proposal"))
				|| Choice.Action == FName(TEXT("accept_verified_proposal")))
			{
				return true;
			}
		}
		return false;
	}

	/** Quest availability gate mirroring GetAvailableSideQuests' check. */
	bool QuestAvailabilityMet(const UQinglanStorySubsystem* Story, const FQinglanRegistrySideQuest& Quest)
	{
		for (const FName Flag : Quest.Availability)
		{
			if (!Story->HasFlag(Flag))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * AUD-17 anchor pass: completed protocol stages replay as agreement
	 * revisions (live choices) instead of the brief recap. When several
	 * protocol quests share one anchor the undecided one (pending !=
	 * effective) goes first, then registry order; one interaction begins one
	 * revision and repeat presses reach the others (per-interaction
	 * limitation of the hub loop). Returns true when a revision began.
	 */
	bool TryBeginRevisionAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, FName SlashForm, AQinglanCharacter* Player)
	{
		if (Story == nullptr || Story->IsEndingConfirmed())
		{
			return false;
		}
		const UQinglanStoryRegistry* Registry = Story->GetRegistry();
		if (Registry == nullptr)
		{
			return false;
		}
		for (int32 TieBreak = 0; TieBreak < 2; ++TieBreak)
		{
			for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
			{
				const bool bUndecided = QinglanStoryRuntime::GetPendingAgreement(Story, Quest.QuestId)
					!= Story->GetEffectiveAgreement(Quest.QuestId);
				if (bUndecided != (TieBreak == 0) || !QuestAvailabilityMet(Story, Quest))
				{
					continue; // pass 0: undecided quests first; pass 1: the rest
				}
				for (const FQinglanRegistryScene& Stage : Quest.Stages)
				{
					if ((Stage.Location == AnchorId || Stage.Location == SlashForm)
						&& IsAgreementRevisionStage(Stage) && Story->IsSceneCompleted(Stage.SceneId))
					{
						if (Player != nullptr)
						{
							// Status before the renegotiation: which plan is effective,
							// which draft still awaits its reply.
							Player->ShowGameMessage(QinglanStoryRuntime::PendingStatusText(Story, Quest.QuestId));
						}
						if (QinglanStoryRuntime::BeginRevisionScene(Story, Stage.SceneId))
						{
							return true;
						}
						// Guarded out (unmet stage prerequisites, live playback):
						// fall back to the recap branch below.
					}
				}
			}
		}
		return false;
	}

	// --- Gameplay-proof guards (AUD-11) ---

	/** Registry objective lookup used by the anchor proof path. */
	const FQinglanRegistryObjective* QinglanFindRegistryObjective(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId)
	{
		const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
		if (Registry == nullptr || SceneId.IsNone() || ObjectiveId.IsNone())
		{
			return nullptr;
		}
		FName QuestId = NAME_None;
		const FQinglanRegistryScene* Scene = Registry->FindMainScene(SceneId);
		if (Scene == nullptr)
		{
			Scene = Registry->FindSideStage(SceneId, QuestId);
		}
		if (Scene == nullptr)
		{
			return nullptr;
		}
		for (const FQinglanRegistryObjective& Objective : Scene->Objectives)
		{
			if (Objective.ObjectiveId == ObjectiveId)
			{
				return &Objective;
			}
		}
		return nullptr;
	}

	bool QinglanObjectiveHasGuard(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId)
	{
		const FQinglanRegistryObjective* Objective = QinglanFindRegistryObjective(Story, SceneId, ObjectiveId);
		return Objective != nullptr && Objective->Guard.AllOf.Num() > 0;
	}

	/**
	 * Next authored ordered step. EvaluateGuard appends ordered terms' remaining
	 * steps to Missing in authored order, so the first remaining step is the
	 * interaction this anchor should register. Returns None when every ordered
	 * prefix is already complete or the objective has no ordered term.
	 */
	FName QinglanNextExpectedOrderedStep(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId)
	{
		const FQinglanRegistryObjective* Objective = QinglanFindRegistryObjective(Story, SceneId, ObjectiveId);
		if (Objective == nullptr)
		{
			return NAME_None;
		}
		const QinglanStoryRuntime::FQinglanGuardReport Report =
			QinglanStoryRuntime::EvaluateObjectiveGuard(Story, SceneId, ObjectiveId);
		for (const FQinglanRegistryGuardTerm& Term : Objective->Guard.AllOf)
		{
			if (Term.Kind != FName(TEXT("ordered")))
			{
				continue;
			}
			for (const FName Step : Term.OrderedSteps)
			{
				if (Report.Missing.Contains(Step))
				{
					return Step;
				}
			}
		}
		return NAME_None;
	}

	/** Localized guard notice; `guard.missing` is not authored yet, so the
	 *  scaffold literal is the fallback (no JSON key is added here). */
	FText QinglanGuardMissingText()
	{
		const FName Key(TEXT("guard.missing"));
		return UQinglanHUDStatics::HasGameText(Key)
			? UQinglanHUDStatics::GameText(Key)
			: NSLOCTEXT("Qinglan", "Guard.Missing", "The required proof is not complete yet.");
	}

	FText QinglanGuardOrderResetText()
	{
		return NSLOCTEXT("Qinglan", "Guard.OrderReset", "The sequence was out of order; start again.");
	}
} // namespace

// ---------------------------------------------------------------------------
// QinglanInteraction helpers
// ---------------------------------------------------------------------------

bool QinglanInteraction::TravelToMap(UObject* WorldContext, const TCHAR* MapPath)
{
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::LogAndReturnNull)
		: nullptr;
	if (!World)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TravelToMap: no world context; travel to %s dropped"), MapPath);
		return false;
	}
	// AUD-01/02: persist the current session before leaving its safe boundary.
	// A failed write keeps the player here and SaveNow broadcasts OnSaveFailed.
	if (UQinglanSaveSubsystem* Save = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UQinglanSaveSubsystem>() : nullptr)
	{
		FText Error;
		if (!Save->SaveActiveSession(Error))
		{
			UE_LOG(LogQinglan, Warning, TEXT("Travel blocked by save failure: %s"), *Error.ToString());
			// TODO(AUD-02/51): bind failure notice to localized retry/cancel UI.
			return false;
		}
	}
	UGameplayStatics::OpenLevel(World, FName(MapPath), /*bAbsolute=*/true, TEXT(""));
	return true;
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

bool QinglanInteraction::BeginSceneAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, bool bAllowRevisit, AQinglanCharacter* Player)
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
	// Pass 2 (opt-in): completed protocol stages replay as agreement
	// revisions (live choices) before any brief revisit branch.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bRequireUncompleted = (Pass == 0) || !bAllowRevisit;
		if (Pass == 1 && bAllowRevisit && TryBeginRevisionAtAnchor(Story, AnchorId, SlashForm, Player))
		{
			return true;
		}
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

	// Only synthesized realm doors enable this tick. Other station/document
	// actors remain event-driven with no per-frame work.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // check the final movement position for this frame
}

void AQinglanInteractable::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!InteractionId.ToString().StartsWith(TEXT("door."))) { return; }
	UGameInstance* Instance = GetGameInstance();
	UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (!Run || !Run->IsInExpedition() || Run->NeedsCheckpointResume()) { return; }
	const int32 Room = Run->GetCurrentRoomIndex();
	if (InteractionId != FName(*FString::Printf(TEXT("door.%d_%d"), Room, Room + 1))) { return; }
	AQinglanCharacter* Player = nullptr;
	for (TActorIterator<AQinglanCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsPlayerControlled()) { Player = *It; break; }
	}
	if (!Player) { return; }
	const FVector Location = Player->GetActorLocation();
	const float Radius = Player->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float InsideX = GetActorLocation().X - Radius - 5.f;
	if (Location.X < InsideX) { return; }

	// Crossing the authored corridor mouth is the same authoritative action
	// as pressing E. Do not leave players walking into rooms with no wave
	// because they missed an invisible interaction point. Existing encounter
	// and mandatory-blessing rules in ExecuteInteraction remain authoritative.
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	const bool bInCorridor = FMath::Abs(Location.Y - GetActorLocation().Y) <= 400.f - Radius;
	if (bInCorridor && !IsAnyEncounterActive(GetWorld()) && Player->CanStartAction(EQinglanActionKind::Move)
		&& (!Story || !Story->IsScenePlaying()))
	{
		ExecuteInteraction(Player);
		if (Run->GetCurrentRoomIndex() != Room) { return; }
	}

	// A live encounter, menu, dialogue, or blessing keeps the player inside
	// the current room. Preserve lateral/backward movement so this is a
	// boundary, rather than an input freeze or a forced teleport to spawn.
	FVector Inside = Location;
	Inside.X = InsideX;
	Player->SetActorLocation(Inside, false, nullptr, ETeleportType::TeleportPhysics);
	if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
	{
		Movement->Velocity.X = FMath::Min(0.f, Movement->Velocity.X);
	}
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
	if (QinglanSceneInspections::Execute(this, Player)) { return; }

	// CH01's inspection route ends at shrine 3, after two actual encounters,
	// the blessing choice and the inspection outlet. Full realms keep their
	// separate guardian/exit rules below.
	if (InteractionId == FName(TEXT("bamboo.tutorial_return")))
	{
		const UQinglanRealmDataAsset* Realm = Run ? Run->GetCurrentRealm() : nullptr;
		if (!Player || !Story || !Realm
			|| Realm->RealmId != FName(UQinglanEconomyLibrary::TutorialRealmIdText)
			|| Run->GetCurrentRoomIndex() != 3 || IsAnyEncounterActive(GetWorld()))
		{
			return;
		}
		if (!Story->HasFlag(FName(TEXT("mq_ch01_short_route_clear"))) || !Run->CanClaimVictory())
		{
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("story.ch01.s02.objective"))));
			return;
		}
		if (Run->RequestExit() && !Run->IsInExpedition())
		{
			QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
		}
		return;
	}

	// AUD-11 producer: the shrine is the free recovery station. Restore health
	// and spiritual energy (no cost) before any scene routing so the authored
	// CH00_S02 proof is registered on every shrine interaction.
	if (InteractionId == GHubShrineId && Player != nullptr)
	{
		if (UQinglanCombatComponent* Combat = Player->GetCombat())
		{
			const FQinglanAttributes Attributes = Combat->GetAttributes();
			Combat->Heal(Attributes.MaxHealth);
			Combat->GrantEnergy(Attributes.MaxSpiritualEnergy);
		}
		if (Story != nullptr)
		{
			QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("shrine.recovery_used")), {});
		}
	}

	// Objective-proof pass (blueprint section 1.9 "the dispatcher binds
	// gameplay events -> mq_* flags"): choice-free scenes commit only through
	// NotifyObjectiveCompleted, so a re-press at the anchor after the script
	// played out IS the gameplay proof for training/examination/preparation
	// objectives. Objectives that author a gameplay guard are evaluated
	// instead; ordered device guards advance one authored step per press
	// (AUD-11). Runs before every scene-beginning branch below.
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
			if (QinglanObjectiveHasGuard(Story, ProofScene, ProofObjective))
			{
				QinglanStoryRuntime::FQinglanGuardReport Report =
					QinglanStoryRuntime::EvaluateObjectiveGuard(Story, ProofScene, ProofObjective);
				if (Report.Status == QinglanStoryRuntime::EQinglanGuardStatus::OrderReset)
				{
					QinglanStoryRuntime::ResetOrderedProgress(Story, ProofScene);
					Player->ShowGameMessage(QinglanGuardOrderResetText());
					return true;
				}
				// Advance the current ordered step, if this objective authors
				// one. The event name comes straight from the registry data.
				const FName NextStep = QinglanNextExpectedOrderedStep(Story, ProofScene, ProofObjective);
				if (NextStep != NAME_None)
				{
					QinglanStoryRuntime::NotifyGameplayEvent(Story, NextStep, {});
					Report = QinglanStoryRuntime::EvaluateObjectiveGuard(Story, ProofScene, ProofObjective);
					if (Report.Status == QinglanStoryRuntime::EQinglanGuardStatus::OrderReset)
					{
						QinglanStoryRuntime::ResetOrderedProgress(Story, ProofScene);
						Player->ShowGameMessage(QinglanGuardOrderResetText());
						return true;
					}
				}
				if (Report.Status == QinglanStoryRuntime::EQinglanGuardStatus::Satisfied)
				{
					Story->NotifySceneObjectiveCompleted(ProofScene, ProofObjective);
				}
				else
				{
					Player->ShowGameMessage(QinglanGuardMissingText());
				}
				return true;
			}
			Story->NotifySceneObjectiveCompleted(ProofScene, ProofObjective);
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
		// AUD-11: authored exit-intake proof term (CH02_S03).
		if (Story != nullptr)
		{
			QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("exit.reconnect_ack")), {});
		}
		if (Run != nullptr && Run->IsInExpedition())
		{
			// AUD-22: a victory is claimable only from the last (exit) room with
			// the realm's boss room completed. Otherwise the run is not finished
			// and this safe-room anchor refuses to settle it; a distinct retreat
			// is UQinglanRunSubsystem::RequestRetreat (70%, never boss proof).
			if (!Run->RequestExit())
			{
				if (Run->CanClaimVictory())
				{
					return; // banking failed; retain the listener's retry message and the run loot
				}
				UE_LOG(LogQinglan, Warning, TEXT("Exit %s refused: boss room not cleared"), *Id);
				if (Player)
				{
					// FIXME-CONTRACT: no string-table key for the sealed-exit state.
					Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Exit.BossNotCleared",
						"The way is sealed until the realm's guardian falls."));
				}
				return;
			}
			if (Run->IsInExpedition())
			{
				return; // AUD-07: banking failed; do not abandon retryable loot.
			}
		}
		QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
		return;
	}

	// AUD-11: CH00_S01's authored proof term. The courtyard bell is the only
	// interaction at this anchor, so the press itself is the pulled bell.
	if (InteractionId == FName(TEXT("hub.courtyard")) && Story != nullptr)
	{
		QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("interact.bell_pulled")), {});
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
			// Record departure only once the route is actually available. An
			// early gate press must neither complete CH00 nor leave a proof
			// event that an unrelated later action can reuse to skip departure.
			if (!Story->HasFlag(FName(TEXT("mq_ch00_route_read"))))
			{
				if (Player) { Player->ShowGameMessage(QinglanGuardMissingText()); }
				return;
			}
			// AUD-11: the gate press is the authored departure confirmation.
			QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("interact.departure_confirmed")), {});
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
				if (!QinglanInteraction::TravelToMap(this, QinglanInteraction::BambooMapPath))
				{
					Run->ResetRun(); // Failed departure must allow the hub gate to retry.
				}
			}
			return;
		}
		if (Run->StartExpedition(FName(UQinglanEconomyLibrary::BambooRealmIdText),
				QinglanInteraction::MakeExpeditionSeed(FName(UQinglanEconomyLibrary::BambooRealmIdText), this)))
		{
			if (!QinglanInteraction::TravelToMap(this, QinglanInteraction::BambooMapPath))
			{
				Run->ResetRun();
			}
		}
		return;
	}

	if (InteractionId == GHubAshenGateId)
	{
		// Ashen follows Foundation and its hub debrief; Bamboo proof alone
		// must not bypass the first trial or its chapter transition.
		if (!Profile || Profile->GetCultivationStage() == EQinglanStage::QiRefining
			|| !Story || !Story->HasFlag(FName(TEXT("mq_ch03_complete"))))
		{
			if (Player)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.blocked.story_required"))));
			}
			return;
		}
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
			if (!QinglanInteraction::TravelToMap(this, QinglanInteraction::AshenMapPath))
			{
				Run->ResetRun();
			}
		}
		return;
	}

	if (InteractionId == GHubTrialGateId)
	{
		// The gate anchors CH05_S04 (Yun's confession aftermath); that scene
		// plays before the admission check can travel anywhere (AUD-10).
		if (QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId, /*bAllowRevisit=*/false))
		{
			return;
		}
		const QinglanTrialAdmission::ETrial Trial = Profile
			? QinglanTrialAdmission::SelectForStage(Profile->GetCultivationStage())
			: QinglanTrialAdmission::ETrial::Foundation;
		const QinglanTrialAdmission::FResult Admission = QinglanTrialAdmission::EvaluateEntry(Trial, Profile, Story);
		if (!Admission.IsAllowed())
		{
			UE_LOG(LogQinglan, Log, TEXT("Trial gate refused: %s"), *Admission.ReasonKey.ToString());
			if (Player)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(Admission.ReasonKey));
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
					// AUD-11: authored CH03/CH06 preparation proof terms.
					if (UQinglanStorySubsystem* RankStory = Player && Player->GetGameInstance()
							? Player->GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
					{
						if (Profile->GetCultivationStage() == EQinglanStage::QiRefining && Profile->GetCultivationRank() >= 3)
						{
							QinglanStoryRuntime::NotifyGameplayEvent(RankStory, FName(TEXT("cultivation.qi_rank_3")), {});
						}
						else if (Profile->GetCultivationStage() == EQinglanStage::FoundationEstablishment && Profile->GetCultivationRank() >= 3)
						{
							QinglanStoryRuntime::NotifyGameplayEvent(RankStory, FName(TEXT("cultivation.foundation_rank_3")), {});
						}
					}
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
		// AUD-11: the authored CH03_S01 proof requires the admission confirmed
		// "at shrine checkpoint"; the shrine IS the confirmation station. Emit
		// it when the profile already meets the Foundation prerequisites (Qi
		// rank 3 + Bamboo clear), which is non-circular: the trial itself is
		// entered later, after CH03_S01 commits mq_ch03_prepared.
		if (Story != nullptr && Profile->GetCultivationStage() == EQinglanStage::QiRefining)
		{
			if (Profile->GetCultivationRank() >= 3 && IsBambooCleared(Profile, Story))
			{
				QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("trial.admission_confirmed")), {});
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
					Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.in_progress"))));
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

	// --- Sanctuary rest station: safe hub time advance (AUD-15) ---
	if (InteractionId == GHubRestStationId)
	{
		if (Story == nullptr)
		{
			UE_LOG(LogQinglan, Warning, TEXT("Rest station: story subsystem unavailable"));
			return;
		}
		if (IsAnyEncounterActive(GetWorld()))
		{
			// Never advance fiction time mid-fight (mirrors the locked door).
			UE_LOG(LogQinglan, Warning, TEXT("Rest station %s refused: encounter active"), *Id);
			if (Player)
			{
				// FIXME-CONTRACT: no string-table key for the refused rest state.
				Player->ShowGameMessage(NSLOCTEXT("Qinglan", "Rest.Locked", "Clear the enemies first."));
			}
			return;
		}
		const QinglanStoryRuntime::FQinglanDeliveryStatus Pending = QinglanStoryRuntime::QueryPendingDeliveries(Story);
		if (!Pending.bHasPending)
		{
			if (Player)
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("hub.rest.none_pending"))));
			}
			return; // no reply in flight: the fiction clock does not move
		}
		// One rest settles every in-flight round trip: whole hours, at least
		// one (an already-elapsed arrival still needs one positive advance).
		const int32 RestHours = FMath::Max(1, FMath::CeilToInt(Pending.RemainingHours));
		QinglanStoryRuntime::AdvanceHubTime(Story, RestHours);
		if (Player)
		{
			const QinglanStoryRuntime::FQinglanDeliveryStatus After = QinglanStoryRuntime::QueryPendingDeliveries(Story);
			if (After.bHasPending)
			{
				// Defensive: a rest that could not settle everything reports
				// what is still in flight.
				FFormatNamedArguments Args;
				Args.Add(TEXT("hours"), FText::AsNumber(RestHours));
				TArray<FString> QuestIds;
				for (const FName Quest : After.Quests)
				{
					QuestIds.Add(Quest.ToString());
				}
				Args.Add(TEXT("quests"), FText::FromString(FString::Join(QuestIds, TEXT(", "))));
				Player->ShowGameMessage(FText::Format(
					UQinglanHUDStatics::GameText(FName(TEXT("hub.rest.summary"))), Args));
			}
			else
			{
				// The arrival notice itself is shown by the delivery-arrived
				// binding (Character); this line only reports the hours passed.
				Player->ShowGameMessage(UQinglanHUDStatics::FormatGameTextWithNumber(
					FName(TEXT("hub.rest.passed")), FName(TEXT("hours")), RestHours));
			}
		}
		return;
	}

	// --- Scene anchors ("hub.*", "sq.*", "bamboo.*"): BeginScene or Verbose ---
	if (!QinglanInteraction::BeginSceneAtAnchor(Story, InteractionId, /*bAllowRevisit=*/true, Player))
	{
		UE_LOG(LogQinglanNarrative, Verbose, TEXT("Interact %s: prerequisites unmet or no scene"), *Id);
	}
}
