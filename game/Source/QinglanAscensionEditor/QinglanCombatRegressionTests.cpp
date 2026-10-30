// Player-facing combat regressions: real effect ticks, collision queries,
// profile changes, and checkpoint restoration. Compatible with -nullrhi.
#include "Abilities/QinglanProjectile.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyProjectile.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Combat/QinglanTalismanComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags CombatRegressionFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FCombatRegressionWorld
	{
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		FString SaveDirectory;

		FCombatRegressionWorld()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("_CombatRegression_") + Id);
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
			if (!GEngine) { return; }
			const FString Name = TEXT("L_CombatRegression_") + Id;
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			if (World)
			{
				World->InitializeActorsForPlay(FURL());
				World->BeginPlay();
				World->SetBegunPlay(true);
			}
		}

		~FCombatRegressionWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
			IFileManager::Get().DeleteDirectory(*SaveDirectory, false, true);
		}

		AActor* Body(const FVector& Location, bool bSecondCollider = false)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			if (!Actor) { return nullptr; }
			for (int32 Index = 0; Index < (bSecondCollider ? 2 : 1); ++Index)
			{
				USphereComponent* Sphere = NewObject<USphereComponent>(Actor);
				Actor->AddOwnedComponent(Sphere);
				Sphere->InitSphereRadius(20.f);
				Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Sphere->SetCollisionObjectType(ECC_Pawn);
				Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
				if (Index == 0) { Actor->SetRootComponent(Sphere); }
				else { Sphere->SetupAttachment(Actor->GetRootComponent()); }
				Sphere->RegisterComponent();
			}
			Actor->SetActorLocation(Location);
			UQinglanCombatComponent* Combat = NewObject<UQinglanCombatComponent>(Actor);
			Actor->AddOwnedComponent(Combat);
			Combat->RegisterComponent();
			FQinglanAttributes Attributes;
			Attributes.Defense = 0.f;
			Combat->SetDerivedAttributes(Attributes);
			return Actor;
		}

		AActor* Wall(const FVector& Location, const FVector& Extent)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
			Actor->AddOwnedComponent(Box);
			Actor->SetRootComponent(Box);
			Box->InitBoxExtent(Extent);
			Box->SetCollisionObjectType(ECC_WorldStatic);
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Block);
			Box->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}
	};

	UQinglanCombatComponent* CombatOf(AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanFreshHealCompletes,
	"Qinglan.Combat.FreshHealCompletesAllFourTicks", CombatRegressionFlags)
bool FQinglanFreshHealCompletes::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	UQinglanCombatComponent* Combat = CombatOf(Owner);
	if (!TestNotNull(TEXT("Heal target"), Combat)) { return false; }
	FQinglanAttributes Attributes = Combat->GetAttributes();
	Attributes.Health = 20.f;
	Combat->SetDerivedAttributes(Attributes);
	AQinglanProjectile* Effect = Fixture.World->SpawnActor<AQinglanProjectile>();
	if (!TestNotNull(TEXT("Fresh effect actor"), Effect)) { return false; }
	Effect->SetupHealOverTime(UQinglanCombatComponent::NextCastId(), TEXT("verdant_pulse"), Owner, 20.f, 4.f, 1.f);
	for (int32 Tick = 1; Tick <= 4; ++Tick)
	{
		Effect->Tick(1.f);
		TestEqual(TEXT("Every authored heal tick lands on a freshly spawned effect"), Combat->GetHealth(), 20.f + 5.f * Tick);
	}
	TestFalse(TEXT("Completed heal returns to the pool"), Effect->IsActive());
	Effect->ResetForPool();
	Effect->SetupHealOverTime(UQinglanCombatComponent::NextCastId(), TEXT("verdant_pulse"), Owner, 20.f, 4.f, 1.f);
	Effect->Tick(4.f);
	TestEqual(TEXT("Reuse has the same total healing as a new effect"), Combat->GetHealth(), 60.f);
	Effect->SetupHealOverTime(UQinglanCombatComponent::NextCastId(), TEXT("verdant_pulse"), Owner, 20.f, 4.f, 1.f);
	Effect->Tick(7.f);
	TestEqual(TEXT("A hitch crossing safety expiry still delivers all authored heal ticks"), Combat->GetHealth(), 80.f);
	TestFalse(TEXT("Expired completed healing retires once"), Effect->IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanWetTrailDelivery,
	"Qinglan.Combat.MistStepTrailOwnerAndOcclusion", CombatRegressionFlags)
