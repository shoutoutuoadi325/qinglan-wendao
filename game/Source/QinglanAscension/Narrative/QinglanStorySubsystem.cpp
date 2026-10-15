// Story subsystem implementation: scene playback, choice resolution, agreement
// versioning, ending preview/confirm, objective bridge, and ambient narrative.
//
// Primary sources:
// - docs/story/narrative-implementation.md (sections 2-9)
// - docs/dev/implementation-blueprint.md section 1.9 and the data contracts
//   in section 3 (data/story/*.json action payloads)
// - docs/dev/code-contracts.md "Narrative semantics"
//
// The scaffold header Narrative/QinglanStorySubsystem.h is frozen: its public
// API carries no initialize override, no tick, no save bridge, and no ambient
// (bark/record/dispatcher) entry points. Following the precedent of
// Persistence/QinglanSaveSubsystem.cpp (FQinglanSaveBookkeeping), all runtime
// state lives beside the instance in a weak-pointer-keyed store, and the extra
// surface is exposed through Narrative/QinglanStoryRuntime.h.
#include "Narrative/QinglanStorySubsystem.h"

#include "AI/QinglanEncounterDirector.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStoryIdentity.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "QinglanAscension.h"
#include "Subsystems/SubsystemCollection.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/** Logs use LogQinglanNarrative (code-contracts.md "Logs"). */

	FText TableText(FName Key)
	{
		if (Key == NAME_None)
		{
			return FText::GetEmpty();
		}
		return FText::FromStringTable(FName(QinglanStoryRuntime::GameStringTableId), *Key.ToString());
	}

	// -----------------------------------------------------------------------
	// Per-instance session store (frozen-header workaround; see file header).
	// -----------------------------------------------------------------------
	TMap<TWeakObjectPtr<UQinglanStorySubsystem>, FQinglanStorySession> GStorySessions;
#if WITH_DEV_AUTOMATION_TESTS
	TStrongObjectPtr<UQinglanStoryRegistry> GTestStoryRegistry;
#endif

	FQinglanStorySession& StorySession(UQinglanStorySubsystem* Story)
	{
		// Prune stale keys so automation instances cannot leak sessions.
		if (GStorySessions.Num() > 8)
		{
			for (auto It = GStorySessions.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid())
				{
					It.RemoveCurrent();
				}
			}
		}
		return GStorySessions.FindOrAdd(Story);
	}

	FQinglanStorySession* FindStorySession(UQinglanStorySubsystem* Story)
	{
		return GStorySessions.Find(Story);
	}

	const FQinglanStorySession* FindStorySession(const UQinglanStorySubsystem* Story)
	{
		// Session identity is the subsystem pointer; the map itself is mutable
		// bookkeeping, so a const query only ever reads it.
		return GStorySessions.Find(const_cast<UQinglanStorySubsystem*>(Story));
	}

	TMap<FName, FName> NormalizeSavedChoices(const UQinglanStoryRegistry* Registry, const TMap<FName, FName>& Input)
	{
		if (Registry == nullptr)
		{
			if (!Input.IsEmpty())
			{
				UE_LOG(LogQinglanNarrative, Warning, TEXT("Cannot restore choice history without story registry; dropping unverified entries"));
			}
			return {};
		}
		TArray<FName> Rejected;
		TMap<FName, FName> Result = QinglanStoryIdentity::NormalizeChoiceDecisions(*Registry, Input, Rejected);
		for (const FName Key : Rejected)
		{
			UE_LOG(LogQinglanNarrative, Warning, TEXT("Dropped unknown, ambiguous, or malformed saved choice '%s'; lost legacy history cannot be inferred"), *Key.ToString());
		}
		return Result;
	}

	// -----------------------------------------------------------------------
	// Registry access.
	// -----------------------------------------------------------------------

	/**
	 * Registry asset soft path /Game/Qinglan/Data/DA_StoryRegistry (module
	 * brief; imported by the editor importer from the data/story JSON files and
	 * immutable at runtime).
	 * FIXME-CONTRACT: the frozen scaffold header cannot declare an Initialize
	 * override, so the load happens lazily on first access instead of at
	 * Initialize; for this offline single-load game the observable behavior is
	 * identical (first query pays one synchronous load).
	 */
	const UQinglanStoryRegistry* LoadStoryRegistryOnce()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (GTestStoryRegistry.IsValid()) { return GTestStoryRegistry.Get(); }
