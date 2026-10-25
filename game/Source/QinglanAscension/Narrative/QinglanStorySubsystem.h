// Story subsystem: narrative registry access, scene playback, choices, endings.
// Data authored in data/story/*.json is imported into data assets at edit time;
// this subsystem executes scenes against imported registries.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanStorySubsystem.generated.h"

class UQinglanStoryRegistry;

/** A single localized script line ready for the dialogue widget. */
USTRUCT(BlueprintType)
struct FQinglanScriptLine
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FText SpeakerName;
	UPROPERTY(BlueprintReadOnly) FText Text;
	UPROPERTY(BlueprintReadOnly) EQinglanSourceKind SourceKind = EQinglanSourceKind::Live;
	UPROPERTY(BlueprintReadOnly) FName SceneId = NAME_None;
	UPROPERTY(BlueprintReadOnly) int32 LineIndex = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanSceneLine, const FQinglanScriptLine&, Line);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanChoicesPresented, const TArray<FQinglanSceneChoice>&, Choices);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanSceneEvent, FName, SceneId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanObjectiveUpdated, FName, ObjectiveId, const FText&, Text);

/**
 * Runtime narrative dispatcher. Owns pending scene playback, choice resolution,
 * agreement versioning (SQ02/SQ03), ending preview/confirm, and objective state.
 * Never freezes the player while enemies are active; defers playback instead.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanStorySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// --- Registry ---
	UFUNCTION(BlueprintPure) const UQinglanStoryRegistry* GetRegistry() const;
	/** Scenes whose prerequisites are satisfied and are not yet completed. */
	UFUNCTION(BlueprintPure) TArray<FName> GetPlayableMainScenes() const;
	UFUNCTION(BlueprintPure) TArray<FName> GetAvailableSideQuests() const;
	/** Ending-specific epilogue text keys for a quest, authored order [open,
	 *  seal, shared] (side-quests.json epilogue_keys); empty for unknown quests. */
	UFUNCTION(BlueprintPure) TArray<FName> GetQuestEpilogueKeys(FName QuestId) const;
	/** The epilogue key for one ending (index 0 open / 1 seal / 2 shared); NAME_None when unauthored. */
	UFUNCTION(BlueprintPure) FName GetQuestEpilogueKey(FName QuestId, EQinglanEnding Ending) const;
	/** Journal key authored for a main scene or side-quest stage (journal_key); NAME_None when unauthored. */
	UFUNCTION(BlueprintPure) FName GetStageJournalKey(FName SceneId) const;

	// --- Scene playback ---
	/** Begins a scene at a safe boundary. Returns false when prerequisites fail. */
	UFUNCTION(BlueprintCallable) bool BeginScene(FName SceneId);
	UFUNCTION(BlueprintCallable) void AdvanceLine();
	UFUNCTION(BlueprintCallable) bool SelectChoice(FName ChoiceId);
	UFUNCTION(BlueprintPure) bool IsScenePlaying() const;
	UFUNCTION(BlueprintPure) FName GetCurrentSceneId() const;
	/** Main-story guidance is independent of dialogue playback. Returns the
	 *  active unresolved main scene, then the first eligible unfinished scene
	 *  in authored chapter order. This query never starts or completes scenes. */
	UFUNCTION(BlueprintPure) FName GetTrackedMainSceneId() const;
	/** Skips remaining lines; still applies journal/objective updates. */
	UFUNCTION(BlueprintCallable) void SkipScene();

	// --- State queries ---
	UFUNCTION(BlueprintPure) bool IsSceneCompleted(FName SceneId) const;
	UFUNCTION(BlueprintPure) bool HasFlag(FName Flag) const;
	UFUNCTION(BlueprintPure) FName GetChoiceDecision(FName ChoiceId) const;
	/** Explicit scene ownership; legacy bare queries return none when ambiguous. */
	UFUNCTION(BlueprintPure) FName GetSceneChoiceDecision(FName SceneId, FName ChoiceId) const;
	UFUNCTION(BlueprintPure) bool IsEndingConfirmed() const;
	UFUNCTION(BlueprintPure) EQinglanEnding GetConfirmedEnding() const;

	// --- Endings ---
	/** Ending ids: ending_open, ending_seal, ending_shared. */
	UFUNCTION(BlueprintPure) bool IsEndingAvailable(FName EndingId, FText& OutRequirementText) const;
	/** Enters preview; creates no ending flag. */
	UFUNCTION(BlueprintCallable) bool PreviewEnding(FName EndingId);
	/** Close preview/confirmation scratch without changing any permanent story state. */
	UFUNCTION(BlueprintCallable) void CancelEndingPreview();
	/** Atomic one-time commit; requires prior preview + warning acknowledgment. */
	UFUNCTION(BlueprintCallable) bool ConfirmEnding(FName EndingId, bool bWarningAcknowledged, FText& OutError);

	// --- Agreements (SQ02/SQ03 pending vs effective) ---
	UFUNCTION(BlueprintCallable) bool SelectProposal(FName QuestId, FName ProposalVersion);
	UFUNCTION(BlueprintPure) FName GetEffectiveAgreement(FName QuestId) const;
	UFUNCTION(BlueprintCallable) bool DeliverAgreement(FName QuestId);

	// --- Objective / journal bridge ---
	UFUNCTION(BlueprintCallable) void NotifyObjectiveCompleted(FName ObjectiveId);
	/** Gameplay proof must name the owning scene; true means accepted (including replay), not necessarily scene complete. */
	UFUNCTION(BlueprintCallable) bool NotifySceneObjectiveCompleted(FName SceneId, FName ObjectiveId);

	UPROPERTY(BlueprintAssignable) FQinglanSceneLine OnSceneLine;
	UPROPERTY(BlueprintAssignable) FQinglanChoicesPresented OnChoicesPresented;
	UPROPERTY(BlueprintAssignable) FQinglanSceneEvent OnSceneEntered;
	UPROPERTY(BlueprintAssignable) FQinglanSceneEvent OnSceneCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanObjectiveUpdated OnObjectiveUpdated;
};
