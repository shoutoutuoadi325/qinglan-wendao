// Active dialogue subscriptions must survive garbage collection and detach on close.
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDialogueSubscriptionLifetime,
	"Qinglan.UI.DialogueSubscriptionsSurviveGarbageCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanDialogueSubscriptionLifetime::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine"), GEngine)) { return false; }
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
	const FString Name = TEXT("L_WidgetLifetime_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
	UWorld* World = Instance->GetWorld();
	ON_SCOPE_EXIT
	{
		if (World) { World->EndPlay(EEndPlayReason::Quit); }
		Instance->Shutdown();
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};
	if (!TestNotNull(TEXT("Owning world"), World)) { return false; }
	UClass* DialogueClass = LoadClass<UQinglanDialogueWidget>(nullptr,
		TEXT("/Game/Qinglan/UI/WBP_Dialogue.WBP_Dialogue_C"));
	if (!TestNotNull(TEXT("Production dialogue class"), DialogueClass)) { return false; }
	TStrongObjectPtr<UQinglanDialogueWidget> Widget(CreateWidget<UQinglanDialogueWidget>(Instance.Get(), DialogueClass));
	if (!TestNotNull(TEXT("Production dialogue widget"), Widget.Get())) { return false; }
	// The viewport retains this Slate wrapper while dialogue is open. Dropping
	// TakeWidget's temporary immediately invokes NativeDestruct in UE 5.8.3,
	// so a strong UObject pointer alone would exercise a closed widget.
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	TWeakObjectPtr<UObject> Relay;
	ForEachObjectWithOuter(Widget.Get(), [&Relay](UObject* Object)
	{
		if (Object->GetClass()->GetFName() == TEXT("QinglanUIEventRelay")) { Relay = Object; }
	}, false);
	if (!TestTrue(TEXT("The real widget creates a subscription adapter"), Relay.IsValid())) { return false; }
	TestTrue(TEXT("Visible dialogue subscribes to story lines"), Story->OnSceneLine.Contains(Relay.Get(), FName(TEXT("HandleSceneLine"))));
	CollectGarbage(RF_NoFlags);
	if (!TestTrue(TEXT("The adapter survives GC while its widget is alive"), Relay.IsValid())) { return false; }
	TestTrue(TEXT("Story lines remain bound after GC"), Story->OnSceneLine.Contains(Relay.Get(), FName(TEXT("HandleSceneLine"))));
	TestTrue(TEXT("Story choices remain bound after GC"), Story->OnChoicesPresented.Contains(Relay.Get(), FName(TEXT("HandleChoicesPresented"))));
	SlateWidget.Reset(); // Remove the real Slate owner; routes NativeDestruct.
	TestFalse(TEXT("Closed dialogue stops receiving story lines"), Story->OnSceneLine.Contains(Relay.Get(), FName(TEXT("HandleSceneLine"))));
	TestFalse(TEXT("Closed dialogue stops receiving choices"), Story->OnChoicesPresented.Contains(Relay.Get(), FName(TEXT("HandleChoicesPresented"))));
	SlateWidget = Widget->TakeWidget(); // Rebuild routes NativeConstruct.
	TestTrue(TEXT("Reopening restores line subscriptions"), Story->OnSceneLine.Contains(Relay.Get(), FName(TEXT("HandleSceneLine"))));
	TestTrue(TEXT("Reopening restores choice subscriptions"), Story->OnChoicesPresented.Contains(Relay.Get(), FName(TEXT("HandleChoicesPresented"))));
	SlateWidget.Reset();
	return true;
}
#endif
