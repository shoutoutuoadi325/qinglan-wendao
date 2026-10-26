// Regression for reaching the departure gate before finishing the sanctuary.
#include "Components/StaticMeshComponent.h"
#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanInteractionSpawner.h"
#include "Core/QinglanRoomLayout.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FObjectiveGateWorld
	{
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		explicit FObjectiveGateWorld(const TCHAR* MapPrefix)
		{
			if (!GEngine) { return; }
			const FString Name = FString(MapPrefix) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
		}
		~FObjectiveGateWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDepartureRequiresPrerequisites,
	"Qinglan.Narrative.DepartureCannotSkipSanctuaryObjectives",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanDepartureRequiresPrerequisites::RunTest(const FString& Parameters)
{
	FObjectiveGateWorld Fixture(TEXT("L_Hub_DepartureTest_"));
	if (!TestNotNull(TEXT("Real owning game world"), Fixture.World)) { return false; }
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production narrative subsystem"), Story)) { return false; }
	FQinglanNarrativeState Fresh;
	QinglanStoryRuntime::ImportNarrativeState(Story, Fresh);
	// Exercise the dispatcher itself: even a matching event must not commit
	// a scene whose authored prerequisites have not been reached.
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("interact.departure_confirmed"), {});
	TestFalse(TEXT("Matching proof cannot complete a locked departure scene"), Story->IsSceneCompleted(TEXT("CH00_S04")));
	TestFalse(TEXT("Matching proof cannot unlock the guided expedition"), Story->HasFlag(TEXT("mq_ch00_complete")));
	QinglanStoryRuntime::ImportNarrativeState(Story, Fresh);

	AQinglanInteractable* Gate = Fixture.World->SpawnActor<AQinglanInteractable>();
	if (!TestNotNull(TEXT("Real gate dispatcher actor"), Gate)) { return false; }
	Gate->InteractionId = TEXT("hub.bamboo_gate");
	Gate->ExecuteInteraction(nullptr);
	FQinglanNarrativeState AfterEarlyPress;
	QinglanStoryRuntime::ExportNarrativeState(Story, AfterEarlyPress);
	TestFalse(TEXT("An early physical gate press does not record reusable departure proof"),
		AfterEarlyPress.FiredEventLog.Contains(TEXT("interact.departure_confirmed")));
	TestFalse(TEXT("Early physical gate press leaves the chapter unfinished"), Story->HasFlag(TEXT("mq_ch00_complete")));

	FQinglanNarrativeState Ready = AfterEarlyPress;
	Ready.CompletedScenes = { TEXT("CH00_S01"), TEXT("CH00_S02"), TEXT("CH00_S03") };
	Ready.Flags = { TEXT("mq_ch00_bell_examined"), TEXT("mq_ch00_training_complete"), TEXT("mq_ch00_route_read") };
	QinglanStoryRuntime::ImportNarrativeState(Story, Ready);
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("combat.dodge"), {});
	TestFalse(TEXT("Unlocking the route and doing something else cannot reuse an early gate attempt"),
		Story->IsSceneCompleted(TEXT("CH00_S04")));
	Gate->ExecuteInteraction(nullptr);
	TestTrue(TEXT("Returning to the gate after the route is read completes departure"), Story->IsSceneCompleted(TEXT("CH00_S04")));
	TestTrue(TEXT("Valid departure unlocks the guided route"), Story->HasFlag(TEXT("mq_ch00_complete")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRealmDoorVisibility,
	"Qinglan.Content.RealmDoorsHaveVisibleInteractionFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanRealmDoorVisibility::RunTest(const FString& Parameters)
{
	FObjectiveGateWorld Fixture(TEXT("L_BambooHollow_DoorTest_"));
	if (!TestNotNull(TEXT("Real realm game world"), Fixture.World)) { return false; }
	UQinglanInteractionSpawner* Spawner = Fixture.World->GetSubsystem<UQinglanInteractionSpawner>();
	if (!TestNotNull(TEXT("Production interaction spawner"), Spawner)) { return false; }
	Spawner->OnWorldBeginPlay(*Fixture.World);
	int32 Doors = 0;
	for (TActorIterator<AQinglanInteractable> It(Fixture.World); It; ++It)
	{
		if (!It->InteractionId.ToString().StartsWith(TEXT("door."))) { continue; }
		++Doors;
		TestEqual(TEXT("Door prompt uses an existing localized action"), It->DisplayNameKey, FName(TEXT("action.continue")));
		TArray<UStaticMeshComponent*> Frames;
		It->GetComponents<UStaticMeshComponent>(Frames);
		TestEqual(TEXT("Every room door has two visible posts and a header"), Frames.Num(), 3);
		for (UStaticMeshComponent* Frame : Frames)
		{
			TestTrue(TEXT("Door frame has a visible registered mesh"), Frame && Frame->GetStaticMesh() && Frame->IsVisible() && Frame->IsRegistered());
			if (Frame) { TestEqual(TEXT("Marker cannot block the interaction or corridor"), Frame->GetCollisionEnabled(), ECollisionEnabled::NoCollision); }
		}
	}
	TestEqual(TEXT("All six full-realm transitions are marked"), Doors, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanWalkingThroughRealmDoor,
	"Qinglan.Gameplay.WalkingAcrossDoorStartsNextRoomAndHonorsCombatLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanWalkingThroughRealmDoor::RunTest(const FString& Parameters)
{
	const FString SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"),
		TEXT("_DoorTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
	ON_SCOPE_EXIT
	{
		UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
		IFileManager::Get().DeleteDirectory(*SaveDirectory, false, true);
	};
	FObjectiveGateWorld Fixture(TEXT("L_BambooHollow_WalkingDoorTest_"));
	if (!TestNotNull(TEXT("Real realm world"), Fixture.World)) { return false; }
	UQinglanSaveSubsystem* Save = Fixture.Instance->GetSubsystem<UQinglanSaveSubsystem>();
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	if (!TestNotNull(TEXT("Save subsystem"), Save) || !TestNotNull(TEXT("Run subsystem"), Run)) { return false; }
	Save->CreateNewProfile(0);
	if (!TestTrue(TEXT("Isolated real profile session starts"), Save->IsProfileSessionActive())) { return false; }
	if (!TestTrue(TEXT("Real full expedition starts"), Run->StartExpedition(FName(UQinglanEconomyLibrary::BambooRealmIdText), 417))) { return false; }
	Fixture.World->InitializeActorsForPlay(FURL());
	Fixture.World->BeginPlay(); // spawns the production corridor doors
	AQinglanInteractable* FirstDoor = nullptr;
	AQinglanInteractable* SecondDoor = nullptr;
	for (TActorIterator<AQinglanInteractable> It(Fixture.World); It; ++It)
	{
		if (It->InteractionId == TEXT("door.0_1")) { FirstDoor = *It; }
		if (It->InteractionId == TEXT("door.1_2")) { SecondDoor = *It; }
	}
	if (!TestNotNull(TEXT("First production doorway"), FirstDoor) || !TestNotNull(TEXT("Second production doorway"), SecondDoor)) { return false; }
	TestTrue(TEXT("Production doorway crossing checks are enabled"), FirstDoor->IsActorTickEnabled());
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(),
		QinglanRoomLayout::RoomEntrance(0), FRotator::ZeroRotator, Spawn);
	APlayerController* Controller = Fixture.World->SpawnActor<APlayerController>();
	AQinglanEncounterDirector* Director = Fixture.World->SpawnActor<AQinglanEncounterDirector>();
	if (!TestNotNull(TEXT("Real moving character"), Player) || !TestNotNull(TEXT("Real player controller"), Controller)
		|| !TestNotNull(TEXT("Real encounter director"), Director)) { return false; }
	Controller->SetPlayerState(Fixture.World->SpawnActor<APlayerState>());
	Controller->Possess(Player);
	if (!TestTrue(TEXT("Crossing actor is player-controlled"), Player->IsPlayerControlled())) { return false; }
	if (!TestTrue(TEXT("Entrance initializes"), Run->EnterRoom(0))) { return false; }
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production scene state"), Story)) { return false; }
	if (!TestTrue(TEXT("A real dialogue can hold the boundary"), Story->BeginScene(TEXT("CH00_S01")))) { return false; }
	Player->SetActorLocation(FirstDoor->GetActorLocation() + FVector(120.f, 0.f, 0.f), false);
	FirstDoor->Tick(1.f / 60.f);
	TestEqual(TEXT("Walking cannot leave while dialogue is playing"), Run->GetCurrentRoomIndex(), 0);
	TestTrue(TEXT("Dialogue boundary keeps the player inside the entrance room"), Player->GetActorLocation().X < FirstDoor->GetActorLocation().X);
	QinglanStoryRuntime::ImportNarrativeState(Story, FQinglanNarrativeState());
	Player->SetActorLocation(FirstDoor->GetActorLocation() + FVector(120.f, 0.f, 0.f), false);
	// Advance the production door tick after moving the actual possessed pawn;
	// this tests boundary routing, not character movement/physics integration.
	FirstDoor->Tick(1.f / 60.f); // no E press or direct room mutation
	TestEqual(TEXT("Walking through the doorway advances to room one"), Run->GetCurrentRoomIndex(), 1);
	TestTrue(TEXT("The actual next-room encounter starts"), Director->IsEncounterActive());
	TestTrue(TEXT("Crossing positions the player at the initialized room entrance"),
		Player->GetActorLocation().Equals(QinglanRoomLayout::RoomEntrance(1), 1.f));
	FQinglanCheckpoint FirstEntry, RepeatedEntry;
	Run->ExportCheckpoint(FirstEntry);
	for (int32 Repeat = 0; Repeat < 5; ++Repeat) { FirstDoor->Tick(1.f / 60.f); }
	Run->ExportCheckpoint(RepeatedEntry);
	TestEqual(TEXT("Repeated doorway ticks cannot advance twice"), Run->GetCurrentRoomIndex(), 1);
	TestEqual(TEXT("Repeated doorway ticks do not reroll the encounter"), RepeatedEntry.EncounterStreamState, FirstEntry.EncounterStreamState);
	Player->SetActorLocation(SecondDoor->GetActorLocation() + FVector(120.f, 0.f, 0.f), false);
	Player->GetCharacterMovement()->Velocity = FVector(600.f, 150.f, 0.f);
	SecondDoor->Tick(1.f / 60.f);
	TestEqual(TEXT("A live encounter cannot be bypassed by walking through the next corridor"), Run->GetCurrentRoomIndex(), 1);
	TestTrue(TEXT("The locked boundary keeps the player inside the active room"), Player->GetActorLocation().X < SecondDoor->GetActorLocation().X);
	TestTrue(TEXT("Locked boundary stops forward crossing"), Player->GetCharacterMovement()->Velocity.X <= 0.f);
	TestEqual(TEXT("Locked boundary preserves lateral player control"), Player->GetCharacterMovement()->Velocity.Y, 150.0);
	return true;
}
#endif
