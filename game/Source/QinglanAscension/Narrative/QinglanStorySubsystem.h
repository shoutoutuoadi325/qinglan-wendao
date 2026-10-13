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
class UQinglanStorySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// --- Registry ---
	UFUNCTION(BlueprintPure) const UQinglanStoryRegistry* GetRegistry() const;
	/** Scenes whose prerequisites are satisfied and are not yet completed. */
	UFUNCTION(BlueprintPure) TArray<FName> GetPlayableMainScenes() const;
	UFUNCTION(BlueprintPure) TArray<FName> GetAvailableSideQuests() const;

	// --- Scene playback ---
	/** Begins a scene at a safe boundary. Returns false when prerequisites fail. */
	UFUNCTION(BlueprintCallable) bool BeginScene(FName SceneId);
	UFUNCTION(BlueprintCallable) void AdvanceLine();
	UFUNCTION(BlueprintCallable) bool SelectChoice(FName ChoiceId);
	UFUNCTION(BlueprintPure) bool IsScenePlaying() const;
	UFUNCTION(BlueprintPure) FName GetCurrentSceneId() const;
	/** Skips remaining lines; still applies journal/objective updates. */
	UFUNCTION(BlueprintCallable) void SkipScene();

	// --- State queries ---
	UFUNCTION(BlueprintPure) bool IsSceneCompleted(FName SceneId) const;
	UFUNCTION(BlueprintPure) bool HasFlag(FName Flag) const;
	UFUNCTION(BlueprintPure) FName GetChoiceDecision(FName ChoiceId) const;
	UFUNCTION(BlueprintPure) bool IsEndingConfirmed() const;
	UFUNCTION(BlueprintPure) EQinglanEnding GetConfirmedEnding() const;

	// --- Endings ---
	/** Ending ids: ending_open, ending_seal, ending_shared. */
	UFUNCTION(BlueprintPure) bool IsEndingAvailable(FName EndingId, FText& OutRequirementText) const;
	/** Enters preview; creates no ending flag. */
	UFUNCTION(BlueprintCallable) bool PreviewEnding(FName EndingId);
	/** Atomic one-time commit; requires prior preview + warning acknowledgment. */
	UFUNCTION(BlueprintCallable) bool ConfirmEnding(FName EndingId, bool bWarningAcknowledged, FText& OutError);

	// --- Agreements (SQ02/SQ03 pending vs effective) ---
	UFUNCTION(BlueprintCallable) bool SelectProposal(FName QuestId, FName ProposalVersion);
	UFUNCTION(BlueprintPure) FName GetEffectiveAgreement(FName QuestId) const;
	UFUNCTION(BlueprintCallable) bool DeliverAgreement(FName QuestId);

	// --- Objective / journal bridge ---
	UFUNCTION(BlueprintCallable) void NotifyObjectiveCompleted(FName ObjectiveId);

	UPROPERTY(BlueprintAssignable) FQinglanSceneLine OnSceneLine;
	UPROPERTY(BlueprintAssignable) FQinglanChoicesPresented OnChoicesPresented;
	UPROPERTY(BlueprintAssignable) FQinglanSceneEvent OnSceneEntered;
	UPROPERTY(BlueprintAssignable) FQinglanSceneEvent OnSceneCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanObjectiveUpdated OnObjectiveUpdated;
};
