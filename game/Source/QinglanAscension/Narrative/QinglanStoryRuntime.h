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
#include "Core/QinglanCoreTypes.h"
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
	FQinglanStoryBarkLine OnBarkLine;
	FQinglanStoryRecordOpened OnRecordOpened;
};

/** One quest's pending courier delivery (SQ02/SQ03 proposal round trip). */
struct FQinglanStoryDelivery
{
	/** Pending version this delivery was dispatched for; NAME_None = none. */
	FName Version = NAME_None;
	/** Story-clock second at which the matching reply arrives. */
	double ReadyAtStorySeconds = 0.0;
	/** True once the safe hub time advance delivered the matching reply. */
	bool bDelivered = false;
};

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
	TSet<FName> FiredEvents;

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

/** Narrative-module tunables; every value cites its authored source. */
namespace QinglanStoryRuntime
{
	/** Registry data asset soft path (module brief / docs/dev/implementation-blueprint.md section 3). */
	inline constexpr const TCHAR* StoryRegistrySoftPath = TEXT("/Game/Qinglan/Data/DA_StoryRegistry");

	/** Single game string table (code-contracts.md "Localization keys"). */
	inline constexpr const TCHAR* GameStringTableId = TEXT("/Game/Qinglan/Data/ST_Game");

	/** Courier round trips are in-fiction time only, never real-time waits
	 * (docs/dev/implementation-blueprint.md section 1.9: "SQ02 18 h, SQ03 12 h,
	 * zero real-time wait"; data/story/side-quests.json delivery_transition). */
	inline constexpr double SQ02DeliveryGameHours = 18.0;
	inline constexpr double SQ03DeliveryGameHours = 12.0;
	/** Seconds per in-fiction hour for the hub story clock. */
	inline constexpr double SecondsPerGameHour = 3600.0;

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
	void NotifyGameplayEvent(UQinglanStorySubsystem* Story, FName Event, const TArray<FName>& SetsFlags);

	// --- Safe hub time advance (courier deliveries) ---
	/** Advances the in-fiction hub clock; marks matching deliveries arrived.
	 *  Returns true when at least one pending delivery became ready. */
	bool AdvanceHubTime(UQinglanStorySubsystem* Story, double GameHours);
	double GetStoryClockSeconds(const UQinglanStorySubsystem* Story);

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
	FName GetPendingAgreement(const UQinglanStorySubsystem* Story, FName QuestId);
	bool WasEndingPreviewed(const UQinglanStorySubsystem* Story, FName EndingId);
} // namespace QinglanStoryRuntime
