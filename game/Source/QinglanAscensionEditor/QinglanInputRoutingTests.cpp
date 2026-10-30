// Exercise production Enhanced actions and native modal bindings together.
#include "Abilities/QinglanTechniqueTypes.h"
#include "AI/QinglanAIController.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Combat/QinglanPlayerInput.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Components/SphereComponent.h"
#include "Core/QinglanTrialDirector.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Platform/QinglanGraphicsSettingsService.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags InputRoutingFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const FKey DPadKeys[] = { EKeys::Gamepad_DPad_Up, EKeys::Gamepad_DPad_Right,
		EKeys::Gamepad_DPad_Down, EKeys::Gamepad_DPad_Left };

	struct FInputRoutingWorld
	{
		FString Name;
		FString Directory;
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		UQinglanSaveSubsystem* Save = nullptr;
		ULocalPlayer* LocalPlayer = nullptr;
		APlayerController* Controller = nullptr;
		AQinglanCharacter* Player = nullptr;
		UQinglanPlayerInput* PlayerInput = nullptr;
		UEnhancedInputComponent* Input = nullptr;
		UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
		UInputMappingContext* Context = nullptr;
		bool bReady = false;

		explicit FInputRoutingWorld(bool bCreateProfile, bool bArena = false)
		{
			Name = FString(bArena ? TEXT("L_TribulationArena_Input_") : TEXT("L_InputRouting_"))
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
			if (!GEngine) { return; }
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
			if (!World || !Save) { return; }
			FText Error;
			if (bCreateProfile && !Save->TryCreateNewProfile(0, Error)) { return; }
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
			// ULocalPlayer is Within=Engine; its game-instance association is
			// established by AddLocalPlayer, rather than by its UObject outer.
			LocalPlayer = NewObject<ULocalPlayer>(GEngine);
			Instance->AddLocalPlayer(LocalPlayer, FPlatformUserId::CreateFromInternalId(0));
			Controller->SetPlayer(LocalPlayer);
			PlayerInput = NewObject<UQinglanPlayerInput>(Controller);
			Controller->PlayerInput = PlayerInput;
			Controller->Possess(Player);
			// Standalone automation worlds do not guarantee actor BeginPlay from
			// SpawnActor. Complete the same lifecycle used by the existing actual
			// story-choice input fixture before exercising delegate-driven menus.
			Controller->DispatchBeginPlay();
			Player->DispatchBeginPlay();
			Player->GetCharacterMovement()->DisableMovement();
			Input = NewObject<UEnhancedInputComponent>(Player);
			Input->RegisterComponent();
			Player->SetupPlayerInputComponent(Input);
			Context = LoadObject<UInputMappingContext>(nullptr, TEXT("/Game/Qinglan/Input/IMC_Default"));
			Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
			if (!Context || !Subsystem || Subsystem->GetPlayerInput() != PlayerInput) { return; }
			FModifyContextOptions Options;
			Options.bForceImmediately = true;
			Subsystem->AddMappingContext(Context, 0, Options);
			bReady = Controller->HasActorBegunPlay() && Player->HasActorBegunPlay();
			Process();
		}

		~FInputRoutingWorld()
		{
			UQinglanTechniqueComponent::SetTechniqueCatalogForTesting({});
			if (Player) { Player->CloseGameMenu(); }
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
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

		// Queue events separately from processing to model subframe taps and
		// physical release versus repeats after the engine's modal flush.
		void Key(FKey InKey, EInputEvent Event) const
		{
			PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(InKey, Event, Event == IE_Released ? 0.f : 1.f));
		}
		void Process() const
		{
			const TArray<UInputComponent*> Stack = { Input };
			PlayerInput->ProcessInputStack(Stack, 1.f / 60.f, UGameplayStatics::IsGamePaused(World));
		}
		void Tap(FKey InKey, bool bSameFrame = false) const
		{
			Key(InKey, IE_Pressed);
			if (!bSameFrame) { Process(); }
			Key(InKey, IE_Released);
			Process();
			// Complete one idle action evaluation after any queued flush release.
			Process();
		}
		FString TravelURL() const
		{
			const FWorldContext* WorldContext = GEngine->GetWorldContextFromWorld(World);
			return WorldContext ? WorldContext->TravelURL : FString();
		}
		APawn* Target(const FVector& Location) const
		{
			APawn* Pawn = World->SpawnActor<APawn>();
			if (!Pawn) { return nullptr; }
			USphereComponent* Sphere = NewObject<USphereComponent>(Pawn);
			Pawn->AddOwnedComponent(Sphere);
			Pawn->SetRootComponent(Sphere);
			Sphere->InitSphereRadius(20.f);
			Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Sphere->SetCollisionObjectType(ECC_Pawn);
			Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
			Sphere->RegisterComponent();
			Pawn->SetActorLocation(Location);
			UQinglanCombatComponent* Combat = NewObject<UQinglanCombatComponent>(Pawn);
			Pawn->AddOwnedComponent(Combat);
			Combat->RegisterComponent();
			Combat->SetDerivedAttributes(FQinglanAttributes());
			return Pawn;
		}
	};

	void CheckDPadNavigation(FAutomationTestBase& Test, const FInputRoutingWorld& Fixture, EQinglanMenuScreen Screen)
	{
		UQinglanMenuWidget* Menu = Fixture.Menu();
		if (!Test.TestNotNull(TEXT("Production modal menu"), Menu)) { return; }
		for (const FKey& Key : DPadKeys)
		{
			const int32 Before = Menu->GetMenuState().SelectionIndex;
			const int32 Count = Menu->GetMenuState().OptionCount;
			if (!Test.TestTrue(TEXT("Modal has selectable rows"), Count > 0)) { return; }
			const int32 Direction = Key == EKeys::Gamepad_DPad_Up ? -1 : (Key == EKeys::Gamepad_DPad_Down ? 1 : 0);
			Fixture.Tap(Key);
			const FString Prefix = Key.ToString() + TEXT(": ");
			Test.TestEqual(Prefix + TEXT("only changes the selected row"), Menu->GetMenuState().SelectionIndex, (Before + Direction + Count) % Count);
			Test.TestEqual(Prefix + TEXT("never activates a numeric shortcut"), Menu->GetScreen(), Screen);
			Test.TestTrue(Prefix + TEXT("keeps simulation paused"), UGameplayStatics::IsGamePaused(Fixture.World));
			Test.TestTrue(Prefix + TEXT("queues no travel"), Fixture.TravelURL().IsEmpty());
			Test.TestTrue(Prefix + TEXT("is not captured as a number key"), Menu->GetMenuState().CapturedNumberKeys.IsEmpty());
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDPadModalRouting,
	"Qinglan.Input.DPadNavigatesPausedMenusWithoutSelecting", InputRoutingFlags)
bool FQinglanDPadModalRouting::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(false);
	if (!TestTrue(TEXT("Real local-player Enhanced stack is ready"), Fixture.bReady)) { return false; }
	CheckDPadNavigation(*this, Fixture, EQinglanMenuScreen::Profile);
	for (int32 Slot = 0; Slot < UQinglanSaveSubsystem::ProfileSlotCount; ++Slot)
	{
		TestFalse(TEXT("D-pad cannot create or load a profile slot"), Fixture.Save->HasProfile(Slot));
	}
	TestFalse(TEXT("Navigation cannot start a profile session"), Fixture.Save->IsProfileSessionActive());
	Fixture.Tap(EKeys::Gamepad_FaceButton_Bottom, true);
	if (!TestTrue(TEXT("A creates the highlighted profile"), Fixture.Save->HasProfile(0))) { return false; }
	TestFalse(TEXT("Cold-profile confirmation releases pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Tap(EKeys::Escape);
	CheckDPadNavigation(*this, Fixture, EQinglanMenuScreen::Pause);
	TestFalse(TEXT("Pause navigation cannot create a second profile"), Fixture.Save->HasProfile(1));
	TestFalse(TEXT("Pause navigation cannot create a third profile"), Fixture.Save->HasProfile(2));
	Fixture.Tap(EKeys::Escape);

	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	UQinglanProfileSubsystem* Profile = Fixture.Instance->GetSubsystem<UQinglanProfileSubsystem>();
	if (!TestNotNull(TEXT("Run authority"), Run) || !TestNotNull(TEXT("Profile authority"), Profile)) { return false; }
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = TEXT("bamboo_hollow");
	Checkpoint.RealmSeed = 73;
	Checkpoint.RoomIndex = 2;
	Checkpoint.PendingBlessingOffer = { TEXT("b_quiet_current"), TEXT("b_jade_shell"), TEXT("b_thunder_mandate") };
	Checkpoint.PendingBlessingRoom = 2;
	Checkpoint.CommittedRoomIds = { 0, 1, 2 };
	Checkpoint.Loot.SpiritShards = 70;
	Run->ImportCheckpoint(Checkpoint);
	Run->MarkCheckpointResumed();
	TArray<FQinglanBlessing> Offers;
	if (!TestTrue(TEXT("Production pending blessing draft"), Run->GetBlessingOffer(Offers))) { return false; }
	Fixture.Player->PresentBlessingOffer(Offers);
	const FQinglanInventory InventoryBefore = Profile->GetInventory();
	CheckDPadNavigation(*this, Fixture, EQinglanMenuScreen::Offer);
	FQinglanCheckpoint AfterNavigation;
	Run->ExportCheckpoint(AfterNavigation);
	TestTrue(TEXT("D-pad preserves the pending draft"), AfterNavigation.PendingBlessingOffer == Checkpoint.PendingBlessingOffer);
	TestTrue(TEXT("D-pad grants no blessing"), AfterNavigation.AcquiredBlessings.IsEmpty());
	TestEqual(TEXT("D-pad preserves unbanked loot"), AfterNavigation.Loot.SpiritShards, Checkpoint.Loot.SpiritShards);
	TestEqual(TEXT("D-pad spends no currency"), Profile->GetInventory().SpiritShards, InventoryBefore.SpiritShards);
	TestEqual(TEXT("D-pad spends no herbs"), Profile->GetInventory().SpiritHerb, InventoryBefore.SpiritHerb);
	TestEqual(TEXT("D-pad spends no crystals"), Profile->GetInventory().VeinCrystal, InventoryBefore.VeinCrystal);
	Fixture.Tap(EKeys::Gamepad_FaceButton_Bottom, true);
	FQinglanCheckpoint AfterAccept;
	Run->ExportCheckpoint(AfterAccept);
	TestEqual(TEXT("A accepts exactly one blessing"), AfterAccept.AcquiredBlessings.Num(), 1);
	TestTrue(TEXT("A accepts the highlighted blessing"), AfterAccept.AcquiredBlessings.Contains(Offers[0].BlessingId));
	TestTrue(TEXT("Accepted draft is consumed"), AfterAccept.PendingBlessingOffer.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGamepadBContextRouting,
	"Qinglan.Input.GamepadBCyclesTargetsWithoutAbandoningTrial", InputRoutingFlags)
bool FQinglanGamepadBContextRouting::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true, true);
	if (!TestTrue(TEXT("Real arena input stack is ready"), Fixture.bReady)) { return false; }
	AQinglanTrialDirector* Director = Fixture.World->SpawnActor<AQinglanTrialDirector>();
	if (!TestNotNull(TEXT("Production trial director"), Director)) { return false; }
	// Admission is setup-only here; the live trial, wave engine and cancellation
	// behavior are production paths, independent of campaign progression.
	if (!TestTrue(TEXT("An actual trial is active"), Director->BeginTrial(TEXT("CH03_S02"), TEXT("CH03_S02_OBJECTIVE"), 90.f, 1, true))) { return false; }
	if (!TestNotNull(TEXT("First living lock candidate"), Fixture.Target(FVector(300.f, 100.f, 0.f)))
		|| !TestNotNull(TEXT("Second living lock candidate"), Fixture.Target(FVector(600.f, -100.f, 0.f)))) { return false; }
	Fixture.Tap(EKeys::Gamepad_LeftShoulder);
	AActor* OriginalTarget = Fixture.Player->GetLockTarget().Get();
	if (!TestNotNull(TEXT("Mapped shoulder acquires a target"), OriginalTarget)) { return false; }
	const float Remaining = Director->GetRemainingSeconds();
	const EQinglanTrialPhase Phase = Director->GetTrialPhase();
	Fixture.Tap(EKeys::Gamepad_FaceButton_Right);
	AActor* CycledTarget = Fixture.Player->GetLockTarget().Get();
	TestTrue(TEXT("Mapped gameplay B cycles the target"), CycledTarget && CycledTarget != OriginalTarget);
	TestTrue(TEXT("Gameplay B cannot abandon the trial"), Director->IsTrialActive());
	TestEqual(TEXT("Gameplay B preserves trial phase"), Director->GetTrialPhase(), Phase);
	TestEqual(TEXT("Input alone preserves the trial clock"), Director->GetRemainingSeconds(), Remaining);
	TestTrue(TEXT("Gameplay B queues no hub travel"), Fixture.TravelURL().IsEmpty());
	Fixture.Tap(EKeys::Escape);
	Fixture.Tap(EKeys::Down);
	Fixture.Tap(EKeys::Enter);
	UQinglanMenuWidget* Menu = Fixture.Menu();
	if (!TestNotNull(TEXT("Actual arena menu"), Menu)) { return false; }
	TestEqual(TEXT("Settings submenu opened through native keys"), Menu->GetScreen(), EQinglanMenuScreen::Settings);
	Fixture.Tap(EKeys::Gamepad_FaceButton_Right);
	TestEqual(TEXT("Modal B performs one Back to Pause"), Menu->GetScreen(), EQinglanMenuScreen::Pause);
	TestTrue(TEXT("Submenu Back keeps the world paused"), UGameplayStatics::IsGamePaused(Fixture.World));
	TestTrue(TEXT("Modal B cannot cycle the target behind the menu"), Fixture.Player->GetLockTarget().Get() == CycledTarget);
	Fixture.Tap(EKeys::Gamepad_FaceButton_Right);
	TestFalse(TEXT("A second B closes Pause"), Menu->IsMenuOpen());
	TestFalse(TEXT("Closing Pause resumes the world"), UGameplayStatics::IsGamePaused(Fixture.World));
	TestTrue(TEXT("Back from Pause keeps the trial active"), Director->IsTrialActive());
	TestEqual(TEXT("Modal Back preserves trial phase"), Director->GetTrialPhase(), Phase);
	TestTrue(TEXT("Modal Back queues no hub travel"), Fixture.TravelURL().IsEmpty());
	TestTrue(TEXT("Closing Pause does not dispatch gameplay B"), Fixture.Player->GetLockTarget().Get() == CycledTarget);
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production story authority"), Story)) { return false; }
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("interact.bell_pulled"), {});
	if (!TestTrue(TEXT("Opening exchange starts"), Story->BeginScene(TEXT("CH00_S01")))) { return false; }
	// Playback waits for a safe boundary while the ordinary wave is active.
	// Clear that real wave through portal materialization and combat damage,
	// leaving the trial clock active during its between-wave breather.
	TestFalse(TEXT("An active wave defers scene playback"), Story->IsScenePlaying());
	AQinglanEncounterDirector* WaveEngine = nullptr;
	for (TActorIterator<AQinglanEncounterDirector> It(Fixture.World); It; ++It)
	{
		if (It->IsEncounterActive()) { WaveEngine = *It; break; }
	}
	if (!TestNotNull(TEXT("The live trial has an ordinary wave engine"), WaveEngine)) { return false; }
	int32 DefeatedEnemies = 0;
	for (int32 Step = 0; Step < 20 && WaveEngine->IsEncounterActive(); ++Step)
	{
		WaveEngine->Tick(0.5f);
		for (TActorIterator<AQinglanEnemyCharacter> It(Fixture.World); It; ++It)
		{
			AQinglanEnemyCharacter* Enemy = *It;
			AQinglanAIController* EnemyController = Cast<AQinglanAIController>(Enemy->GetController());
			if (!EnemyController || !Enemy->IsAlive()) { continue; }
			EnemyController->Tick(0.81f);
			if (EnemyController->IsPortalPending()) { continue; }
			FQinglanDamageEvent Lethal;
			Lethal.Attacker = Fixture.Player;
			Lethal.Target = Enemy;
			Lethal.CastId = UQinglanCombatComponent::NextCastId();
			Lethal.RawMagnitude = 100000.f;
			if (!TestTrue(TEXT("An ordinary trial enemy takes the killing blow"), Enemy->EnemyCombat->ApplyDamage(Lethal))) { return false; }
			++DefeatedEnemies;
		}
	}
	if (!TestTrue(TEXT("The actual wave was defeated"), DefeatedEnemies > 0)
		|| !TestFalse(TEXT("Wave completion opens the safe dialogue boundary"), WaveEngine->IsEncounterActive())
		|| !TestTrue(TEXT("Wave completion resumes the deferred scene"), Story->IsScenePlaying())
		|| !TestTrue(TEXT("The trial remains active between waves"), Director->IsTrialActive())) { return false; }
	Story->SkipScene();
	FQinglanActionGateState Gate;
	Fixture.Player->GetActionGateState(Gate);
	if (!TestTrue(TEXT("A mandatory scene choice owns input"), Gate.bSceneChoicePending)) { return false; }
	Fixture.Tap(EKeys::Gamepad_FaceButton_Right);
	Fixture.Player->GetActionGateState(Gate);
	TestTrue(TEXT("B cannot dismiss a mandatory choice"), Gate.bSceneChoicePending);
	TestTrue(TEXT("Choice Back cannot fall through to arena retreat"), Director->IsTrialActive());
	TestTrue(TEXT("Choice Back cannot cycle behind the modal"), Fixture.Player->GetLockTarget().Get() == CycledTarget);
	TestTrue(TEXT("Choice Back queues no hub travel"), Fixture.TravelURL().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGamepadASubframeRouting,
	"Qinglan.Input.SubframeGamepadAConfirmsExactlyOnce", InputRoutingFlags)
bool FQinglanGamepadASubframeRouting::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true);
	if (!TestTrue(TEXT("Real local-player Enhanced stack is ready"), Fixture.bReady)) { return false; }
	Fixture.Tap(EKeys::Escape);
	Fixture.Tap(EKeys::Down);
	UQinglanMenuWidget* Menu = Fixture.Menu();
	if (!TestNotNull(TEXT("Paused menu"), Menu)) { return false; }
	const float Stamina = Fixture.Player->GetCombat()->GetAttributes().Stamina;
	Fixture.Tap(EKeys::SpaceBar, true);
	TestEqual(TEXT("Space cannot confirm the selected Settings row"), Menu->GetScreen(), EQinglanMenuScreen::Pause);
	TestEqual(TEXT("Modal Space cannot spend dodge stamina"), Fixture.Player->GetCombat()->GetAttributes().Stamina, Stamina);

	// This service is not DLL-exported. Reflection invokes its real read API
	// without introducing an export or replacing production settings state.
	UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanGraphicsSettingsService"));
	UObject* Settings = SettingsClass ? Fixture.Instance->GetSubsystemBase(SettingsClass) : nullptr;
	UFunction* GetSettings = Settings ? Settings->FindFunction(TEXT("GetCurrentSettings")) : nullptr;
	UFunction* ApplySettings = Settings ? Settings->FindFunction(TEXT("ApplySettings")) : nullptr;
	if (!TestNotNull(TEXT("Production settings read function"), GetSettings)
		|| !TestNotNull(TEXT("Production settings restore function"), ApplySettings)) { return false; }
	struct FSettingsReturn { FQinglanGraphicsSettings ReturnValue; } Before, After;
	Settings->ProcessEvent(GetSettings, &Before);
	ON_SCOPE_EXIT
	{
		if (After.ReturnValue.bLowFlashMode != Before.ReturnValue.bLowFlashMode)
		{
			struct FSettingsArgument { FQinglanGraphicsSettings NewSettings; } Restore = { Before.ReturnValue };
			Settings->ProcessEvent(ApplySettings, &Restore);
		}
	};
	Fixture.Tap(EKeys::Gamepad_FaceButton_Bottom, true);
	TestEqual(TEXT("A press and release in one frame opens Settings"), Menu->GetScreen(), EQinglanMenuScreen::Settings);
	Settings->ProcessEvent(GetSettings, &After);
	TestEqual(TEXT("One A does not also toggle Settings' first row"), After.ReturnValue.bLowFlashMode, Before.ReturnValue.bLowFlashMode);
	TestEqual(TEXT("A confirmation cannot spend dodge stamina"), Fixture.Player->GetCombat()->GetAttributes().Stamina, Stamina);
	Fixture.Tap(EKeys::Gamepad_FaceButton_Right);
	Fixture.Controller->FlushPressedKeys();
	Fixture.Process();
	Fixture.Tap(EKeys::Gamepad_FaceButton_Bottom, true);
	TestFalse(TEXT("A same-frame tap still confirms Resume after a flush"), Menu->IsMenuOpen());
	TestFalse(TEXT("The confirmed Resume releases pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Tap(EKeys::Escape);
	Fixture.Tap(EKeys::Two);
	TestEqual(TEXT("Physical number-row shortcut still opens Settings"), Menu->GetScreen(), EQinglanMenuScreen::Settings);
	Settings->ProcessEvent(GetSettings, &After);
	TestEqual(TEXT("Opening Settings leaves its first setting unchanged"), After.ReturnValue.bLowFlashMode, Before.ReturnValue.bLowFlashMode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGameplayDodgeInputRouting,
	"Qinglan.Input.GamepadAndKeyboardDodgeRetainGameplay", InputRoutingFlags)
bool FQinglanGameplayDodgeInputRouting::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true);
	if (!TestTrue(TEXT("Production input stack"), Fixture.bReady)) { return false; }
	UQinglanDodgeComponent* Dodge = Fixture.Player->FindComponentByClass<UQinglanDodgeComponent>();
	if (!TestNotNull(TEXT("Production dodge component"), Dodge)) { return false; }
	Fixture.Player->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	float FirstCost = 0.f;
	for (const FKey& Key : { EKeys::Gamepad_FaceButton_Bottom, EKeys::SpaceBar })
	{
		const float Before = Fixture.Player->GetCombat()->GetAttributes().Stamina;
		Fixture.Tap(Key, true);
		TestTrue(Key.ToString() + TEXT(" starts an actual gameplay dodge"), Dodge->IsDodging());
		const float Cost = Before - Fixture.Player->GetCombat()->GetAttributes().Stamina;
		TestTrue(TEXT("Gameplay dodge spends stamina"), Cost > 0.f);
		if (FirstCost == 0.f) { FirstCost = Cost; }
		else { TestEqual(TEXT("A and Space each spend one identical dodge cost"), Cost, FirstCost); }
		Dodge->CancelDodge();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGameplayDPadTechniqueRouting,
	"Qinglan.Input.DPadRetainsAllFourGameplayTechniqueSlots", InputRoutingFlags)
bool FQinglanGameplayDPadTechniqueRouting::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true);
	if (!TestTrue(TEXT("Production input stack"), Fixture.bReady)) { return false; }
	UClass* TechniqueClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanTechniqueDataAsset"));
	UQinglanTechniqueComponent* Techniques = Fixture.Player->GetTechniques();
	if (!TestNotNull(TEXT("Technique data class"), TechniqueClass)
		|| !TestNotNull(TEXT("Production technique component"), Techniques)) { return false; }
	TArray<UQinglanTechniqueDataAsset*> Assets;
	TArray<FName> Ids;
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		UQinglanTechniqueDataAsset* Asset = static_cast<UQinglanTechniqueDataAsset*>(NewObject<UObject>(GetTransientPackage(), TechniqueClass));
		Asset->TechniqueId = FName(*FString::Printf(TEXT("input_routing_slot_%d"), Slot));
		Asset->Targeting = EQinglanTargeting::SelfPulses;
		Asset->Phases.Anticipation = 1.f;
		Assets.Add(Asset);
		Ids.Add(Asset->TechniqueId);
	}
	UQinglanTechniqueComponent::SetTechniqueCatalogForTesting(Assets);
	if (!TestTrue(TEXT("Four-slot runtime loadout"), Techniques->ApplyEquippedTechniqueIds(Ids))) { return false; }
	// Only the slot under test is affordable, so a wrong D-pad-to-slot
	// mapping cannot pass by starting some other valid cast.
	for (int32 Slot = 0; Slot < 4; ++Slot)
	{
		for (int32 Index = 0; Index < Assets.Num(); ++Index) { Assets[Index]->EnergyCost = Index == Slot ? 0 : 10000; }
		Fixture.Tap(DPadKeys[Slot]);
		TestTrue(DPadKeys[Slot].ToString() + TEXT(" casts its original gameplay slot"), Techniques->IsInAnticipation());
		TestFalse(TEXT("Gameplay D-pad never opens a menu"), Fixture.Player->IsMenuInputActive());
		Techniques->InterruptCast();
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHeldKeysAfterModalFlush,
	"Qinglan.Input.HeldKeysRequireReleaseAfterModalFlush", InputRoutingFlags)