#endif
		static TStrongObjectPtr<UQinglanStoryRegistry> Cached;
		if (Cached.IsValid())
		{
			return Cached.Get();
		}
		// Retry on every query instead of latching the first failure: the game
		// instance pre-warms the registry asynchronously, and a query racing
		// that warm-up (e.g. first-launch profile creation) must not cache the
		// miss permanently. Truly-missing content logs once at Warning then at
		// Verbose.
		static bool bWarned = false;
		const FString RegistrySoftPath(QinglanStoryRuntime::StoryRegistrySoftPath);
		// StaticLoadObject logs its own failure reason; LoadSynchronous fails
		// silently and the quiet null was hiding the actual loader error.
		if (UQinglanStoryRegistry* Loaded = Cast<UQinglanStoryRegistry>(
				StaticLoadObject(UQinglanStoryRegistry::StaticClass(), nullptr, *RegistrySoftPath)))
		{
			Cached = TStrongObjectPtr<UQinglanStoryRegistry>(Loaded);
			return Loaded;
		}
		// Missing asset (e.g. headless tests before the importer runs):
		// every registry query degrades to empty, never crash.
		if (!bWarned)
		{
			UE_LOG(LogQinglanNarrative, Warning, TEXT("Story registry asset not found; narrative queries return empty"));
			bWarned = true;
		}
		else
		{
			UE_LOG(LogQinglanNarrative, Verbose, TEXT("Story registry still missing"));
		}
		return nullptr;
	}

	/** Finds a scene in main scenes or side-quest stages; OutQuestId names the owning side quest. */
	const FQinglanRegistryScene* FindSceneAny(const UQinglanStoryRegistry* Registry, FName SceneId, FName& OutQuestId)
	{
		OutQuestId = NAME_None;
		if (Registry == nullptr)
		{
			return nullptr;
		}
		if (const FQinglanRegistryScene* Main = Registry->FindMainScene(SceneId))
		{
			return Main;
		}
		return Registry->FindSideStage(SceneId, OutQuestId);
	}

	const FQinglanRegistryScene* CurrentScene(const UQinglanStoryRegistry* Registry, const FQinglanStorySession& Session)
	{
		if (Registry == nullptr || !Session.bScenePlaying)
		{
			return nullptr;
		}
		if (Session.CurrentSceneQuestId != NAME_None)
		{
			FName QuestId = NAME_None;
			return Registry->FindSideStage(Session.CurrentSceneId, QuestId);
		}
		return Registry->FindMainScene(Session.CurrentSceneId);
	}

	// -----------------------------------------------------------------------
	// Encounter safety (docs/story/narrative-implementation.md section 3.2:
	// "Never freeze the player while enemies remain active").
	// -----------------------------------------------------------------------

	bool IsAnyEncounterActive(const UGameInstance* GameInstance)
	{
		if (GEngine == nullptr)
		{
			return false;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld())
			{
				continue;
			}
			if (GameInstance != nullptr && Context.OwningGameInstance != GameInstance)
			{
				continue; // another game instance's world (PIE sandboxes)
			}
			for (TActorIterator<AQinglanEncounterDirector> It(World); It; ++It)
			{
				if (It->IsEncounterActive())
				{
					return true;
				}
			}
		}
		return false;
	}

	/** Paused-game-time timestamp for cooldown bookkeeping (code-contracts.md). */
	double WorldTimeSeconds(const UQinglanStorySubsystem* Story)
	{
		const UGameInstance* GameInstance = Story->GetGameInstance();
		const UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
		return World ? World->GetTimeSeconds() : 0.0;
	}

	void StopPlayback(FQinglanStorySession& Session)
	{
		Session.bScenePlaying = false;
		Session.bRevisitBranch = false;
		Session.bChoicesPresented = false;
		Session.bChoicesResolved = true;
		Session.ResolvedChoiceGroups.Reset();
		Session.PresentedChoiceIds.Reset();
		Session.ActiveChoiceGroup = NAME_None;
		Session.ActiveChoicePin = NAME_None;
		Session.bAdvanceHalted = false;
		Session.NextScriptLine = 0;
		Session.PendingResponseKeys.Reset();
		Session.LastSelectedAction = NAME_None;
		Session.CurrentSceneId = NAME_None;
		Session.CurrentSceneQuestId = NAME_None;
	}

	/** Set semantics make every flag application idempotent: replaying a scene
	 *  can never re-apply or roll back a committed flag (spec section 3). */
	void ApplyFlags(FQinglanStorySession& Session, const TArray<FName>& FlagsToSet)
	{
		for (const FName Flag : FlagsToSet)
		{
			if (Flag != NAME_None)
			{
				Session.Flags.Add(Flag);
			}
		}
	}

	bool HasAllFlags(const FQinglanStorySession& Session, const TArray<FName>& Required)
	{
		for (const FName Flag : Required)
		{
			if (!Session.Flags.Contains(Flag))
			{
				return false;
			}
		}
		return true;
	}

	bool AllObjectivesComplete(const FQinglanStorySession& Session, const FQinglanRegistryScene& Scene)
	{
		for (const FQinglanRegistryObjective& Objective : Scene.Objectives)
		{
			if (!Session.CompletedObjectives.Contains(QinglanStoryIdentity::ObjectiveKey(Scene.SceneId, Objective.ObjectiveId)))
			{
				return false;
			}
		}
		return true;
	}

	FQinglanScriptLine MakeLine(const FQinglanRegistryLine& RegistryLine, FName SceneId, int32 LineIndex)
	{
		FQinglanScriptLine Line;
		Line.SpeakerName = TableText(RegistryLine.SpeakerKey);
		Line.Text = TableText(RegistryLine.TextKey);
		Line.SourceKind = RegistryLine.SourceKind;
		Line.SceneId = SceneId;
		Line.LineIndex = LineIndex;
		return Line;
	}

	/** Ordered delivery filter: WhenAll/WhenNone flag conditions plus optional
	 *  gameplay-event triggers (docs/story/narrative-implementation.md 3.4
	 *  "Preserve conditional responses exactly as authored"). */
	bool LineConditionsMet(const FQinglanRegistryLine& Line, const FQinglanStorySession& Session)
	{
		if (!HasAllFlags(Session, Line.WhenAll))
		{
			return false;
		}
		for (const FName Flag : Line.WhenNone)
		{
			if (Session.Flags.Contains(Flag))
			{
				return false;
			}
		}
		if (Line.Trigger != NAME_None && !Session.FiredEvents.Contains(Line.Trigger))
		{
			return false;
		}
		if (Line.Event != NAME_None && !Session.FiredEvents.Contains(Line.Event))
		{
			return false;
		}
		return true;
	}

	/** Registry action names (data/story JSON `action.type`). */
	bool IsAction(const FName& Action, const TCHAR* Name) { return Action == FName(Name); }

	/** Actions that must NOT auto-commit their scene when the script ends:
	 *  preview/stay/cancel/return keep the scene replayable
	 *  (data payload commits_scene=false); confirm_ending and
	 *  accept_verified_proposal commit only through their atomic operations. */
	bool ActionCommitsScene(FName Action)
	{
		if (Action == NAME_None)
		{
			return true;
		}
		return !(IsAction(Action, TEXT("stay_at_hub"))
			|| IsAction(Action, TEXT("preview_ending"))
			|| IsAction(Action, TEXT("confirm_ending"))
			|| IsAction(Action, TEXT("cancel_confirmation"))
			|| IsAction(Action, TEXT("return_to_hub"))
			|| IsAction(Action, TEXT("accept_verified_proposal")));
	}

	/** True when the choice's SetsFlags are applied by an atomic operation
	 *  instead of at selection (ending flags must never be creatable by a
	 *  mere choice click or preview; agreement flags only on effective commit). */
	bool ChoiceFlagsDeferredToAtomicOp(FName Action)
	{
		return IsAction(Action, TEXT("confirm_ending")) || IsAction(Action, TEXT("accept_verified_proposal"));
	}

	// FIXME-CONTRACT: FQinglanRegistryChoice stores only the action name; the
	// JSON payload map (ending_id, requires_preview, idempotency_key, ...) is
	// not imported. The dispatcher therefore derives the ending from the
	// authored choice id (CH06_S04: s04.preview_open / s04.confirm_shared ...)
	// and idempotency keys from scene+action (see MakeActionIdempotencyKey).
	FName EndingIdFromChoiceId(FName ChoiceId)
	{
		const FString Id = ChoiceId.ToString();
		if (Id.Contains(TEXT("shared")))
		{
			return FName(QinglanEnding::EndingSharedId);
		}
		if (Id.Contains(TEXT("seal")))
		{
			return FName(QinglanEnding::EndingSealId);
		}
		if (Id.Contains(TEXT("open")))
		{
			return FName(QinglanEnding::EndingOpenId);
		}
		return NAME_None;
	}

	void CompleteObjective(UQinglanStorySubsystem* Story, FQinglanStorySession& Session, FName SceneId, const FQinglanRegistryObjective& Objective)
	{
		const FName Key = QinglanStoryIdentity::ObjectiveKey(SceneId, Objective.ObjectiveId);
		if (Key.IsNone() || Session.CompletedObjectives.Contains(Key))
		{
			return;
		}
		Session.CompletedObjectives.Add(Key);
		Story->OnObjectiveUpdated.Broadcast(Key, TableText(Objective.TextKey));
	}

	/** True when every completion flag of the quest is set; set semantics make
	 *  this idempotent. SQ01/SQ04-SQ08 reach it through the final stage's scene
	 *  transaction, SQ02/SQ03 through the atomic effective-agreement commit. */
	bool IsQuestComplete(const FQinglanStorySession& Session, const FQinglanRegistrySideQuest& Quest)
	{
		return HasAllFlags(Session, Quest.CompletionFlags);
	}

	/**
	 * Data-driven quest-completion reward: unlocks the quest's imported
	 * rewards.technique_access ids (RewardTechniqueIds; side-quests.json
	 * rewards, blueprint section 2.5 "Mist Step (SQ04) + Orbiting Blades (SQ05)
	 * via training access") through the profile subsystem's idempotent
	 * UnlockTechnique. Unknown technique ids are rejected by the profile
	 * subsystem itself, so an unresolvable reward is a null-safe no-op.
	 */
	void GrantQuestRewardTechniques(const UQinglanStorySubsystem* Story, const FQinglanStorySession& Session, const FQinglanRegistrySideQuest& Quest)
	{
		if (Quest.RewardTechniqueIds.Num() == 0 || !IsQuestComplete(Session, Quest))
		{
			return;
		}
		const UGameInstance* GameInstance = Story->GetGameInstance();
		UQinglanProfileSubsystem* Profile = GameInstance ? GameInstance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		if (Profile == nullptr)
		{
			return;
		}
		for (const FName TechniqueId : Quest.RewardTechniqueIds)
		{
			if (TechniqueId == NAME_None)
			{
				continue;
			}
			// Transaction id style mirrors the agreement reward journaling key
			// ("sq.<questid>.reward"), suffixed with the technique id.
			Profile->UnlockTechnique(
				FName(*FString::Printf(TEXT("sq.%s.reward.%s"), *Quest.QuestId.ToString(), *TechniqueId.ToString())),
				TechniqueId);
		}
	}

	void CompleteSceneById(UQinglanStorySubsystem* Story, FQinglanStorySession& Session, FName SceneId, bool bRevisit)
	{
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		FName QuestId = NAME_None;
		const FQinglanRegistryScene* Scene = FindSceneAny(Registry, SceneId, QuestId);
		if (Scene == nullptr)
		{
			return;
		}

		const bool bSide = QuestId != NAME_None;
		TSet<FName>& CompletedSet = bSide ? Session.CompletedSideStages : Session.CompletedMainScenes;
		if (CompletedSet.Contains(SceneId))
		{
			// Idempotent: committed scenes never re-award, re-apply, or
			// re-broadcast (crash-after-commit never re-awards; spec section 3).
			if (Session.bScenePlaying && Session.CurrentSceneId == SceneId)
			{
				StopPlayback(Session);
			}
			return;
		}

		if (!bRevisit)
		{
			// Scene transaction: flags + objectives + completion commit together.
			ApplyFlags(Session, Scene->SetsFlags);
			for (const FQinglanRegistryObjective& Objective : Scene->Objectives)
			{
				CompleteObjective(Story, Session, SceneId, Objective);
			}
		}

		CompletedSet.Add(SceneId);
		if (bSide)
		{
			if (UGameInstance* GameInstance = Story->GetGameInstance())
			{
				if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
				{
					Profile->MarkSideQuestStageComplete(SceneId);
				}
			}
			// Quest-completion rewards ride the same one-time transaction: the
			// completion flags were just applied by this scene's SetsFlags (or by
			// the atomic effective commit before it called in), so a now-complete
			// quest grants its technique access exactly once and replays of any
			// stage stay no-ops (crash-after-commit never re-awards; spec section 3).
			if (const FQinglanRegistrySideQuest* Quest = Registry->FindQuest(QuestId))
			{
				GrantQuestRewardTechniques(Story, Session, *Quest);
			}
		}

		const bool bWasPlaying = Session.bScenePlaying && Session.CurrentSceneId == SceneId;
		if (bWasPlaying)
		{
			StopPlayback(Session);
		}
		QinglanStoryRuntime::SaveStoryBoundary(Story);
		Story->OnSceneCompleted.Broadcast(SceneId);
	}

	FQinglanSceneChoice BuildSceneChoice(const FQinglanRegistryChoice& RegistryChoice, const FQinglanStorySession& Session)
	{
		FQinglanSceneChoice Choice;
		Choice.ChoiceId = RegistryChoice.ChoiceId;
		Choice.Label = TableText(RegistryChoice.LabelKey);
		for (const FName ResponseKey : RegistryChoice.ResponseKeys)
		{
			Choice.Responses.Add(TableText(ResponseKey));
		}
		Choice.SetsFlags = RegistryChoice.SetsFlags;
		// A practical option changes an explicit quest action, allocation, or
		// preparation (docs/story/narrative-implementation.md section 4):
		// action-carrying or requirement-carrying choices.
		Choice.bPractical = RegistryChoice.Action != NAME_None || RegistryChoice.Prerequisites.Num() > 0;
		for (const FName Prerequisite : RegistryChoice.Prerequisites)
		{
			if (!Session.Flags.Contains(Prerequisite))
			{
				Choice.UnmetRequirements.Add(Prerequisite);
			}
		}
		return Choice;
	}

	bool AllChoiceGroupsResolved(const FQinglanRegistryScene& Scene, const FQinglanStorySession& Session)
	{
		for (const FQinglanRegistryChoice& Choice : Scene.Choices)
		{
			if (!Session.ResolvedChoiceGroups.Contains(Choice.PresentationGroup)) { return false; }
		}
		return true;
	}

	/** Present exactly one authored decision family at its pin. NAME_None selects
	 * the next unresolved family for end-of-script or explicit text skip.
	 * A missing/filtered pin cannot bypass an unresolved mandatory decision.
	 */
	bool PresentChoices(UQinglanStorySubsystem* Story, FQinglanStorySession& Session,
		const FQinglanRegistryScene& Scene, FName AfterTextKey = NAME_None)
	{
		const FQinglanRegistryChoice* First = nullptr;
		for (const FQinglanRegistryChoice& Choice : Scene.Choices)
		{
			if (!Session.ResolvedChoiceGroups.Contains(Choice.PresentationGroup)
				&& (AfterTextKey.IsNone() || Choice.ShowAfterTextKey == AfterTextKey))
			{
				First = &Choice;
				break;
			}
		}
		if (!First) { return false; }
		Session.ActiveChoiceGroup = First->PresentationGroup;
		Session.ActiveChoicePin = First->ShowAfterTextKey;
		Session.PresentedChoiceIds.Reset();
		TArray<FQinglanSceneChoice> Choices;
		for (const FQinglanRegistryChoice& Choice : Scene.Choices)
		{
			if (Choice.PresentationGroup == Session.ActiveChoiceGroup && Choice.ShowAfterTextKey == Session.ActiveChoicePin)
			{
				Session.PresentedChoiceIds.Add(Choice.ChoiceId);
				Choices.Add(BuildSceneChoice(Choice, Session));
			}
		}
		Session.bChoicesPresented = true;
		Session.bChoicesResolved = false;
		Story->OnChoicesPresented.Broadcast(Choices);
		return true;
	}

	void FinishSceneIfScriptDone(UQinglanStorySubsystem* Story, FQinglanStorySession& Session, const FQinglanRegistryScene& Scene)
	{
		if (!Session.bScenePlaying || Session.CurrentSceneId != Scene.SceneId)
		{
			return;
		}
		if (Session.PendingResponseKeys.Num() > 0 || Session.NextScriptLine < Scene.Script.Num())
		{
			return; // lines still to deliver
		}
		if (Scene.Choices.Num() > 0 && !Session.bChoicesResolved)
		{
			PresentChoices(Story, Session, Scene);
			return; // unresolved later decision families must still be selected
		}
		if (Scene.Choices.Num() > 0)
		{
			// Choice-driven transaction: the authored selection is the decision
			// proof (e.g. CH06_S01 objective "trial entry selected"), so the
			// commit auto-completes the scene's objectives.
			if (!ActionCommitsScene(Session.LastSelectedAction))
			{
				// Non-committing actions (preview / stay / cancel / return / a
				// failed-or-pending atomic op) leave the scene replayable with
				// story state preserved (spec section 4 "Return exits to the
				// pre-trial safe hub ... not a forced repeat fight").
				StopPlayback(Session);
				return;
			}
			CompleteSceneById(Story, Session, Scene.SceneId, false);
			return;
		}
		// No-choice scene: dialogue progress alone never completes it; the
		// objectives are earned through gameplay (docs/story/narrative-
		// implementation.md section 2: "Main chapter completion implies all
		// mandatory gameplay and scene requirements, not merely that a player
		// opened a dialogue window"). NotifyObjectiveCompleted commits it.
		StopPlayback(Session);
	}

	void DeliverNextLine(UQinglanStorySubsystem* Story, FQinglanStorySession& Session)
	{
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		const FQinglanRegistryScene* Scene = CurrentScene(Registry, Session);
		if (Scene == nullptr)
		{
			StopPlayback(Session);
			return;
		}

		// 1) Selected-choice responses play before the script continues.
		if (Session.PendingResponseKeys.Num() > 0)
		{
			const FName ResponseKey = Session.PendingResponseKeys[0];
			Session.PendingResponseKeys.RemoveAt(0, 1, EAllowShrinking::No);
			FQinglanRegistryLine ResponseLine;
			ResponseLine.TextKey = ResponseKey;
			Story->OnSceneLine.Broadcast(MakeLine(ResponseLine, Session.CurrentSceneId, Scene->Script.Num() + 1));
			return;
		}

		// 2) Ordered script lines with WhenAll/WhenNone/trigger filtering.
		for (int32 Index = Session.NextScriptLine; Index < Scene->Script.Num(); ++Index)
		{
			const FQinglanRegistryLine& RegistryLine = Scene->Script[Index];
			if (!LineConditionsMet(RegistryLine, Session))
			{
				continue; // conditional filter; re-evaluated per delivery so flags
				          // set by an in-scene choice unlock later lines
			}
			Session.NextScriptLine = Index + 1;
			Story->OnSceneLine.Broadcast(MakeLine(RegistryLine, Session.CurrentSceneId, Index));
			if (!Session.bChoicesResolved && !RegistryLine.TextKey.IsNone())
			{
				PresentChoices(Story, Session, *Scene, RegistryLine.TextKey);
			}
			return;
		}

		// 3) Script exhausted: present unpinned choices, else try to commit.
		Session.NextScriptLine = Scene->Script.Num();
		if (!Session.bChoicesResolved && Scene->Choices.Num() > 0)
		{
			PresentChoices(Story, Session, *Scene);
			return;
		}
		FinishSceneIfScriptDone(Story, Session, *Scene);
	}

	void StartScenePlayback(UQinglanStorySubsystem* Story, FQinglanStorySession& Session, const FQinglanRegistryScene& Scene, FName QuestId, bool bRevisit)
	{
		StopPlayback(Session);
		Session.CurrentSceneId = Scene.SceneId;
		Session.CurrentSceneQuestId = QuestId;
		Session.bScenePlaying = true;
		Session.bRevisitBranch = bRevisit;
		Session.bChoicesResolved = Scene.Choices.Num() == 0;

		Story->OnSceneEntered.Broadcast(Scene.SceneId);

		if (bRevisit)
		{
			// Brief revisit branch: one passing line, then complete; no
			// choices, no re-applied flags, no rewards
			// (docs/story/narrative-implementation.md section 3.3).
			for (const FQinglanRegistryLine& RegistryLine : Scene.Script)
			{
				if (LineConditionsMet(RegistryLine, Session))
				{
					Story->OnSceneLine.Broadcast(MakeLine(RegistryLine, Scene.SceneId, 0));
					break;
				}
			}
			CompleteSceneById(Story, Session, Scene.SceneId, true);
			return;
		}
		DeliverNextLine(Story, Session);
	}

	/** Finds a stage of Quest containing a choice with the given action type. */
	const FQinglanRegistryScene* FindStageWithAction(const FQinglanRegistrySideQuest& Quest, const TCHAR* ActionName)
	{
		for (const FQinglanRegistryScene& Stage : Quest.Stages)
		{
			for (const FQinglanRegistryChoice& Choice : Stage.Choices)
			{
				if (IsAction(Choice.Action, ActionName))
				{
					return &Stage;
				}
			}
		}
		return nullptr;
	}

	/** Courier round-trip hours per quest (data/story/side-quests.json
	 *  delivery_transition.elapsed_game_hours; blueprint section 1.9). */
	double DeliveryHoursForQuest(FName QuestId)
	{
		if (QuestId == FName(TEXT("SQ02")))
		{
			return QinglanStoryRuntime::SQ02DeliveryGameHours; // 18 h
		}
		return QinglanStoryRuntime::SQ03DeliveryGameHours; // 12 h (SQ03 and any later proposal quest)
	}

	void BindActiveEncounterDirectors(UQinglanStorySubsystem* Story, FQinglanStorySession& Session)
	{
		if (!Session.Relay.IsValid())
		{
			Session.Relay = TStrongObjectPtr<UQinglanStorySignalRelay>(NewObject<UQinglanStorySignalRelay>(Story));
			Session.Relay->StorySubsystem = Story;
		}
		UGameInstance* GameInstance = Story->GetGameInstance();
		if (GEngine == nullptr)
		{
			return;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld())
			{
				continue;
			}
			if (GameInstance != nullptr && Context.OwningGameInstance != GameInstance)
			{
				continue;
			}
			for (TActorIterator<AQinglanEncounterDirector> It(World); It; ++It)
			{
				// Only active encounters defer playback; binding their
				// completion lets the deferred scene resume without polling.
				// AddUniqueDynamic binds the relay's UFUNCTION by name.
				if (It->IsEncounterActive())
				{
					It->OnEncounterCompleted.AddUniqueDynamic(Session.Relay.Get(), &UQinglanStorySignalRelay::HandleEncounterCompleted);
				}
			}
		}
	}

	void DispatchChoiceAction(UQinglanStorySubsystem* Story, FQinglanStorySession& Session, const FQinglanRegistryScene& Scene, const FQinglanRegistryChoice& Choice)
	{
		const FName Action = Choice.Action;
		if (Action == NAME_None)
		{
			return;
		}
		FQinglanStoryDelegates& Delegates = Session.Delegates;
		const FName Key = QinglanStoryRuntime::MakeActionIdempotencyKey(Scene.SceneId, Action);

		if (IsAction(Action, TEXT("enter_trial")))
		{
			Delegates.OnEnterTrial.Broadcast(Key, Scene.SceneId);
		}
		else if (IsAction(Action, TEXT("stay_at_hub")))
		{
			Delegates.OnStayAtHub.Broadcast(Key, Scene.SceneId);
		}
		else if (IsAction(Action, TEXT("release_guardian")))
		{
			// payload sets_only_after_dissolution: mq_shen_released is a scene
			// flag applied at scene completion (post-farewell lines), i.e. only
			// after the release sequence; blueprint section 1.9 "mq_shen_released
			// saved only after pattern dissolution".
			Delegates.OnReleaseGuardian.Broadcast(Key, Scene.SceneId, FName(QinglanEnding::ShenReleasedFlag));
		}
		else if (IsAction(Action, TEXT("preview_ending")))
		{
			const FName EndingId = EndingIdFromChoiceId(Choice.ChoiceId);
			if (EndingId != NAME_None)
			{
				// Re-validates availability; creates no ending flag.
				Story->PreviewEnding(EndingId);
			}
		}
		else if (IsAction(Action, TEXT("confirm_ending")))
		{
			// Selection only requests confirmation; the UI shows the warning
			// (requires_warning_acknowledgment) and then calls ConfirmEnding,
			// which performs the atomic commit (spec section 4).
			const FName EndingId = EndingIdFromChoiceId(Choice.ChoiceId);
			if (EndingId != NAME_None)
			{
				Session.PendingConfirmSceneId = Scene.SceneId;
				Session.PendingConfirmFlags = Choice.SetsFlags;
				Delegates.OnConfirmEndingRequested.Broadcast(EndingId, Scene.SceneId);
			}
		}
		else if (IsAction(Action, TEXT("cancel_confirmation")))
		{
			Story->CancelEndingPreview();
			Delegates.OnCancelConfirmation.Broadcast(Scene.SceneId);
		}
		else if (IsAction(Action, TEXT("return_to_hub")))
		{
			Story->CancelEndingPreview();
			Delegates.OnReturnToHub.Broadcast(Key, Scene.SceneId);
		}
		else if (IsAction(Action, TEXT("complete_campaign")))
		{
			Delegates.OnCompleteCampaign.Broadcast(Key, Scene.SceneId);
		}
		else if (IsAction(Action, TEXT("select_proposal")))
		{
			if (Session.CurrentSceneQuestId != NAME_None)
			{
				Story->SelectProposal(Session.CurrentSceneQuestId, Choice.ChoiceId);
			}
		}
		else if (IsAction(Action, TEXT("accept_verified_proposal")))
		{
			if (Session.CurrentSceneQuestId != NAME_None)
			{
				// Atomic pending->effective replace; on failure (reply not yet
				// delivered / version mismatch) the stage stays incomplete and
				// the authored retry flow applies.
				Story->DeliverAgreement(Session.CurrentSceneQuestId);
			}
		}
		else
		{
			UE_LOG(LogQinglanNarrative, Warning, TEXT("Unrecognized choice action %s in scene %s"), *Action.ToString(), *Scene.SceneId.ToString());
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

const UQinglanStoryRegistry* UQinglanStorySubsystem::GetRegistry() const
{
	return LoadStoryRegistryOnce();
}

TArray<FName> UQinglanStorySubsystem::GetPlayableMainScenes() const
{
	// Scenes whose prerequisites are satisfied and are not yet completed
	// (header contract); prerequisites are pure narrative flags, and NextSceneIds
	// availability is derived from them after a completion (spec section 3.7).
	TArray<FName> Result;
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanStorySession* Session = FindStorySession(this);
	if (Registry == nullptr || Session == nullptr)
	{
		return Result;
	}
	for (const FQinglanRegistryScene& Scene : Registry->MainScenes)
	{
		if (Session->CompletedMainScenes.Contains(Scene.SceneId))
		{
			continue;
		}
		if (::HasAllFlags(*Session, Scene.Prerequisites)) // :: bypasses UObject::HasAllFlags name hiding
		{
			Result.Add(Scene.SceneId);
		}
	}
	return Result;
}

TArray<FName> UQinglanStorySubsystem::GetAvailableSideQuests() const
{
	TArray<FName> Result;
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanStorySession* Session = FindStorySession(this);
	if (Registry == nullptr || Session == nullptr)
	{
		return Result;
	}
	for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
	{
		if (!::HasAllFlags(*Session, Quest.Availability)) // :: bypasses UObject::HasAllFlags name hiding
		{
			continue;
		}
		// A quest with every completion flag still set is finished; its
		// epilogues are read through the main-story epilogue scenes instead.
		bool bAnyCompletionFlagMissing = false;
		for (const FName Flag : Quest.CompletionFlags)
		{
			if (!Session->Flags.Contains(Flag))
			{
				bAnyCompletionFlagMissing = true;
				break;
			}
		}
		if (bAnyCompletionFlagMissing)
		{
			Result.Add(Quest.QuestId);
		}
	}
	return Result;
}

TArray<FName> UQinglanStorySubsystem::GetQuestEpilogueKeys(FName QuestId) const
{
	// Authored order [open, seal, shared] (side-quests.json epilogue_keys);
	// CH07_S04's closure cards index the entry for the confirmed ending.
	TArray<FName> Result;
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanRegistrySideQuest* Quest = Registry ? Registry->FindQuest(QuestId) : nullptr;
	if (Quest != nullptr)
	{
		Result = Quest->EpilogueKeys;
	}
	return Result;
}

FName UQinglanStorySubsystem::GetQuestEpilogueKey(FName QuestId, EQinglanEnding Ending) const
{
	// epilogue_keys are authored [open, seal, shared]; EQinglanEnding declares
	// the same order after None (Core/QinglanCoreTypes.h), so the enum indexes
	// the authored slots directly.
	if (Ending == EQinglanEnding::None)
	{
		return NAME_None;
	}
	const TArray<FName> Keys = GetQuestEpilogueKeys(QuestId);
	const int32 Index = static_cast<int32>(Ending) - 1;
	return Keys.IsValidIndex(Index) ? Keys[Index] : NAME_None;
}

FName UQinglanStorySubsystem::GetStageJournalKey(FName SceneId) const
{
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	FName QuestId = NAME_None;
	const FQinglanRegistryScene* Scene = FindSceneAny(Registry, SceneId, QuestId);
	return Scene != nullptr ? Scene->JournalKey : NAME_None;
}

// ---------------------------------------------------------------------------
// Scene playback
// ---------------------------------------------------------------------------

bool UQinglanStorySubsystem::BeginScene(FName SceneId)
{
	FQinglanStorySession& Session = StorySession(this);
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	if (Registry == nullptr)
	{
		return false;
	}
	FName QuestId = NAME_None;
	const FQinglanRegistryScene* Scene = FindSceneAny(Registry, SceneId, QuestId);
	if (Scene == nullptr)
	{
		UE_LOG(LogQinglanNarrative, Warning, TEXT("BeginScene: unknown scene %s"), *SceneId.ToString());
		return false;
	}
	if (Session.bScenePlaying)
	{
		// Idempotent re-entry; any other scene must wait for the safe boundary.
		return Session.CurrentSceneId == SceneId;
	}
	// Prerequisite gate: fail (do not defer) when the chain is unmet.
	if (!::HasAllFlags(Session, Scene->Prerequisites)) // :: bypasses UObject::HasAllFlags name hiding
	{
		return false;
	}
	// Already-completed scenes resolve to their brief revisit branch
	// (docs/story/narrative-implementation.md section 3.3).
	const bool bRevisit = Session.CompletedMainScenes.Contains(SceneId) || Session.CompletedSideStages.Contains(SceneId);

	if (IsAnyEncounterActive(GetGameInstance()))
	{
		// Defer playback until no encounter director reports active
		// (spec section 3.2: never freeze the player while enemies remain).
		Session.DeferredSceneId = SceneId;
		BindActiveEncounterDirectors(this, Session);
		return true;
	}

	StartScenePlayback(this, Session, *Scene, QuestId, bRevisit);
	return true;
}

void UQinglanStorySubsystem::AdvanceLine()
{
	FQinglanStorySession& Session = StorySession(this);
	if (!Session.bScenePlaying)
	{
		return;
	}
	// Input freeze while a choice is pending (blueprint section 1.9) and
	// skip-halt keep the exchange frozen until a selection happens.
	if (Session.bChoicesPresented || Session.bAdvanceHalted)
	{
		return;
	}
	DeliverNextLine(this, Session);
}

bool UQinglanStorySubsystem::SelectChoice(FName ChoiceId)
{
	FQinglanStorySession& Session = StorySession(this);
	if (!Session.bScenePlaying || !Session.bChoicesPresented || Session.bRevisitBranch)
	{
		return false;
	}
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanRegistryScene* Scene = CurrentScene(Registry, Session);
	if (Scene == nullptr)
	{
		return false;
	}
	const FQinglanRegistryChoice* Choice = nullptr;
	for (const FQinglanRegistryChoice& Candidate : Scene->Choices)
	{
		if (Candidate.ChoiceId == ChoiceId && Session.PresentedChoiceIds.Contains(ChoiceId))
		{
			Choice = &Candidate;
			break;
		}
	}
	if (Choice == nullptr)
	{
		return false;
	}
	// Practical choices with unmet requirements were presented disabled
	// (UnmetRequirements); selecting one changes nothing.
	for (const FName Prerequisite : Choice->Prerequisites)
	{
		if (!Session.Flags.Contains(Prerequisite))
		{
			return false;
		}
	}

	// Journal the decision; options of one decision family are mutually
	// exclusive (blueprint 1.9 "choices mutually exclusive attitude flags"),
	// so a sibling selection replaces the earlier recorded decision.
	Session.ChoiceDecisions.Add(QinglanStoryIdentity::ChoiceKey(Scene->SceneId, Choice->ChoiceId), Choice->ChoiceId);
	for (const FQinglanRegistryChoice& Sibling : Scene->Choices)
	{
		if (Sibling.ChoiceId != Choice->ChoiceId && Sibling.PresentationGroup == Choice->PresentationGroup)
		{
			Session.ChoiceDecisions.Remove(QinglanStoryIdentity::ChoiceKey(Scene->SceneId, Sibling.ChoiceId));
		}
	}

	// Choice flags apply ONCE at the scene's safe boundary: the frozen choice
	// moment is the authored safe boundary for branch flags; the scene-level
	// transaction commits at completion. Atomic-op actions (confirm_ending,
	// accept_verified_proposal) defer their flags to that operation so no
	// intermediate click can create an ending or agreement flag.
	if (!ChoiceFlagsDeferredToAtomicOp(Choice->Action))
	{
		ApplyFlags(Session, Choice->SetsFlags);
	}

	Session.bChoicesPresented = false;
	Session.PresentedChoiceIds.Reset();
	Session.ResolvedChoiceGroups.Add(Choice->PresentationGroup);
	// Ending controls share one repeatable decision family. Preview/cancel/
	// return and pending confirmation keep that menu available until commit.
	if (IsAction(Choice->Action, TEXT("preview_ending")) || IsAction(Choice->Action, TEXT("confirm_ending"))
		|| IsAction(Choice->Action, TEXT("cancel_confirmation")) || IsAction(Choice->Action, TEXT("return_to_hub")))
	{
		Session.ResolvedChoiceGroups.Remove(Choice->PresentationGroup);
	}
	Session.bChoicesResolved = AllChoiceGroupsResolved(*Scene, Session);
	Session.bAdvanceHalted = false; // AUD-12: skip only halts until the presented choice is resolved.
	Session.PendingResponseKeys = Choice->ResponseKeys;
	Session.LastSelectedAction = Choice->Action;

	DispatchChoiceAction(this, Session, *Scene, *Choice);

	if (Session.PendingResponseKeys.Num() == 0 && Session.NextScriptLine >= Scene->Script.Num())
	{
		// Nothing further to display: resolve the scene boundary now instead
		// of waiting for an AdvanceLine that has nothing left to deliver.
		FinishSceneIfScriptDone(this, Session, *Scene);
	}
	QinglanStoryRuntime::SaveStoryBoundary(this);
	return true;
}

bool UQinglanStorySubsystem::IsScenePlaying() const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session != nullptr && Session->bScenePlaying;
}

FName UQinglanStorySubsystem::GetCurrentSceneId() const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session ? Session->CurrentSceneId : NAME_None;
}

