// Returning to optional agreements must not strand the final chapter.
#include "Core/QinglanInteractable.h"
#include "Core/QinglanTrialAdmission.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Progression/QinglanProfileSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGoldenCoreArenaReturn,
	"Qinglan.Progression.GoldenCoreCanReturnToEndingPlatforms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanGoldenCoreArenaReturn::RunTest(const FString& Parameters)
{
	using namespace QinglanTrialAdmission;
	UGameInstance* Owner = NewObject<UGameInstance>();
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Owner);
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(Owner);
	if (!TestNotNull(TEXT("Production story registry"), Story->GetRegistry())) { return false; }
	FQinglanProfileSnapshot ProfileState;
	ProfileState.CultivationStage = EQinglanStage::GoldenCore;
	ProfileState.CultivationRank = 0;
	Profile->ImportStateFromSave(ProfileState);
	FQinglanNarrativeState Narrative;
	Narrative.CompletedScenes = { TEXT("CH06_S01"), TEXT("CH06_S02") };
	Narrative.Flags = { TEXT("mq_ch05_complete"), TEXT("mq_core_trial_entered"),
		TEXT("mq_core_trial_won"), TEXT("mq_governor_shell_disabled") };
	QinglanStoryRuntime::ImportNarrativeState(Story, Narrative);
	TestTrue(TEXT("Rankless victor can re-enter the arena through the production gate policy"),
		EvaluateEntry(SelectForStage(Profile->GetCultivationStage()), Profile, Story).IsAllowed());
	TestFalse(TEXT("Story re-entry cannot start or reward the completed trial again"),
		EvaluateScene(TEXT("CH06_S02"), TEXT("s02.objective"), Profile, Story).IsAllowed());
	TestTrue(TEXT("Actual arena chapter routing resumes the release scene"),
		QinglanInteraction::BeginFirstSceneOfChapters(Story, { FName(TEXT("CH06")) }));
	TestEqual(TEXT("Completed trial is skipped"), Story->GetCurrentSceneId(), FName(TEXT("CH06_S03")));
	Narrative.CompletedScenes.Add(TEXT("CH06_S03"));
	Narrative.Flags.Add(TEXT("mq_shen_released"));
	QinglanStoryRuntime::ImportNarrativeState(Story, Narrative);
	TestTrue(TEXT("Returning after release is also admitted"), EvaluateEntry(ETrial::GoldenCore, Profile, Story).IsAllowed());
	TestTrue(TEXT("Actual chapter routing resumes the ending decision"),
		QinglanInteraction::BeginFirstSceneOfChapters(Story, { FName(TEXT("CH06")) }));
	TestEqual(TEXT("Decision platform remains reachable"), Story->GetCurrentSceneId(), FName(TEXT("CH06_S04")));
	Narrative.CompletedScenes.Remove(TEXT("CH06_S02"));
	QinglanStoryRuntime::ImportNarrativeState(Story, Narrative);
	TestFalse(TEXT("Golden Core stage alone cannot bypass the trial proof"), EvaluateEntry(ETrial::GoldenCore, Profile, Story).IsAllowed());
	TestFalse(TEXT("Return route cannot bypass the Foundation entry gate"), EvaluateEntry(ETrial::Foundation, Profile, Story).IsAllowed());
	TestFalse(TEXT("Missing story is never a completed-trial return"), EvaluateEntry(ETrial::GoldenCore, Profile, nullptr).IsAllowed());
	return true;
}
#endif
