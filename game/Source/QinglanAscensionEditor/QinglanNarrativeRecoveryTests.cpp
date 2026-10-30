// Interrupted dialogue and profile reloads must not retain conflicting decisions.
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanInterruptedChoiceReplacesBranch,
	"Qinglan.Narrative.InterruptedChoiceReplacesBranchFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanInterruptedChoiceReplacesBranch::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>();
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = TEXT("recovery.choice");
	FQinglanRegistryChoice First;
	First.ChoiceId = TEXT("first");
	First.SetsFlags = { TEXT("branch.first"), TEXT("branch.common") };
	First.ResponseKeys = { TEXT("story.ch00.s01.response_LOOK") }; // Leaves a reply pending at the save boundary.
	Scene.Choices.Add(First);
	FQinglanRegistryChoice Second = First;
	Second.ChoiceId = TEXT("second");
	Second.SetsFlags = { TEXT("branch.second"), TEXT("branch.common") };
	Scene.Choices.Add(Second);
	QinglanStoryRuntime::SetRegistryForTesting(Registry);
	ON_SCOPE_EXIT { QinglanStoryRuntime::SetRegistryForTesting(nullptr); };
	TestTrue(TEXT("Conversation begins"), Story->BeginScene(Scene.SceneId));
	TestTrue(TEXT("First answer is selected"), Story->SelectChoice(First.ChoiceId));
	TestTrue(TEXT("The unplayed response keeps the scene incomplete"), Story->IsScenePlaying());
	FQinglanNarrativeState Saved;
	QinglanStoryRuntime::ExportNarrativeState(Story, Saved);
	QinglanStoryRuntime::ImportNarrativeState(Story, Saved);
	TestTrue(TEXT("Interrupted exchange restarts"), Story->BeginScene(Scene.SceneId));
	TestTrue(TEXT("Player can replace the interrupted answer"), Story->SelectChoice(Second.ChoiceId));
	TestFalse(TEXT("The old answer no longer activates its conditional lines"), Story->HasFlag(TEXT("branch.first")));
	TestTrue(TEXT("The replacement activates its own conditional lines"), Story->HasFlag(TEXT("branch.second")));
	TestTrue(TEXT("Flags shared by both answers stay present"), Story->HasFlag(TEXT("branch.common")));
	TestTrue(TEXT("The old journal decision is removed"), Story->GetSceneChoiceDecision(Scene.SceneId, First.ChoiceId).IsNone());
	TestEqual(TEXT("The journal records only the replacement"),
		Story->GetSceneChoiceDecision(Scene.SceneId, Second.ChoiceId), Second.ChoiceId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanReloadDiscardsPendingEnding,
	"Qinglan.Narrative.ReloadDiscardsPendingEndingConfirmation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanReloadDiscardsPendingEnding::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>();
	FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
	Scene.SceneId = TEXT("recovery.ending");
	FQinglanRegistryChoice& Choice = Scene.Choices.AddDefaulted_GetRef();
	Choice.ChoiceId = TEXT("confirm_open");
	Choice.Action = TEXT("confirm_ending");
	Choice.SetsFlags = { TEXT("ending_open"), TEXT("previous_profile.confirmation") };
	QinglanStoryRuntime::SetRegistryForTesting(Registry);
	ON_SCOPE_EXIT { QinglanStoryRuntime::SetRegistryForTesting(nullptr); };
	FQinglanNarrativeState Ready;
	Ready.Flags = { TEXT("mq_shen_released") };
	QinglanStoryRuntime::ImportNarrativeState(Story, Ready);
	TestTrue(TEXT("First profile previews Open River"), Story->PreviewEnding(TEXT("ending_open")));
	TestTrue(TEXT("First profile opens ending controls"), Story->BeginScene(Scene.SceneId));
	TestTrue(TEXT("First profile requests Open River confirmation"), Story->SelectChoice(Choice.ChoiceId));
	TestFalse(TEXT("The request itself does not commit an ending"), Story->IsEndingConfirmed());
	QinglanStoryRuntime::ImportNarrativeState(Story, Ready);
	TestFalse(TEXT("Reload clears prior preview authorization"), QinglanStoryRuntime::WasEndingPreviewed(Story, TEXT("ending_open")));
	TestTrue(TEXT("Reloaded profile previews Quiet Summit"), Story->PreviewEnding(TEXT("ending_seal")));
	FText Error;
	TestTrue(TEXT("Reloaded profile confirms its selected ending"), Story->ConfirmEnding(TEXT("ending_seal"), true, Error));
	TestTrue(TEXT("Exactly the new ending flag is committed"), Story->HasFlag(TEXT("ending_seal")));
	TestFalse(TEXT("Previous confirmation cannot add a contradictory ending"), Story->HasFlag(TEXT("ending_open")));
	TestFalse(TEXT("Previous profile payload cannot leak through reload"), Story->HasFlag(TEXT("previous_profile.confirmation")));
	TestFalse(TEXT("Previous profile's pending scene cannot complete in the reloaded profile"), Story->IsSceneCompleted(Scene.SceneId));
	return true;
}
#endif