void UQinglanStorySubsystem::SkipScene()
{
	// Skipping text applies mandatory journal/objective updates but never
	// rewards, never an unearned clear, and never silently selects a
	// consequential choice (docs/story/narrative-implementation.md section 2,
	// blueprint section 1.9 "skip applies mandatory journal facts then pauses
	// at unselected choice").
	FQinglanStorySession& Session = StorySession(this);
	if (!Session.bScenePlaying)
	{
		return;
	}
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanRegistryScene* Scene = CurrentScene(Registry, Session);
	if (Scene == nullptr)
	{
		StopPlayback(Session);
		return;
	}

	// Journal refresh for objectives not yet earned.
	for (const FQinglanRegistryObjective& Objective : Scene->Objectives)
	{
		if (!Session.CompletedObjectives.Contains(QinglanStoryIdentity::ObjectiveKey(Scene->SceneId, Objective.ObjectiveId)))
		{
			OnObjectiveUpdated.Broadcast(QinglanStoryIdentity::ObjectiveKey(Scene->SceneId, Objective.ObjectiveId), TableText(Objective.TextKey));
		}
	}

	if (Scene->Choices.Num() > 0 && !Session.bChoicesResolved)
	{
		// Pause at the unselected choice (no reward path, no silent decision).
		Session.bAdvanceHalted = true;
		if (!Session.bChoicesPresented)
		{
			PresentChoices(this, Session, *Scene);
		}
		return;
	}

	Session.PendingResponseKeys.Reset();
	Session.NextScriptLine = Scene->Script.Num();
	if (Scene->Choices.Num() > 0)
	{
		// Choice-driven scene with the selection already made: the commit is
		// the same transaction as normal completion (journal/objective updates
		// + flags); no action delegates fire from a skip, so no rewards are
		// dispatched. Non-committing selections leave the scene replayable.
		if (ActionCommitsScene(Session.LastSelectedAction))
		{
			CompleteSceneById(this, Session, Scene->SceneId, false);
		}
		else
		{
			StopPlayback(Session);
		}
	}
	else if (AllObjectivesComplete(Session, *Scene))
	{
		// No-choice gameplay scene whose objectives were already earned:
		// skipping applies the journal/objective updates and commits the
		// already-earned flags without re-awarding anything.
		CompleteSceneById(this, Session, Scene->SceneId, false);
	}
	else
	{
		StopPlayback(Session);
	}
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool UQinglanStorySubsystem::IsSceneCompleted(FName SceneId) const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session
		&& (Session->CompletedMainScenes.Contains(SceneId) || Session->CompletedSideStages.Contains(SceneId));
}

