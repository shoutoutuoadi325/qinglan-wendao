// Combat timing regressions use real component state and swept collisions.
#include "Abilities/QinglanTechniqueTypes.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEnemyProjectile.h"
#include "Camera/CameraComponent.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags CombatTimingFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FCombatTimingWorld
	{
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		FString SaveDirectory;

		FCombatTimingWorld()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("_CombatTiming_") + Id);
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
			if (!GEngine) { return; }
			const FString Name = TEXT("L_CombatTiming_") + Id;
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

		~FCombatTimingWorld()
		{
			UQinglanTechniqueComponent::SetTechniqueCatalogForTesting({});
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

		AActor* Body(const FVector& Location)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			if (!Actor) { return nullptr; }
			USphereComponent* Sphere = NewObject<USphereComponent>(Actor);
			Actor->AddOwnedComponent(Sphere);
			Actor->SetRootComponent(Sphere);
			Sphere->InitSphereRadius(20.f);
			Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Sphere->SetCollisionObjectType(ECC_Pawn);
			Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
			Sphere->RegisterComponent();
			Actor->SetActorLocation(Location);
			UQinglanCombatComponent* Combat = NewObject<UQinglanCombatComponent>(Actor);
			Actor->AddOwnedComponent(Combat);
			Combat->RegisterComponent();
			FQinglanAttributes Attributes;
			Attributes.Defense = 0.f;
			Combat->SetDerivedAttributes(Attributes);
			return Actor;
		}

		UQinglanSwordComponent* Sword(AActor* Owner)
		{
			UQinglanSwordComponent* Component = NewObject<UQinglanSwordComponent>(Owner);
			Owner->AddOwnedComponent(Component);
			Component->SwingMontageSoftPaths.Reset();
			Component->RegisterComponent();
			return Component;
		}

		AActor* AimWall()
		{
			AActor* Wall = World->SpawnActor<AActor>();
			UBoxComponent* Box = NewObject<UBoxComponent>(Wall);
			Wall->AddOwnedComponent(Box);
			Wall->SetRootComponent(Box);
			Box->InitBoxExtent(FVector(10.f, 500.f, 1000.f));
			Box->SetCollisionObjectType(ECC_WorldStatic);
			Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Box->SetCollisionResponseToAllChannels(ECR_Block);
			Box->RegisterComponent();
			Wall->SetActorLocation(FVector(1000.f, 0.f, 0.f));
			return Wall;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSwordPhaseCarry,
	"Qinglan.Combat.SwordCarriesTimeAcrossPhaseBoundaries", CombatTimingFlags)