bool FQinglanHeldKeysAfterModalFlush::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true);
	if (!TestTrue(TEXT("Production local-player input stack"), Fixture.bReady)) { return false; }
	TestTrue(TEXT("The runtime config selects the release-aware input class"),
		UInputSettings::GetDefaultPlayerInputClass() == UQinglanPlayerInput::StaticClass());
	UInputAction* Attack = LoadObject<UInputAction>(nullptr, TEXT("/Game/Qinglan/Input/IA_Attack"));
	UQinglanSwordComponent* Sword = Fixture.Player->FindComponentByClass<UQinglanSwordComponent>();
	if (!TestNotNull(TEXT("Mapped attack action"), Attack) || !TestNotNull(TEXT("Production sword"), Sword)) { return false; }
	int32 AttackStarts = 0;
	Fixture.Input->BindActionValueLambda(Attack, ETriggerEvent::Started,
		[&AttackStarts](const FInputActionValue&) { ++AttackStarts; });
	Fixture.Key(EKeys::J, IE_Pressed);
	Fixture.Process();
	TestEqual(TEXT("The first physical attack starts once"), AttackStarts, 1);
	TestTrue(TEXT("The mapped press starts the actual sword"), Sword->IsSwinging());
	Fixture.Key(EKeys::Escape, IE_Pressed);
	Fixture.Process();
	UQinglanMenuWidget* Menu = Fixture.Menu();
	if (!TestNotNull(TEXT("Escape opens the actual modal"), Menu)) { return false; }
	TestTrue(TEXT("Escape pauses the world"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Process(); // consume the engine's synthetic flush releases
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Fixture.Controller->FlushPressedKeys();
		Fixture.Process();
		Fixture.Key(EKeys::Escape, IE_Repeat);
		Fixture.Process();
		TestTrue(TEXT("Held Escape cannot close Pause after repeated flushes"), Menu->IsMenuOpen());
		TestTrue(TEXT("Held Escape retains world pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	}
	// Release and press again in the same frame, leaving the new press held.
	Fixture.Key(EKeys::Escape, IE_Released);
	Fixture.Key(EKeys::Escape, IE_Pressed);
	Fixture.Process();
	TestFalse(TEXT("A real release and fresh press resumes gameplay"), Menu->IsMenuOpen());
	TestFalse(TEXT("The fresh Escape releases pause"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Process();
	Fixture.Key(EKeys::Escape, IE_Repeat);
	Fixture.Key(EKeys::J, IE_Repeat);
	Fixture.Process();
	TestFalse(TEXT("The newly held Escape cannot reopen Pause"), Menu->IsMenuOpen());
	TestEqual(TEXT("An attack held through pause and resume cannot restart"), AttackStarts, 1);
	Fixture.Key(EKeys::J, IE_Released);
	Fixture.Key(EKeys::J, IE_Pressed);
	Fixture.Process();
	TestEqual(TEXT("A real attack release and press is accepted once"), AttackStarts, 2);
	Fixture.Key(EKeys::J, IE_Released);
	Fixture.Key(EKeys::Escape, IE_Released);
	Fixture.Process();
	Fixture.Tap(EKeys::Escape, true);
	TestTrue(TEXT("A complete Escape tap can open Pause again"), Menu->IsMenuOpen());
	Fixture.Tap(EKeys::Gamepad_FaceButton_Bottom, true);
	TestFalse(TEXT("A complete A tap can resume after held-key suppression"), Menu->IsMenuOpen());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanPendingInputFlush,
	"Qinglan.Input.FlushDropsPendingHoldsAndPreservesTapsAndAxes", InputRoutingFlags)
bool FQinglanPendingInputFlush::RunTest(const FString& Parameters)
{
	FInputRoutingWorld Fixture(true);
	if (!TestTrue(TEXT("Production local-player input stack"), Fixture.bReady)) { return false; }
	UInputAction* Attack = LoadObject<UInputAction>(nullptr, TEXT("/Game/Qinglan/Input/IA_Attack"));
	UInputAction* Move = LoadObject<UInputAction>(nullptr, TEXT("/Game/Qinglan/Input/IA_Move"));
	UInputAction* Look = LoadObject<UInputAction>(nullptr, TEXT("/Game/Qinglan/Input/IA_Look"));
	UQinglanSwordComponent* Sword = Fixture.Player->FindComponentByClass<UQinglanSwordComponent>();
	if (!TestNotNull(TEXT("Mapped attack"), Attack) || !TestNotNull(TEXT("Mapped movement"), Move)
		|| !TestNotNull(TEXT("Mapped look"), Look) || !TestNotNull(TEXT("Production sword"), Sword)) { return false; }
	int32 AttackStarts = 0;
	Fixture.Input->BindActionValueLambda(Attack, ETriggerEvent::Started,
		[&AttackStarts](const FInputActionValue&) { ++AttackStarts; });
	Fixture.Key(EKeys::J, IE_Pressed); // not evaluated yet
	Fixture.Controller->FlushPressedKeys();
	Fixture.Process();
	TestEqual(TEXT("Flush removes a pending held attack before evaluation"), AttackStarts, 0);
	TestFalse(TEXT("The discarded press cannot start the actual sword"), Sword->IsSwinging());
	Fixture.Key(EKeys::J, IE_Repeat);
	Fixture.Process();
	TestEqual(TEXT("The pending hold stays suppressed until release"), AttackStarts, 0);
	Fixture.Key(EKeys::J, IE_Released);
	Fixture.Key(EKeys::J, IE_Pressed);
	Fixture.Process();
	TestEqual(TEXT("A fresh post-flush attack is accepted"), AttackStarts, 1);
	TestTrue(TEXT("The fresh mapped attack reaches the sword"), Sword->IsSwinging());
	Fixture.Key(EKeys::J, IE_Released);
	Fixture.Process();
	// Completed taps are not physically held and must survive a pre-evaluation flush.
	Fixture.Key(EKeys::Escape, IE_Pressed);
	Fixture.Key(EKeys::Escape, IE_Released);
	Fixture.Controller->FlushPressedKeys();
	Fixture.Process();
	TestTrue(TEXT("Flush preserves a completed Escape tap"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Process();
	Fixture.Key(EKeys::Gamepad_FaceButton_Bottom, IE_Pressed);
	Fixture.Key(EKeys::Gamepad_FaceButton_Bottom, IE_Released);
	Fixture.Controller->FlushPressedKeys();
	Fixture.Process();
	TestFalse(TEXT("Flush preserves completed A confirmation"), UGameplayStatics::IsGamePaused(Fixture.World));
	Fixture.Process();
	const auto Axes = [&Fixture]()
	{
		for (const FKey& Key : { EKeys::Gamepad_LeftX, EKeys::Gamepad_LeftY,
			EKeys::Gamepad_RightX, EKeys::Gamepad_RightY })
		{
			Fixture.PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Axis, 0.65f, 1));
		}
		Fixture.Process();
	};
	Axes();
	const FVector2D MoveBefore = Fixture.PlayerInput->GetActionValue(Move).Get<FVector2D>();
	const FVector2D LookBefore = Fixture.PlayerInput->GetActionValue(Look).Get<FVector2D>();
	TestTrue(TEXT("Real analog samples drive the movement mapping"), MoveBefore.SizeSquared() > 0.1f);
	TestTrue(TEXT("Real analog samples drive the camera mapping"), LookBefore.SizeSquared() > 0.1f);
	Fixture.Controller->FlushPressedKeys();
	Axes();
	TestTrue(TEXT("Analog movement accepts fresh samples without returning to zero"),
		Fixture.PlayerInput->GetActionValue(Move).Get<FVector2D>().Equals(MoveBefore, 0.001f));
	TestTrue(TEXT("Analog look accepts fresh samples without returning to zero"),
		Fixture.PlayerInput->GetActionValue(Look).Get<FVector2D>().Equals(LookBefore, 0.001f));
	return true;
}
#endif