bool UQinglanStorySubsystem::HasFlag(FName Flag) const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session && Session->Flags.Contains(Flag);
}

FName UQinglanStorySubsystem::GetChoiceDecision(FName ChoiceId) const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	if (!Session || !Registry) { return NAME_None; }
	const FName Key = QinglanStoryIdentity::ResolveChoiceKey(*Registry, ChoiceId);
	const FName* Decision = Key.IsNone() ? nullptr : Session->ChoiceDecisions.Find(Key);
	return Decision ? *Decision : NAME_None;
}

FName UQinglanStorySubsystem::GetSceneChoiceDecision(FName SceneId, FName ChoiceId) const
{
	if (SceneId.IsNone()) { return NAME_None; }
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	if (!Registry) { return NAME_None; }
	const FName Key = QinglanStoryIdentity::ResolveChoiceKey(*Registry, ChoiceId, SceneId);
	return Key.IsNone() ? NAME_None : GetChoiceDecision(Key);
}

bool UQinglanStorySubsystem::IsEndingConfirmed() const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session && Session->ConfirmedEnding != EQinglanEnding::None;
}

EQinglanEnding UQinglanStorySubsystem::GetConfirmedEnding() const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	return Session ? Session->ConfirmedEnding : EQinglanEnding::None;
}

