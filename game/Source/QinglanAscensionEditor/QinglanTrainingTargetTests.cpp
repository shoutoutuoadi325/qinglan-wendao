// Real Hub spawner + pawn sword collision + accepted damage -> story proof.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanTrainingTarget.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/QinglanInteractionSpawner.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/TargetPoint.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTrainingTargetRealSwordHits,
	"Qinglan.Combat.TrainingTargetRealSwordHits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanTrainingTargetRealSwordHits::RunTest(const FString& Parameters)
{
	const FString WorldName = FString::Printf(TEXT("L_Hub_Training_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UWorld* World = nullptr;
	UGameInstance* Instance = nullptr;
	auto CleanupWorld = [&]()
	{
		// Actor EndPlay must run while its gameplay subsystems still exist.
		// DestroyWorld alone performs cleanup, not the EndPlay lifecycle.
		if (World) { World->EndPlay(EEndPlayReason::Quit); }
		if (Instance) { Instance->Shutdown(); }
		if (World)
		{
			if (GEngine) { GEngine->DestroyWorldContext(World); }
			World->DestroyWorld(false);
		}
		World = nullptr;
		Instance = nullptr;
	};
	ON_SCOPE_EXIT { CleanupWorld(); };
	if (!TestNotNull(TEXT("Engine supplies the standalone world context"), GEngine)) { return false; }
	Instance = NewObject<UGameInstance>(GEngine);
	// The supported standalone initializer sets both the instance's internal
	// context and Context.OwningGameInstance. Merely SetGameInstance + Init
	// leaves subsystem GetWorld() null, including the sword's real audio cue.
	Instance->InitializeStandalone(FName(*WorldName), CreatePackage(*(TEXT("/Temp/") + WorldName)));
	World = Instance->GetWorld();
	if (!TestNotNull(TEXT("Private physics game world exists"), World)) { return false; }
	const FVector YardLocation(3000.f, 2500.f, 600.f);
	FActorSpawnParameters AnchorParams;
	AnchorParams.Name = TEXT("hub.training_yard");
	AnchorParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	ATargetPoint* Yard = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(),
		YardLocation, FRotator::ZeroRotator, AnchorParams);
	if (!TestNotNull(TEXT("Real map training anchor exists"), Yard)) { return false; }
	// Run the real initialization pass, including PostInitializeComponents on
	// map actors and dynamically spawned actors. BeginPlay alone does not do it.
	World->InitializeActorsForPlay(FURL());
	UQinglanInteractionSpawner* Spawner = World->GetSubsystem<UQinglanInteractionSpawner>();
	if (!TestNotNull(TEXT("Production world spawner exists"), Spawner)) { return false; }
	World->BeginPlay(); // dispatches the production world subsystem, plus physics startup
	AQinglanTrainingTarget* Target = nullptr;
	int32 TargetCount = 0;
	for (TActorIterator<AQinglanTrainingTarget> It(World); It; ++It) { Target = *It; ++TargetCount; }
	TestEqual(TEXT("Exactly one real target is spawned"), TargetCount, 1);
	if (!TestNotNull(TEXT("Practice target is available"), Target)) { return false; }
	TestTrue(TEXT("Practice tag prevents sword energy rewards"),
		Target->ActorHasTag(FName(QinglanCombatTunables::QinglanTrainingTargetTag)));
	TestTrue(TEXT("Target stays clear of the yard interaction anchor"),
		FVector::Dist2D(Target->GetActorLocation(), YardLocation) > 300.f);
	const UCapsuleComponent* TargetCollision = Cast<UCapsuleComponent>(Target->GetRootComponent());
	if (!TestNotNull(TEXT("Target has physical collision"), TargetCollision)) { return false; }
	TestEqual(TEXT("Sword object query can hit the target"), TargetCollision->GetCollisionObjectType(), ECC_Pawn);
	TestTrue(TEXT("Target collision supports queries"), TargetCollision->IsQueryCollisionEnabled());
	const UStaticMeshComponent* Post = Target->FindComponentByClass<UStaticMeshComponent>();
	TestTrue(TEXT("Practice post has a visible mesh"), Post && Post->GetStaticMesh() && Post->IsVisible());

	FActorSpawnParameters PlayerParams;
	PlayerParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(),
		Target->GetActorLocation() + FVector(-150.f, 0.f, 0.f), FRotator::ZeroRotator, PlayerParams);
	if (!TestNotNull(TEXT("Actual player character exists"), Player)) { return false; }
	// No GameMode is installed in this private world, so actor BeginPlay must
	// be dispatched explicitly (the same seam as the campaign world fixture).
	World->SetBegunPlay(true);
	Player->DispatchBeginPlay();
	Target->DispatchBeginPlay();
	UQinglanSwordComponent* Sword = Player->FindComponentByClass<UQinglanSwordComponent>();
	UQinglanCombatComponent* PlayerCombat = Player->GetCombat();
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>();
	if (!TestNotNull(TEXT("Player sword exists"), Sword) || !TestNotNull(TEXT("Player combat exists"), PlayerCombat) ||
		!TestNotNull(TEXT("Production story exists"), Story) || !TestNotNull(TEXT("Production profile exists"), Profile)) { return false; }
	// Align the real blade socket with the real target collider. The test
	// never calls ApplyHit, ApplyDamage, or NotifyGameplayEvent for these hits.
	if (!TestTrue(TEXT("Production sword base socket is imported"),
		Player->GetMesh()->DoesSocketExist(FName(TEXT("sword_base"))))) { return false; }
	const FVector Blade = Player->GetMesh()->GetSocketLocation(FName(TEXT("sword_base")));
	Player->SetActorLocation(Player->GetActorLocation() + Target->GetActorLocation() - Blade, false);
	FQinglanAttributes Attributes = PlayerCombat->GetAttributes();
	Attributes.SpiritualEnergy = 1.f;
	PlayerCombat->SetDerivedAttributes(Attributes);
	const float EnergyBefore = PlayerCombat->GetAttributes().SpiritualEnergy;
	const float PlayerHealthBefore = PlayerCombat->GetHealth();
	const FVector TargetLocation = Target->GetActorLocation();
	FQinglanProfileSnapshot Before, After;
	Profile->ExportStateForSave(Before);
	auto ProofCount = [Story]()
	{
		FQinglanNarrativeState State;
		QinglanStoryRuntime::ExportNarrativeState(Story, State);
		return State.FiredEventLog.FilterByPredicate([](FName Event) { return Event == FName(TEXT("combat.sword_hit")); }).Num();
	};
	const int32 ProofBefore = ProofCount();
	auto Swing = [Sword]()
	{
		const bool bStarted = Sword->RequestAttack();
		Sword->TickComponent(0.25f, LEVELTICK_All, nullptr); // real timer opens the hit window
		Sword->TickComponent(0.01f, LEVELTICK_All, nullptr); // real Pawn collision overlap/sweep
		Sword->TickComponent(0.01f, LEVELTICK_All, nullptr); // dedupe within the same swing
		Sword->TickComponent(0.5f, LEVELTICK_All, nullptr);
		Sword->TickComponent(0.5f, LEVELTICK_All, nullptr);
		return bStarted;
	};
	for (int32 Hit = 1; Hit <= 3; ++Hit)
	{
		if (Hit == 3)
		{
			// The third genuine sword collision is otherwise lethal. A practice
			// target must still recover and accept future training attempts.
			FQinglanAttributes StrongHit = PlayerCombat->GetAttributes();
			StrongHit.TechniquePower = 10000.f;
			PlayerCombat->SetDerivedAttributes(StrongHit);
		}
		TestTrue(TEXT("Production sword attack starts"), Swing());
		TestEqual(TEXT("One accepted collision per swing"), Target->GetAcceptedHitCount(), Hit);
		TestEqual(TEXT("Every accepted swing produces exactly one real training proof"), ProofCount(), ProofBefore + Hit);
		TestTrue(TEXT("Target is alive and fully restored after every accepted hit"), Target->GetCombat()->IsAlive() &&
			FMath::IsNearlyEqual(Target->GetCombat()->GetHealth(), Target->GetCombat()->GetMaxHealth()));
	}
	// Rejected damage must not emit proof (the victim is inside an i-frame).
	Target->GetCombat()->NotifyDodgeStarted(World->GetTimeSeconds() - 0.15f);
	TestTrue(TEXT("A rejected-impact swing can still start"), Swing());
	TestEqual(TEXT("Rejected damage does not count as a target hit"), Target->GetAcceptedHitCount(), 3);
	TestEqual(TEXT("Rejected damage does not advance training proof"), ProofCount(), ProofBefore + 3);
	TestEqual(TEXT("Training grants no sword energy"), PlayerCombat->GetAttributes().SpiritualEnergy, EnergyBefore);
	TestEqual(TEXT("Target never damages the player"), PlayerCombat->GetHealth(), PlayerHealthBefore);
	TestTrue(TEXT("Target never pursues the player"), Target->GetActorLocation().Equals(TargetLocation));
	Profile->ExportStateForSave(After);
	TestEqual(TEXT("Training grants no shards"), After.Inventory.SpiritShards, Before.Inventory.SpiritShards);
	TestEqual(TEXT("Training grants no herbs"), After.Inventory.SpiritHerb, Before.Inventory.SpiritHerb);
	TestEqual(TEXT("Training grants no crystals"), After.Inventory.VeinCrystal, Before.Inventory.VeinCrystal);
	TestTrue(TEXT("Training creates no campaign flags"), After.CampaignFlags == Before.CampaignFlags);
	TestTrue(TEXT("Training creates no reward transactions"), After.CommittedTransactionIds == Before.CommittedTransactionIds);
	// Catch world retention in this test instead of crashing the next real
	// map load. The actor-owned proof relay must not keep the world rooted.
	const TWeakObjectPtr<UWorld> ReleasedWorld(World);
	const TWeakObjectPtr<AQinglanCharacter> ReleasedPlayer(Player);
	CleanupWorld();
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("Training player is collectable after EndPlay"), ReleasedPlayer.IsValid());
	TestFalse(TEXT("Training world is collectable before the next map load"), ReleasedWorld.IsValid());
	return true;
}
#endif
