// Real runtime resolver + player delegates + world-spawned interaction actors.
// Proof history/checkpoints are explicit fixture inputs; these are guidance
// regressions, not a substitute for input/combat or rendered playthroughs.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanTrainingTarget.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanObjectiveGuide.h"
#include "Core/QinglanRoomLayout.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FObjectiveGuideWorld
	{
		UWorld* World = nullptr;
		UGameInstance* Instance = nullptr;
		AQinglanCharacter* Player = nullptr;
		explicit FObjectiveGuideWorld(const TCHAR* Map)
		{
			if (!GEngine) { return; }
			const FString Name = FString(Map) + TEXT("_Guide_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
		}
		~FObjectiveGuideWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
		}
		void AddAnchor(FName Id, FVector Location)
		{
			FActorSpawnParameters Params;
			Params.Name = Id;
			Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
			if (ATargetPoint* Point = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Location, FRotator::ZeroRotator, Params))
			{
				Point->Tags.Add(Id);
			}
		}
		void BeginPlay()
		{
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay(); // Runs the production spawner and physical plates.
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(), FVector(0.f, 0.f, 700.f), FRotator::ZeroRotator, Params);
			World->SetBegunPlay(true);
			if (Player) { Player->DispatchBeginPlay(); }
		}
		AQinglanInteractable* Find(FName Id) const
		{
			for (TActorIterator<AQinglanInteractable> It(World); It; ++It) { if (It->InteractionId == Id) { return *It; } }
			return nullptr;
		}
	};

	FQinglanNarrativeState BeforeScene(const UQinglanStoryRegistry* Registry, FName SceneId)
	{
		FQinglanNarrativeState State;
		for (const FQinglanRegistryChapter& Chapter : Registry->Chapters)
		{
			for (FName PreviousId : Chapter.SceneIds)
			{
				if (PreviousId == SceneId) { return State; }
				if (const FQinglanRegistryScene* Previous = Registry->FindMainScene(PreviousId))
				{
					State.CompletedScenes.Add(PreviousId);
					State.Flags.Append(Previous->SetsFlags);
				}
			}
		}
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHubObjectiveGuide,
	"Qinglan.Narrative.HubObjectiveGuideTargetsPendingProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanHubObjectiveGuide::RunTest(const FString& Parameters)
{
	FObjectiveGuideWorld Fixture(TEXT("L_Hub"));
	if (!TestNotNull(TEXT("Private game world"), Fixture.World)) { return false; }
	Fixture.AddAnchor(TEXT("hub.courtyard"), FVector(0.f, 0.f, 600.f));
	Fixture.AddAnchor(TEXT("hub.training_yard"), FVector(3000.f, 2500.f, 600.f));
	Fixture.AddAnchor(TEXT("hub.shrine"), FVector(0.f, 3200.f, 600.f));
	Fixture.AddAnchor(TEXT("hub.bamboo_gate"), FVector(0.f, -4800.f, 600.f));
	Fixture.BeginPlay();
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production player"), Fixture.Player) || !TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (!TestNotNull(TEXT("Imported narrative"), Registry)) { return false; }
	FQinglanObjectiveGuide Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Fresh game tracks the courtyard scene"), Guide.SceneId, FName(TEXT("CH00_S01")));
	TestEqual(TEXT("Fresh game points at the actual courtyard actor"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("hub.courtyard"))));
	TestEqual(TEXT("Fresh game teaches interaction"), Guide.HintKey, FName(TEXT("guide.interact")));
	TestFalse(TEXT("Guidance did not begin dialogue"), Story->IsScenePlaying());

	QinglanStoryRuntime::ImportNarrativeState(Story, BeforeScene(Registry, TEXT("CH00_S02")));
	if (!TestTrue(TEXT("Training dialogue starts"), Story->BeginScene(TEXT("CH00_S02")))) { return false; }
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Playing dialogue teaches continue"), Guide.HintKey, FName(TEXT("guide.dialogue")));
	Story->SkipScene();
	TestTrue(TEXT("Player received real script exhaustion delegates"), Fixture.Player->HasSceneScriptExhaustedForInspection(TEXT("CH00_S02")));
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Training asks for missing sword hits first"), Guide.HintKey, FName(TEXT("guide.training_hits")));
	TestNotNull(TEXT("Sword guidance finds the real spawned training post"), Cast<AQinglanTrainingTarget>(Guide.Target));
	for (int32 Hit = 0; Hit < 3; ++Hit) { QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("combat.sword_hit"), {}); }
	TestEqual(TEXT("Completed sword proof changes next hint to dodge"), QinglanObjectiveGuide::Resolve(Fixture.Player).HintKey, FName(TEXT("guide.training_dodge")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("combat.dodge"), {});
	TestEqual(TEXT("Dodge proof changes next hint to Arc"), QinglanObjectiveGuide::Resolve(Fixture.Player).HintKey, FName(TEXT("guide.training_arc")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("cast.jade_sword_arc"), {});
	TestEqual(TEXT("Arc proof changes next hint to Ward"), QinglanObjectiveGuide::Resolve(Fixture.Player).HintKey, FName(TEXT("guide.training_ward")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("cast.flowing_ward"), {});
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Training finally directs recovery"), Guide.HintKey, FName(TEXT("guide.recover")));
	TestEqual(TEXT("Recovery points at shrine, not the old training station"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("hub.shrine"))));
	FQinglanNarrativeState Before, After;
	QinglanStoryRuntime::ExportNarrativeState(Story, Before);
	for (int32 Frame = 0; Frame < 5; ++Frame) { QinglanObjectiveGuide::Resolve(Fixture.Player); }
	QinglanStoryRuntime::ExportNarrativeState(Story, After);
	TestTrue(TEXT("Guidance does not manufacture missing recovery proof"), Before.FiredEventLog == After.FiredEventLog);
	TestFalse(TEXT("Guidance does not finish training"), Story->IsSceneCompleted(TEXT("CH00_S02")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("shrine.recovery_used"), {});
	if (!TestTrue(TEXT("Next route-letter dialogue starts"), Story->BeginScene(TEXT("CH00_S03")))) { return false; }
	Story->SkipScene();
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Exhausted route-letter dialogue teaches physical reading"), Guide.HintKey, FName(TEXT("guide.read")));
	TestNotNull(TEXT("Production spawner created the physical route letter"), Fixture.Find(TEXT("inspect.route_letter")));
	TestEqual(TEXT("Reading targets the document plate rather than replaying the shrine dialogue"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("inspect.route_letter"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTutorialObjectiveGuide,
	"Qinglan.Narrative.TutorialObjectiveGuideTracksDoorsShutterAndReturn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanTutorialObjectiveGuide::RunTest(const FString& Parameters)
{
	FObjectiveGuideWorld Fixture(TEXT("L_BambooHollow"));
	if (!TestNotNull(TEXT("Private tutorial game world"), Fixture.World)) { return false; }
	Fixture.AddAnchor(TEXT("bamboo.room_0"), FVector(0.f, 0.f, 150.f));
	Fixture.AddAnchor(TEXT("bamboo.room_1_2_guided"), FVector(5500.f, 0.f, 150.f));
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	if (!TestNotNull(TEXT("Run subsystem"), Run)) { return false; }
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = FName(UQinglanEconomyLibrary::TutorialRealmIdText);
	Checkpoint.RealmSeed = 73;
	Checkpoint.RoomIndex = 0;
	// The expedition exists before map BeginPlay in production, so the real
	// world spawner authors the short route and its room-three return station.
	Run->ImportCheckpoint(Checkpoint);
	Fixture.BeginPlay();
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production player"), Fixture.Player) || !TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (!TestNotNull(TEXT("Imported narrative"), Registry)) { return false; }
	FQinglanNarrativeState LegacyDeparture;
	LegacyDeparture.Flags.Add(TEXT("mq_ch00_complete"));
	LegacyDeparture.CompletedScenes.Add(TEXT("CH00_S04"));
	QinglanStoryRuntime::ImportNarrativeState(Story, LegacyDeparture);
	TestEqual(TEXT("Legacy early departure still has unfinished sanctuary work"), Story->GetTrackedMainSceneId(), FName(TEXT("CH00_S01")));
	TestEqual(TEXT("An active legacy expedition guides its reachable entrance objective"), QinglanObjectiveGuide::Resolve(Fixture.Player).SceneId, FName(TEXT("CH01_S01")));
	TestFalse(TEXT("Reachable expedition guidance does not manufacture the skipped courtyard proof"), Story->IsSceneCompleted(TEXT("CH00_S01")));
	QinglanStoryRuntime::ImportNarrativeState(Story, BeforeScene(Registry, TEXT("CH01_S01")));
	if (!TestTrue(TEXT("Inspection dialogue begins"), Story->BeginScene(TEXT("CH01_S01")))) { return false; }
	Story->SkipScene();
	FQinglanObjectiveGuide Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("First unread document is water marks"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("inspect.water_marks"))));
	TestEqual(TEXT("Inspection teaches reading"), Guide.HintKey, FName(TEXT("guide.read")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("record.opened.water_marks"), {});
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("After water marks next unread document is delivery note"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("inspect.delivery_note"))));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("record.opened.delivery_note"), {});
	if (!TestTrue(TEXT("Guided encounters dialogue begins"), Story->BeginScene(TEXT("CH01_S02")))) { return false; }
	Story->SkipScene();
	Run->ImportCheckpoint(Checkpoint);
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestNotNull(TEXT("Production spawner created entrance door"), Fixture.Find(TEXT("door.0_1")));
	TestEqual(TEXT("Entrance points forward to actual door"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("door.0_1"))));
	Checkpoint.RoomIndex = 1;
	Run->ImportCheckpoint(Checkpoint);
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Room one guidance follows the next door"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("door.1_2"))));
	TestTrue(TEXT("Room guidance does not keep stale scene-station target"), Guide.Target != Fixture.Find(TEXT("bamboo.room_1_2_guided")));
	Checkpoint.RoomIndex = 2;
	Checkpoint.PendingBlessingRoom = 2;
	Checkpoint.PendingBlessingOffer = {FName(TEXT("b_sharp_edge")), FName(TEXT("b_quiet_current")), FName(TEXT("b_jade_shell"))};
	Run->ImportCheckpoint(Checkpoint);
	TestEqual(TEXT("Pending actual blessing offer takes guidance priority"), QinglanObjectiveGuide::Resolve(Fixture.Player).HintKey, FName(TEXT("guide.blessing")));
	Checkpoint.PendingBlessingRoom = INDEX_NONE;
	Checkpoint.PendingBlessingOffer.Reset();
	Run->ImportCheckpoint(Checkpoint);
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("encounter.room_cleared.tutorial.1"), {});
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("encounter.room_cleared.tutorial.2"), {});
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("blessing.taken"), {});
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("After blessing the missing shutter proof is explained"), Guide.HintKey, FName(TEXT("guide.shutter")));
	TestNotNull(TEXT("Production spawner created the inspection shutter"), Fixture.Find(TEXT("device.inspection_shutter")));
	TestEqual(TEXT("Shutter hint targets the physical device"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("device.inspection_shutter"))));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("device.shutter_tested"), {});
	Checkpoint.RoomIndex = 3;
	Run->ImportCheckpoint(Checkpoint);
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Return room teaches return"), Guide.HintKey, FName(TEXT("guide.return")));
	TestNotNull(TEXT("Production spawner created the short-route return station"), Fixture.Find(TEXT("bamboo.tutorial_return")));
	TestEqual(TEXT("Return hint points at actual station instead of a nonexistent fourth door"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("bamboo.tutorial_return"))));
	QinglanStoryRuntime::ImportNarrativeState(Story, BeforeScene(Registry, TEXT("CH01_S03")));
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Completed guided objective tracks the sanctuary banking scene"), Guide.SceneId, FName(TEXT("CH01_S03")));
	TestEqual(TEXT("Banking objective still teaches return while standing at shrine three"), Guide.HintKey, FName(TEXT("guide.return")));
	TestEqual(TEXT("Cross-map banking guidance retains the actual tutorial return station"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("bamboo.tutorial_return"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCatalogObjectiveGuide,
	"Qinglan.Narrative.LateObjectiveGuideUsesUnreadCatalogDocuments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanCatalogObjectiveGuide::RunTest(const FString& Parameters)
{
	FObjectiveGuideWorld Fixture(TEXT("L_AshenVerge"));
	if (!TestNotNull(TEXT("Private Ashen game world"), Fixture.World)) { return false; }
	// Use production scene-station positions; the production spawner derives
	// each physical plate's distinct position from the real inspection catalog.
	Fixture.AddAnchor(TEXT("ashen.approach_1_2"), FVector(8250.f, 0.f, 100.f));
	Fixture.AddAnchor(TEXT("ashen.archive_3"), FVector(16500.f, 0.f, 100.f));
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	if (!TestNotNull(TEXT("Run subsystem"), Run)) { return false; }
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = FName(UQinglanEconomyLibrary::AshenRealmIdText);
	Checkpoint.RealmSeed = 73;
	Checkpoint.RoomIndex = 2;
	Run->ImportCheckpoint(Checkpoint);
	Fixture.BeginPlay();
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production player"), Fixture.Player) || !TestNotNull(TEXT("Story subsystem"), Story)) { return false; }
	const UQinglanStoryRegistry* Registry = Story->GetRegistry();
	if (!TestNotNull(TEXT("Imported narrative"), Registry)) { return false; }
	QinglanStoryRuntime::ImportNarrativeState(Story, BeforeScene(Registry, TEXT("CH04_S01")));
	if (!TestTrue(TEXT("Ashen inspection dialogue begins"), Story->BeginScene(TEXT("CH04_S01")))) { return false; }
	Story->SkipScene();
	TestTrue(TEXT("Unresolved authored question retains dialogue ownership"), Story->IsScenePlaying());
	TestEqual(TEXT("The active question takes priority over physical documents"), QinglanObjectiveGuide::Resolve(Fixture.Player).HintKey, FName(TEXT("guide.dialogue")));
	// Explicit interruption fixture: importing the pending narrative snapshot
	// drops playback without choosing or completing this choice-driven scene.
	// The same pawn retains its observed script line for the resolver's read
	// guidance seam. This isolates catalog selection; it is not a claim that
	// ordinary input exits an unresolved question or that these reads gate its
	// authored choice transaction.
	FQinglanNarrativeState Interrupted;
	QinglanStoryRuntime::ExportNarrativeState(Story, Interrupted);
	QinglanStoryRuntime::ImportNarrativeState(Story, Interrupted);
	TestFalse(TEXT("Imported pending snapshot has no live playback"), Story->IsScenePlaying());
	TestFalse(TEXT("Interruption does not complete the inspection scene"), Story->IsSceneCompleted(TEXT("CH04_S01")));
	// Explicit completed-combat fixture inputs keep this test focused on the
	// two distinct physical documents sharing the event_count alias "scale".
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("encounter.room_cleared.ashen.1"), {});
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("encounter.room_cleared.ashen.2"), {});
	FQinglanObjectiveGuide Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestNotNull(TEXT("Production catalog spawns first scale plate"), Fixture.Find(TEXT("inspect.scale_1")));
	TestNotNull(TEXT("Production catalog spawns second scale plate"), Fixture.Find(TEXT("inspect.scale_2")));
	TestEqual(TEXT("First unread late-chapter plate is the actual target"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("inspect.scale_1"))));
	TestEqual(TEXT("Late-chapter catalog document teaches reading"), Guide.HintKey, FName(TEXT("guide.read")));
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("record.opened.scale"), {FName(TEXT("inspection.read.scale_1"))});
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Shared alias count advances to the second unread physical plate"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("inspect.scale_2"))));
	TestEqual(TEXT("Second plate retains reading guidance"), Guide.HintKey, FName(TEXT("guide.read")));
	TestEqual(TEXT("One scale leaves the shared two-document proof missing"),
		QinglanStoryRuntime::EvaluateObjectiveGuard(Story, TEXT("CH04_S01"), TEXT("s01.objective")).Status,
		QinglanStoryRuntime::EQinglanGuardStatus::Missing);
	QinglanStoryRuntime::NotifyGameplayEvent(Story, TEXT("record.opened.scale"), {FName(TEXT("inspection.read.scale_2"))});
	TestEqual(TEXT("Both distinct reads satisfy the objective's authored proof"),
		QinglanStoryRuntime::EvaluateObjectiveGuard(Story, TEXT("CH04_S01"), TEXT("s01.objective")).Status,
		QinglanStoryRuntime::EQinglanGuardStatus::Satisfied);
	TestFalse(TEXT("Document proof does not silently select the pending question"), Story->IsSceneCompleted(TEXT("CH04_S01")));
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestTrue(TEXT("Satisfied document proof removes the old read instruction"), Guide.HintKey != FName(TEXT("guide.read")));
	if (!TestTrue(TEXT("The interrupted authored conversation can restart"), Story->BeginScene(TEXT("CH04_S01")))) { return false; }
	Story->SkipScene();
	if (!TestTrue(TEXT("The authored question is resolved explicitly"), Story->SelectChoice(TEXT("s01.ask_water")))) { return false; }
	Story->SkipScene();
	TestTrue(TEXT("Resolving the authored conversation completes the inspected approach"), Story->IsSceneCompleted(TEXT("CH04_S01")));
	Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
	TestEqual(TEXT("Completed approach tracks the next archive scene"), Guide.SceneId, FName(TEXT("CH04_S02")));
	TestNotNull(TEXT("Production spawner creates the next room door"), Fixture.Find(TEXT("door.2_3")));
	TestEqual(TEXT("Distant next scene routes through the current room exit"), Guide.Target, static_cast<AActor*>(Fixture.Find(TEXT("door.2_3"))));
	TestEqual(TEXT("Door guidance replaces the old document instruction"), Guide.HintKey, FName(TEXT("guide.exit")));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanLegacyHubInspectionStations,
	"Qinglan.Narrative.LegacyHubSynthesizesRequiredDocuments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanLegacyHubInspectionStations::RunTest(const FString& Parameters)
{
	for (const bool bAuthoredStations : { false, true })
	{
		FObjectiveGuideWorld Fixture(TEXT("L_Hub"));
		if (!TestNotNull(TEXT("Private hub world"), Fixture.World)) { return false; }
		Fixture.AddAnchor(TEXT("hub.shrine"), FVector(0.f, 3200.f, 600.f));
		Fixture.AddAnchor(TEXT("hub.archive_table"), FVector(2000.f, 0.f, 600.f));
		if (bAuthoredStations)
		{
			Fixture.AddAnchor(TEXT("hub.ledger_table"), FVector(-2300.f, 400.f, 600.f));
			Fixture.AddAnchor(TEXT("hub.correspondence_table"), FVector(2300.f, 800.f, 600.f));
		}
		Fixture.BeginPlay();
		for (const TCHAR* Id : { TEXT("hub.ledger_table"), TEXT("hub.correspondence_table"),
			TEXT("inspect.sealed_page_1"), TEXT("inspect.sealed_page_4"), TEXT("inspect.morning_ledger"),
			TEXT("inspect.thirty_day_card"), TEXT("inspect.outcome_report"), TEXT("inspect.yun_new_letter") })
		{
			TestNotNull(*FString::Printf(TEXT("%s exists with authored stations=%d"), Id, bAuthoredStations), Fixture.Find(Id));
		}
		if (bAuthoredStations && Fixture.Find(TEXT("hub.ledger_table")))
		{
			TestEqual(TEXT("Authored station position takes precedence"), Fixture.Find(TEXT("hub.ledger_table"))->GetActorLocation(),
				FVector(-2300.f, 400.f, 600.f));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDoorMissingEntranceFallback,
	"Qinglan.Progression.DoorCannotTeleportToAnotherRoomsEntrance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanDoorMissingEntranceFallback::RunTest(const FString& Parameters)
{
	FObjectiveGuideWorld Fixture(TEXT("L_DoorRegression"));
	if (!TestNotNull(TEXT("Private room world"), Fixture.World)) { return false; }
	Fixture.BeginPlay();
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	if (!TestNotNull(TEXT("Player"), Fixture.Player) || !TestNotNull(TEXT("Run"), Run)) { return false; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerStart* OtherEntrance = Fixture.World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(),
		FVector(100.f, 0.f, 150.f), FRotator::ZeroRotator, Params);
	AQinglanInteractable* Door = Fixture.World->SpawnActor<AQinglanInteractable>();
	if (!TestNotNull(TEXT("Other entrance"), OtherEntrance) || !TestNotNull(TEXT("Door"), Door)) { return false; }
	Door->InteractionId = TEXT("door.2_3");
	for (const FName Tag : { FName(TEXT("Qinglan.RoomEntrance.0")), FName(TEXT("Qinglan.RoomEntrance")) })
	{
		OtherEntrance->PlayerStartTag = Tag;
		FQinglanCheckpoint Checkpoint;
		Checkpoint.RealmId = UQinglanEconomyLibrary::BambooRealmIdText;
		Checkpoint.RoomIndex = 2;
		Checkpoint.CommittedRoomIds = { 0, 1, 2 };
		Run->ImportCheckpoint(Checkpoint);
		Run->MarkCheckpointResumed();
		Door->ExecuteInteraction(Fixture.Player);
		TestEqual(TEXT("The safe room becomes current"), Run->GetCurrentRoomIndex(), 3);
		TestEqual(TEXT("A missing destination uses its own layout entrance"), Fixture.Player->GetActorLocation(),
			QinglanRoomLayout::RoomEntrance(3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRealmExitGuidance,
	"Qinglan.Narrative.CompletedRealmGuidesToItsReturnStation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanRealmExitGuidance::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Map; const TCHAR* Realm; const TCHAR* NextScene; const TCHAR* Exit; };
	const FCase Cases[] = {
		{ TEXT("L_BambooHollow"), UQinglanEconomyLibrary::BambooRealmIdText, TEXT("CH03_S01"), TEXT("bamboo.room_5_safe") },
		// CH05_S01 is the Ashen Guardian fight at the exit station. A finished
		// realm returns after CH05_S02's release controls, toward the hub ledger.
		{ TEXT("L_AshenVerge"), UQinglanEconomyLibrary::AshenRealmIdText, TEXT("CH05_S03"), TEXT("ashen.boss_5") }
	};
	for (const FCase& Entry : Cases)
	{
		FObjectiveGuideWorld Fixture(Entry.Map);
		if (!TestNotNull(TEXT("Private realm world"), Fixture.World)) { return false; }
		Fixture.AddAnchor(Entry.Exit, QinglanRoomLayout::RoomCenter(6));
		Fixture.BeginPlay();
		UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
		UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
		if (!TestNotNull(TEXT("Player"), Fixture.Player) || !TestNotNull(TEXT("Story"), Story)
			|| !TestNotNull(TEXT("Run"), Run) || !TestNotNull(TEXT("Registry"), Story->GetRegistry())) { return false; }
		QinglanStoryRuntime::ImportNarrativeState(Story, BeforeScene(Story->GetRegistry(), Entry.NextScene));
		FQinglanCheckpoint Checkpoint;
		Checkpoint.RealmId = Entry.Realm;
		Checkpoint.RoomIndex = 6;
		Checkpoint.CommittedRoomIds = { 0, 1, 2, 3, 4, 5, 6 };
		Run->ImportCheckpoint(Checkpoint);
		Run->MarkCheckpointResumed();
		const FQinglanObjectiveGuide Guide = QinglanObjectiveGuide::Resolve(Fixture.Player);
		TestTrue(TEXT("Fixture has completed the realm's victory requirements"), Run->CanClaimVictory());
		TestEqual(TEXT("The next tracked objective is the authored hub scene"), Guide.SceneId, FName(Entry.NextScene));
		const FQinglanRegistryScene* Scene = Story->GetRegistry()->FindMainScene(Guide.SceneId);
		TestTrue(TEXT("The pending objective belongs to the hub"), Scene && Scene->Location.ToString().StartsWith(TEXT("hub.")));
		TestNotNull(TEXT("Production return station exists"), Fixture.Find(Entry.Exit));
		TestEqual(TEXT("A hub objective directs the completed realm to its own return station"),
			Guide.Target, static_cast<AActor*>(Fixture.Find(Entry.Exit)));
		TestEqual(TEXT("Exit station teaches returning"), Guide.HintKey, FName(TEXT("guide.return")));
	}
	return true;
}
#endif