// ---------------------------------------------------------------------------
// Endings
// ---------------------------------------------------------------------------

bool UQinglanStorySubsystem::IsEndingAvailable(FName EndingId, FText& OutRequirementText) const
{
	const EQinglanEnding Ending = QinglanEnding::EndingIdToEnum(EndingId);
	if (Ending == EQinglanEnding::None)
	{
		OutRequirementText = TableText(FName(TEXT("story.ch06.s04.objective")));
		return false;
	}
	const FQinglanStorySession* Session = FindStorySession(this);
	// Owned closure (auto), not TFunctionRef: binding TFunctionRef to the
	// lambda temporary would dangle at the end of this statement.
	auto HasFlag = [&Session](FName Flag)
	{
		return Session != nullptr && Session->Flags.Contains(Flag);
	};
	// Preview/commit read effective agreements only: each agreement flag is
	// set solely by its quest's atomic effective commit (SQ02/SQ03
	// proposal_state.ending_reads_effective_not_pending).
	TArray<FName> Missing;
	const bool bAvailable = QinglanEnding::EvaluateAvailability(Ending, HasFlag, Missing);
	OutRequirementText = QinglanEnding::RequirementText(Ending, Missing);
	return bAvailable;
}

bool UQinglanStorySubsystem::PreviewEnding(FName EndingId)
{
	// Preview creates no ending flag (spec section 4); it only records the
	// session preview state ConfirmEnding later requires.
	FQinglanStorySession& Session = StorySession(this);
	const EQinglanEnding Ending = QinglanEnding::EndingIdToEnum(EndingId);
	const FName CanonicalId = QinglanEnding::EndingEnumToId(Ending);
	if (Ending == EQinglanEnding::None)
	{
		return false;
	}
	FText RequirementText;
	if (!IsEndingAvailable(EndingId, RequirementText))
	{
		return false;
	}
	Session.PreviewedEndings.Add(QinglanEnding::EndingEnumToId(Ending));
	Session.Delegates.OnEndingPreviewed.Broadcast(CanonicalId, Session.CurrentSceneId);
	return true;
}