bool FQinglanWetTrailDelivery::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	ACharacter* Owner = Fixture.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("Character trail owner"), Owner)) { return false; }
	Owner->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Owner->GetCapsuleComponent()->SetCollisionResponseToAllChannels(ECR_Overlap);
	Owner->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	UQinglanCombatComponent* OwnerCombat = NewObject<UQinglanCombatComponent>(Owner);
	Owner->AddOwnedComponent(OwnerCombat);
	OwnerCombat->RegisterComponent();
	UQinglanDodgeComponent* Dodge = NewObject<UQinglanDodgeComponent>(Owner);
	Owner->AddOwnedComponent(Dodge);
	Dodge->RegisterComponent();
	TestTrue(TEXT("Actual dodge state starts"), Dodge->StartDodge(FVector::ForwardVector, 600.f, 0.45f, 0.1f, 0.28f));
	AActor* Reached = Fixture.Body(FVector(300.f, -70.f, 0.f));
	AActor* Blocked = Fixture.Body(FVector(300.f, 80.f, 0.f));
	Fixture.Wall(FVector(300.f, 40.f, 0.f), FVector(100.f, 5.f, 100.f));
	AQinglanProjectile* Trail = Fixture.World->SpawnActor<AQinglanProjectile>();
	Trail->SetupWetTrail(UQinglanCombatComponent::NextCastId(), TEXT("mist_step"), Owner, 100.f, 2.f);
	TestNull(TEXT("Pool intentionally has no AActor owner"), Trail->GetOwner());
	Owner->SetActorLocation(FVector(600.f, 0.f, 0.f));
	Trail->Tick(0.3f);
	TestTrue(TEXT("Swept middle of actual movement receives Wet without an owner crash"), CombatOf(Reached)->HasStatus(EQinglanStatus::Wet));
	TestEqual(TEXT("Wet bonus reaches authoritative duration"), CombatOf(Reached)->GetStatusRemaining(EQinglanStatus::Wet), 7.f);
	TestFalse(TEXT("Wall-separated target stays dry"), CombatOf(Blocked)->HasStatus(EQinglanStatus::Wet));
	TestFalse(TEXT("Caster never wets itself"), CombatOf(Owner)->HasStatus(EQinglanStatus::Wet));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanOrbitUniqueTargets,
	"Qinglan.Combat.OrbitPulseCountsUniqueVisibleVictims", CombatRegressionFlags)
bool FQinglanOrbitUniqueTargets::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	AActor* First = Fixture.Body(FVector(100.f, 0.f, 0.f), true);
	AActor* Second = Fixture.Body(FVector(0.f, 180.f, 0.f));
	AActor* Hidden = Fixture.Body(FVector(0.f, -150.f, 0.f));
	Fixture.Wall(FVector(0.f, -70.f, 0.f), FVector(60.f, 5.f, 100.f));
	AQinglanProjectile* Pulse = Fixture.World->SpawnActor<AQinglanProjectile>();
	Pulse->SetupOrbitPulses(UQinglanCombatComponent::NextCastId(), TEXT("orbiting_blades"), EQinglanElement::Metal,
		Owner, 10.f, 1, 0.5f, 350.f, 2, 1.f);
	Pulse->Tick(0.01f);
	TestEqual(TEXT("Two collision components take one hit"), CombatOf(First)->GetHealth(), 90.f);
	TestEqual(TEXT("Second unique visible target still receives its hit"), CombatOf(Second)->GetHealth(), 90.f);
	TestEqual(TEXT("Pulse does not hit through geometry"), CombatOf(Hidden)->GetHealth(), 100.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanProjectileRangeAndDodge,
	"Qinglan.Combat.ProjectileRangeAndDodgedContact", CombatRegressionFlags)
