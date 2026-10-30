// Pooled enemies must stay physically parked until their portal materializes.
#include "AI/QinglanAIController.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEnemyProjectile.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanCharacter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VFX/QinglanVFXCueComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags EnemyLifecycleFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FEnemyLifecycleWorld
	{
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		FString SaveDirectory;
		FEnemyLifecycleWorld()
		{
			const FString Name = TEXT("L_EnemyLifecycle_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
			if (!GEngine) { return; }
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
		~FEnemyLifecycleWorld()
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
		UQinglanEnemyDataAsset* Data()
		{
			UQinglanEnemyDataAsset* CatalogData = QinglanEnemyCatalog::CreateDataAssetFromSpec(
				QinglanEnemyCatalog::GetAllArchetypes()[0], GetTransientPackage());
			UQinglanEnemyDataAsset* Result = CatalogData
				? static_cast<UQinglanEnemyDataAsset*>(StaticDuplicateObject(CatalogData, GetTransientPackage())) : nullptr;
			if (Result)
			{
				Result->Mesh.Reset();
				Result->IdleAnimation.Reset();
				Result->AttackMontage.Reset();
				Result->HitReactMontage.Reset();
			}
			return Result;
		}
	};

	AQinglanAIController* ControllerFor(AQinglanEnemyCharacter* Enemy)
	{
		AQinglanAIController* Controller = Enemy ? Cast<AQinglanAIController>(Enemy->GetController()) : nullptr;
		if (!Controller && Enemy)
		{
			Controller = Enemy->GetWorld()->SpawnActor<AQinglanAIController>();
			if (Controller) { Controller->Possess(Enemy); }
		}
		return Controller;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanPortalMovementFreeze,
	"Qinglan.AI.PortalAndPoolFreezeCharacterMovement", EnemyLifecycleFlags)
bool FQinglanPortalMovementFreeze::RunTest(const FString& Parameters)
{
	FEnemyLifecycleWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>(
		AQinglanEnemyCharacter::StaticClass(), FVector(0.f, 0.f, 500.f), FRotator::ZeroRotator);
	UQinglanEnemyDataAsset* Data = Fixture.Data();
	if (!TestNotNull(TEXT("Enemy"), Enemy) || !TestNotNull(TEXT("Data"), Data)) { return false; }
	Enemy->InitializeFromData(Data);
	AQinglanAIController* Controller = ControllerFor(Enemy);
	UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
	if (!TestNotNull(TEXT("Controller"), Controller) || !TestNotNull(TEXT("Movement"), Movement)) { return false; }
	Controller->Initialize(nullptr, 73);

	// Establish that this fixture really consumes deferred LaunchCharacter
	// velocity; a disabled/no-physics fixture must not produce a false pass.
	Enemy->LaunchCharacter(FVector(800.f, 0.f, 100.f), true, true);
	Movement->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Control launch moves the active enemy"), Enemy->GetActorLocation().X > 1.f);

	const FVector PortalLocation(0.f, 0.f, 500.f);
	Enemy->SetActorLocation(PortalLocation);
	Enemy->LaunchCharacter(FVector(1400.f, 0.f, -300.f), true, true);
	Enemy->SetActorHiddenInGame(true);
	Enemy->SetActorEnableCollision(false);
	Enemy->SetActorTickEnabled(false);
	Controller->BeginPortalWarning(0.8f);
	Movement->TickComponent(0.3f, LEVELTICK_All, nullptr);
	Controller->Tick(0.4f);
	Movement->TickComponent(0.3f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Portal is still warning"), Controller->IsPortalPending());
	TestTrue(TEXT("Independent movement ticks cannot displace a hidden portal pawn"), Enemy->GetActorLocation().Equals(PortalLocation, 0.01f));
	Controller->Tick(0.5f);
	TestFalse(TEXT("Portal materializes normally"), Controller->IsPortalPending());
	TestTrue(TEXT("Materialization restores walking"), Movement->MovementMode == MOVE_Walking);
	Movement->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Old queued lunge cannot reappear after materialization"), FMath::IsNearlyZero(Enemy->GetActorLocation().X, 0.01));

	Enemy->LaunchCharacter(FVector(1400.f, 0.f, 0.f), true, true);
	Enemy->ResetForPool();
	const FVector PooledLocation = Enemy->GetActorLocation();
	Movement->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Pooled pawns stay parked despite movement-component ticks"), Enemy->GetActorLocation().Equals(PooledLocation, 0.01f));
	Enemy->InitializeFromData(Data);
	Controller->Initialize(nullptr, 73);
	Controller->BeginPortalWarning(0.01f);
	Controller->Tick(0.02f);
	Enemy->LaunchCharacter(FVector(400.f, 0.f, 100.f), true, true);
	Movement->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("A reused materialized enemy accepts new motion"), Enemy->GetActorLocation().X > PooledLocation.X + 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanEnemyDeathPoolPresentation,
	"Qinglan.AI.DeathAndReuseClearPendingPresentation", EnemyLifecycleFlags)
bool FQinglanEnemyDeathPoolPresentation::RunTest(const FString& Parameters)
{
	FEnemyLifecycleWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>();
	UQinglanEnemyDataAsset* Data = Fixture.Data();
	if (!TestNotNull(TEXT("Enemy"), Enemy) || !TestNotNull(TEXT("Data"), Data)) { return false; }
	Enemy->InitializeFromData(Data);
	AQinglanAIController* Controller = ControllerFor(Enemy);
	if (!TestNotNull(TEXT("Controller"), Controller)) { return false; }
	Controller->Initialize(nullptr, 73);
	UQinglanVFXCueComponent* Cues = Enemy->VFXCues;
	if (!TestNotNull(TEXT("Presenter"), Cues)) { return false; }
	Cues->PlayTelegraph(FVector(300.f, 0.f, 0.f), 200.f, 10.f, EQinglanElement::Fire);
	TestTrue(TEXT("The old life owns an active warning"), Cues->GetActiveCueCount() > 0);
	Enemy->LaunchCharacter(FVector(1400.f, 0.f, 100.f), true, true);
	FQinglanDamageEvent StaggerHit;
	StaggerHit.Target = Enemy;
	StaggerHit.CastId = UQinglanCombatComponent::NextCastId();
	StaggerHit.RawMagnitude = 20.f;
	Enemy->EnemyCombat->ApplyDamage(StaggerHit);
	TestTrue(TEXT("A real nonlethal hit staggers the ordinary enemy"), Enemy->GetAIState() == EQinglanAIState::Stagger);
	TestEqual(TEXT("Interrupting a windup removes its canceled warning"), Cues->GetActiveCueCount(), 0);
	const double StaggerX = Enemy->GetActorLocation().X;
	Enemy->GetCharacterMovement()->TickComponent(0.1f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Stagger cancels a queued lunge"), FMath::IsNearlyEqual(Enemy->GetActorLocation().X, StaggerX, 0.01));
	Cues->PlayTelegraph(FVector(300.f, 0.f, 0.f), 200.f, 10.f, EQinglanElement::Fire);
	TestTrue(TEXT("The old controller owns an active projectile"), Controller->SpawnPooledProjectileInDirection(
		10.f, 1000.f, Data->EnemyId, NAME_None, FVector::ForwardVector, 1000.f));
	Enemy->ResetForPool();
	TestEqual(TEXT("Restarting an encounter immediately removes old warning geometry"), Cues->GetActiveCueCount(), 0);
	for (TActorIterator<AQinglanEnemyProjectile> It(Fixture.World); It; ++It)
	{
		TestFalse(TEXT("Reset also retires the controller's pending shots"), It->IsInFlight());
	}

	Enemy->InitializeFromData(Data);
	Controller->Initialize(nullptr, 73);
	Controller->BeginPortalWarning(0.01f);
	Controller->Tick(0.02f);
	Cues->PlayTelegraph(FVector(600.f, 0.f, 0.f), 200.f, 10.f, EQinglanElement::Fire);
	Enemy->LaunchCharacter(FVector(1400.f, 0.f, 100.f), true, true);
	FQinglanDamageEvent Hit;
	Hit.Target = Enemy;
	Hit.CastId = UQinglanCombatComponent::NextCastId();
	Hit.RawMagnitude = 10000.f;
	TestTrue(TEXT("Real lethal damage resolves"), Enemy->EnemyCombat->ApplyDamage(Hit));
	TestFalse(TEXT("The old life is dead"), Enemy->IsAlive());
	TestEqual(TEXT("Death removes canceled warnings immediately"), Cues->GetActiveCueCount(), 0);
	const FVector DeathLocation = Enemy->GetActorLocation();
	Enemy->GetCharacterMovement()->TickComponent(0.3f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("A queued lunge cannot move the dead pawn"), Enemy->GetActorLocation().Equals(DeathLocation, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanChargeUsesContact,
	"Qinglan.AI.ChargeRequiresSweptCapsuleContact", EnemyLifecycleFlags)
bool FQinglanChargeUsesContact::RunTest(const FString& Parameters)
{
	FEnemyLifecycleWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	const FQinglanArchetypeSpec* Authored = QinglanEnemyCatalog::FindSpec(TEXT("ashen_guardian"));
	if (!TestNotNull(TEXT("Ashen archetype"), Authored)) { return false; }
	FQinglanArchetypeSpec Spec = *Authored;
	Spec.Patterns.RemoveAll([](const FQinglanBossPattern& Pattern) { return Pattern.PatternId != QinglanPatternIds::ChargingStrike(); });
	UQinglanEnemyDataAsset* CatalogData = QinglanEnemyCatalog::CreateDataAssetFromSpec(*Authored, GetTransientPackage());
	UQinglanEnemyDataAsset* Data = CatalogData
		? static_cast<UQinglanEnemyDataAsset*>(StaticDuplicateObject(CatalogData, GetTransientPackage())) : nullptr;
	if (!TestNotNull(TEXT("Charge-only test archetype"), Data) || !TestEqual(TEXT("One authored charge"), Spec.Patterns.Num(), 1)) { return false; }
	static_cast<UQinglanBossDataAsset*>(Data)->Patterns = Spec.Patterns;
	Data->Mesh.Reset();
	Data->IdleAnimation.Reset();
	Data->AttackMontage.Reset();
	Data->HitReactMontage.Reset();
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>(
		AQinglanEnemyCharacter::StaticClass(), FVector(0.f, 0.f, 500.f), FRotator::ZeroRotator);
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>(
		AQinglanCharacter::StaticClass(), FVector(400.f, 0.f, 500.f), FRotator::ZeroRotator);
	APlayerController* PlayerController = Fixture.World->SpawnActor<APlayerController>();
	AQinglanEncounterDirector* Director = Fixture.World->SpawnActor<AQinglanEncounterDirector>();
	if (!TestNotNull(TEXT("Enemy"), Enemy) || !TestNotNull(TEXT("Player"), Player)
		|| !TestNotNull(TEXT("Player controller"), PlayerController) || !TestNotNull(TEXT("Director"), Director)) { return false; }
	PlayerController->Possess(Player);
	FQinglanAttributes Attributes;
	Attributes.Defense = 0.f;
	Player->GetCombat()->SetDerivedAttributes(Attributes);
	Enemy->InitializeFromData(Data);
	AQinglanAIController* Controller = ControllerFor(Enemy);
	if (!TestNotNull(TEXT("Enemy controller"), Controller)) { return false; }
	Controller->Initialize(Director, 73);
	Controller->Tick(0.2f);
	TestTrue(TEXT("Real sensing acquires the player"), Controller->HasTarget());
	Enemy->Tick(0.6f); // idle -> acquire
	Enemy->Tick(0.2f); // acquire -> approach
	Enemy->Tick(0.2f); // approach -> windup, with a real arbitration slot
	if (!TestTrue(TEXT("The production state machine starts its charge windup"), Enemy->GetAIState() == EQinglanAIState::Windup)) { return false; }
	Enemy->Tick(Spec.Patterns[0].TelegraphDuration + 0.01f);
	if (!TestTrue(TEXT("The production state machine commits the charge"), Enemy->GetAIState() == EQinglanAIState::Attack)) { return false; }

	// Remain well inside the boss's attack-start radius, but step off its
	// actual path. A radial active check previously hit this clear sidestep.
	Player->SetActorLocation(FVector(0.f, 300.f, 500.f));
	Enemy->Tick(0.01f);
	TestEqual(TEXT("Sidestepping the charging body avoids damage"), Player->GetCombat()->GetHealth(), 100.f);

	// The capsule passes the target between two attack samples. Neither end
	// overlaps the target, so this also checks the continuous swept contact.
	Player->SetActorLocation(FVector(130.f, 0.f, 500.f));
	Enemy->SetActorLocation(FVector(300.f, 0.f, 500.f));
	Enemy->Tick(0.01f);
	const float HealthAfterContact = Player->GetCombat()->GetHealth();
	TestTrue(TEXT("A genuine crossed contact lands even between sampled endpoints"), HealthAfterContact < 100.f);
	Enemy->Tick(0.01f);
	TestEqual(TEXT("One committed charge can damage the victim only once"), Player->GetCombat()->GetHealth(), HealthAfterContact);
	return true;
}
#endif