void UQinglanStorySubsystem::CancelEndingPreview()
{
	FQinglanStorySession& Session = StorySession(this);
	Session.PreviewedEndings.Reset();
	Session.PendingConfirmSceneId = NAME_None;
	Session.PendingConfirmFlags.Reset();
}

bool UQinglanStorySubsystem::ConfirmEnding(FName EndingId, bool bWarningAcknowledged, FText& OutError)
{
	// One atomic, one-time, mutually exclusive commit per profile
	// (spec sections 4-5; data CH06_S04 confirm_ending payload:
	// requires_preview + requires_warning_acknowledgment +
	// mutually_exclusive_flags + commit_once).
	OutError = FText::GetEmpty();
	FQinglanStorySession& Session = StorySession(this);
	const EQinglanEnding Ending = QinglanEnding::EndingIdToEnum(EndingId);
	const FName CanonicalId = QinglanEnding::EndingEnumToId(Ending);
	if (Ending == EQinglanEnding::None)
	{
		OutError = TableText(FName(TEXT("story.ch06.s04.objective")));
		return false;
	}
	if (Session.ConfirmedEnding != EQinglanEnding::None)
	{
		// Already committed for this profile: changing it requires a new
		// campaign, and no Continue button may rerun confirmation
		// (spec section 5).
		OutError = TableText(FName(TEXT("story.ch06.s04.journal")));
		return false;
	}
	if (!Session.PreviewedEndings.Contains(CanonicalId))
	{
		// Confirm requires a prior preview of this ending (payload
		// requires_preview, e.g. s04.preview_open before s04.confirm_open).
		OutError = TableText(FName(TEXT("story.ch06.s04.objective")));
		return false;
	}
	TArray<FName> Missing;
	// Owned closure (auto), not TFunctionRef: see EvaluateAvailability call above.
	auto HasFlag = [&Session](FName Flag) { return Session.Flags.Contains(Flag); };
	if (!QinglanEnding::EvaluateAvailability(Ending, HasFlag, Missing))
	{
		OutError = QinglanEnding::RequirementText(Ending, Missing);
		return false;
	}
	if (!bWarningAcknowledged)
	{
		// The warning explains irreversibility and New Game (spec section 5).
		OutError = TableText(FName(TEXT("story.ch06.s04.confirm_warning")));
		return false;
	}

	// ---- Atomic commit: ending record + exactly one ending flag + the
	// authored confirm payload flags (mq_ch06_complete) + scene completion.
	Session.ConfirmedEnding = Ending;
	ApplyFlags(Session, { CanonicalId });
	ApplyFlags(Session, Session.PendingConfirmFlags);
	ApplyFlags(Session, { FName(TEXT("mq_ch06_complete")) }); // mirrors CH06_S04 data for direct (non-choice) commits
	const FName ConfirmSceneId = Session.PendingConfirmSceneId;
	Session.PendingConfirmSceneId = NAME_None;
	Session.PendingConfirmFlags.Reset();
	if (ConfirmSceneId != NAME_None)
	{
		CompleteSceneById(this, Session, ConfirmSceneId, false);
	}
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			// Zero-value journal commit: marks the one-time ending transaction
			// id and dirties the profile so a listener's autosave persists it.
			Profile->TryGrant(FName(TEXT("main.ending_commit")), FQinglanInventory());
		}
	}
	QinglanStoryRuntime::SaveStoryBoundary(this);
	Session.Delegates.OnEndingConfirmed.Broadcast(CanonicalId, QinglanEnding::EndingCommitFlag(Ending));
	return true;
}

// ---------------------------------------------------------------------------
// Agreements (SQ02/SQ03 pending vs effective versioning)
// ---------------------------------------------------------------------------

bool UQinglanStorySubsystem::SelectProposal(FName QuestId, FName ProposalVersion)
{
	// docs/story/narrative-implementation.md section 9: selecting a new
	// proposal replaces sibling proposal flags and invalidates any earlier
	// reply; the old effective agreement remains valid until a matching
	// delivered reply, verification, and explicit confirmation atomically
	// replace it.
	FQinglanStorySession& Session = StorySession(this);
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanRegistrySideQuest* Quest = Registry ? Registry->FindQuest(QuestId) : nullptr;
	if (Quest == nullptr)
	{
		return false;
	}
	const FQinglanRegistryScene* ProposalStage = FindStageWithAction(*Quest, TEXT("select_proposal"));
	if (ProposalStage == nullptr)
	{
		return false;
	}
	const FQinglanRegistryChoice* Chosen = nullptr;
	for (const FQinglanRegistryChoice& Candidate : ProposalStage->Choices)
	{
		if (Candidate.ChoiceId == ProposalVersion)
		{
			Chosen = &Candidate;
			break;
		}
	}
	if (Chosen == nullptr)
	{
		return false;
	}

	// Replace sibling proposal flags (payload replace_choice_flags): clear
	// every sibling option's flags, then set the selected option's.
	for (const FQinglanRegistryChoice& Sibling : ProposalStage->Choices)
	{
		if (Sibling.ChoiceId == Chosen->ChoiceId)
		{
			continue;
		}
		for (const FName Flag : Sibling.SetsFlags)
		{
			Session.Flags.Remove(Flag);
		}
	}
	ApplyFlags(Session, Chosen->SetsFlags);

	// Pending version slot (payload pending_slot); commits nothing effective.
	Session.PendingAgreements.Add(QuestId, ProposalVersion);

	// Invalidate the earlier delivered reply and restart the courier clock
	// ("Pending dispatch can be canceled and replaced; restart the timestamp
	// and retain old accepted agreement until a new agreement is confirmed",
	// side-quests.json SQ02 implementation.closure).
	FQinglanStoryDelivery& Delivery = Session.Deliveries.FindOrAdd(QuestId);
	Delivery.Version = ProposalVersion;
	Delivery.ReadyAtStorySeconds = Session.StoryClockSeconds + DeliveryHoursForQuest(QuestId) * QinglanStoryRuntime::SecondsPerGameHour;
	Delivery.bDelivered = false;

	QinglanStoryRuntime::SaveStoryBoundary(this);
	Session.Delegates.OnProposalSelected.Broadcast(QuestId, ProposalVersion);
	return true;
}

FName UQinglanStorySubsystem::GetEffectiveAgreement(FName QuestId) const
{
	const FQinglanStorySession* Session = FindStorySession(this);
	if (!Session)
	{
		return NAME_None;
	}
	const FName* Effective = Session->EffectiveAgreements.Find(QuestId);
	return Effective ? *Effective : NAME_None;
}

