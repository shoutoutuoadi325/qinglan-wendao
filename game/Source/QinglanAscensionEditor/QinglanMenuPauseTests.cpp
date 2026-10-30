// Modal UI must stop the real world clock and retain usable paused input.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "TimerManager.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FMenuPauseWorld
	{
		const uint64 SavedFrameCounter = GFrameCounter;
		FString Name = TEXT("L_MenuPause_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		UQinglanSaveSubsystem* Save = nullptr;
		APlayerController* Controller = nullptr;
		AQinglanCharacter* Player = nullptr;
		UEnhancedInputComponent* Input = nullptr;

		explicit FMenuPauseWorld(bool bCreateProfile)
		{
			if (!GEngine) { return; }
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
			if (!World || !Save) { return; }
			FText Error;
			if (bCreateProfile && !Save->TryCreateNewProfile(0, Error)) { return; }
			// A real authority game mode and PlayerState own UE's pause flag.
			// The engine base mode avoids unrelated campaign arrival work.
			FURL URL;
			URL.AddOption(TEXT("game=/Script/Engine.GameModeBase"));
			if (!World->SetGameMode(URL)) { return; }
			World->InitializeActorsForPlay(URL);
			World->BeginPlay();
			World->SetBegunPlay(true);
			Controller = World->SpawnActor<APlayerController>();
			Player = World->SpawnActor<AQinglanCharacter>();
			if (!Controller || !Player) { return; }
			Controller->SetPlayerState(World->SpawnActor<APlayerState>());
			ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
			Instance->AddLocalPlayer(LocalPlayer, FPlatformUserId::CreateFromInternalId(0));
			Controller->SetPlayer(LocalPlayer);
			Controller->PlayerInput = NewObject<UPlayerInput>(Controller);
			Controller->Possess(Player);
			Player->GetCharacterMovement()->DisableMovement();
			Input = NewObject<UEnhancedInputComponent>(Player);
			Input->RegisterComponent();
			Player->SetupPlayerInputComponent(Input);
		}
		~FMenuPauseWorld()
		{
			if (Player) { Player->CloseGameMenu(); }
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			GFrameCounter = SavedFrameCounter;
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}
		UQinglanMenuWidget* Menu() const
		{
			for (TObjectIterator<UQinglanMenuWidget> It; It; ++It)
			{
				if (It->GetOwningPlayer() == Controller && It->GetWorld() == World) { return *It; }
			}
			return nullptr;
		}
		void Key(FKey InKey, EInputEvent Event) const
		{
			Controller->PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(InKey, Event, Event == IE_Released ? 0.f : 1.f));
			const TArray<UInputComponent*> Stack = { Input };
			Controller->PlayerInput->ProcessInputStack(Stack, 1.f / 60.f, UGameplayStatics::IsGamePaused(World));
		}
		void Press(FKey InKey) const
		{
			Key(InKey, IE_Pressed);
			Key(InKey, IE_Released);
		}
		void Tick(float Seconds) const
		{
			for (int32 Index = 0; Index < FMath::CeilToInt(Seconds * 60.f); ++Index)
			{
				World->Tick(LEVELTICK_All, 1.f / 60.f);
				// Mirror FTestWorldWrapper: the world does not advance the
				// engine scheduler's frame, even when simulation is paused.
				++GFrameCounter;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanMenuPausesSimulation,
	"Qinglan.UI.PauseStopsDamageAndTimersWhileMenuKeysStillWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanMenuPausesSimulation::RunTest(const FString& Parameters)
{
	FMenuPauseWorld Fixture(true);
	if (!TestNotNull(TEXT("Production player and input"), Fixture.Input)) { return false; }
	UQinglanCombatComponent* Combat = Fixture.Player->GetCombat();
	if (!TestNotNull(TEXT("Real combat component"), Combat)) { return false; }
	Combat->ApplyStatus(EQinglanStatus::Burn);
	const float HealthBefore = Combat->GetHealth();
	const float BurnBefore = Combat->GetStatusRemaining(EQinglanStatus::Burn);
	bool bTimerElapsed = false;
	FTimerHandle Timer;
	Fixture.World->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateLambda([&bTimerElapsed]() { bTimerElapsed = true; }), 0.5f, false);
	Fixture.Key(EKeys::W, IE_Pressed);
	TestTrue(TEXT("Fixture starts with a held movement key"), Fixture.Controller->IsInputKeyDown(EKeys::W));
	Fixture.Player->OpenGameMenu();
	UQinglanMenuWidget* Menu = Fixture.Menu();
	if (!TestNotNull(TEXT("The actual character creates its menu"), Menu)) { return false; }
	TestTrue(TEXT("Pause menu pauses the whole world"), UGameplayStatics::IsGamePaused(Fixture.World));
	TestFalse(TEXT("Opening the menu flushes the held gameplay key"), Fixture.Controller->IsInputKeyDown(EKeys::W));
	Fixture.Tick(2.f);
	TestEqual(TEXT("Burn cannot damage the defenseless player while browsing"), Combat->GetHealth(), HealthBefore);
	TestEqual(TEXT("Status duration stays frozen"), Combat->GetStatusRemaining(EQinglanStatus::Burn), BurnBefore);
	TestFalse(TEXT("World timers stop during the menu"), bTimerElapsed);
	Fixture.Press(EKeys::Down);
	TestEqual(TEXT("Raw navigation executes while paused"), Menu->GetMenuState().SelectionIndex, 1);
	Fixture.Press(EKeys::Enter);
	TestEqual(TEXT("Paused Enter opens Settings"), Menu->GetScreen(), EQinglanMenuScreen::Settings);
	TestTrue(TEXT("Submenus keep the world paused"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Press(EKeys::BackSpace);
	TestEqual(TEXT("Paused Back returns to Pause"), Menu->GetScreen(), EQinglanMenuScreen::Pause);
	Fixture.Press(EKeys::Enter);
	TestFalse(TEXT("Paused Enter can resume gameplay"), Menu->IsMenuOpen());
	TestFalse(TEXT("Closing the final modal resumes the world"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Tick(1.1f);
	TestTrue(TEXT("World timers continue after resume"), bTimerElapsed);
	TestTrue(TEXT("Existing Burn continues after resume"), Combat->GetHealth() < HealthBefore);
	Fixture.Player->InputPause(FInputActionValue(true));
	TestTrue(TEXT("The shared Pause handler can reopen the menu"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Player->InputPause(FInputActionValue(true));
	TestFalse(TEXT("The shared Pause handler can resume it"), UGameplayStatics::IsGamePaused(Fixture.World));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanPausedFrontendSelection,
	"Qinglan.UI.PausedFrontendStillAcceptsProfileSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanPausedFrontendSelection::RunTest(const FString& Parameters)
{
	FMenuPauseWorld Fixture(false);
	if (!TestNotNull(TEXT("Production player and input"), Fixture.Input)) { return false; }
	UQinglanMenuWidget* Menu = Fixture.Menu();
	if (!TestNotNull(TEXT("Cold frontend menu"), Menu)) { return false; }
	TestTrue(TEXT("No active profile opens the real frontend"), Fixture.Player->IsProfileFrontendOpen());
	TestTrue(TEXT("Frontend pauses game time"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Press(EKeys::BackSpace);
	TestTrue(TEXT("Back cannot expose a sessionless world"), Fixture.Player->IsProfileFrontendOpen());
	Fixture.Press(EKeys::Enter);
	TestTrue(TEXT("Paused confirmation durably creates the selected slot"), Fixture.Save->HasProfile(0));
	TestTrue(TEXT("The selected profile becomes active"), Fixture.Save->IsProfileSessionActive());
	TestFalse(TEXT("Selection closes the cold frontend"), Fixture.Player->IsProfileFrontendOpen());
	TestFalse(TEXT("Cold selection releases its own pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	for (const TCHAR* Path : { TEXT("/Game/Qinglan/Input/IA_Pause"), TEXT("/Game/Qinglan/Input/IA_Dodge"),
		TEXT("/Game/Qinglan/Input/IA_Technique_1"), TEXT("/Game/Qinglan/Input/IA_Technique_2"),
		TEXT("/Game/Qinglan/Input/IA_Technique_3"), TEXT("/Game/Qinglan/Input/IA_Technique_4") })
	{
		const UInputAction* Action = LoadObject<UInputAction>(nullptr, Path);
		if (TestNotNull(TEXT("Shared modal input action exists"), Action))
		{
			TestTrue(TEXT("Escape, gamepad confirm and number-row actions execute while paused"), Action->bTriggerWhenPaused);
		}
	}
	TestTrue(TEXT("An external owner can pause the fixture"), Fixture.Controller->SetPause(true));
	Fixture.Player->OpenGameMenu();
	Fixture.Player->CloseGameMenu();
	TestTrue(TEXT("Closing a menu does not release another owner's pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Controller->SetPause(false);
	Fixture.Player->OpenGameMenu();
	FText Notice;
	TestTrue(TEXT("An active slot can be switched from its paused menu"), Fixture.Player->SelectProfileFromMenu(1, true, Notice));
	TestTrue(TEXT("The outgoing simulation stays paused until replacement"), UGameplayStatics::IsGamePaused(Fixture.World));
	const FWorldContext* Context = GEngine->GetWorldContextFromWorld(Fixture.World);
	TestTrue(TEXT("Profile replacement queues a new world while paused"), Context && Context->TravelURL.Contains(TEXT("L_Hub")));
	return true;
}
#endif
