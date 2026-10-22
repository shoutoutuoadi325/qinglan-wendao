// Story runtime support types: typed choice-action delegates, the per-instance
// playback/ambient session store, and the entry points the fixed scaffold
// header (Narrative/QinglanStorySubsystem.h) does not declare (barks, records,
// gameplay-event dispatch, safe hub time advance, narrative save bridge).
//
// The subsystem scaffold header is frozen (public API fixed by contract), so -
// exactly like Persistence/QinglanSaveSubsystem.cpp's FQinglanSaveBookkeeping -
// per-instance runtime state lives beside the subsystem, keyed by weak pointer,
// and the additional surface is exposed as free functions + typed dynamic
// delegates here (code-contracts.md "Typed dynamic delegates only").
#pragma once

#include "CoreMinimal.h"
#include "Combat/QinglanCombatComponent.h" // FQinglanResolvedDamage for the gameplay-proof relay
#include "Core/QinglanCoreTypes.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveGame.h"
#include "UObject/StrongObjectPtr.h"
#include "QinglanStoryRuntime.generated.h"

class UQinglanStorySubsystem;

// ---------------------------------------------------------------------------
// Typed choice-action delegates.
// data/story/main-early.json / main-late.json `action` payloads dispatch here
// when their choice is selected; each carries a stable idempotency key so
// gameplay handlers can journal one-time effects
// (docs/dev/implementation-blueprint.md section 3 action payloads).
// ---------------------------------------------------------------------------
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryEnterTrial, FName, IdempotencyKey, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryStayAtHub, FName, IdempotencyKey, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FQinglanStoryReleaseGuardian, FName, IdempotencyKey, FName, SceneId, FName, DissolutionFlag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryEndingPreviewed, FName, EndingId, FName, SceneId);
/** Confirm requested from a scene choice: UI shows the warning, then calls
 *  UQinglanStorySubsystem::ConfirmEnding with the acknowledgment result. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryConfirmEndingRequested, FName, EndingId, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryEndingConfirmed, FName, EndingId, FName, CommitFlag);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanStoryCancelConfirmation, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryReturnToHub, FName, IdempotencyKey, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryCompleteCampaign, FName, IdempotencyKey, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryProposalSelected, FName, QuestId, FName, PendingVersion);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryAgreementEffective, FName, QuestId, FName, EffectiveVersion);
/** A courier reply arrived for QuestId (a safe hub time advance flipped the
 *  delivery); Version names the pending agreement it answers. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryDeliveryArrived, FName, QuestId, FName, Version);
/** One ambient bark line, reusing the localized script-line struct. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanStoryBarkLine, FName, BarkId, const FQinglanScriptLine&, Line);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FQinglanStoryRecordOpened, FName, RecordId, const FText&, Title, const FText&, Body);

/** All typed story delegates; obtain via QinglanStoryRuntime::Delegates(Story). */
struct FQinglanStoryDelegates
{
	FQinglanStoryEnterTrial OnEnterTrial;
	FQinglanStoryStayAtHub OnStayAtHub;
	FQinglanStoryReleaseGuardian OnReleaseGuardian;
	FQinglanStoryEndingPreviewed OnEndingPreviewed;
	FQinglanStoryConfirmEndingRequested OnConfirmEndingRequested;
	FQinglanStoryEndingConfirmed OnEndingConfirmed;
	FQinglanStoryCancelConfirmation OnCancelConfirmation;
	FQinglanStoryReturnToHub OnReturnToHub;
	FQinglanStoryCompleteCampaign OnCompleteCampaign;
	FQinglanStoryProposalSelected OnProposalSelected;
	FQinglanStoryAgreementEffective OnAgreementEffective;
	FQinglanStoryDeliveryArrived OnDeliveryArrived;
	FQinglanStoryBarkLine OnBarkLine;
	FQinglanStoryRecordOpened OnRecordOpened;
};

/** One quest's pending courier delivery (SQ02/SQ03 proposal round trip);
 *  persisted form lives in the save schema so migrations and validation share it. */
using FQinglanStoryDelivery = FQinglanNarrativeDelivery;

/**
 * Per-subsystem runtime state: persistent narrative mirror (FQinglanNarrativeState
 * fields), in-flight scene playback, ending preview/confirm scratch, agreement
 * delivery clocks, objective journal, and ambient bark bookkeeping.
 */
struct FQinglanStorySession
{
	// --- Persistent narrative mirror (docs/story/narrative-implementation.md section 2) ---
	FName Revision = NAME_None;
	TSet<FName> CompletedMainScenes;
	TSet<FName> CompletedSideStages;
	TSet<FName> Flags;
	/** Scene-scoped choice key -> authored option ID. */
	TMap<FName, FName> ChoiceDecisions;
	/** QuestId -> pending version (selected draft, not yet effective). */
	TMap<FName, FName> PendingAgreements;
	/** QuestId -> effective version (delivered + verified + confirmed). */
	TMap<FName, FName> EffectiveAgreements;
	EQinglanEnding ConfirmedEnding = EQinglanEnding::None;