bool FQinglanSwordPhaseCarry::RunTest(const FString& Parameters)
{
	FCombatTimingWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AActor* Owner = Fixture.Body(FVector::ZeroVector);
	AActor* Victim = Fixture.Body(FVector::ZeroVector);
	UQinglanSwordComponent* Sword = Fixture.Sword(Owner);
	TestTrue(TEXT("Cut one starts"), Sword->RequestAttack());
	Sword->TickComponent(0.35f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("A 350ms frame reaches recovery"), Sword->GetPhase() == EQinglanSwordPhase::Recovery);
	TestTrue(TEXT("Recovery has already passed the 100ms dodge lockout"), Sword->CanDodgeCancelNow());
	TestEqual(TEXT("An active phase crossed in one frame still hits"), Victim->FindComponentByClass<UQinglanCombatComponent>()->GetHealth(), 84.f);
	Sword->TickComponent(0.15f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("The authored 460ms swing is finished at 500ms"), Sword->GetPhase() == EQinglanSwordPhase::Idle);
	TestEqual(TEXT("The swing never repeats its damage"), Victim->FindComponentByClass<UQinglanCombatComponent>()->GetHealth(), 84.f);

	AActor* SmoothOwner = Fixture.Body(FVector(1000.f, 0.f, 0.f));
	AActor* SmoothVictim = Fixture.Body(FVector(1000.f, 0.f, 0.f));
	UQinglanSwordComponent* SmoothSword = Fixture.Sword(SmoothOwner);
	SmoothSword->RequestAttack();
	for (int32 Tick = 0; Tick < 50; ++Tick) { SmoothSword->TickComponent(0.01f, LEVELTICK_All, nullptr); }
	TestTrue(TEXT("Fine and coarse frames end in the same phase"), SmoothSword->GetPhase() == Sword->GetPhase());
	TestEqual(TEXT("Fine and coarse frames deal the same damage"),
		SmoothVictim->FindComponentByClass<UQinglanCombatComponent>()->GetHealth(), Victim->FindComponentByClass<UQinglanCombatComponent>()->GetHealth());
	Sword->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("New input starts after the combo window expires"), Sword->RequestAttack());
	TestEqual(TEXT("The combo window uses the actual recovery boundary"), Sword->GetComboIndex(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSwordBufferBoundary,
	"Qinglan.Combat.SwordBufferUsesRecoveryBoundary", CombatTimingFlags)
bool FQinglanSwordBufferBoundary::RunTest(const FString& Parameters)
{
	FCombatTimingWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	UQinglanSwordComponent* Sword = Fixture.Sword(Fixture.Body(FVector::ZeroVector));
	Sword->RequestAttack();
	Sword->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("Windup input waits in the buffer"), Sword->RequestAttack());
	Sword->TickComponent(0.3f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("A press 140ms before recovery survives a 300ms frame"), Sword->GetComboIndex(), 1);
	TestTrue(TEXT("Remaining frame time advances the chained cut"), Sword->GetPhase() != EQinglanSwordPhase::Recovery);

	Sword->InterruptSwing();
	Sword->RequestAttack();
	Sword->RequestAttack();
	Sword->TickComponent(0.4f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("A press already stale at recovery does not chain"), Sword->GetComboIndex(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanEnemyProjectileBoundedStep,
	"Qinglan.Combat.EnemyProjectileMovesOnlyItsRemainingRange", CombatTimingFlags)
bool FQinglanEnemyProjectileBoundedStep::RunTest(const FString& Parameters)
{
	FCombatTimingWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	UClass* EnemyClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanEnemyCharacter"));
	if (!TestNotNull(TEXT("Enemy class"), EnemyClass)) { return false; }
	ACharacter* EnemyPawn = Fixture.World->SpawnActor<ACharacter>(EnemyClass, FVector::ZeroVector, FRotator::ZeroRotator);
	AQinglanEnemyProjectile* Shot = Fixture.World->SpawnActor<AQinglanEnemyProjectile>();
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>(
		AQinglanCharacter::StaticClass(), FVector(500.f, 0.f, 60.f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Enemy pawn"), EnemyPawn) || !TestNotNull(TEXT("Shot"), Shot) || !TestNotNull(TEXT("Player"), Player)) { return false; }
	FQinglanAttributes Attributes;
	Attributes.Defense = 0.f;
	Player->GetCombat()->SetDerivedAttributes(Attributes);
	AQinglanEnemyCharacter* Enemy = static_cast<AQinglanEnemyCharacter*>(EnemyPawn);
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 100.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	const FVector ShotStart = Shot->GetActorLocation();
	Shot->Tick(0.5f);
	// ProjectileMovement consumes float seconds in a double-precision world.
	// Verify its measured travel to 0.01 cm, including the owner-relative muzzle.
	const double Travel = FVector::Dist(ShotStart, Shot->GetActorLocation());
	TestTrue(FString::Printf(TEXT("Slow frame reaches the 100 cm range endpoint (travel %.9f cm)"), Travel),
		FMath::IsNearlyEqual(Travel, 100.0, 0.01));
	TestFalse(TEXT("Range exhaustion retires the shot"), Shot->IsInFlight());
	TestEqual(TEXT("A slow frame never hits a player beyond range"), Player->GetCombat()->GetHealth(), 100.f);

	Player->SetActorLocation(FVector(130.f, 0.f, 60.f));
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 100.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->Tick(0.5f);
	TestEqual(TEXT("The final in-range segment can still hit"), Player->GetCombat()->GetHealth(), 90.f);

	EnemyPawn->SetActorLocation(FVector(0.f, 1000.f, 0.f));
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 250.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->Tick(0.3f);
	const FVector CoarseEndpoint = Shot->GetActorLocation();
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 250.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	for (int32 Tick = 0; Tick < 3; ++Tick) { Shot->Tick(0.1f); }
	TestTrue(TEXT("Frame partition does not change projectile reach"), Shot->GetActorLocation().Equals(CoarseEndpoint, 0.01f));
	TestFalse(TEXT("Partitioned flight also retires at range"), Shot->IsInFlight());
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 1000.f, 10.f, 100000.f, UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->Tick(10.f);
	TestTrue(TEXT("The lifetime cap also bounds the last movement step"), FMath::IsNearlyEqual(Shot->GetActorLocation().X, 6000.f));
	TestFalse(TEXT("Long-range shots retire at the six-second cap"), Shot->IsInFlight());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTechniqueReleaseValidation,
	"Qinglan.Combat.TechniquesRevalidateTargetAndRootBeforeRelease", CombatTimingFlags)
bool FQinglanTechniqueReleaseValidation::RunTest(const FString& Parameters)
{
	FCombatTimingWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>();
	if (!TestNotNull(TEXT("Player"), Player)) { return false; }
	// Unpossessed characters start with movement disabled. Establish the
	// walking state whose Root/disabled-movement transitions this test varies.
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	Movement->bRunPhysicsWithNoController = true;
	Movement->SetMovementMode(MOVE_Walking);
	if (!TestTrue(TEXT("Release-validation caster can initially move"), Movement->IsMovingOnGround())) { return false; }
	UQinglanTechniqueComponent* Techniques = Player->FindComponentByClass<UQinglanTechniqueComponent>();
	UCameraComponent* Camera = Player->FindComponentByClass<UCameraComponent>();
	if (!TestNotNull(TEXT("Techniques"), Techniques) || !TestNotNull(TEXT("Camera"), Camera)) { return false; }
	Camera->SetWorldLocationAndRotation(FVector(0.f, 0.f, 200.f), FRotator::ZeroRotator);
	UClass* TechniqueClass = FindObject<UClass>(nullptr, TEXT("/Script/QinglanAscension.QinglanTechniqueDataAsset"));
	if (!TestNotNull(TEXT("Technique data class"), TechniqueClass)) { return false; }
	UQinglanTechniqueDataAsset* Strike = static_cast<UQinglanTechniqueDataAsset*>(NewObject<UObject>(GetTransientPackage(), TechniqueClass));
	Strike->TechniqueId = TEXT("thunder_seal");
	Strike->Targeting = EQinglanTargeting::GroundStrike;
	Strike->EnergyCost = 24;
	Strike->CooldownSeconds = 7.f;
	Strike->Phases.Anticipation = 0.1f;
	UQinglanTechniqueDataAsset* Dash = static_cast<UQinglanTechniqueDataAsset*>(NewObject<UObject>(GetTransientPackage(), TechniqueClass));
	Dash->TechniqueId = TEXT("mist_step");
	Dash->Targeting = EQinglanTargeting::SelfDash;
	Dash->EnergyCost = 15;
	Dash->CooldownSeconds = 9.f;
	Dash->Phases.Anticipation = 0.1f;
	UQinglanTechniqueComponent::SetTechniqueCatalogForTesting({ Strike, Dash });
	TestTrue(TEXT("Regression techniques equipped"), Techniques->ApplyEquippedTechniqueIds({ Strike->TechniqueId, Dash->TechniqueId }));
	UQinglanCombatComponent* Combat = Player->GetCombat();
	const float InitialEnergy = Combat->GetAttributes().SpiritualEnergy;
	AActor* Wall = Fixture.AimWall();
	TestTrue(TEXT("In-range ground target starts anticipation"), Techniques->TryCast(0));
	Wall->Destroy();
	Fixture.World->TimeSeconds += 0.2f;
	Techniques->TickComponent(0.2f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("Losing the valid release aim cancels the cast"), Techniques->IsCasting());
	TestEqual(TEXT("Invalid release aim spends no energy"), Combat->GetAttributes().SpiritualEnergy, InitialEnergy);
	TestEqual(TEXT("Invalid release aim starts no cooldown"), Techniques->GetCooldownRemaining(Strike->TechniqueId), 0.f);

	Combat->ApplyStatus(EQinglanStatus::Root, 1.f);
	TestFalse(TEXT("Mist Step cannot begin while rooted"), Techniques->TryCast(1));
	Combat->RemoveStatus(EQinglanStatus::Root);
	TestTrue(TEXT("Mist Step begins after Root is removed"), Techniques->TryCast(1));
	Combat->ApplyStatus(EQinglanStatus::Root, 1.f);
	Fixture.World->TimeSeconds += 0.2f;
	Techniques->TickComponent(0.2f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("Root acquired during anticipation blocks dash release"), Techniques->IsCasting());
	TestEqual(TEXT("A newly rooted dash spends no energy"), Combat->GetAttributes().SpiritualEnergy, InitialEnergy);
	TestEqual(TEXT("A newly rooted dash starts no cooldown"), Techniques->GetCooldownRemaining(Dash->TechniqueId), 0.f);
	Combat->RemoveStatus(EQinglanStatus::Root);
	const EMovementMode OriginalMode = Movement->MovementMode;
	const float OriginalWalkSpeed = Movement->MaxWalkSpeed;
	Movement->DisableMovement();
	TestFalse(TEXT("Disabled movement rejects Mist Step before anticipation"), Techniques->TryCast(1));
	Movement->SetMovementMode(OriginalMode);
	TestTrue(TEXT("Movable Mist Step starts anticipation"), Techniques->TryCast(1));
	Movement->DisableMovement();
	Fixture.World->TimeSeconds += 0.2f;
	Techniques->TickComponent(0.2f, LEVELTICK_All, nullptr);
	TestFalse(TEXT("Movement disabled during anticipation cancels Mist Step"), Techniques->IsCasting());
	TestEqual(TEXT("Rejected movement spends no energy"), Combat->GetAttributes().SpiritualEnergy, InitialEnergy);
	TestEqual(TEXT("Rejected movement starts no cooldown"), Techniques->GetCooldownRemaining(Dash->TechniqueId), 0.f);
	TestEqual(TEXT("Rejected movement preserves walk speed"), Movement->MaxWalkSpeed, OriginalWalkSpeed);
	Movement->SetMovementMode(OriginalMode);
	Camera->SetWorldRotation(FRotator(90.f, 0.f, 0.f));
	TestFalse(TEXT("Vertical-only aim rejects a planar dash"), Techniques->TryCast(1));
	TestEqual(TEXT("Invalid dash aim spends no energy"), Combat->GetAttributes().SpiritualEnergy, InitialEnergy);
	TestEqual(TEXT("Invalid dash aim starts no cooldown"), Techniques->GetCooldownRemaining(Dash->TechniqueId), 0.f);
	return true;
}
#endif
