// Regression for the actual player-facing training interaction. The target
// must be hit from a legal standing position with the real animated mesh;
// socket-aligned actor teleports or injected proof cannot satisfy this test.
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanTrainingTarget.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/QinglanInteractionSpawner.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "InputActionValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTrainingStandingAnimatedSwordHits,
	"Qinglan.Combat.TrainingTargetStandingAnimatedSwordHits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanTrainingStandingAnimatedSwordHits::RunTest(const FString& Parameters)
{
	const uint64 SavedFrameCounter = GFrameCounter;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Name = TEXT("L_Hub_StandingTraining_") + Id;
	const FString SaveDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"), TEXT("_StandingTraining_") + Id);
	UGameInstance* Instance = nullptr;
	UWorld* World = nullptr;
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(SaveDirectory);
	ON_SCOPE_EXIT
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
	};
	if (!TestNotNull(TEXT("Engine supports the real standalone world"), GEngine)) { return false; }
	Instance = NewObject<UGameInstance>(GEngine);
	Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
	World = Instance->GetWorld();
	if (!TestNotNull(TEXT("Physics game world"), World)) { return false; }
	UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
	UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Production save subsystem"), Save) || !TestNotNull(TEXT("Production story subsystem"), Story)) { return false; }
	Save->CreateNewProfile(0);
	if (!TestTrue(TEXT("The real profile is active before pawn initialization"), Save->IsProfileSessionActive())) { return false; }

	const FVector YardLocation(3000.f, 2500.f, 600.f);
	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("Physical floor"), Floor)) { return false; }
	Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
	Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetActorScale3D(FVector(20.f, 20.f, 1.f));
	Floor->SetActorLocation(YardLocation + FVector(0.f, 400.f, -50.f));
	FActorSpawnParameters AnchorParams;
	AnchorParams.Name = TEXT("hub.training_yard");
	AnchorParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	ATargetPoint* Yard = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), YardLocation, FRotator::ZeroRotator, AnchorParams);
	if (!TestNotNull(TEXT("Authored training-yard anchor"), Yard)) { return false; }
	Yard->Tags.Add(TEXT("hub.training_yard"));
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay(); // invokes the actual world interaction spawner
	World->SetBegunPlay(true);
	AQinglanTrainingTarget* Target = nullptr;
	for (TActorIterator<AQinglanTrainingTarget> It(World); It; ++It) { Target = *It; }
	if (!TestNotNull(TEXT("Production spawner creates the practice target"), Target)) { return false; }
	Target->DispatchBeginPlay();

	FActorSpawnParameters PlayerParams;
	PlayerParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// The sole positioning operation precedes combat: 110cm center separation
	// exceeds both capsule radii (34+45cm), and both actors stand on Z=600.
	const FVector StandingLocation = Target->GetActorLocation() + FVector(-110.f, 0.f, 0.f);
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(), StandingLocation, FRotator::ZeroRotator, PlayerParams);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Actual playable character"), Player) || !TestNotNull(TEXT("Actual player controller"), Controller)) { return false; }
	Controller->SetPlayerState(World->SpawnActor<APlayerState>());
	Controller->Possess(Player);
	Controller->DispatchBeginPlay();
	Player->DispatchBeginPlay();
	USkeletalMeshComponent* Mesh = Player->GetMesh();
	UQinglanSwordComponent* Sword = Player->FindComponentByClass<UQinglanSwordComponent>();
	if (!TestNotNull(TEXT("Real skeletal mesh"), Mesh) || !TestNotNull(TEXT("Production sword component"), Sword)) { return false; }
	if (!TestNotNull(TEXT("Production animation instance"), Mesh->GetAnimInstance())) { return false; }
	if (!TestTrue(TEXT("Actual rig has the sword base socket"), Mesh->DoesSocketExist(TEXT("sword_base")))
		|| !TestTrue(TEXT("Actual rig has the sword tip socket"), Mesh->DoesSocketExist(TEXT("sword_tip")))) { return false; }
	// A commandlet has no rendered visibility, but must run the same skeletal
	// pose and bone-refresh path as a visible Shipping character.
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	Mesh->SetComponentTickEnabled(true);
	Player->GetCharacterMovement()->GravityScale = 0.f;
	auto TickWorld = [World](int32 Frames)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			World->Tick(LEVELTICK_All, 1.f / 60.f);
			// Match FTestWorldWrapper so both tick scheduling and skeletal
			// poses receive a new engine frame on the next world step. The
			// fixture restores the outer frame counter after EndPlay.
			++GFrameCounter;
		}
	};
	TickWorld(30); // settle normal standing and the production idle pose
	TestTrue(TEXT("The real blade has human-scale length after imported bone scaling"),
		FMath::IsNearlyEqual(FVector::Dist(Mesh->GetSocketLocation(TEXT("sword_base")), Mesh->GetSocketLocation(TEXT("sword_tip"))), 90.0, 1.0));
	const UCapsuleComponent* TargetCapsule = Cast<UCapsuleComponent>(Target->GetRootComponent());
	if (!TestNotNull(TEXT("Target collision capsule"), TargetCapsule)) { return false; }
	const float MinimumSeparation = TargetCapsule->GetScaledCapsuleRadius() + Player->GetCapsuleComponent()->GetScaledCapsuleRadius();
	TestTrue(TEXT("Normal standing capsules do not overlap before combat"), FVector::Dist2D(Player->GetActorLocation(), Target->GetActorLocation()) > MinimumSeparation + 1.f);
	TestTrue(TEXT("Player remains at the legal standing spot"), FVector::Dist2D(Player->GetActorLocation(), StandingLocation) < 1.f);
	auto ProofCount = [Story]()
	{
		FQinglanNarrativeState State;
		QinglanStoryRuntime::ExportNarrativeState(Story, State);
		return State.FiredEventLog.FilterByPredicate([](FName Event) { return Event == FName(TEXT("combat.sword_hit")); }).Num();
	};
	const int32 InitialHits = Target->GetAcceptedHitCount();
	const int32 InitialProof = ProofCount();
	for (int32 Swing = 1; Swing <= 3; ++Swing)
	{
		const FVector BaseBefore = Mesh->GetSocketLocation(TEXT("sword_base"));
		const FVector TipBefore = Mesh->GetSocketLocation(TEXT("sword_tip"));
		double MaxBoneMovement = 0.0;
		bool bMontageObserved = false;
		float FurthestAttackMontagePosition = 0.f;
		TArray<FString> Samples;
		Player->InputSwordAttack(FInputActionValue(true));
		UAnimMontage* Attack = Cast<UAnimMontage>(Sword->SwingMontageSoftPaths[Sword->GetComboIndex()].TryLoad());
		AddInfo(FString::Printf(TEXT("Attack begin: instance=%s active=%s asset=%s length=%.3f rate=%.3f baseTransform=%s"),
			*Mesh->GetAnimInstance()->GetClass()->GetName(), *GetNameSafe(Mesh->GetAnimInstance()->GetCurrentActiveMontage()),
			*GetNameSafe(Attack), Attack ? Attack->GetPlayLength() : -1.f, Attack ? Mesh->GetAnimInstance()->Montage_GetPlayRate(Attack) : -1.f,
			*Mesh->GetSocketTransform(TEXT("sword_base")).ToString()));
		TestTrue(TEXT("Actual attack input starts a sword swing"), Sword->GetPhase() != EQinglanSwordPhase::Idle);
		// Independent presses allow the full animation and combo window to end.
		// We never tick a sword phase, refresh a pose, or move a socket directly.
		for (int32 Frame = 0; Frame < 78; ++Frame)
		{
			TickWorld(1);
			const FVector Base = Mesh->GetSocketLocation(TEXT("sword_base"));
			const FVector Tip = Mesh->GetSocketLocation(TEXT("sword_tip"));
			MaxBoneMovement = FMath::Max(MaxBoneMovement, FMath::Max(FVector::Dist(Base, BaseBefore), FVector::Dist(Tip, TipBefore)));
			UAnimInstance* Anim = Mesh->GetAnimInstance();
			UAnimMontage* Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
			if (Montage && Montage->GetName().StartsWith(TEXT("AM_Human_SwordCut_")))
			{
				bMontageObserved = true;
				FurthestAttackMontagePosition = FMath::Max(FurthestAttackMontagePosition, Anim->Montage_GetPosition(Montage));
			}
			if (Frame % 6 == 0 || Sword->GetPhase() == EQinglanSwordPhase::Active)
			{
				Samples.Add(FString::Printf(TEXT("swing=%d t=%.3f phase=%d montage=%s pos=%.3f base=%s tip=%s target=%s"),
					Swing, (Frame + 1) / 60.f, static_cast<int32>(Sword->GetPhase()), *GetNameSafe(Montage),
					Montage ? Anim->Montage_GetPosition(Montage) : -1.f, *Base.ToCompactString(), *Tip.ToCompactString(), *Target->GetActorLocation().ToCompactString()));
			}
		}
		TestTrue(TEXT("The attack actually plays an animation montage"), bMontageObserved);
		TestTrue(TEXT("The real sword montage advances with simulated engine frames"), FurthestAttackMontagePosition > 0.1f);
		TestTrue(TEXT("The actual blade bone pose moves during the attack"), MaxBoneMovement > 1.f);
		const bool bAccepted = TestEqual(TEXT("Each ordinary standing swing hits the practice target exactly once"), Target->GetAcceptedHitCount(), InitialHits + Swing);
		TestEqual(TEXT("Each real accepted hit emits one training proof"), ProofCount(), InitialProof + Swing);
		TestTrue(TEXT("The attack never requires penetrating the target capsule"), FVector::Dist2D(Player->GetActorLocation(), Target->GetActorLocation()) > MinimumSeparation + 1.f);
		if (!bAccepted) { for (const FString& Sample : Samples) { AddInfo(Sample); } }
	}
	return true;
}
#endif