	// --- Scene playback (transient; a reload restarts the exchange coherently) ---
	FName CurrentSceneId = NAME_None;
	FName CurrentSceneQuestId = NAME_None; // side-quest owner; NAME_None for main
	bool bScenePlaying = false;
	bool bRevisitBranch = false;
	bool bChoicesPresented = false;
	bool bChoicesResolved = true;
	/** Playback-local decision families; authored unnamed choices form one family. */
	TSet<FName> ResolvedChoiceGroups;
	TSet<FName> PresentedChoiceIds;
	FName ActiveChoiceGroup = NAME_None;
	FName ActiveChoicePin = NAME_None;
	bool bAdvanceHalted = false;
	int32 NextScriptLine = 0;
	TArray<FName> PendingResponseKeys;
	FName DeferredSceneId = NAME_None;
	/** Action name of the last selected choice; decides scene auto-commit. */
	FName LastSelectedAction = NAME_None;

	// --- Endings ---
	/** Session ending-preview state; preview creates no flag
	 *  (docs/story/narrative-implementation.md section 4). */
	TSet<FName> PreviewedEndings;
	/** Scene whose confirm_ending choice is awaiting warning acknowledgment. */
	FName PendingConfirmSceneId = NAME_None;
	/** Extra flags from the confirm choice payload (e.g. mq_ch06_complete),
	 *  applied atomically by ConfirmEnding, never at selection. */
	TArray<FName> PendingConfirmFlags;

	// --- Agreements (SQ02/SQ03 pending vs effective versioning) ---
	/** Hub story clock in seconds; advanced only by safe hub time advances. */
	double StoryClockSeconds = 0.0;
	TMap<FName, FQinglanStoryDelivery> Deliveries;

	// --- Objectives / journal ---
	TSet<FName> CompletedObjectives;
	/** Prevent save delegates from recursively autosaving the same transaction. */
	bool bSavingStoryBoundary = false;

	// --- Ambient (barks) / dispatcher events ---
	FName CurrentLocation = NAME_None;
	/** BarkId -> last fired world-time seconds (paused-game-time timestamps). */
	TMap<FName, double> BarkLastWorldSeconds;
	/** BarkId -> times fired during the current visit (MaxPerVisit budget). */
	TMap<FName, int32> BarkPlaysThisVisit;
	/** Deterministic rotation cursor; no gameplay randomness (code-contracts). */
	int32 BarkRotationCursor = 0;
	/** Set form: trigger-gated script lines test membership. */
	TSet<FName> FiredEvents;
	/** Ordered history for proof evaluation (event_count repetition and the
	 *  ordered prefix). Every dispatch appends; bounded on write and in save. */
	TArray<FName> FiredEventLog;

	// --- GC-safe bindings + delegates ---
	TStrongObjectPtr<class UQinglanStorySignalRelay> Relay;
	FQinglanStoryDelegates Delegates;
};

/**
 * Minimal relay object: dynamic multicast delegates (encounter completion) can
 * only bind UFUNCTIONs, and the frozen subsystem header cannot grow them.
 */
UCLASS()
class UQinglanStorySignalRelay : public UObject
{
	GENERATED_BODY()

public:
	/** Bound to every active AQinglanEncounterDirector::OnEncounterCompleted. */
	UFUNCTION()
	void HandleEncounterCompleted();

	UPROPERTY()
	TWeakObjectPtr<UQinglanStorySubsystem> StorySubsystem;
};

/**
 * Gameplay-proof event relay (AUD-11 event producers). The player character's
 * combat/dodge/technique delegates are dynamic, so they can only bind
 * UFUNCTIONs; the character header is frozen, so these handlers live here and
 * forward stable proof event names to QinglanStoryRuntime::NotifyGameplayEvent.
 * Owned (and GC-rooted) by the character's binding helpers.
 */
UCLASS()
class UQinglanGameplayProofRelay : public UObject
{
	GENERATED_BODY()

public:
	/** Accepted damage resolved; emits combat.sword_hit when the owner attacked. */
	UFUNCTION()
	void HandleDamageResultResolved(const FQinglanResolvedDamage& Result);
	UFUNCTION()
	void HandleDodgeStarted(float Duration);
	UFUNCTION()
	void HandleCastReleased(FName TechniqueId, FQinglanCastId CastId);

	UPROPERTY()
	TWeakObjectPtr<UQinglanStorySubsystem> StorySubsystem;
	UPROPERTY()
	TWeakObjectPtr<AActor> OwnerActor;
};