bool FQinglanProjectileRangeAndDodge::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	AActor* BeyondRange = Fixture.Body(FVector(300.f, 0.f, 0.f));
	AQinglanProjectile* Shot = Fixture.World->SpawnActor<AQinglanProjectile>();
	Shot->SetupLinear(UQinglanCombatComponent::NextCastId(), TEXT("jade_sword_arc"), EQinglanElement::Metal,
		Owner, FVector::ForwardVector, 2000.f, 100.f, 10.f, false, true, 1.f);
	Shot->Tick(0.2f);
	TestEqual(TEXT("A slow frame cannot extend projectile range"), CombatOf(BeyondRange)->GetHealth(), 100.f);
	TestFalse(TEXT("Range exhaustion returns the shot"), Shot->IsActive());
	BeyondRange->SetActorLocation(FVector(1000.f, 0.f, 0.f));
	AActor* InRange = Fixture.Body(FVector(150.f, 0.f, 0.f));
	Shot->SetupLinear(UQinglanCombatComponent::NextCastId(), TEXT("jade_sword_arc"), EQinglanElement::Metal,
		Owner, FVector::ForwardVector, 2000.f, 100.f, 10.f, false, true, 1.f);
	Shot->Tick(7.f);
	TestEqual(TEXT("A safety-crossing hitch still sweeps its in-range contact"), CombatOf(InRange)->GetHealth(), 90.f);
	TestEqual(TEXT("Safety-crossing movement remains range bounded"), CombatOf(BeyondRange)->GetHealth(), 100.f);
	InRange->SetActorLocation(FVector(1000.f, 1000.f, 0.f));
	AActor* Dodger = Fixture.Body(FVector(95.f, 0.f, 0.f));
	CombatOf(Dodger)->NotifyDodgeStarted(Fixture.World->GetTimeSeconds() - 0.2f);
	Shot->ResetForPool();
	Shot->SetupLinear(UQinglanCombatComponent::NextCastId(), TEXT("jade_sword_arc"), EQinglanElement::Metal,
		Owner, FVector::ForwardVector, 10.f, 100.f, 10.f, false, true, 1.f);
	Shot->Tick(0.1f);
	Fixture.World->TimeSeconds += 0.2f;
	Shot->Tick(0.1f);
	TestEqual(TEXT("Contact dodged once cannot hit again with a fabricated ordinal"), CombatOf(Dodger)->GetHealth(), 100.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanEnemyProjectileWallsAndReuse,
	"Qinglan.Combat.EnemyProjectileWallsAndReuse", CombatRegressionFlags)
