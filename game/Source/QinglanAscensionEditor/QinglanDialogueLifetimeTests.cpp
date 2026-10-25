// Production widget tree: subtitles end with playback, while proof and
// independent document/notice presentation retain their separate lifetimes.
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDialoguePlaybackLifetime,
	"Qinglan.UI.FinishedTrainingDialogueReleasesViewWithoutCompletingObjective",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanDialoguePlaybackLifetime::RunTest(const FString& Parameters)
{
	UGameInstance* Instance = nullptr;
	UWorld* World = nullptr;
	ON_SCOPE_EXIT
	{
		if (World) { World->EndPlay(EEndPlayReason::Quit); }
		if (Instance) { Instance->Shutdown(); }
		if (World)
		{
			if (GEngine) { GEngine->DestroyWorldContext(World); }
			World->DestroyWorld(false);
		}
	};
	if (!TestNotNull(TEXT("Engine"), GEngine)) { return false; }
	const FString Name = TEXT("L_DialogueLifetime_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Instance = NewObject<UGameInstance>(GEngine);
	Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
	World = Instance->GetWorld();
	if (!TestNotNull(TEXT("Owning world"), World)) { return false; }
	UClass* Class = LoadClass<UQinglanDialogueWidget>(nullptr, TEXT("/Game/Qinglan/UI/WBP_Dialogue.WBP_Dialogue_C"));
	if (!TestNotNull(TEXT("Production dialogue class"), Class)) { return false; }
	// The GameInstance overload builds a real widget tree without creating a
	// viewport/local player in an unattended commandlet world.
	UQinglanDialogueWidget* Widget = CreateWidget<UQinglanDialogueWidget>(Instance, Class);
	if (!TestNotNull(TEXT("Real dialogue widget"), Widget)) { return false; }
	Widget->TakeWidget(); // Builds the production Slate/UMG tree.
	UTextBlock* Line = Cast<UTextBlock>(Widget->WidgetTree->FindWidget(TEXT("Txt_Line")));
	UBorder* Panel = Line && Line->GetParent() && Line->GetParent()->GetParent()
		&& Line->GetParent()->GetParent()->GetParent()
		? Cast<UBorder>(Line->GetParent()->GetParent()->GetParent()) : nullptr;
	// Txt_Line sits inside ScrollBox -> VerticalBox -> Border.
	if (!TestNotNull(TEXT("Visible dialogue line"), Line) || !TestNotNull(TEXT("Actual subtitle panel"), Panel)) { return false; }
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Story authority"), Story)) { return false; }
	FQinglanNarrativeState Ready;
	Ready.CompletedScenes.Add(TEXT("CH00_S01"));
	Ready.Flags.Add(TEXT("mq_ch00_bell_examined"));
	QinglanStoryRuntime::ImportNarrativeState(Story, Ready);
	if (!TestTrue(TEXT("Production training playback begins"), Story->BeginScene(TEXT("CH00_S02")))) { return false; }
	Widget->SetScenePlaybackLine(FText::FromString(TEXT("Keeper Lan")), FText::FromString(TEXT("Training instruction")));
	Widget->RefreshScenePlayback(Story->IsScenePlaying());
	TestEqual(TEXT("Live playback keeps the subtitle panel visible"), Panel->GetVisibility(), ESlateVisibility::Visible);
	Story->SkipScene();
	TestFalse(TEXT("Training playback ends before gameplay proof"), Story->IsScenePlaying());
	Widget->RefreshScenePlayback(Story->IsScenePlaying());
	TestEqual(TEXT("Finished instruction releases the player view"), Panel->GetVisibility(), ESlateVisibility::Collapsed);
	TestTrue(TEXT("Old subtitle text is cleared"), Line->GetText().IsEmpty());
	TestFalse(TEXT("Hiding subtitles cannot complete training"), Story->IsSceneCompleted(TEXT("CH00_S02")));
	TestEqual(TEXT("Training remains the tracked main objective"), Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S02")));
	Widget->ShowRecordEntry(FText::FromString(TEXT("Route letter")), FText::FromString(TEXT("Document body")));
	Widget->RefreshScenePlayback(false);
	TestEqual(TEXT("Independent document remains readable after playback"), Panel->GetVisibility(), ESlateVisibility::Visible);
	TestEqual(TEXT("Document body is preserved"), Line->GetText().ToString(), FString(TEXT("Document body")));
	TestTrue(TEXT("Back/pause can dismiss the physical document"), Widget->DismissRecord());
	TestEqual(TEXT("Document dismissal releases the gameplay view"), Panel->GetVisibility(), ESlateVisibility::Collapsed);
	TestTrue(TEXT("Dismissed document leaves no stale body"), Line->GetText().IsEmpty());
	TestFalse(TEXT("A second dismissal does not consume normal pause input"), Widget->DismissRecord());
	Widget->SetCurrentLine(FText::GetEmpty(), FText::FromString(TEXT("Interaction notice")));
	Widget->RefreshScenePlayback(false);
	TestEqual(TEXT("Independent notice is not mistaken for an expired subtitle"), Line->GetText().ToString(), FString(TEXT("Interaction notice")));
	return true;
}
#endif