/** Narrative-module tunables; every value cites its authored source. */
namespace QinglanStoryRuntime
{
	/** Registry data asset soft path (module brief / docs/dev/implementation-blueprint.md section 3). */
	inline constexpr const TCHAR* StoryRegistrySoftPath = TEXT("/Game/Qinglan/Data/DA_StoryRegistry");

	/** Single game string table (code-contracts.md "Localization keys"). */
	inline constexpr const TCHAR* GameStringTableId = TEXT("/Game/Qinglan/Data/ST_Game.ST_Game");

	/** Courier round trips are in-fiction time only, never real-time waits
	 * (docs/dev/implementation-blueprint.md section 1.9: "SQ02 18 h, SQ03 12 h,
	 * zero real-time wait"; data/story/side-quests.json delivery_transition). */
	inline constexpr double SQ02DeliveryGameHours = 18.0;
	inline constexpr double SQ03DeliveryGameHours = 12.0;
	/** Seconds per in-fiction hour for the hub story clock. */
	inline constexpr double SecondsPerGameHour = 3600.0;
	/** Bounded ordered event history; matches the save validator's
	 *  MaxNarrativeEntries ceiling so an exported log always validates. */
	inline constexpr int32 MaxFiredEventLogEntries = 1024;
	/** Hour fractions accumulate rounding error (17.9 h + 0.1 h can land a
	 *  hair under an exact 18 h ready time); arrivals compare with this
	 *  tolerance so authored whole-hour boundaries stay exact on both sides. */
	inline constexpr double DeliveryArrivalToleranceSeconds = 1e-3;

	/** Courier round-trip hours per quest (side-quests.json
	 *  delivery_transition.elapsed_game_hours): SQ02 18 h, SQ03 12 h, and any
	 *  later proposal quest inherits the SQ03 trip. Shared with the save
	 *  migration so synthesized deliveries match live dispatches. */
	inline double DeliveryGameHoursForQuest(FName QuestId)
	{
		return QuestId == FName(TEXT("SQ02")) ? SQ02DeliveryGameHours : SQ03DeliveryGameHours;
	}

	/** Bark scheduling defaults (docs/dev/implementation-blueprint.md section
	 * 2.6: "cooldown 180 s, <=1/visit, combat-suppressed"); per-bark registry
	 * values override these. */
	inline constexpr float DefaultBarkCooldownSeconds = 180.f;
	inline constexpr int32 DefaultBarkMaxPerVisit = 1;
	inline constexpr bool DefaultBarkSuppressDuringCombat = true;

	/** Idempotency-key convention for registry choice actions; the imported
	 * FQinglanRegistryChoice carries only the action name, not its payload map,
	 * so keys are derived deterministically from scene + action. */
	FName MakeActionIdempotencyKey(FName SceneId, FName ActionName);

	// --- Delegates ---
	/** Typed choice-action delegates for this subsystem instance. */
	FQinglanStoryDelegates& Delegates(UQinglanStorySubsystem* Story);

	// --- Ambient narrative ---
	/** Starts a new location visit: resets per-visit bark budgets. */
	void NotifyLocationEntered(UQinglanStorySubsystem* Story, FName Location);
	/** Evaluates registry barks and fires at most one eligible line
	 *  deterministically (rotating authored order; no gameplay randomness). */
	bool TryPlayBark(UQinglanStorySubsystem* Story);
	/** Opens one lore record if its RequiredFlags are satisfied; records the
	 *  read through UQinglanProfileSubsystem::MarkRecordRead and sets its
	 *  CollectionFlag exactly once. */
	bool TryInspectRecord(UQinglanStorySubsystem* Story, FName RecordId);

	// --- Dispatcher bridge: gameplay events -> trigger-gated lines + mq_* flags ---
	QINGLANASCENSION_API void NotifyGameplayEvent(UQinglanStorySubsystem* Story, FName Event, const TArray<FName>& SetsFlags);

	// --- Gameplay-proof guards (AUD-10/AUD-11) ---
	/** Guard evaluation outcome. OrderReset and DeliveryPending/TimeShort are
	 *  more actionable than a generic Missing, so they win when terms fail. */
	enum class EQinglanGuardStatus : uint8
	{
		Satisfied,
		Missing,
		OrderReset,
		DeliveryPending,
		TimeShort
	};

	struct FQinglanGuardReport
	{
		EQinglanGuardStatus Status = EQinglanGuardStatus::Satisfied;
		/** Unmet event/flag/quest names; ordered terms list their remaining
		 *  steps in authored order so a UI can describe the next action. */
		TArray<FName> Missing;
		/** Fully satisfied terms. */
		int32 Progress = 0;
		/** Total authored terms (0 = unauthored guard, vacuously satisfied). */
		int32 Total = 0;
	};