bool UQinglanStorySubsystem::DeliverAgreement(FName QuestId)
{
	// The atomic effective replace: requires a matching delivered reply, then
	// copies pending -> effective, sets the quest completion flags, completes
	// the accept stage, and journals the one-time reward in one transaction
	// (side-quests.json accept_verified_proposal payload; spec section 9).
	FQinglanStorySession& Session = StorySession(this);
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FQinglanRegistrySideQuest* Quest = Registry ? Registry->FindQuest(QuestId) : nullptr;
	if (Quest == nullptr)
	{
		return false;
	}
	const FName* PendingVersion = Session.PendingAgreements.Find(QuestId);
	if (PendingVersion == nullptr)
	{
		return false; // nothing selected
	}
	const FQinglanStoryDelivery* Delivery = Session.Deliveries.Find(QuestId);
	if (Delivery == nullptr || !Delivery->bDelivered || Delivery->Version != *PendingVersion)
	{
		return false; // reply not delivered (or invalidated by a newer selection)
	}
	const FQinglanRegistryScene* AcceptStage = FindStageWithAction(*Quest, TEXT("accept_verified_proposal"));
	if (AcceptStage == nullptr)
	{
		return false;
	}

	// Old effective remains until here; this block is the atomic replace.
	Session.EffectiveAgreements.Add(QuestId, *PendingVersion);
	for (const FName Flag : Quest->CompletionFlags)
	{
		ApplyFlags(Session, { Flag });
	}
	for (const FQinglanRegistryChoice& Choice : AcceptStage->Choices)
	{
		if (IsAction(Choice.Action, TEXT("accept_verified_proposal")))
		{
			ApplyFlags(Session, Choice.SetsFlags); // e.g. sq_allocation_agreed
		}
	}

	// Stage completion (applies stage sets_flags, marks complete, journals
	// through the profile) and the one-time reward id
	// (payload rewards_once; amounts are authored per-quest and granted by
	// the quest handler, so this only journals the idempotency key).
	CompleteSceneById(this, Session, AcceptStage->SceneId, false);
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			Profile->TryGrant(FName(*FString::Printf(TEXT("sq.%s.reward"), *QuestId.ToString())), FQinglanInventory());
		}
	}

	QinglanStoryRuntime::SaveStoryBoundary(this);
	Session.Delegates.OnAgreementEffective.Broadcast(QuestId, *PendingVersion);
	return true;
}

// ---------------------------------------------------------------------------
// Objective / journal bridge
// ---------------------------------------------------------------------------

void UQinglanStorySubsystem::NotifyObjectiveCompleted(FName ObjectiveId)
{
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FName Key = Registry ? QinglanStoryIdentity::ResolveObjectiveKey(*Registry, ObjectiveId) : NAME_None;
	if (Key.IsNone())
	{
		UE_LOG(LogQinglanNarrative, Warning, TEXT("Rejected unknown/ambiguous objective '%s'; provide owning scene"), *ObjectiveId.ToString());
		return;
	}
	FString Owner, Remainder;
	Key.ToString().Split(TEXT("::"), &Owner, &Remainder);
	NotifySceneObjectiveCompleted(FName(*Owner), Key);
}

