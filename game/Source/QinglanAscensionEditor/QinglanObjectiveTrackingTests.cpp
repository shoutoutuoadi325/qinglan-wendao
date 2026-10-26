// Guidance follows unfinished authored scenes even when no dialogue is playing.
// These tests cover tracking state, not physical navigation or combat input.
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanMainObjectiveTracking,
	"Qinglan.Narrative.MainObjectiveSurvivesDialogueAndReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanMainObjectiveTracking::RunTest(const FString& Parameters)
{
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(NewObject<UGameInstance>());
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (!TestNotNull(TEXT("Production narrative registry"), Registry)) { return false; }
	TestEqual(TEXT("Fresh session immediately guides the player to the courtyard"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S01")));
	TestFalse(TEXT("Reading guidance never opens dialogue"), Story->IsScenePlaying());
	TestFalse(TEXT("Reading guidance never grants courtyard completion"), Story->IsSceneCompleted(TEXT("CH00_S01")));

	FQinglanNarrativeState State;
	State.CompletedScenes.Add(TEXT("CH00_S01"));
	State.Flags.Add(TEXT("mq_ch00_bell_examined"));
	QinglanStoryRuntime::ImportNarrativeState(Story, State);
	TestEqual(TEXT("Reloaded courtyard completion advances guidance to training"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S02")));
	if (!TestTrue(TEXT("Training dialogue starts through the production subsystem"), Story->BeginScene(TEXT("CH00_S02")))) { return false; }
	Story->SkipScene();
	TestFalse(TEXT("Choice-free training dialogue has finished"), Story->IsScenePlaying());
	TestTrue(TEXT("Playback cursor is cleared at dialogue end"), Story->GetCurrentSceneId().IsNone());
	TestFalse(TEXT("Finishing dialogue cannot grant the actual training objective"), Story->IsSceneCompleted(TEXT("CH00_S02")));
	TestEqual(TEXT("Training objective remains tracked after playback stops"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S02")));
	FQinglanNarrativeState Before;
	QinglanStoryRuntime::ExportNarrativeState(Story, Before);
	for (int32 Query = 0; Query < 10; ++Query) { Story->GetTrackedMainSceneId(); }
	FQinglanNarrativeState After;
	QinglanStoryRuntime::ExportNarrativeState(Story, After);
	TestTrue(TEXT("Drawing guidance does not manufacture gameplay events"), Before.FiredEventLog == After.FiredEventLog);
	TestTrue(TEXT("Drawing guidance does not change saved flags"), Before.Flags == After.Flags);
	TestTrue(TEXT("Drawing guidance does not complete scenes"), Before.CompletedScenes == After.CompletedScenes);

	State.CompletedScenes.Add(TEXT("CH00_S02"));
	State.Flags.Add(TEXT("mq_ch00_training_complete"));
	QinglanStoryRuntime::ImportNarrativeState(Story, State);
	TestEqual(TEXT("Completed training advances guidance to the route letter"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S03")));
	QinglanStoryRuntime::ExportNarrativeState(Story, After);
	QinglanStoryRuntime::ImportNarrativeState(Story, After);
	TestEqual(TEXT("Guidance survives a second narrative reload without a playback cursor"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S03")));

	// A currently playing main scene wins over other eligible scenes. This
	// matters for later chapters with parallel prerequisite branches.
	FQinglanNarrativeState Parallel;
	Parallel.Flags.Add(TEXT("mq_ch00_training_complete"));
	QinglanStoryRuntime::ImportNarrativeState(Story, Parallel);
	TestEqual(TEXT("Without active playback authored chapter order wins"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S01")));
	if (!TestTrue(TEXT("Another eligible main scene can start"), Story->BeginScene(TEXT("CH00_S03")))) { return false; }
	TestEqual(TEXT("Active unfinished main scene takes guidance priority"),
		Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S03")));

	FQinglanNarrativeState Finished;
	for (const FQinglanRegistryScene& Scene : Registry->MainScenes) { Finished.CompletedScenes.Add(Scene.SceneId); }
	QinglanStoryRuntime::ImportNarrativeState(Story, Finished);
	TestTrue(TEXT("Finished campaign has no stale main objective"), Story->GetTrackedMainSceneId().IsNone());
	return true;
}
#endif
