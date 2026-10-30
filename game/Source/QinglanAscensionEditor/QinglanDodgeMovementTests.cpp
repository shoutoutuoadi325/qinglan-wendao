// Exercise dodge through the real CharacterMovement and world tick scheduler.
// Direct component ticks cannot detect frame integration, collision, or ordering bugs.
#include "Abilities/QinglanProjectile.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEnemyProjectile.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "CoreGlobals.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags DodgeMovementFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	// UE adds KINDA_SMALL_NUMBER to a source's partial terminal tick.
	constexpr double DistanceToleranceCm = 0.5;

	struct FDodgeMovementWorld
	{
		const uint64 SavedFrameCounter = GFrameCounter;
		FString Name = TEXT("L_DodgeMovement_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;

		FDodgeMovementWorld()
		{
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
			if (!GEngine) { return; }
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			if (!World) { return; }
			FURL URL;
			URL.AddOption(TEXT("game=/Script/Engine.GameModeBase"));
			World->SetGameMode(URL);
			World->InitializeActorsForPlay(URL);
			World->BeginPlay();
			World->SetBegunPlay(true);
		}

		~FDodgeMovementWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			GFrameCounter = SavedFrameCounter;
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
			IFileManager::Get().DeleteDirectory(*SaveDirectory, false, true);
		}

		AActor* Box(const FVector& Location, const FVector& Extent)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			if (!Actor) { return nullptr; }
			UBoxComponent* Shape = NewObject<UBoxComponent>(Actor);
			Actor->AddOwnedComponent(Shape);
			Actor->SetRootComponent(Shape);
			Shape->InitBoxExtent(Extent);
			Shape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Shape->SetCollisionObjectType(ECC_WorldStatic);
			Shape->SetCollisionResponseToAllChannels(ECR_Block);
			Shape->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}

		void Floor()
		{
			Box(FVector(0.f, 0.f, -5.f), FVector(2000.f, 2000.f, 5.f));
		}

		ACharacter* Character(const FVector& Location, bool bProductionPlayer = false)
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			UClass* Class = bProductionPlayer ? AQinglanCharacter::StaticClass() : ACharacter::StaticClass();
			ACharacter* Pawn = World->SpawnActor<ACharacter>(Class, Location, FRotator::ZeroRotator, Spawn);
			if (!Pawn) { return nullptr; }
			Pawn->GetCapsuleComponent()->SetCapsuleSize(34.f, 88.f);
			Pawn->GetCapsuleComponent()->SetGenerateOverlapEvents(true);
			Pawn->GetMesh()->SetComponentTickEnabled(false);
			UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement();
			Movement->bRunPhysicsWithNoController = true;
			Movement->GroundFriction = 8.f;
			Movement->BrakingDecelerationWalking = 2048.f;
			Movement->MaxWalkSpeed = 400.f;
			Movement->SetMovementMode(MOVE_Walking);
			if (!Pawn->FindComponentByClass<UQinglanDodgeComponent>())
			{
				UQinglanDodgeComponent* Dodge = NewObject<UQinglanDodgeComponent>(Pawn);
				Pawn->AddOwnedComponent(Dodge);
				Dodge->RegisterComponent();
			}
			return Pawn;
		}

		UQinglanCombatComponent* Target(const FVector& Location)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			if (!Actor) { return nullptr; }
			USphereComponent* Sphere = NewObject<USphereComponent>(Actor);
			Actor->AddOwnedComponent(Sphere);
			Actor->SetRootComponent(Sphere);
			Sphere->InitSphereRadius(10.f);
			Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Sphere->SetCollisionObjectType(ECC_Pawn);
			Sphere->SetCollisionResponseToAllChannels(ECR_Overlap);
			Sphere->RegisterComponent();
			Actor->SetActorLocation(Location);
			UQinglanCombatComponent* Combat = NewObject<UQinglanCombatComponent>(Actor);
			Actor->AddOwnedComponent(Combat);
			Combat->RegisterComponent();
			return Combat;
		}

		void Tick(float Seconds) const
		{
			World->Tick(LEVELTICK_All, Seconds);
			// Match UE's FTestWorldWrapper: synchronous automation frames must
			// advance the scheduler's global frame, including while paused.
			++GFrameCounter;
		}
	};

	UQinglanDodgeComponent* DodgeOf(AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UQinglanDodgeComponent>() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDodgeFrameIndependent,
	"Qinglan.Combat.DodgeDistanceAcrossFrameRatesAndTerminalHitch", DodgeMovementFlags)
