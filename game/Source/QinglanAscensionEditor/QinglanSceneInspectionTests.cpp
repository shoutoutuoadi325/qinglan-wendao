#include "Core/QinglanSceneInspections.h"
#include "Combat/QinglanCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanInteractionSpawner.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FInspectionTestWorld
	{
		UWorld* World = nullptr;
		UGameInstance* Instance = nullptr;
		explicit FInspectionTestWorld(const FString& Name)
		{
			if (!GEngine) { return; }
			Instance = NewObject<UGameInstance>(GEngine);
			// Subsystems, controllers and pawn initialization need the actual
			// owning instance context, not merely World->SetGameInstance.
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
		}
		void BeginPlay()
		{
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			// This fixture deliberately has no GameMode to dispatch actor play.
			World->SetBegunPlay(true);
		}
		~FInspectionTestWorld()
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSceneInspectionCatalog,
	"Qinglan.Narrative.SceneInspectionCatalogCoverage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanSceneInspectionCatalog::RunTest(const FString& Parameters)
{
	UQinglanStorySubsystem* Story = NewObject<UQinglanStorySubsystem>(NewObject<UGameInstance>());
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (!TestNotNull(TEXT("Production imported registry loads"), Registry)) { return false; }
	TSet<FName> Ids;
	TMap<FName, int32> Counts;
	for (const QinglanSceneInspections::FDocument& Document : QinglanSceneInspections::Catalog())
	{
		TestFalse(TEXT("Each physical document has a unique identity"), Ids.Contains(Document.Id));
		Ids.Add(Document.Id);
		const FQinglanRegistryScene* Scene = QinglanSceneInspections::FindScene(Story, Document.Scene);
		if (!TestNotNull(Document.Scene.ToString(), Scene)) { continue; }
		TestTrue(TEXT("Every inspection has original authored body keys"), !Document.BodyKeys.IsEmpty());
		for (FName Key : Document.BodyKeys)
		{
			TestTrue(Key.ToString(), Scene->Script.ContainsByPredicate([Key](const FQinglanRegistryLine& Line) { return Line.TextKey == Key; }));
		}
		TestTrue(TEXT("Each inspection resolves nonempty authored text"), !QinglanSceneInspections::Body(Story, Document).IsEmpty());
		Counts.FindOrAdd(FName(*(Document.Scene.ToString() + TEXT("|") + Document.Alias.ToString())))++;
	}
	auto CheckScene = [this, &Counts](const FQinglanRegistryScene& Scene)
	{
		for (const FQinglanRegistryObjective& Objective : Scene.Objectives)
		{
			for (const FQinglanRegistryGuardTerm& Term : Objective.Guard.AllOf)
			{
				FString Event = Term.Event.ToString();
				if (!Event.RemoveFromStart(TEXT("record.opened."))) { continue; }
				const int32 Required = Term.Kind == FName(TEXT("event_count")) ? Term.Count : 1;
				TestTrue(TEXT("Catalog supplies every authored record event/count: ") + Scene.SceneId.ToString() + TEXT("/") + Event,
					Counts.FindRef(FName(*(Scene.SceneId.ToString() + TEXT("|") + Event))) >= Required);
			}
		}
	};
	for (const FQinglanRegistryScene& Scene : Registry->MainScenes) { CheckScene(Scene); }
	for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
	{
		for (const FQinglanRegistryScene& Scene : Quest.Stages) { CheckScene(Scene); }
	}
	TestEqual(TEXT("Scene inspections leave the authored 24 collectible records separate"), Registry->Records.Num(), 24);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSceneInspectionReadFlow,
	"Qinglan.Narrative.SceneInspectionRealReadFlow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanSceneInspectionReadFlow::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("L_Hub_Inspection_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FInspectionTestWorld Fixture(Name);
	UWorld* World = Fixture.World;
	UGameInstance* Instance = Fixture.Instance;
	if (!TestNotNull(TEXT("Private game world"), World)) { return false; }
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production story subsystem"), Story)) { return false; }
	const FName SceneId(TEXT("CH00_S03"));
	const QinglanSceneInspections::FDocument* Document = QinglanSceneInspections::Catalog().FindByPredicate(
		[](const QinglanSceneInspections::FDocument& D) { return D.Id == FName(TEXT("route_letter")); });
	if (!TestNotNull(TEXT("Route-letter catalog entry"), Document)) { return false; }
	TMap<FName, FVector> Anchors;
	Anchors.Add(TEXT("hub.shrine"), FVector(0.f, 0.f, 100.f));
	Fixture.BeginPlay();
	QinglanSceneInspections::Spawn(World, Anchors);
	QinglanSceneInspections::Spawn(World, Anchors);
	AQinglanInteractable* Plate = nullptr;
	int32 Count = 0;
	for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
	{
		if (It->InteractionId == FName(TEXT("inspect.route_letter"))) { Plate = *It; ++Count; }
	}
	TestEqual(TEXT("Repeated spawn preserves one plate per document"), Count, 1);
	if (!TestNotNull(TEXT("Real route-letter interactable"), Plate)) { return false; }
	const UStaticMeshComponent* Mesh = Plate->FindComponentByClass<UStaticMeshComponent>();
	TestTrue(TEXT("Document has a visible physical plate"), Mesh && Mesh->GetStaticMesh() && Mesh->IsVisible());
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(), Plate->GetActorLocation(), FRotator::ZeroRotator, Params);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Real character"), Player) || !TestNotNull(TEXT("Player controller"), Controller)) { return false; }
	// APawn::IsPlayerControlled (UE 5.8) requires a non-bot PlayerState. A bare
	// spawned controller has none, so supply the state a logged-in player has.
	if (!Controller->GetPlayerState<APlayerState>())
	{
		Controller->SetPlayerState(World->SpawnActor<APlayerState>(APlayerState::StaticClass()));
	}
	Controller->Possess(Player);
	TestTrue(TEXT("Controller actually possesses the real player"), Player->IsPlayerControlled() && Player->GetController() == Controller);
	World->SetBegunPlay(true);
	Controller->DispatchBeginPlay();
	Player->DispatchBeginPlay();
	FQinglanNarrativeState Initial;
	QinglanStoryRuntime::ImportNarrativeState(Story, Initial);
	TestFalse(TEXT("Prerequisites refuse premature inspection"), QinglanSceneInspections::CanRead(Story, Player, *Document));
	Plate->ExecuteInteraction(Player);
	TestFalse(TEXT("Refused read creates no persistent document marker"), Story->HasFlag(TEXT("inspection.read.route_letter")));
	Initial.Flags.Add(TEXT("mq_ch00_training_complete"));
	QinglanStoryRuntime::ImportNarrativeState(Story, Initial);
	TestFalse(TEXT("Prerequisites alone cannot bypass dialogue"), QinglanSceneInspections::CanRead(Story, Player, *Document));
	if (!TestTrue(TEXT("Original route-letter dialogue begins"), Story->BeginScene(SceneId))) { return false; }
	Story->SkipScene();
	TestTrue(TEXT("Original dialogue was delivered/exhausted"), Player->HasSceneScriptExhaustedForInspection(SceneId));
	TestTrue(TEXT("Exhausted scene unlocks its inspection"), QinglanSceneInspections::CanRead(Story, Player, *Document));
	Plate->ExecuteInteraction(Player);
	TestTrue(TEXT("Accepted inspection stores its physical read marker"), Story->HasFlag(TEXT("inspection.read.route_letter")));
	FQinglanNarrativeState After;
	QinglanStoryRuntime::ExportNarrativeState(Story, After);
	TestTrue(TEXT("Actual Character record UI delegate produces the record event"), After.FiredEventLog.Contains(TEXT("record.opened.route_letter")));
	TestTrue(TEXT("Scene document does not enter collectible lore reads"), After.ReadRecords.IsEmpty());
	const int32 Events = After.FiredEventLog.Num();
	Plate->ExecuteInteraction(Player);
	QinglanStoryRuntime::ExportNarrativeState(Story, After);
	TestEqual(TEXT("Reopening displays the document without farming event_count"), After.FiredEventLog.Num(), Events);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanInspectionShutterCycle,
	"Qinglan.Narrative.InspectionShutterOpenClose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanInspectionShutterCycle::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("L_BambooHollow_Inspection_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FInspectionTestWorld Fixture(Name);
	UWorld* World = Fixture.World;
	UGameInstance* Instance = Fixture.Instance;
	if (!TestNotNull(TEXT("Private guided-route world"), World)) { return false; }
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Story instance"), Story)) { return false; }
	Fixture.BeginPlay();
	QinglanSceneInspections::Spawn(World, {});
	AQinglanInteractable* Shutter = nullptr;
	for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
	{
		if (It->InteractionId == FName(TEXT("device.inspection_shutter"))) { Shutter = *It; }
	}
	if (!TestNotNull(TEXT("Separate physical inspection shutter"), Shutter)) { return false; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(), Shutter->GetActorLocation(), FRotator::ZeroRotator, Params);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Player"), Player) || !TestNotNull(TEXT("Controller"), Controller)) { return false; }
	// APawn::IsPlayerControlled (UE 5.8) requires a non-bot PlayerState.
	if (!Controller->GetPlayerState<APlayerState>())
	{
		Controller->SetPlayerState(World->SpawnActor<APlayerState>(APlayerState::StaticClass()));
	}
	Controller->Possess(Player);
	TestTrue(TEXT("Device user is a possessed player"), Player->IsPlayerControlled() && Player->GetController() == Controller);
	World->SetBegunPlay(true); Controller->DispatchBeginPlay(); Player->DispatchBeginPlay();
	// This fixture isolates device behavior after dialogue, with the actual
	// combat producer's persisted room-clear event supplied as input below.
	FQinglanNarrativeState State;
	State.Flags.Add(TEXT("mq_ch01_watermarks_read")); State.CompletedScenes.Add(TEXT("CH01_S02"));
	QinglanStoryRuntime::ImportNarrativeState(Story, State);
	Shutter->ExecuteInteraction(Player);
	TestFalse(TEXT("Dialogue alone cannot open an uncleared inspection outlet"), Story->HasFlag(TEXT("inspection.shutter.opened")));
	State.FiredEventLog.Add(TEXT("encounter.room_cleared.tutorial.2"));
	QinglanStoryRuntime::ImportNarrativeState(Story, State);
	Shutter->ExecuteInteraction(Player);
	QinglanStoryRuntime::ExportNarrativeState(Story, State);
	TestTrue(TEXT("First accepted press persists open state"), State.Flags.Contains(TEXT("inspection.shutter.opened")));
	TestFalse(TEXT("Opening alone cannot satisfy the shutter proof"), State.FiredEventLog.Contains(TEXT("device.shutter_tested")));
	QinglanSceneInspections::Spawn(World, {});
	TestTrue(TEXT("Spawn restores the persisted open marker"), Shutter->ActorHasTag(TEXT("InspectionShutterOpen")));
	Shutter->ExecuteInteraction(Player);
	QinglanStoryRuntime::ExportNarrativeState(Story, State);
	TestTrue(TEXT("Second press closes and persists the device"), State.Flags.Contains(TEXT("inspection.shutter.closed")));
	TestTrue(TEXT("Only the completed open/close cycle emits proof"), State.FiredEventLog.Contains(TEXT("device.shutter_tested")));
	TestTrue(TEXT("Finished device visibly expresses closed state"), Shutter->ActorHasTag(TEXT("InspectionShutterClosed")));
	const int32 EventCount = State.FiredEventLog.Num();
	Shutter->ExecuteInteraction(Player);
	QinglanStoryRuntime::ExportNarrativeState(Story, State);
	TestEqual(TEXT("Completed device inspection cannot duplicate the proof"), State.FiredEventLog.Num(), EventCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanAshenInspectionDialogueAnchors,
	"Qinglan.Narrative.AshenInspectionDialogueAnchors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanAshenInspectionDialogueAnchors::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("L_AshenVerge_Inspection_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FInspectionTestWorld Fixture(Name);
	UWorld* World = Fixture.World;
	UGameInstance* Instance = Fixture.Instance;
	if (!TestNotNull(TEXT("Private Ashen game world"), World)) { return false; }
	const FName Anchors[] = {TEXT("ashen.approach_1_2"), TEXT("ashen.archive_3"), TEXT("ashen.release_antechamber")};
	const FVector Positions[] = {FVector(8250.f, 0.f, 100.f), FVector(16500.f, 0.f, 100.f), FVector(27500.f, 2000.f, 100.f)};
	const FName Scenes[] = {TEXT("CH04_S01"), TEXT("CH04_S02"), TEXT("CH04_S04")};
	for (int32 I = 0; I < UE_ARRAY_COUNT(Anchors); ++I)
	{
		FActorSpawnParameters Params;
		Params.Name = FName(*(Anchors[I].ToString() + TEXT("_729")));
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		ATargetPoint* Point = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Positions[I], FRotator::ZeroRotator, Params);
		if (Point) { Point->Tags.AddUnique(Anchors[I]); }
	}
	// Legacy maps can have either an exact numeric ID or an additional
	// uniquification suffix. Both must resolve to the same real station.
	for (const TCHAR* RawName : {TEXT("ashen.archive_3"), TEXT("ashen.archive_3_812")})
	{
		FActorSpawnParameters Params;
		Params.Name = FName(RawName); Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Positions[1], FRotator::ZeroRotator, Params);
	}
	UQinglanInteractionSpawner* Spawner = World->GetSubsystem<UQinglanInteractionSpawner>();
	if (!TestNotNull(TEXT("Production world spawner"), Spawner)) { return false; }
	Fixture.BeginPlay();
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	FQinglanNarrativeState State;
	State.Flags = {TEXT("mq_ch03_complete"), TEXT("mq_ashen_marks_read"), TEXT("mq_thirty_day_consent_read"), TEXT("mq_renewals_read")};
	QinglanStoryRuntime::ImportNarrativeState(Story, State);
	for (int32 I = 0; I < UE_ARRAY_COUNT(Anchors); ++I)
	{
		AQinglanInteractable* Dialogue = nullptr;
		int32 Count = 0;
		for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
		{
			if (It->InteractionId == Anchors[I]) { Dialogue = *It; ++Count; }
		}
		TestEqual(TEXT("Tagged/exact/uniquified anchors resolve one dialogue station"), Count, 1);
		if (!TestNotNull(TEXT("Authored Ashen station creates a dialogue interactable: ") + Anchors[I].ToString(), Dialogue)) { continue; }
		TestTrue(TEXT("Dialogue remains at the actual map anchor"), Dialogue->GetActorLocation().Equals(Positions[I], 0.01f));
		Dialogue->ExecuteInteraction(nullptr);
		TestTrue(TEXT("Actual anchor dispatch starts its authored dialogue"), Story->IsScenePlaying());
		TestEqual(TEXT("Actual anchor routes the correct scene"), Story->GetCurrentSceneId(), Scenes[I]);
		// These chapter scenes pause at an unselected choice; re-importing the
		// fixture state drops in-flight playback so the next anchor is not
		// blocked by a scene this test never intended to play through.
		Story->SkipScene();
		QinglanStoryRuntime::ImportNarrativeState(Story, State);
	}
	return true;
}
#endif