	/** Evaluates one conjunction against the session's fired-event log/flags,
	 *  courier deliveries, and story clock. Never mutates state. */
	QINGLANASCENSION_API FQinglanGuardReport EvaluateGuard(const UQinglanStorySubsystem* Story, const FQinglanRegistryGuard& Guard);
	/** Evaluates the named objective's authored proof. */
	QINGLANASCENSION_API FQinglanGuardReport EvaluateObjectiveGuard(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId);
	/** Evaluates a scene's structured confirmation_guard. Unauthored guards
	 *  report Satisfied so legacy scenes keep their existing commit path. */
	QINGLANASCENSION_API FQinglanGuardReport EvaluateSceneCommitGuard(const UQinglanStorySubsystem* Story, FName SceneId);
	/** True when the scene authors at least one confirmation_guard term. */
	QINGLANASCENSION_API bool SceneHasCommitGuard(const UQinglanStorySubsystem* Story, FName SceneId);
	/** Clears the ordered-prefix history for every ordered term of the scene
	 *  (objective and confirmation guards) so the player may retry the sequence. */
	QINGLANASCENSION_API void ResetOrderedProgress(UQinglanStorySubsystem* Story, FName SceneId);

	// --- Safe hub time advance (courier deliveries) ---
	/** Advances the in-fiction hub clock; marks matching deliveries arrived.
	 *  Returns true when at least one pending delivery became ready.
	 *  Exported for the editor automation specs (courier flow cases). */
	QINGLANASCENSION_API bool AdvanceHubTime(UQinglanStorySubsystem* Story, double GameHours);
	double GetStoryClockSeconds(const UQinglanStorySubsystem* Story);

	// --- Agreement delivery / revision (SQ02/SQ03 courier round trips) ---
	/** Pending-delivery snapshot for the sanctuary rest station and UI. */
	struct FQinglanDeliveryStatus
	{
		/** True while at least one dispatched reply has not arrived yet. */
		bool bHasPending = false;
		/** Hours until the last in-flight reply arrives (longest round trip);
		 *  what a single sanctuary rest must cover. */
		double RemainingHours = 0.0;
		/** Quests with a reply still in flight, lexical order. */
		TArray<FName> Quests;
	};
	/** Queries the pending courier deliveries against the current clock. */
	QINGLANASCENSION_API FQinglanDeliveryStatus QueryPendingDeliveries(const UQinglanStorySubsystem* Story);
	/**
	 * Replays one completed protocol stage with live choices so a settled
	 * agreement can be renegotiated (docs/story/narrative-implementation.md
	 * section 9). Guards: the stage belongs to a proposal quest, the quest is
	 * available, no ending is confirmed, and the stage is completed; a live
	 * encounter defers instead of freezing the player. Returns false when a
	 * guard rejects the revision (callers fall back to the recap branch).
	 */
	QINGLANASCENSION_API bool BeginRevisionScene(UQinglanStorySubsystem* Story, FName SceneId);
	/** "Accepted plan X. Proposal awaiting confirmation Y." for one quest
	 *  (authored pending_status key); empty for non-protocol quests. */
	QINGLANASCENSION_API FText PendingStatusText(const UQinglanStorySubsystem* Story, FName QuestId);
	/** Authored arrival notice for one quest's courier reply (the accept
	 *  stage's delivery_time key); empty when unauthored. */
	QINGLANASCENSION_API FText DeliveryTimeText(const UQinglanStorySubsystem* Story, FName QuestId);

	// --- Save bridge (see QinglanStorySubsystem.cpp FIXME-CONTRACT) ---
	QINGLANASCENSION_API void ExportNarrativeState(const UQinglanStorySubsystem* Story, FQinglanNarrativeState& Out);
	QINGLANASCENSION_API void ImportNarrativeState(UQinglanStorySubsystem* Story, const FQinglanNarrativeState& In);
	/** Persist the current narrative transaction through the active profile save path. */
	QINGLANASCENSION_API bool SaveStoryBoundary(UQinglanStorySubsystem* Story);
#if WITH_DEV_AUTOMATION_TESTS
	/** Transient registry injection; tests must clear this with nullptr before returning. */
	QINGLANASCENSION_API void SetRegistryForTesting(UQinglanStoryRegistry* Registry);
#endif

	// --- State queries for UI / automation specs ---
	QINGLANASCENSION_API bool IsObjectiveCompleted(const UQinglanStorySubsystem* Story, FName ObjectiveId);
	QINGLANASCENSION_API bool IsSceneObjectiveCompleted(const UQinglanStorySubsystem* Story, FName SceneId, FName ObjectiveId);
	QINGLANASCENSION_API FName GetPendingAgreement(const UQinglanStorySubsystem* Story, FName QuestId);
	bool WasEndingPreviewed(const UQinglanStorySubsystem* Story, FName EndingId);
} // namespace QinglanStoryRuntime