bool FQinglanDodgeFrameIndependent::RunTest(const FString& Parameters)
{
	for (const int32 FrameRate : { 30, 60, 120, 0 })
	for (const bool bAirborne : { false, true })
	{
		FDodgeMovementWorld Fixture;
		if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
		Fixture.Floor();
		ACharacter* Pawn = Fixture.Character(FVector(0.f, 0.f, bAirborne ? 1000.f : 92.f));
		UQinglanDodgeComponent* Dodge = DodgeOf(Pawn);
		if (!TestNotNull(TEXT("Registered dodge"), Dodge)) { return false; }
		Fixture.Tick(0.05f);
		UCharacterMovementComponent* Movement = Pawn->GetCharacterMovement();
		if (bAirborne)
		{
			Movement->SetMovementMode(MOVE_Falling);
			Movement->Velocity.Z = 100.f; // apex falls between frames at every tested rate
		}
		else { TestTrue(TEXT("Fixture is walking on the real floor"), Movement->IsMovingOnGround()); }
		const FVector Start = Pawn->GetActorLocation();
		TestTrue(TEXT("Dodge begins"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
		for (int32 Frame = 0; Dodge->IsDodging() && Frame < 100; ++Frame)
		{
			Pawn->AddMovementInput(FVector(-1.f, 1.f, 0.f).GetSafeNormal(), 1.f, true);
			Fixture.Tick(FrameRate > 0 ? 1.f / FrameRate : (Frame == 0 ? 0.35f : 0.25f));
		}
		const FString Case = (FrameRate > 0 ? FString::Printf(TEXT("%d FPS"), FrameRate) : FString(TEXT("Terminal hitch")))
			+ (bAirborne ? TEXT(" across jump apex") : TEXT(" on ground"));
		const FVector End = Pawn->GetActorLocation();
		TestFalse(Case + TEXT(" finishes"), Dodge->IsDodging());
		TestTrue(Case + TEXT(" covers authored distance"), FMath::IsNearlyEqual(End.X - Start.X, 450.0, DistanceToleranceCm));
		TestTrue(Case + TEXT(" resists held sideways input"), FMath::IsNearlyZero(End.Y - Start.Y, DistanceToleranceCm));
		TestTrue(Case + TEXT(" clears planar momentum"), Movement->Velocity.SizeSquared2D() < 0.01);
		Fixture.Tick(0.2f);
		TestTrue(Case + TEXT(" has no drift after completion"), FVector::Dist2D(End, Pawn->GetActorLocation()) < DistanceToleranceCm);
		TestTrue(Case + TEXT(" keeps normal movement enabled"), Movement->IsComponentTickEnabled()
			&& (bAirborne ? Movement->IsFalling() : Movement->IsMovingOnGround()));
		TestEqual(Case + TEXT(" preserves walk speed"), Movement->MaxWalkSpeed, 400.f);
		Pawn->AddMovementInput(FVector::RightVector, 1.f, true);
		Fixture.Tick(0.1f);
		TestTrue(Case + TEXT(" accepts movement after dodge"), Pawn->GetActorLocation().Y > End.Y + 0.1f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDodgeWorldGeometry,
	"Qinglan.Combat.DodgeRetainsWallsStepsAndGravity", DodgeMovementFlags)
bool FQinglanDodgeWorldGeometry::RunTest(const FString& Parameters)
{
	FDodgeMovementWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	Fixture.Floor();
	Fixture.Box(FVector(200.f, -400.f, 150.f), FVector(10.f, 100.f, 150.f));
	Fixture.Box(FVector(200.f, 400.f, 15.f), FVector(60.f, 100.f, 15.f));
	ACharacter* WallPawn = Fixture.Character(FVector(0.f, -400.f, 92.f));
	ACharacter* StepPawn = Fixture.Character(FVector(0.f, 400.f, 92.f));
	ACharacter* FallingPawn = Fixture.Character(FVector(0.f, 0.f, 600.f));
	if (!TestNotNull(TEXT("Wall character"), WallPawn) || !TestNotNull(TEXT("Step character"), StepPawn)
		|| !TestNotNull(TEXT("Falling character"), FallingPawn)) { return false; }
	Fixture.Tick(0.05f);
	FallingPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	FallingPawn->GetCharacterMovement()->Velocity.Z = 200.f;
	const double FallingStartZ = FallingPawn->GetActorLocation().Z;
	const double GroundZ = StepPawn->GetActorLocation().Z;
	for (ACharacter* Pawn : { WallPawn, StepPawn, FallingPawn })
	{
		TestTrue(TEXT("Geometry dodge starts"), DodgeOf(Pawn)->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	}
	Fixture.Tick(1.f / 60.f);
	TestTrue(TEXT("Dodge preserves upward jump velocity"), FallingPawn->GetCharacterMovement()->Velocity.Z > 0.f);
	TestTrue(TEXT("Jump continues rising initially"), FallingPawn->GetActorLocation().Z > FallingStartZ);
	double HighestStepZ = GroundZ;
	for (int32 Frame = 0; Frame < 28; ++Frame)
	{
		Fixture.Tick(1.f / 60.f);
		HighestStepZ = FMath::Max(HighestStepZ, StepPawn->GetActorLocation().Z);
	}
	TestTrue(TEXT("Capsule stops before tall wall"), WallPawn->GetActorLocation().X > 140.f && WallPawn->GetActorLocation().X < 157.f);
	TestTrue(TEXT("Normal floor stepping climbs the low obstacle"), HighestStepZ > GroundZ + 20.f);
	TestTrue(TEXT("Dodge crosses the low step"), StepPawn->GetActorLocation().X > 400.f);
	TestTrue(TEXT("Low step retains grounded movement"), StepPawn->GetCharacterMovement()->IsMovingOnGround());
	TestTrue(TEXT("Air dodge covers bounded horizontal distance"), FMath::IsNearlyEqual(FallingPawn->GetActorLocation().X, 450.0, DistanceToleranceCm));
	TestTrue(TEXT("Air dodge retains gravity"), FallingPawn->GetCharacterMovement()->Velocity.Z < -200.f);
	TestTrue(TEXT("Air dodge ends while still falling"), FallingPawn->GetCharacterMovement()->IsFalling());
	for (ACharacter* Pawn : { WallPawn, StepPawn, FallingPawn })
	{
		TestFalse(TEXT("Geometry cannot leave dodge stuck active"), DodgeOf(Pawn)->IsDodging());
		TestTrue(TEXT("Geometry completion clears only planar force"), Pawn->GetCharacterMovement()->Velocity.SizeSquared2D() < 0.01);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDodgeLifecycle,
	"Qinglan.Combat.DodgeCancelsOnDamageDeathAndTeardown", DodgeMovementFlags)
bool FQinglanDodgeLifecycle::RunTest(const FString& Parameters)
{
	FDodgeMovementWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	Fixture.Floor();
	AQinglanCharacter* Player = Cast<AQinglanCharacter>(Fixture.Character(FVector(0.f, 0.f, 92.f), true));
	if (!TestNotNull(TEXT("Production player"), Player)) { return false; }
	UQinglanDodgeComponent* Dodge = DodgeOf(Player);
	UQinglanCombatComponent* Combat = Player->GetCombat();
	Fixture.Tick(0.05f);
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>(
		AQinglanEnemyCharacter::StaticClass(), FVector(-1000.f, 0.f, 92.f), FRotator::ZeroRotator);
	AQinglanEnemyProjectile* Shot = Fixture.World->SpawnActor<AQinglanEnemyProjectile>();
	if (!TestNotNull(TEXT("Enemy caster"), Enemy) || !TestNotNull(TEXT("Enemy projectile"), Shot)) { return false; }
	Shot->LaunchFrom(Enemy, FVector::ForwardVector, 0.f, 10.f, 1000.f,
		UQinglanCombatComponent::NextCastId(), NAME_None, NAME_None);
	Shot->SetActorLocation(Player->GetActorLocation() + FVector(100.f, 0.f, 0.f));
	Shot->SetActorTickEnabled(false);
	// Force overlap dispatch inside a movement substep, exposing deferred
	// root-source removal that could otherwise reapply the dash in this frame.
	Player->GetCharacterMovement()->bEnableScopedMovementUpdates = false;
	TestTrue(TEXT("Dodge into hostile projectile starts"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	const float BeforeHit = Combat->GetHealth();
	Fixture.Tick(0.35f); // beyond i-frames, with multiple walking substeps
	TestTrue(TEXT("Swept overlap delivers real damage"), Combat->GetHealth() < BeforeHit);
	TestFalse(TEXT("Accepted hit cancels immediately"), Dodge->IsDodging());
	TestTrue(TEXT("Prepared force is not reapplied in later substeps"), Player->GetActorLocation().X < 160.f);
	const FVector HitStop = Player->GetActorLocation();
	Fixture.Tick(0.2f);
	TestTrue(TEXT("Hit reaction cannot restart dash next frame"), FVector::Dist2D(HitStop, Player->GetActorLocation()) < DistanceToleranceCm);
	Fixture.Tick(0.3f);
	const float BeforeRejectedDodge = Combat->GetAttributes().Stamina;
	Player->GetCharacterMovement()->DisableMovement();
	Player->TryDodge();
	TestFalse(TEXT("Disabled movement rejects the player dodge"), Dodge->IsDodging());
	TestEqual(TEXT("Rejected player dodge cannot spend stamina"), Combat->GetAttributes().Stamina, BeforeRejectedDodge);
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	TestTrue(TEXT("Fresh dodge after reaction starts"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	Fixture.Tick(0.15f);
	TestTrue(TEXT("Independent immunity clock is live"), Combat->IsInvulnerable());
	Dodge->CancelDodge();
	TestFalse(TEXT("Cancellation also clears combat immunity"), Combat->IsInvulnerable());
	const FVector CancelStop = Player->GetActorLocation();
	Fixture.Tick(0.1f);
	TestTrue(TEXT("Cancelled source cannot resume"), FVector::Dist2D(CancelStop, Player->GetActorLocation()) < DistanceToleranceCm);
	for (const float RemovalTime : { 0.05f, 0.15f })
	{
		TestTrue(TEXT("Source-removal dodge starts"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
		Fixture.Tick(RemovalTime);
		Player->GetCharacterMovement()->RemoveRootMotionSource(FName(TEXT("QinglanDodge")));
		Fixture.Tick(0.01f);
		TestFalse(TEXT("External source removal cancels dodge state"), Dodge->IsDodging());
		TestFalse(TEXT("External source removal clears immunity"), Combat->IsInvulnerable());
		Fixture.Tick(0.1f);
		TestFalse(TEXT("Removed source cannot grant delayed immunity"), Combat->IsInvulnerable());
	}
	TestTrue(TEXT("Dodge before lethal damage starts"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	FQinglanDamageEvent Lethal;
	Lethal.Target = Player;
	Lethal.RawMagnitude = 10000.f;
	Lethal.CastId = UQinglanCombatComponent::NextCastId();
	Lethal.Timestamp = Fixture.World->GetTimeSeconds();
	TestTrue(TEXT("Lethal damage is accepted before i-frames"), Combat->ApplyDamage(Lethal));
	TestFalse(TEXT("Death cancels before another movement tick"), Dodge->IsDodging());
	TestFalse(TEXT("Dead character cannot install a new source"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	const FVector DeathStop = Player->GetActorLocation();
	Fixture.Tick(0.2f);
	TestTrue(TEXT("Death has no residual dash"), FVector::Dist2D(DeathStop, Player->GetActorLocation()) < DistanceToleranceCm);
	ACharacter* Survivor = Fixture.Character(FVector(0.f, 600.f, 92.f));
	if (!TestNotNull(TEXT("Teardown character"), Survivor)) { return false; }
	UQinglanDodgeComponent* RemovedDodge = DodgeOf(Survivor);
	TestTrue(TEXT("Teardown dodge starts"), RemovedDodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	Fixture.Tick(0.05f);
	RemovedDodge->DestroyComponent();
	const FVector TeardownStop = Survivor->GetActorLocation();
	Fixture.Tick(0.2f);
	TestTrue(TEXT("Component teardown removes movement force"), FVector::Dist2D(TeardownStop, Survivor->GetActorLocation()) < DistanceToleranceCm);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDodgePauseAndTrail,
	"Qinglan.Combat.DodgePauseAndFinalMistStepSample", DodgeMovementFlags)
bool FQinglanDodgePauseAndTrail::RunTest(const FString& Parameters)
{
	FDodgeMovementWorld Fixture;
	if (!TestNotNull(TEXT("Physics world"), Fixture.World)) { return false; }
	Fixture.Floor();
	ACharacter* Pawn = Fixture.Character(FVector(0.f, 0.f, 92.f));
	APlayerController* Controller = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Character"), Pawn) || !TestNotNull(TEXT("Pause controller"), Controller)) { return false; }
	Controller->SetPlayerState(Fixture.World->SpawnActor<APlayerState>());
	// A possessed character must be locally controlled to run physics. A
	// controller without ULocalPlayer is nonlocal in this standalone world.
	ULocalPlayer* LocalPlayer = NewObject<ULocalPlayer>(GEngine);
	Fixture.Instance->AddLocalPlayer(LocalPlayer, FPlatformUserId::CreateFromInternalId(0));
	Controller->SetPlayer(LocalPlayer);
	Controller->Possess(Pawn);
	if (!TestTrue(TEXT("Pause fixture owns local character simulation"), Pawn->IsLocallyControlled())) { return false; }
	UQinglanDodgeComponent* Dodge = DodgeOf(Pawn);
	Fixture.Tick(0.05f);
	const FVector Start = Pawn->GetActorLocation();
	UQinglanCombatComponent* LastSegmentTarget = Fixture.Target(Start + FVector(450.f, 65.f, 0.f));
	UQinglanCombatComponent* BeyondTarget = Fixture.Target(Start + FVector(600.f, 65.f, 0.f));
	AQinglanProjectile* Trail = Fixture.World->SpawnActor<AQinglanProjectile>();
	if (!TestNotNull(TEXT("Terminal target"), LastSegmentTarget) || !TestNotNull(TEXT("Beyond target"), BeyondTarget)
		|| !TestNotNull(TEXT("Mist Step trail"), Trail)) { return false; }
	TestTrue(TEXT("Mist Step dodge starts"), Dodge->StartDodge(FVector::ForwardVector, 450.f, 0.45f, 0.1f, 0.28f));
	Trail->SetupWetTrail(UQinglanCombatComponent::NextCastId(), TEXT("mist_step"), Pawn, 75.f, 0.f);
	Fixture.Tick(0.15f);
	TestTrue(TEXT("Dodge has entered its i-frame window"), Dodge->IsInvulnerableNow());
	const FVector PausedLocation = Pawn->GetActorLocation();
	const float PausedElapsed = Dodge->GetElapsedSeconds();
	const double PausedWorldTime = Fixture.World->GetTimeSeconds();
	TestTrue(TEXT("Authority pauses actual world"), Controller->SetPause(true));
	Fixture.Tick(0.5f);
	Fixture.Tick(0.5f);
	TestEqual(TEXT("Pause freezes the world clock"), Fixture.World->GetTimeSeconds(), PausedWorldTime);
	TestEqual(TEXT("Pause freezes source elapsed"), Dodge->GetElapsedSeconds(), PausedElapsed);
	TestTrue(TEXT("Pause freezes capsule location"), Pawn->GetActorLocation().Equals(PausedLocation, 0.01));
	TestTrue(TEXT("Pause retains i-frame state"), Dodge->IsInvulnerableNow());
	TestTrue(TEXT("Authority resumes world"), Controller->SetPause(false));
	Fixture.Tick(0.20f);
	TestFalse(TEXT("Target is outside all pre-terminal trail samples"), LastSegmentTarget->HasStatus(EQinglanStatus::Wet));
	Fixture.Tick(0.25f);
	TestFalse(TEXT("Terminal movement ends dodge"), Dodge->IsDodging());
	TestTrue(TEXT("Final segment wets the target in the same world tick"), LastSegmentTarget->HasStatus(EQinglanStatus::Wet));
	TestFalse(TEXT("Trail retires in the terminal world tick"), Trail->IsActive());
	TestFalse(TEXT("Retired trail drops its old dodge prerequisite"), Trail->PrimaryActorTick.GetPrerequisites().ContainsByPredicate(
		[Dodge](const FTickPrerequisite& Prerequisite) { return Prerequisite.PrerequisiteObject.Get() == Dodge; }));
	Pawn->AddMovementInput(FVector::ForwardVector, 1.f, true);
	Fixture.Tick(0.4f);
	TestTrue(TEXT("Normal movement reaches the beyond-dodge target"), Pawn->GetActorLocation().X > Start.X + 550.f);
	TestFalse(TEXT("Normal movement after dodge cannot extend the trail"), BeyondTarget->HasStatus(EQinglanStatus::Wet));
	Trail->SetupWetTrail(UQinglanCombatComponent::NextCastId(), TEXT("mist_step"), Pawn, 75.f, 0.f);
	Trail->ResetForPool();
	TestFalse(TEXT("Explicit pooling also releases the prerequisite"), Trail->PrimaryActorTick.GetPrerequisites().ContainsByPredicate(
		[Dodge](const FTickPrerequisite& Prerequisite) { return Prerequisite.PrerequisiteObject.Get() == Dodge; }));
	return true;
}
#endif
