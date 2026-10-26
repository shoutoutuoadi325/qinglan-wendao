// The shared dodge action must confirm with gamepad A while a choice owns
// input, without turning keyboard Space into a confirm key or losing dodge.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGamepadChoiceConfirm,
	"Qinglan.Input.GamepadAConfirmsChoiceWhileSpaceRemainsDodge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanGamepadChoiceConfirm::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("L_Input_Gamepad_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
	UWorld* World = nullptr;
	UGameInstance* Instance = nullptr;
	ON_SCOPE_EXIT
	{
		if (World) { World->EndPlay(EEndPlayReason::Quit); }
		if (Instance) { Instance->Shutdown(); }
		if (World)
		{
			if (GEngine) { GEngine->DestroyWorldContext(World); }
			World->DestroyWorld(false);
		}
		UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestNotNull(TEXT("Engine"), GEngine)) { return false; }
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
	Instance = NewObject<UGameInstance>(GEngine);
	Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
	World = Instance->GetWorld();
	if (!TestNotNull(TEXT("Private input game world"), World)) { return false; }
	UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Save subsystem"), Save) || !TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	Save->CreateNewProfile(0); // Avoid opening a frontend in this headless input fixture.
	if (!TestTrue(TEXT("Isolated profile is active"), Save->IsProfileSessionActive())) { return false; }
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(), FVector(0.f, 0.f, 150.f), FRotator::ZeroRotator, Params);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Production character"), Player) || !TestNotNull(TEXT("Player controller"), Controller)) { return false; }
	Controller->SetPlayerState(World->SpawnActor<APlayerState>());
	Controller->PlayerInput = NewObject<UPlayerInput>(Controller);
	Controller->Possess(Player);
	World->SetBegunPlay(true);
	Controller->DispatchBeginPlay();
	Player->DispatchBeginPlay();
	if (!TestNotNull(TEXT("Combat component"), Player->GetCombat())) { return false; }

	// Feed the real PlayerInput key-state path used by IsInputKeyDown. The
	// production Enhanced Input handler is public, so no test-only API or
	// raw-key binding is needed to exercise its routing.
	auto Key = [Controller](FKey InKey, EInputEvent Event)
	{
		Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(InKey, Event, Event == IE_Released ? 0.f : 1.f));
		const TArray<UInputComponent*> EmptyStack;
		Controller->PlayerInput->ProcessInputStack(EmptyStack, 1.f / 60.f, false);
	};
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("interact.bell_pulled"), {});
	if (!TestTrue(TEXT("Production opening exchange starts"), Story->BeginScene(TEXT("CH00_S01")))) { return false; }
	Story->SkipScene();
	FQinglanActionGateState Gate;
	Player->GetActionGateState(Gate);
	if (!TestTrue(TEXT("Actual scene-choice delegate owns input"), Gate.bSceneChoicePending)) { return false; }
	const float StaminaBefore = Player->GetCombat()->GetAttributes().Stamina;
	Key(EKeys::SpaceBar, IE_Pressed);
	Player->InputDodge(FInputActionValue(true));
	TestTrue(TEXT("Space does not confirm a pending gamepad/keyboard choice"), Story->GetSceneChoiceDecision(TEXT("CH00_S01"), TEXT("CH00_S01_LOOK")).IsNone());
	TestEqual(TEXT("Frozen-choice Space consumes no dodge stamina"), Player->GetCombat()->GetAttributes().Stamina, StaminaBefore);
	Key(EKeys::SpaceBar, IE_Released);
	Key(EKeys::Gamepad_FaceButton_Bottom, IE_Pressed);
	if (!TestTrue(TEXT("Controller sees physical gamepad A state"), Controller->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom))) { return false; }
	Player->InputDodge(FInputActionValue(true));
	TestEqual(TEXT("A confirms the currently selected actual story choice"), Story->GetSceneChoiceDecision(TEXT("CH00_S01"), TEXT("CH00_S01_LOOK")), FName(TEXT("CH00_S01_LOOK")));
	TestEqual(TEXT("A confirmation consumes no dodge stamina"), Player->GetCombat()->GetAttributes().Stamina, StaminaBefore);
	Key(EKeys::Gamepad_FaceButton_Bottom, IE_Released);
	for (int32 Line = 0; Line < 10 && Story->IsScenePlaying(); ++Line) { Story->AdvanceLine(); }
	TestTrue(TEXT("Choice response finishes the opening scene"), Story->IsSceneCompleted(TEXT("CH00_S01")));
	Key(EKeys::Gamepad_FaceButton_Bottom, IE_Pressed);
	Player->InputDodge(FInputActionValue(true));
	TestEqual(TEXT("A returns to gameplay dodge once overlays close"), Player->GetCombatState(), EQinglanCombatState::Dodging);
	TestTrue(TEXT("Gameplay A consumes real dodge stamina"), Player->GetCombat()->GetAttributes().Stamina < StaminaBefore);
	Key(EKeys::Gamepad_FaceButton_Bottom, IE_Released);
	return true;
}
#endif