bool FQinglanEnemyProjectileWallsAndReuse::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	UClass* EnemyClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanEnemyCharacter"));
	ACharacter* EnemyPawn = EnemyClass ? Fixture.World->SpawnActor<ACharacter>(EnemyClass) : nullptr;
	if (!TestNotNull(TEXT("Enemy caster"), EnemyPawn)) { return false; }
	AQinglanEnemyCharacter* Enemy = static_cast<AQinglanEnemyCharacter*>(EnemyPawn);
	Fixture.Wall(FVector(100.f, 0.f, 60.f), FVector(10.f, 100.f, 100.f));
	AQinglanEnemyProjectile* Shot = Fixture.World->SpawnActor<AQinglanEnemyProjectile>();
	UProjectileMovementComponent* Movement = Shot->FindComponentByClass<UProjectileMovementComponent>();
	if (!TestNotNull(TEXT("Projectile movement"), Movement)) { return false; }
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 1000.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->Tick(0.2f);
	TestFalse(TEXT("Wall consumes enemy shot"), Shot->IsInFlight());
	TestTrue(TEXT("Shot never crosses the wall"), Shot->GetActorLocation().X < 100.f);
	Shot->LaunchFrom(Enemy, FVector::RightVector, 1000.f, 10.f, 1000.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	AQinglanEnemyProjectile* SecondShot = Fixture.World->SpawnActor<AQinglanEnemyProjectile>();
	SecondShot->LaunchFrom(Enemy, FVector::RightVector, 1000.f, 10.f, 1000.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->Tick(0.1f);
	SecondShot->Tick(0.1f);
	TestTrue(TEXT("Reused shot resumes movement after StopSimulating"), Shot->GetActorLocation().Y > 50.f);
	TestTrue(TEXT("Two simultaneous bolts do not block one another at the muzzle"), SecondShot->GetActorLocation().Y > 50.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCultivationAndBlessingsRuntime,
	"Qinglan.Combat.CultivationBlessingsAndCheckpointAttributes", CombatRegressionFlags)
bool FQinglanCultivationAndBlessingsRuntime::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>();
	if (!TestNotNull(TEXT("Player"), Player)) { return false; }
	Player->DispatchBeginPlay();
	UQinglanCombatComponent* Combat = Player->GetCombat();
	UQinglanProfileSubsystem* Profile = Fixture.Instance->GetSubsystem<UQinglanProfileSubsystem>();
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	FQinglanProfileSnapshot ProfileState;
	ProfileState.CultivationStage = EQinglanStage::FoundationEstablishment;
	ProfileState.CultivationRank = 3;
	ProfileState.OwnedRelics = { TEXT("mountain_token") };
	ProfileState.EquippedRelic = TEXT("mountain_token");
	Profile->ImportStateFromSave(ProfileState);
	TestEqual(TEXT("Ranks, Foundation breakthrough, and relic add health"), Combat->GetMaxHealth(), 165.f);
	TestEqual(TEXT("Foundation adds maximum energy"), Combat->GetAttributes().MaxSpiritualEnergy, 120.f);
	TestEqual(TEXT("Six purchased ranks add technique power"), Combat->GetTechniquePower(), 32.f);
	Profile->ImportStateFromSave(ProfileState);
	TestEqual(TEXT("Repeated profile commit cannot stack attributes"), Combat->GetMaxHealth(), 165.f);
	AQinglanCharacter* FreshPlayer = Fixture.World->SpawnActor<AQinglanCharacter>(
		AQinglanCharacter::StaticClass(), FVector(3000.f, 0.f, 0.f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Fresh cultivated pawn"), FreshPlayer)) { return false; }
	FreshPlayer->DispatchBeginPlay();
	TestEqual(TEXT("Fresh pawn starts at its cultivated maximum health"), FreshPlayer->GetCombat()->GetHealth(), 165.f);
	TestEqual(TEXT("Fresh pawn starts at its cultivated maximum energy"), FreshPlayer->GetCombat()->GetAttributes().SpiritualEnergy, 120.f);
	FreshPlayer->Destroy();

	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = TEXT("bamboo_hollow");
	Checkpoint.RoomIndex = 2;
	Checkpoint.AcquiredBlessings = { TEXT("b_draining_edge"), TEXT("b_rooted_stance") };
	Checkpoint.CommittedRoomIds = { 0, 1, 2 };
	Checkpoint.PendingBlessingOffer = { TEXT("b_second_wind") };
	Checkpoint.PendingBlessingRoom = 2;
	Run->ImportCheckpoint(Checkpoint);
	TestTrue(TEXT("Actual blessing acceptance succeeds"), Run->AcceptBlessing(TEXT("b_second_wind")));
	TestEqual(TEXT("Health blessing applies immediately"), Combat->GetMaxHealth(), 180.f);
	Combat->RestoreCheckpointAttributes(Checkpoint.PlayerAttributes);
	TestEqual(TEXT("Old entrance snapshot cannot erase a newly chosen health blessing"), Combat->GetMaxHealth(), 180.f);
	FQinglanAttributes Vitals = Combat->GetAttributes();
	Vitals.SpiritualEnergy = 0.f;
	Vitals.Defense = 0.f;
	Combat->SetDerivedAttributes(Vitals);
	AActor* First = Fixture.Body(FVector(500.f, 0.f, 0.f));
	AActor* Second = Fixture.Body(FVector(700.f, 0.f, 0.f));
	Combat->NotifySwordSwingStarted();
	Combat->NotifySwordHitTarget(First);
	TestEqual(TEXT("Draining Edge grants six energy per target"), Combat->GetAttributes().SpiritualEnergy, 6.f);
	Combat->NotifySwordHitTarget(Second);
	TestEqual(TEXT("Draining Edge caps the swing at ten"), Combat->GetAttributes().SpiritualEnergy, 10.f);
	FQinglanDamageEvent Hit;
	Hit.Attacker = First;
	Hit.RawMagnitude = 20.f;
	Hit.CastId = UQinglanCombatComponent::NextCastId();
	const float HealthBefore = Combat->GetHealth();
	TestTrue(TEXT("Incoming hit resolves"), Combat->ApplyDamage(Hit));
	TestEqual(TEXT("Rooted Stance reduces actual incoming damage by ten percent"), Combat->GetHealth(), HealthBefore - 18.f);
	Run->ResetRun();
	TestEqual(TEXT("Run reset removes temporary maximum health exactly once"), Combat->GetMaxHealth(), 165.f);
	ProfileState.EquippedRelic = NAME_None;
	Profile->ImportStateFromSave(ProfileState);
	TestEqual(TEXT("Unequip after restoring snapshot removes only that relic bonus"), Combat->GetMaxHealth(), 145.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanEncounterCollectsLastBody,
	"Qinglan.Combat.EncounterCollectsAndRearmsLastEnemy", CombatRegressionFlags)
bool FQinglanEncounterCollectsLastBody::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	const FQinglanArchetypeSpec& Spec = QinglanEnemyCatalog::GetAllArchetypes()[0];
	UQinglanEnemyDataAsset* Data = QinglanEnemyCatalog::CreateDataAssetFromSpec(Spec, GetTransientPackage());
	AQinglanEncounterDirector* Director = Fixture.World->SpawnActor<AQinglanEncounterDirector>();
	Director->BeginEncounter({ Data }, 1, false);
	Director->Tick(0.01f);
	ACharacter* Enemy = nullptr;
	UClass* EnemyClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanEnemyCharacter"));
	for (TActorIterator<ACharacter> It(Fixture.World); It; ++It)
	{
		if (EnemyClass && It->GetClass()->IsChildOf(EnemyClass)) { Enemy = *It; break; }
	}
	if (!TestNotNull(TEXT("Portal creates enemy"), Enemy)
		|| !TestNotNull(TEXT("Enemy has controller"), Enemy->GetController())) { return false; }
	Enemy->GetController()->Tick(0.81f);
	Director->Tick(0.01f);
	TestEqual(TEXT("Materialized enemy participates in the encounter"), Director->GetLivingEnemyCount(), 1);
	FQinglanDamageEvent Lethal;
	Lethal.RawMagnitude = 10000.f;
	Lethal.CastId = UQinglanCombatComponent::NextCastId();
	TestTrue(TEXT("Actual damage kills the enemy"), CombatOf(Enemy)->ApplyDamage(Lethal));
	Director->Tick(0.01f);
	TestFalse(TEXT("Last kill completes the encounter"), Director->IsEncounterActive());
	Fixture.World->TimeSeconds += 3.f;
	Director->Tick(3.f);
	TestTrue(TEXT("Completed encounter still collects its last corpse"), Enemy->IsHidden());
	Director->BeginEncounter({ Data }, 2, false);
	Director->Tick(0.01f);
	Enemy->GetController()->Tick(0.81f);
	Director->Tick(0.01f);
	int32 EnemyCount = 0;
	for (TActorIterator<ACharacter> It(Fixture.World); It; ++It)
	{
		if (EnemyClass && It->GetClass()->IsChildOf(EnemyClass)) { ++EnemyCount; }
	}
	TestEqual(TEXT("Next wave reuses the existing actor"), EnemyCount, 1);
	TestFalse(TEXT("Reused enemy is visible"), Enemy->IsHidden());
	TestTrue(TEXT("Reused enemy can be hit and block movement again"), Enemy->GetCapsuleComponent()->IsQueryCollisionEnabled());
	TestTrue(TEXT("Reused enemy has fresh health"), CombatOf(Enemy)->IsAlive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanStatusAndRegenBoundaries,
	"Qinglan.Combat.StatusDurationAndRegenBoundaries", CombatRegressionFlags)
bool FQinglanStatusAndRegenBoundaries::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	UQinglanCombatComponent* Combat = CombatOf(Owner);
	UQinglanStatusComponent* Status = NewObject<UQinglanStatusComponent>(Owner);
	Owner->AddOwnedComponent(Status);
	Status->RegisterComponent();
	Combat->ApplyStatus(EQinglanStatus::Wet, 2.f);
	Combat->ApplyStatus(EQinglanStatus::Burn, 5.f);
	TestEqual(TEXT("HUD Wet duration includes duration bonus"), Status->GetStatusRemaining(EQinglanStatus::Wet), 7.f);
	TestEqual(TEXT("HUD Burn duration follows authoritative applied duration"), Status->GetStatusRemaining(EQinglanStatus::Burn), 5.f);
	Combat->RemoveStatus(EQinglanStatus::Burn);
	Combat->TrySpendEnergy(20.f);
	Combat->TrySpendStamina(20.f);
	Fixture.World->TimeSeconds += 1.1f;
	Combat->TickComponent(1.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Energy regen only counts the 0.1 seconds after its delay"), FMath::IsNearlyEqual(Combat->GetAttributes().SpiritualEnergy, 80.4f));
	TestTrue(TEXT("Stamina regen only counts the 0.3 seconds after its delay"), FMath::IsNearlyEqual(Combat->GetAttributes().Stamina, 86.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDepletedTalismansCanBeRemoved,
	"Qinglan.Combat.DepletedTalismanSlotsCanBeRemoved", CombatRegressionFlags)
bool FQinglanDepletedTalismansCanBeRemoved::RunTest(const FString& Parameters)
{
	FCombatRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	UQinglanProfileSubsystem* Profile = Fixture.Instance->GetSubsystem<UQinglanProfileSubsystem>();
	Profile->RefillTalismans();
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	UQinglanTalismanComponent* Talismans = NewObject<UQinglanTalismanComponent>(Owner);
	Owner->AddOwnedComponent(Talismans);
	Talismans->RegisterComponent();
	const TArray<FName> Equipped = Talismans->GetEquippedTalismans();
	if (!TestEqual(TEXT("Two charged slots are equipped"), Equipped.Num(), 2)) { return false; }
	for (FName Id : Equipped)
	{
		while (Profile->GetTalismanChargeCount(Id) > 0) { Profile->TryConsumeTalismanCharge(Id); }
	}
	TestTrue(TEXT("One exhausted slot can be removed while retaining the other"), Talismans->SetEquippedTalismans({ Equipped[1] }));
	TestFalse(TEXT("A depleted unequipped talisman cannot be added again"), Talismans->SetEquippedTalismans(Equipped));
	TestTrue(TEXT("The remaining exhausted slot can be removed"), Talismans->SetEquippedTalismans({}));
	return true;
}
#endif