bool UQinglanStorySubsystem::NotifySceneObjectiveCompleted(FName SceneId, FName ObjectiveId)
{
	if (SceneId.IsNone()) { return false; }
	const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
	const FName Key = Registry ? QinglanStoryIdentity::ResolveObjectiveKey(*Registry, ObjectiveId, SceneId) : NAME_None;
	FName QuestId = NAME_None;
	const FQinglanRegistryScene* Owner = FindSceneAny(Registry, SceneId, QuestId);
	if (Key.IsNone() || Owner == nullptr)
	{
		UE_LOG(LogQinglanNarrative, Warning, TEXT("Rejected objective '%s' for scene '%s'"), *ObjectiveId.ToString(), *SceneId.ToString());
		return false;
	}
	FQinglanStorySession& Session = StorySession(this);
	for (const FQinglanRegistryObjective& Objective : Owner->Objectives)
	{
		if (QinglanStoryIdentity::ObjectiveKey(SceneId, Objective.ObjectiveId) == Key)
		{
			CompleteObjective(this, Session, SceneId, Objective);
			break;
		}
	}
	const bool bCompleted = Session.CompletedMainScenes.Contains(SceneId) || Session.CompletedSideStages.Contains(SceneId);
	if (bCompleted || !AllObjectivesComplete(Session, *Owner)) { return true; }
	if (Owner->Choices.Num() == 0)
	{
		CompleteSceneById(this, Session, SceneId, false);
	}
	else if (Session.bScenePlaying && Session.CurrentSceneId == SceneId)
	{
		FinishSceneIfScriptDone(this, Session, *Owner);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Relay (dynamic delegates bind UFUNCTIONs only; the frozen header has none)
// ---------------------------------------------------------------------------

void UQinglanStorySignalRelay::HandleEncounterCompleted()
{
	UQinglanStorySubsystem* Story = StorySubsystem.Get();
	if (Story == nullptr)
	{
		return;
	}
	// A deferred scene begins once no encounter director is active anymore.
	FQinglanStorySession& Session = StorySession(Story);
	if (Session.DeferredSceneId == NAME_None)
	{
		return;
	}
	if (IsAnyEncounterActive(Story->GetGameInstance()))
	{
		return; // another encounter still holds the deferral
	}
	const FName DeferredSceneId = Session.DeferredSceneId;
	Session.DeferredSceneId = NAME_None;
	// BeginScene re-validates prerequisites at the safe boundary.
	Story->BeginScene(DeferredSceneId);
}

// ---------------------------------------------------------------------------
// QinglanStoryRuntime free functions (see QinglanStoryRuntime.h)
// ---------------------------------------------------------------------------

namespace QinglanStoryRuntime
{
#if WITH_DEV_AUTOMATION_TESTS
	void SetRegistryForTesting(UQinglanStoryRegistry* Registry)
	{
		GTestStoryRegistry.Reset(Registry);
	}
#endif

	bool SaveStoryBoundary(UQinglanStorySubsystem* Story)
	{
		if (Story == nullptr) { return false; }
		UGameInstance* GameInstance = Story->GetGameInstance();
		UQinglanSaveSubsystem* Save = GameInstance ? GameInstance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
		if (Save == nullptr || !Save->IsProfileSessionActive()) { return true; }
		FQinglanStorySession& Session = StorySession(Story);
		if (Session.bSavingStoryBoundary) { return false; }
		TGuardValue<bool> SavingGuard(Session.bSavingStoryBoundary, true);
		FText Error;
		if (!Save->SaveActiveSession(Error))
		{
			UE_LOG(LogQinglanNarrative, Warning, TEXT("Narrative boundary save failed: %s"), *Error.ToString());
			return false;
		}
		return true;
	}
	FName MakeActionIdempotencyKey(FName SceneId, FName ActionName)
	{
		return FName(*FString::Printf(TEXT("story.%s.%s"), *SceneId.ToString(), *ActionName.ToString()));
	}

	FQinglanStoryDelegates& Delegates(UQinglanStorySubsystem* Story)
	{
		return StorySession(Story).Delegates;
	}

	void NotifyLocationEntered(UQinglanStorySubsystem* Story, FName Location)
	{
		FQinglanStorySession& Session = StorySession(Story);
		Session.CurrentLocation = Location;
		// A new visit refreshes per-visit bark budgets
		// (docs/dev/implementation-blueprint.md section 2.6 "<=1/visit").
		Session.BarkPlaysThisVisit.Reset();
	}

	bool TryPlayBark(UQinglanStorySubsystem* Story)
	{
		// Deterministic ambient scheduling: authored-order rotation, per-bark
		// world-time cooldown, per-visit budget, combat suppression. No
		// gameplay randomness (code-contracts.md); the rotation cursor is the
		// only ordering state.
		FQinglanStorySession& Session = StorySession(Story);
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		if (Registry == nullptr || Registry->Barks.Num() == 0 || Session.CurrentLocation == NAME_None)
		{
			return false;
		}
		if (IsAnyEncounterActive(Story->GetGameInstance()))
		{
			return false; // suppressed during combat
		}
		const double Now = WorldTimeSeconds(Story);
		const int32 BarkCount = Registry->Barks.Num();
		for (int32 Scan = 0; Scan < BarkCount; ++Scan)
		{
			const int32 Index = (Session.BarkRotationCursor + Scan) % BarkCount;
			const FQinglanRegistryBark& Bark = Registry->Barks[Index];
			if (Bark.Location != Session.CurrentLocation)
			{
				continue;
			}
			if (!HasAllFlags(Session, Bark.RequiredFlags))
			{
				continue;
			}
			bool bForbidden = false;
			for (const FName Flag : Bark.ForbiddenFlags)
			{
				if (Session.Flags.Contains(Flag))
				{
					bForbidden = true;
					break;
				}
			}
			if (bForbidden)
			{
				continue;
			}
			const int32 MaxPerVisit = Bark.MaxPerVisit > 0 ? Bark.MaxPerVisit : DefaultBarkMaxPerVisit;
			const int32* Plays = Session.BarkPlaysThisVisit.Find(Bark.BarkId);
			if (Plays != nullptr && *Plays >= MaxPerVisit)
			{
				continue;
			}
			const float Cooldown = Bark.CooldownSeconds > 0.f ? Bark.CooldownSeconds : DefaultBarkCooldownSeconds;
			if (const double* LastFired = Session.BarkLastWorldSeconds.Find(Bark.BarkId))
			{
				if (Now - *LastFired < static_cast<double>(Cooldown))
				{
					continue;
				}
			}

			Session.BarkPlaysThisVisit.Add(Bark.BarkId, (Plays ? *Plays : 0) + 1);
			Session.BarkLastWorldSeconds.Add(Bark.BarkId, Now);
			Session.BarkRotationCursor = (Index + 1) % BarkCount;

			FQinglanScriptLine Line;
			Line.SpeakerName = TableText(Bark.Speaker);
			Line.Text = TableText(Bark.TextKey);
			Line.SourceKind = EQinglanSourceKind::Live;
			Line.SceneId = Bark.BarkId;
			Line.LineIndex = INDEX_NONE;
			Session.Delegates.OnBarkLine.Broadcast(Bark.BarkId, Line);
			return true;
		}
		return false;
	}

	bool TryInspectRecord(UQinglanStorySubsystem* Story, FName RecordId)
	{
		// Records gate on RequiredFlags; the read itself is journaled through
		// UQinglanProfileSubsystem::MarkRecordRead and the collection flag is
		// set exactly once (set semantics).
		FQinglanStorySession& Session = StorySession(Story);
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		if (Registry == nullptr)
		{
			return false;
		}
		const FQinglanRegistryRecord* Record = nullptr;
		for (const FQinglanRegistryRecord& Candidate : Registry->Records)
		{
			if (Candidate.RecordId == RecordId)
			{
				Record = &Candidate;
				break;
			}
		}
		if (Record == nullptr || !HasAllFlags(Session, Record->RequiredFlags))
		{
			return false;
		}
		ApplyFlags(Session, { Record->CollectionFlag });
		if (UGameInstance* GameInstance = Story->GetGameInstance())
		{
			if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
			{
				Profile->MarkRecordRead(RecordId);
			}
		}
		Session.Delegates.OnRecordOpened.Broadcast(RecordId, TableText(Record->TitleKey), TableText(Record->BodyKey));
		return true;
	}

	void NotifyGameplayEvent(UQinglanStorySubsystem* Story, FName Event, const TArray<FName>& SetsFlags)
	{
		FQinglanStorySession& Session = StorySession(Story);
		if (Event != NAME_None)
		{
			// Unlocks trigger-gated script lines (e.g. CH06_S02 storm beats).
			Session.FiredEvents.Add(Event);
		}
		// Dispatcher binding: gameplay events may set mq_* proof flags
		// (blueprint section 1.9); main-quest flags are never settable from
		// side-quest registries because only this validated path sets them.
		ApplyFlags(Session, SetsFlags);
	}

	bool AdvanceHubTime(UQinglanStorySubsystem* Story, double GameHours)
	{
		// Safe hub time advance: courier deliveries arrive in fiction time
		// with zero real-time wait (blueprint section 1.9).
		if (GameHours <= 0.0)
		{
			return false;
		}
		FQinglanStorySession& Session = StorySession(Story);
		Session.StoryClockSeconds += GameHours * SecondsPerGameHour;
		bool bAnyDeliveryReady = false;
		for (TPair<FName, FQinglanStoryDelivery>& Pair : Session.Deliveries)
		{
			FQinglanStoryDelivery& Delivery = Pair.Value;
			if (!Delivery.bDelivered && Delivery.Version != NAME_None && Session.StoryClockSeconds >= Delivery.ReadyAtStorySeconds)
			{
				Delivery.bDelivered = true; // the matching reply has arrived
				bAnyDeliveryReady = true;
			}
		}
		return bAnyDeliveryReady;
	}

	double GetStoryClockSeconds(const UQinglanStorySubsystem* Story)
	{
		const FQinglanStorySession* Session = FindStorySession(Story);
		return Session ? Session->StoryClockSeconds : 0.0;
	}

	void ExportNarrativeState(const UQinglanStorySubsystem* Story, FQinglanNarrativeState& Out)
	{
		// Save bridge: fills exactly the story-owned members of the v1
		// narrative save section (data/examples/save.json); the profile
		// subsystem merges CompletedSideStages/ReadRecords on top.
		const FQinglanStorySession* Session = FindStorySession(Story);
		if (Session == nullptr)
		{
			return;
		}
		if (const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce())
		{
			Out.Revision = Registry->NarrativeRevision;
		}
		Out.CompletedScenes = Session->CompletedMainScenes.Array();
		Out.CompletedScenes.Sort(FNameLexicalLess());
		Out.CompletedSideStages = Session->CompletedSideStages.Array();
		Out.CompletedSideStages.Sort(FNameLexicalLess());
		Out.Flags = Session->Flags.Array();
		Out.Flags.Sort(FNameLexicalLess());
		Out.ChoiceDecisions = NormalizeSavedChoices(LoadStoryRegistryOnce(), Session->ChoiceDecisions);
		Out.PendingAgreements = Session->PendingAgreements;
		Out.EffectiveAgreements = Session->EffectiveAgreements;
		Out.ConfirmedEnding = Session->ConfirmedEnding;
	}

	void ImportNarrativeState(UQinglanStorySubsystem* Story, const FQinglanNarrativeState& In)
	{
		FQinglanStorySession& Session = StorySession(Story);
		// Drop in-flight playback: an interrupted conversation restarts at a
		// coherent exchange (spec section 3.8); committed flags and scenes are
		// never rolled back by replaying text.
		StopPlayback(Session);
		Session.CompletedMainScenes.Reset();
		Session.CompletedSideStages.Reset();
		for (const FName SceneId : In.CompletedScenes)
		{
			Session.CompletedMainScenes.Add(SceneId);
		}
		for (const FName StageId : In.CompletedSideStages)
		{
			Session.CompletedSideStages.Add(StageId);
		}
		Session.Flags.Reset();
		for (const FName Flag : In.Flags)
		{
			Session.Flags.Add(Flag);
		}
		Session.ChoiceDecisions = NormalizeSavedChoices(LoadStoryRegistryOnce(), In.ChoiceDecisions);
		Session.PendingAgreements = In.PendingAgreements;
		Session.EffectiveAgreements = In.EffectiveAgreements;
		Session.ConfirmedEnding = In.ConfirmedEnding;
		// Objectives are derived, not persisted: every completed scene's
		// objectives count as earned so the journal resumes coherently on
		// reload (docs/story/narrative-implementation.md section 3.7 "Resume
		// gameplay using the next scene/objective, including on reload").
		Session.CompletedObjectives.Reset();
		if (const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce())
		{
			for (const FName SceneId : Session.CompletedMainScenes)
			{
				FName QuestId = NAME_None;
				if (const FQinglanRegistryScene* Scene = FindSceneAny(Registry, SceneId, QuestId))
				{
					for (const FQinglanRegistryObjective& Objective : Scene->Objectives)
					{
						Session.CompletedObjectives.Add(QinglanStoryIdentity::ObjectiveKey(Scene->SceneId, Objective.ObjectiveId));
					}
				}
			}
			for (const FName StageId : Session.CompletedSideStages)
			{
				FName QuestId = NAME_None;
				if (const FQinglanRegistryScene* Stage = FindSceneAny(Registry, StageId, QuestId))
				{
					for (const FQinglanRegistryObjective& Objective : Stage->Objectives)
					{
						Session.CompletedObjectives.Add(QinglanStoryIdentity::ObjectiveKey(Stage->SceneId, Objective.ObjectiveId));
					}
				}
			}
		}
		// v1 persists no dispatch timestamps: restored pending selections keep
		// their version but restart the courier clock on the next selection
		// (side-quests.json SQ02 closure: "restart the timestamp and retain old
		// accepted agreement until a new agreement is confirmed").
		Session.Deliveries.Reset();
		Session.PreviewedEndings.Reset();
	}

	bool IsObjectiveCompleted(const UQinglanStorySubsystem* Story, FName ObjectiveId)
	{
		const FQinglanStorySession* Session = FindStorySession(Story);
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		if (!Session || !Registry) { return false; }
		const FName Key = QinglanStoryIdentity::ResolveObjectiveKey(*Registry, ObjectiveId);
		return !Key.IsNone() && Session->CompletedObjectives.Contains(Key);
	}

	bool IsSceneObjectiveCompleted(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId)
	{
		if (SceneId.IsNone()) { return false; }
		const UQinglanStoryRegistry* Registry = LoadStoryRegistryOnce();
		const FName Key = Registry ? QinglanStoryIdentity::ResolveObjectiveKey(*Registry, ObjectiveId, SceneId) : NAME_None;
		return !Key.IsNone() && IsObjectiveCompleted(Story, Key);
	}

	FName GetPendingAgreement(const UQinglanStorySubsystem* Story, FName QuestId)
	{
		const FQinglanStorySession* Session = FindStorySession(Story);
		if (!Session)
		{
			return NAME_None;
		}
		const FName* Pending = Session->PendingAgreements.Find(QuestId);
		return Pending ? *Pending : NAME_None;
	}

	bool WasEndingPreviewed(const UQinglanStorySubsystem* Story, FName EndingId)
	{
		const FQinglanStorySession* Session = FindStorySession(Story);
		return Session && Session->PreviewedEndings.Contains(EndingId);
	}
} // namespace QinglanStoryRuntime
