// Player character implementation: Enhanced Input wiring, third-person camera with
// sphere-sweep obstruction, lock-on, dodge, and routing into the sword/technique
// components. Numbers from docs/02-combat-and-camera.md §2-§4.
#include "Combat/QinglanCharacter.h"

#include "QinglanAscension.h"
#include "Combat/QinglanSwordComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTalismanComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "Progression/QinglanEconomyLibrary.h"

// The scaffold references UQinglanStatusComponent but the Combat folder scaffold does
// not ship that header (parallel module SPELL-001 provides it). Compile with or
// without it; the member stays null until the header lands.
// FIXME-CONTRACT: AQinglanCharacter::GetStatus() returns null until UQinglanStatusComponent exists.
#if __has_include("Combat/QinglanStatusComponent.h")
#include "Combat/QinglanStatusComponent.h"
#define QINGLAN_HAS_STATUS_COMPONENT 1
#else
#define QINGLAN_HAS_STATUS_COMPONENT 0
#endif

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "Components/InputComponent.h"
#include "Framework/Commands/InputChord.h"
#include "InputTriggers.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"
#include "Blueprint/UserWidget.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanGameModeBase.h"
#include "Core/QinglanTrialAdmission.h"
#include "Core/QinglanTrialDirector.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "UI/QinglanHUDStatics.h"
#include "UI/QinglanUIWidgets.h"

// ---------------------------------------------------------------------------
// UQinglanDodgeComponent
// ---------------------------------------------------------------------------

UQinglanDodgeComponent::UQinglanDodgeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(false);
}

bool UQinglanDodgeComponent::StartDodge(const FVector& Direction, float Distance, float Duration, float InIFrameStart, float InIFrameEnd)
{
	if (bDodgeActive || Duration <= 0.f || Distance <= 0.f)
	{
		return false;
	}

	FVector Planar = Direction;
	Planar.Z = 0.f;
	if (Planar.SizeSquared2D() < FMath::Square(KINDA_SMALL_NUMBER))
	{
		AActor* const OwnerActor = GetOwner();
		Planar = OwnerActor ? OwnerActor->GetActorForwardVector() : FVector::ForwardVector;
		Planar.Z = 0.f;
	}
	DodgeDirection = Planar.GetSafeNormal2D();
	DodgeDuration = Duration;
	DodgeSpeed = Distance / Duration; // constant speed integrates to exactly Distance
	IFrameStartTime = InIFrameStart;
	IFrameEndTime = InIFrameEnd;
	DodgeElapsed = 0.f;
	bDodgeActive = true;
	OnDodgeStarted.Broadcast(DodgeDuration);
	return true;
}

bool UQinglanDodgeComponent::IsInvulnerableNow() const
{
	// docs/02 §2: i-frames [0.10, 0.28] s of the 0.45 s dodge; elapsed accumulates
	// only while unpaused, so pausing freezes the window.
	return bDodgeActive && DodgeElapsed >= IFrameStartTime && DodgeElapsed <= IFrameEndTime;
}

void UQinglanDodgeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bDodgeActive)
	{
		return;
	}

	ACharacter* const OwnerCharacter = Cast<ACharacter>(GetOwner());
	if (!OwnerCharacter)
	{
		FinishDodge();
		return;
	}

	// Drive the capsule through CharacterMovement with a constant planar velocity:
	// the movement component sweeps the capsule, so the dodge cannot pass through
	// walls (docs/02 §2) and the distance is identical at 30/60/120 FPS.
	if (UCharacterMovementComponent* const Movement = OwnerCharacter->GetCharacterMovement())
	{
		const float Remaining = FMath::Max(0.f, DodgeDuration - DodgeElapsed);
		const float StepDeltaTime = FMath::Min(DeltaTime, Remaining);
		const FVector PlanarVelocity = DodgeDirection * DodgeSpeed;
		Movement->Velocity = FVector(PlanarVelocity.X, PlanarVelocity.Y, Movement->Velocity.Z);
		if (bOrientToDodgeDirection && StepDeltaTime > 0.f)
		{
			OwnerCharacter->SetActorRotation(FRotator(0.f, DodgeDirection.Rotation().Yaw, 0.f));
		}
		DodgeElapsed += StepDeltaTime;
	}
	else
	{
		DodgeElapsed += DeltaTime;
	}

	if (DodgeElapsed >= DodgeDuration)
	{
		FinishDodge();
	}
}

void UQinglanDodgeComponent::FinishDodge()
{
	if (!bDodgeActive)
	{
		return;
	}
	bDodgeActive = false;
	DodgeElapsed = DodgeDuration;
	OnDodgeFinished.Broadcast(); // AQinglanCharacter::HandleDodgeFinished restores walk speed
}

// ---------------------------------------------------------------------------
// AQinglanCharacter - file-local helpers and tunables
// ---------------------------------------------------------------------------

namespace
{
	// --- Authored tunables (docs/02-combat-and-camera.md) ---
	constexpr float QinglanCameraArmLength = 350.f;   // docs/02 §2 spring-arm distance
	constexpr float QinglanCameraShoulderOffset = 60.f; // docs/02 §2 shoulder offset (cm, +Y)
	constexpr float QinglanDefaultFOV = 75.f;         // docs/02 §2
	constexpr float QinglanMinUserFOV = 65.f;         // docs/02 §2 user FOV range
	constexpr float QinglanMaxUserFOV = 95.f;         // docs/02 §2 user FOV range
	constexpr float QinglanLockOnDropDelay = 1.5f;    // docs/02 §2 continuous out-of-range/visibility seconds

	// --- Implementation-only tunables (not authored in docs; kept named for tuning) ---
	constexpr float QinglanCameraProbeRadius = 12.f;     // sphere-sweep probe size
	constexpr float QinglanCameraPullInSpeed = 900.f;    // cm/s inward interpolation ("in fast")
	constexpr float QinglanCameraPushOutSpeed = 150.f;   // cm/s outward interpolation ("out slow")
	constexpr float QinglanCameraObstructionPadding = 10.f; // cm kept between obstacle and lens
	constexpr float QinglanCameraMinArmLength = 60.f;    // never pull in closer than this
	constexpr float QinglanLockOnScreenWeight = 0.6f;    // weighted score weights; docs give the
	constexpr float QinglanLockOnWorldWeight = 0.4f;     // formula shape, not the split
	constexpr float QinglanLockOnRotationSpeed = 270.f;  // deg/s camera steering while locked
	constexpr float QinglanMoveStateSpeedThreshold = 25.f; // cm/s above which state reads Move
	constexpr float QinglanAimTraceDistance = 10000.f;   // cm free-aim ray length
	constexpr float QinglanCapsuleRadius = 34.f;         // scaffold does not author capsule size;
	constexpr float QinglanCapsuleHalfHeight = 88.f;     // template humanoid default
	constexpr float QinglanRotationRate = 640.f;         // deg/s orient-to-movement, template default

	// --- Playable-loop glue tunables (task contract; interaction layer) ---
	constexpr float QinglanInteractRange = 300.f;        // cm proximity for interact aiming
	constexpr float QinglanInteractAimDot = 0.5f;        // camera-aim cone half-angle (~60 deg)
	constexpr float QinglanDefeatTravelDelay = 1.75f;    // s death beat before the hub reload
	constexpr float QinglanOnScreenMessageSeconds = 8.f; // fallback notice duration
	constexpr FColor QinglanOnScreenMessageColor(0, 200, 255);

	/** On-screen notice: last-resort presentation while WBP_Dialogue is unauthored (FIXME-CONTRACT: UMG frontends land with the UI pass). */
	void ShowOnScreenMessage(const FString& Message)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(/*Key=*/-1, QinglanOnScreenMessageSeconds, QinglanOnScreenMessageColor, Message);
		}
	}

	/** Nearest interactable inside the camera-aim cone (no trace; simple distance + dot). */
	AQinglanInteractable* QinglanFindAimedInteractable(AQinglanCharacter* Self)
	{
		UWorld* const World = Self ? Self->GetWorld() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		const FVector AimOrigin = Self->FollowCamera ? Self->FollowCamera->GetComponentLocation() : Self->GetActorLocation();
		const FVector AimDirection = Self->FollowCamera
			? Self->FollowCamera->GetComponentRotation().Vector()
			: Self->GetActorForwardVector();
		AQinglanInteractable* Best = nullptr;
		float BestDot = QinglanInteractAimDot;
		for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
		{
			const FVector Delta = It->GetActorLocation() - AimOrigin;
			if (Delta.Size() > QinglanInteractRange)
			{
				continue;
			}
			const float Dot = FVector::DotProduct(AimDirection, Delta.GetSafeNormal());
			if (Dot > BestDot)
			{
				BestDot = Dot;
				Best = *It;
			}
		}
		return Best;
	}

	template <typename T>
	T* QinglanFindComponent(AActor* Owner)
	{
		return Owner ? Owner->FindComponentByClass<T>() : nullptr;
	}

	template <typename T>
	const T* QinglanFindComponent(const AActor* Owner)
	{
		return Owner ? Owner->FindComponentByClass<T>() : nullptr;
	}

	/** Visibility check from the follow camera toward the candidate (docs/02 §2). */
	bool QinglanHasLineOfSight(const AQinglanCharacter* Viewer, const AActor* Target)
	{
		if (!Viewer || !Target)
		{
			return false;
		}
		UWorld* const World = Viewer->GetWorld();
		if (!World)
		{
			return false;
		}
		const FVector Start = Viewer->FollowCamera ? Viewer->FollowCamera->GetComponentLocation() : Viewer->GetActorLocation();
		const FVector End = Target->GetActorLocation();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(QinglanLockOnLOS), /*bTraceComplex=*/false, Viewer);
		Params.AddIgnoredActor(Viewer);
		FHitResult Hit;
		const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
		return !bBlocked || Hit.GetActor() == Target;
	}

	struct FQinglanLockCandidate
	{
		TWeakObjectPtr<AActor> Actor;
		float Score = 0.f;
	};

	/** Alive, in-range, visible pawns scored by weighted screen + world distance (docs/02 §2). */
	void QinglanCollectLockCandidates(AQinglanCharacter* Self, TArray<FQinglanLockCandidate>& OutCandidates)
	{
		if (!Self)
		{
			return;
		}
		UWorld* const World = Self->GetWorld();
		if (!World)
		{
			return;
		}

		APlayerController* const PC = Cast<APlayerController>(Self->GetController());
		FIntPoint ViewportSize(1, 1);
		if (PC)
		{
			// APlayerController::GetViewportSize takes two int32 refs, not FIntPoint.
			PC->GetViewportSize(ViewportSize.X, ViewportSize.Y);
		}
		const FVector2D ScreenCenter(static_cast<float>(ViewportSize.X) * 0.5f, static_cast<float>(ViewportSize.Y) * 0.5f);
		const float HalfDiagonal = FMath::Sqrt(FMath::Square(ScreenCenter.X) + FMath::Square(ScreenCenter.Y));

		for (TActorIterator<APawn> It(World); It; ++It)
		{
			APawn* const Pawn = *It;
			if (!Pawn || Pawn == Self)
			{
				continue;
			}
			UQinglanCombatComponent* const Combat = Pawn->FindComponentByClass<UQinglanCombatComponent>();
			if (!Combat || !Combat->IsAlive())
			{
				continue; // lock-on needs a living combatant
			}
			const float WorldDistance = FVector::Dist(Self->GetActorLocation(), Pawn->GetActorLocation());
			if (WorldDistance > Self->LockOnRange)
			{
				continue; // docs/02 §2: 2,000 cm acquisition range
			}
			if (!QinglanHasLineOfSight(Self, Pawn))
			{
				continue; // visibility required at acquisition (docs/02 §2)
			}

			float ScreenDistanceNorm = 1.f;
			if (PC)
			{
				FVector2D ScreenPos(0.f, 0.f);
				if (PC->ProjectWorldLocationToScreen(Pawn->GetActorLocation(), ScreenPos) && HalfDiagonal > 1.f)
				{
					ScreenDistanceNorm = FVector2D::Distance(ScreenPos, ScreenCenter) / HalfDiagonal;
				}
			}
			const float Score =
				QinglanLockOnScreenWeight * ScreenDistanceNorm +
				QinglanLockOnWorldWeight * (WorldDistance / Self->LockOnRange);
			OutCandidates.Add({ Pawn, Score });
		}

		OutCandidates.Sort([](const FQinglanLockCandidate& A, const FQinglanLockCandidate& B) { return A.Score < B.Score; });
	}

	/** Dodge direction: last movement input, else facing (lock target while locked). */
	FVector QinglanComputeDodgeDirection(const AQinglanCharacter* Character)
	{
		if (!Character)
		{
			return FVector::ForwardVector;
		}
		FVector Direction = Character->GetLastMovementInputVector();
		Direction.Z = 0.f;
		if (Direction.SizeSquared2D() < 1.e-4f)
		{
			const TWeakObjectPtr<AActor> Target = Character->GetLockTarget();
			if (Target.IsValid())
			{
				Direction = Target->GetActorLocation() - Character->GetActorLocation();
				Direction.Z = 0.f;
			}
			if (Direction.SizeSquared2D() < 1.e-4f)
			{
				Direction = Character->GetActorForwardVector();
				Direction.Z = 0.f;
			}
		}
		const FVector Normalized = Direction.GetSafeNormal2D();
		return Normalized.IsNearlyZero() ? FVector::ForwardVector : Normalized;
	}
}

// ---------------------------------------------------------------------------
// AQinglanCharacter - lifecycle
// ---------------------------------------------------------------------------

AQinglanCharacter::AQinglanCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(QinglanCapsuleRadius, QinglanCapsuleHalfHeight);

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = WalkSpeed; // docs/02 §2: 420 cm/s walking, no sprint
		Movement->bOrientRotationToMovement = true;
		Movement->RotationRate = FRotator(0.f, QinglanRotationRate, 0.f);
	}

	// Third-person camera (docs/02 §2): 350 cm boom, 60 cm shoulder, 75 deg FOV.
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetCapsuleComponent());
	CameraBoom->TargetArmLength = QinglanCameraArmLength;
	CameraBoom->SocketOffset = FVector(0.f, QinglanCameraShoulderOffset, 0.f);
	CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, 70.f)); // pivot height not authored; template default
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bDoCollisionTest = false; // the per-frame sphere sweep below is authoritative
	CameraBoom->bEnableCameraLag = false;  // deterministic camera; no authored lag numbers

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->SetFieldOfView(QinglanDefaultFOV);
	FollowCamera->bUsePawnControlRotation = false;

	CombatComponent = CreateDefaultSubobject<UQinglanCombatComponent>(TEXT("CombatComponent"));
	TechniqueComponent = CreateDefaultSubobject<UQinglanTechniqueComponent>(TEXT("TechniqueComponent"));
#if QINGLAN_HAS_STATUS_COMPONENT
	StatusComponent = CreateDefaultSubobject<UQinglanStatusComponent>(TEXT("StatusComponent"));
#endif
	// Action helpers created here and retrieved on demand; the scaffold header exposes
	// no members for them (owned by this actor as default subobjects).
	CreateDefaultSubobject<UQinglanSwordComponent>(TEXT("SwordComponent"));
	CreateDefaultSubobject<UQinglanDodgeComponent>(TEXT("DodgeComponent"));
	// Talisman slots for the Q/R bindings (docs/02 section 1; docs/03 section 5);
	// charges live in the profile subsystem, effects resolve against CombatComponent.
	CreateDefaultSubobject<UQinglanTalismanComponent>(TEXT("TalismanComponent"));

	// Enhanced Input assets. The mapping context path is contractual; action assets
	// are null-guarded everywhere they are used.
	ConstructorHelpers::FObjectFinder<UInputMappingContext> ContextFinder(TEXT("/Game/Qinglan/Input/IMC_Default"));
	if (ContextFinder.Succeeded())
	{
		DefaultMappingContext = ContextFinder.Object;
	}

	auto FindAction = [](const TCHAR* Path) -> UInputAction*
	{
		ConstructorHelpers::FObjectFinder<UInputAction> Finder(Path);
		return Finder.Succeeded() ? Finder.Object : nullptr;
	};

	MoveAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Move"));
	LookAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Look"));
	AttackAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Attack"));
	DodgeAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Dodge"));
	LockAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Lock"));
	CycleTargetAction = FindAction(TEXT("/Game/Qinglan/Input/IA_CycleTarget"));
	InteractAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Interact"));
	PauseAction = FindAction(TEXT("/Game/Qinglan/Input/IA_Pause"));
	for (int32 Slot = 0; Slot < 4; ++Slot) // docs/02 §1: technique slots 1-4
	{
		TechniqueActions.Add(FindAction(*FString::Printf(TEXT("/Game/Qinglan/Input/IA_Technique_%d"), Slot + 1)));
	}
	for (int32 Slot = 0; Slot < 2; ++Slot) // docs/02 §1: talisman slots 1-2
	{
		TalismanActions.Add(FindAction(*FString::Printf(TEXT("/Game/Qinglan/Input/IA_Talisman_%d"), Slot + 1)));
	}
}

void AQinglanCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = WalkSpeed;
	}

	// Presentation wiring: resolve the imported player rig by convention
	// (mesh import + anim bootstrap authors these); null-safe in test worlds.
	if (USkeletalMeshComponent* const MeshComp = GetMesh())
	{
		if (USkeletalMesh* const Mesh = Cast<USkeletalMesh>(
				FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/SK_Human")).TryLoad()))
		{
			MeshComp->SetSkeletalMesh(Mesh);
			MeshComp->SetRelativeLocation(FVector(0.f, 0.f, -QinglanCapsuleHalfHeight));
			MeshComp->SetRelativeRotation(FRotator(0.f, -90.f, 0.f)); // rig faces +Y in the export
			if (UAnimSequence* const Idle = Cast<UAnimSequence>(
					FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Idle")).TryLoad()))
			{
				MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				MeshComp->PlayAnimation(Idle, /*bLooping=*/true);
			}
		}
	}

	if (UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this))
	{
		Dodge->OnDodgeFinished.AddDynamic(this, &AQinglanCharacter::HandleDodgeFinished);
	}

	// Playable-loop glue: defeat bookkeeping + story display/choice-action
	// bindings. Null-safe in test worlds (subsystems absent -> logs only).
	if (CombatComponent)
	{
		CombatComponent->OnHealthChanged.AddDynamic(this, &AQinglanCharacter::HandleHealthChanged);
	}
	BindStoryDelegates();
}

void AQinglanCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindStoryDelegates();
	if (UWorld* const World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DefeatTravelTimer);
	}
	Super::EndPlay(EndPlayReason);
}

void AQinglanCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Possession can precede world BeginPlay on travel flows; binding here
	// catches a realm entry scene that begins the moment the map loads.
	BindStoryDelegates();

	APlayerController* const PC = Cast<APlayerController>(NewController);
	if (!PC || !PC->GetLocalPlayer())
	{
		return;
	}
	if (UEnhancedInputLocalPlayerSubsystem* const Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
	{
		if (DefaultMappingContext)
		{
			Subsystem->AddMappingContext(DefaultMappingContext, /*Priority=*/0);
		}
		else
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("DefaultMappingContext missing on %s; input actions inert"), *GetNameSafe(this));
		}
	}
}

void AQinglanCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// HUD lands on the first player-controlled tick (possession may complete
	// after BeginPlay on dedicated-client flows; UI/QinglanHUDStatics ids).
	if (!bHudCreated && IsPlayerControlled())
	{
		bHudCreated = true;
		EnsureHudCreated();
	}

	UpdateLockOn(DeltaSeconds);
	UpdateCameraObstruction(DeltaSeconds);

	if (FollowCamera) // docs/02 §2: FOV user range 65-95
	{
		const float CurrentFOV = FollowCamera->FieldOfView;
		if (CurrentFOV < QinglanMinUserFOV)
		{
			FollowCamera->SetFieldOfView(QinglanMinUserFOV);
		}
		else if (CurrentFOV > QinglanMaxUserFOV)
		{
			FollowCamera->SetFieldOfView(QinglanMaxUserFOV);
		}
	}
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AQinglanCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* const EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("Player input component is not Enhanced Input on %s"), *GetNameSafe(this));
		return;
	}

	if (MoveAction)
	{
		EnhancedInput->BindActionValueLambda(MoveAction, ETriggerEvent::Triggered, [this](const FInputActionValue& Value) { InputMove(Value); });
	}
	if (LookAction)
	{
		EnhancedInput->BindActionValueLambda(LookAction, ETriggerEvent::Triggered, [this](const FInputActionValue& Value) { InputLook(Value); });
	}
	if (AttackAction) // docs/02 §1: basic sword attack
	{
		EnhancedInput->BindActionValueLambda(AttackAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputSwordAttack(Value); });
	}
	if (DodgeAction)
	{
		EnhancedInput->BindActionValueLambda(DodgeAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputDodge(Value); });
	}
	if (LockAction)
	{
		EnhancedInput->BindActionValueLambda(LockAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputLockTarget(Value); });
	}
	if (CycleTargetAction)
	{
		EnhancedInput->BindActionValueLambda(CycleTargetAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputCycleTarget(Value); });
	}
	if (InteractAction)
	{
		EnhancedInput->BindActionValueLambda(InteractAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputInteract(Value); });
	}
	if (PauseAction)
	{
		EnhancedInput->BindActionValueLambda(PauseAction, ETriggerEvent::Started, [this](const FInputActionValue& Value) { InputPause(Value); });
	}
	for (int32 Slot = 0; Slot < TechniqueActions.Num(); ++Slot)
	{
		if (UInputAction* const Action = TechniqueActions[Slot])
		{
			EnhancedInput->BindActionValueLambda(Action, ETriggerEvent::Started, [this, Slot](const FInputActionValue&) { InputTechniqueSlot(Slot); });
		}
	}
	for (int32 Slot = 0; Slot < TalismanActions.Num(); ++Slot)
	{
		if (UInputAction* const Action = TalismanActions[Slot])
		{
			EnhancedInput->BindActionValueLambda(Action, ETriggerEvent::Started, [this, Slot](const FInputActionValue&) { InputTalismanSlot(Slot); });
		}
	}
	// Native menu bindings work with current cooked input assets. The first
	// four number keys retain technique bindings; keys 5..8 are menu-only.
	const TArray<FKey> ExtraNumbers = { EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight };
	for (int32 Index = 0; Index < ExtraNumbers.Num(); ++Index)
	{
		FInputKeyBinding Binding(FInputChord(ExtraNumbers[Index]), IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([this, Index]() { ConsumeMenuSelection(Index + 4); });
		PlayerInputComponent->KeyBindings.Add(MoveTemp(Binding));
	}
	const auto BindMenuKey = [this, PlayerInputComponent](FKey Key, TFunction<void()> Handler)
	{
		FInputKeyBinding Binding(FInputChord(Key), IE_Pressed);
		Binding.KeyDelegate.GetDelegateForManualSet().BindLambda([Handler = MoveTemp(Handler)]() { Handler(); });
		PlayerInputComponent->KeyBindings.Add(MoveTemp(Binding));
	};
	BindMenuKey(EKeys::Up, [this]() { MoveMenuSelection(-1); });
	BindMenuKey(EKeys::Down, [this]() { MoveMenuSelection(1); });
	BindMenuKey(EKeys::Gamepad_DPad_Up, [this]() { MoveMenuSelection(-1); });
	BindMenuKey(EKeys::Gamepad_DPad_Down, [this]() { MoveMenuSelection(1); });
	BindMenuKey(EKeys::Enter, [this]() { ConfirmMenuSelection(); });
	BindMenuKey(EKeys::BackSpace, [this]() { CancelMenu(); });
	BindMenuKey(EKeys::Gamepad_Special_Left, [this]() { CancelMenu(); });
	BindMenuKey(EKeys::SpaceBar, [this]()
	{
		if (UQinglanEndingWidget* Widget = EndingWidget.Get(); Widget && Widget->IsInViewport())
		{
			Widget->ToggleWarningAcknowledgment();
		}
	});

}

void AQinglanCharacter::InputMove(const FInputActionValue& Value)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	if (!Controller || bDefeatHandled)
	{
		return;
	}
	if (const UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this); Dodge && Dodge->IsDodging())
	{
		return; // dodge owns the velocity until HandleDodgeFinished
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y), Axis.X);
}

void AQinglanCharacter::InputLook(const FInputActionValue& Value)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	if (!Controller)
	{
		return;
	}
	// While locked on the camera is steered toward the target by UpdateLockOn;
	// free look resumes when the lock is released (hold/toggle is a settings option,
	// docs/01: "hold/toggle lock-on").
	if (IsLockedOn())
	{
		return;
	}
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AQinglanCharacter::InputSwordAttack(const FInputActionValue& /*Value*/)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	if (bDefeatHandled)
	{
		return;
	}
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->RequestAttack(); // buffering and combo-window rules live in the sword component
	}
}

void AQinglanCharacter::InputDodge(const FInputActionValue& /*Value*/)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	TryDodge();
}

void AQinglanCharacter::InputLockTarget(const FInputActionValue& /*Value*/)
{
	ToggleLockOn();
}

void AQinglanCharacter::InputCycleTarget(const FInputActionValue& /*Value*/)
{
	CycleLockTarget();
}

void AQinglanCharacter::InputTechniqueSlot(int32 Slot)
{
	if (bDefeatHandled)
	{
		return;
	}
	// Open key menus capture the number row first (scene choices, blessing
	// drafts, shop entries); casting falls through when none is open.
	if (ConsumeMenuSelection(Slot))
	{
		return;
	}
	if (TechniqueComponent)
	{
		TechniqueComponent->TryCast(Slot); // validation/anticipation/release pipeline is there
	}
}

void AQinglanCharacter::InputTalismanSlot(int32 Slot)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	// docs/02 §1 slots Q/R; charge accounting and effect execution live in the
	// talisman component (docs/03 §5).
	if (bDefeatHandled)
	{
		return;
	}
	if (UQinglanTalismanComponent* const Talisman = QinglanFindComponent<UQinglanTalismanComponent>(this))
	{
		Talisman->UseSlot(Slot);
	}
}

void AQinglanCharacter::InputInteract(const FInputActionValue& /*Value*/)
{
	// Playable-loop routing (docs/dev/level-layout.md interaction contract):
	// 1) a playing scene advances its dialogue;
	// 2) else the nearest interactable in the aim cone executes;
	// 3) else an open menu re-presents itself.
	if (bDefeatHandled)
	{
		return;
	}
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport())
	{
		ConfirmMenuSelection();
		return;
	}
	UGameInstance* const Instance = GetGameInstance();
	UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
		Run && Run->NeedsCheckpointResume())
	{
		if (AQinglanRealmGameMode* Mode = GetWorld() ? Cast<AQinglanRealmGameMode>(GetWorld()->GetAuthGameMode()) : nullptr)
		{
			Mode->RequestCheckpointResume();
		}
		else
		{
			const FName Map = Run->GetPendingCheckpointMap();
			if (!Map.IsNone()) { QinglanInteraction::TravelToMap(this, *Map.ToString()); }
		}
		return;
	}
	if (Story && Story->IsScenePlaying())
	{
		UQinglanHUDStatics::AdvanceDialogueLine(this);
		return;
	}
	if (AQinglanInteractable* const Interactable = QinglanFindAimedInteractable(this))
	{
		Interactable->ExecuteInteraction(this);
		return;
	}
	// Epilogue acknowledgment: the CH07 retrospective scenes carry prose
	// locations (no anchor matches), so a bare press after their script ran
	// out is the "seen" proof that commits them (framing chapter; the
	// combat chapters prove through boss clears and trial anchors instead).
	if (Story && LastEnteredSceneId != NAME_None && WasSceneScriptExhausted(LastEnteredSceneId)
		&& QinglanInteraction::ChapterIdOfScene(Story, LastEnteredSceneId) == FName(TEXT("CH07")))
	{
		FName ProofScene = NAME_None;
		FName ProofObjective = NAME_None;
		const TArray<FName> EpilogueChapters = { FName(TEXT("CH07")) };
		if (QinglanInteraction::FindObjectiveProofInChapters(Story, EpilogueChapters, ProofScene, ProofObjective))
		{
			Story->NotifySceneObjectiveCompleted(ProofScene, ProofObjective);
			return;
		}
	}
	if (PendingBlessingChoices.Num() > 0)
	{
		PresentBlessingOffer(PendingBlessingChoices);
		return;
	}
	if (PendingShopChoices.Num() > 0)
	{
		PresentShopOffer(PendingShopChoices);
		return;
	}
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Interact pressed: nothing in range"));
}

void AQinglanCharacter::InputPause(const FInputActionValue& /*Value*/)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport())
	{
		CancelMenu();
		return;
	}
	UWorld* const World = GetWorld();
	if (!World)
	{
		return;
	}
	// docs/01: "Pause freezes combat and timers" - paused game time halts component
	// ticking (DeltaTime accumulation) and world-time timestamps everywhere.
	UGameplayStatics::SetGamePaused(World, !UGameplayStatics::IsGamePaused(World));
}

// ---------------------------------------------------------------------------
// Dodge (docs/02 §2: ~450 cm over 0.45 s, 25 stamina, i-frames 0.10-0.28 s,
// no wall pass-through; cancel rules docs/02 §3)
// ---------------------------------------------------------------------------

bool AQinglanCharacter::CanDodge() const
{
	// Priority: Dead > forced HitReaction > allowed DodgeCancel > committed action
	// > buffered input (docs/02 §3).
	if (!CombatComponent || !CombatComponent->IsAlive())
	{
		return false; // Dead
	}
	if (CombatComponent->HasStatus(EQinglanStatus::Root))
	{
		return false; // rooted (docs/02 §7 Root)
	}
	// FIXME-CONTRACT: forced HitReaction has no query API on the scaffold components;
	// insert the reaction gate here when one exists.
	const UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this);
	if (Dodge && Dodge->IsDodging())
	{
		return false; // one dodge at a time
	}
	const UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this);
	if (Sword && !Sword->CanDodgeCancelNow())
	{
		return false; // active hit window is committed (docs/02 §3)
	}
	// Casting is dodge-interruptible only before the release notify; pre-release
	// interruption costs nothing (docs/02 §3). Post-release casts reject the dodge
	// via the technique component's own validation on the next attempt.
	return true;
}

void AQinglanCharacter::TryDodge()
{
	UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this);
	if (!Dodge || !CanDodge())
	{
		return;
	}
	// Stamina gate first: 25 stamina, rejected outright when insufficient (docs/02 §2).
	if (!CombatComponent || !CombatComponent->TrySpendStamina(DodgeStaminaCost))
	{
		return;
	}

	// Cancel lower-priority committed actions (docs/02 §3).
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->InterruptSwing();
	}
	if (TechniqueComponent && TechniqueComponent->IsCasting())
	{
		TechniqueComponent->InterruptCast(); // pre-release only; already released is not refunded
	}

	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		// Keep MaxWalkSpeed at dodge speed so the movement component never clamps the
		// driven velocity; HandleDodgeFinished restores WalkSpeed.
		Movement->MaxWalkSpeed = DodgeDistance / DodgeDuration;
	}

	Dodge->StartDodge(QinglanComputeDodgeDirection(this), DodgeDistance, DodgeDuration, GetDodgeIFrameStart(), GetDodgeIFrameEnd());
}

void AQinglanCharacter::HandleDodgeFinished()
{
	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = WalkSpeed; // docs/02 §2 walking speed restored
	}
}

// ---------------------------------------------------------------------------
// Lock-on (docs/02 §2: 2,000 cm, visibility at acquisition, drop after death or
// 1.5 s continuously out of range/visibility, weighted screen+world score)
// ---------------------------------------------------------------------------

void AQinglanCharacter::ToggleLockOn()
{
	if (LockTarget.IsValid())
	{
		LockTarget = nullptr;
		TimeSinceTargetLost = 0.f;
		bUseControllerRotationYaw = false;
		return;
	}

	TArray<FQinglanLockCandidate> Candidates;
	QinglanCollectLockCandidates(this, Candidates);
	if (Candidates.IsEmpty())
	{
		return;
	}
	LockTarget = Candidates[0].Actor; // best score
	TimeSinceTargetLost = 0.f;
	bUseControllerRotationYaw = true; // strafe facing while locked
}

void AQinglanCharacter::CycleLockTarget()
{
	TArray<FQinglanLockCandidate> Candidates;
	QinglanCollectLockCandidates(this, Candidates);
	if (Candidates.IsEmpty())
	{
		return;
	}

	int32 CurrentIndex = INDEX_NONE;
	if (LockTarget.IsValid())
	{
		AActor* const Current = LockTarget.Get();
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (Candidates[Index].Actor.Get() == Current)
			{
				CurrentIndex = Index;
				break;
			}
		}
	}
	// Wraps to the best candidate when nothing is currently locked.
	const int32 NextIndex = (CurrentIndex + 1) % Candidates.Num();
	LockTarget = Candidates[NextIndex].Actor;
	TimeSinceTargetLost = 0.f;
	bUseControllerRotationYaw = true;
}

void AQinglanCharacter::UpdateLockOn(float DeltaSeconds)
{
	if (!LockTarget.IsValid())
	{
		TimeSinceTargetLost = 0.f;
		return;
	}
	AActor* const Target = LockTarget.Get();

	// Drop immediately after death (docs/02 §2).
	UQinglanCombatComponent* const TargetCombat = Target->FindComponentByClass<UQinglanCombatComponent>();
	if (TargetCombat && !TargetCombat->IsAlive())
	{
		LockTarget = nullptr;
		TimeSinceTargetLost = 0.f;
		bUseControllerRotationYaw = false;
		return;
	}

	// Drop after 1.5 s continuously outside range/visibility (docs/02 §2).
	const bool bInRange = FVector::Dist(GetActorLocation(), Target->GetActorLocation()) <= LockOnRange;
	const bool bVisible = QinglanHasLineOfSight(this, Target);
	if (bInRange && bVisible)
	{
		TimeSinceTargetLost = 0.f;
	}
	else
	{
		TimeSinceTargetLost += DeltaSeconds;
		if (TimeSinceTargetLost >= QinglanLockOnDropDelay)
		{
			LockTarget = nullptr;
			TimeSinceTargetLost = 0.f;
			bUseControllerRotationYaw = false;
			return;
		}
	}

	// Steer the control rotation (and therefore the camera boom) toward the target.
	if (AController* const CurrentController = GetController())
	{
		const FVector CameraLocation = FollowCamera ? FollowCamera->GetComponentLocation() : GetActorLocation();
		const FRotator Desired = (Target->GetActorLocation() - CameraLocation).Rotation();
		CurrentController->SetControlRotation(FMath::RInterpConstantTo(
			CurrentController->GetControlRotation(),
			FRotator(Desired.Pitch, Desired.Yaw, 0.f),
			DeltaSeconds,
			QinglanLockOnRotationSpeed));
	}
}

// ---------------------------------------------------------------------------
// Camera (docs/02 §2: per-frame sphere-sweep obstruction, in fast / out slow)
// ---------------------------------------------------------------------------

void AQinglanCharacter::UpdateCameraObstruction(float DeltaSeconds)
{
	if (!CameraBoom || !FollowCamera)
	{
		return;
	}
	UWorld* const World = GetWorld();
	if (!World)
	{
		return;
	}

	// Probe from the boom pivot to the desired socket position at the full authored
	// arm length along the current camera rotation.
	const FRotator CameraRotation = CameraBoom->GetTargetRotation();
	const FVector PivotLocation = CameraBoom->GetComponentLocation();
	const FVector DesiredCameraLocation =
		PivotLocation +
		CameraRotation.RotateVector(FVector(-QinglanCameraArmLength, 0.f, 0.f)) +
		CameraRotation.RotateVector(CameraBoom->SocketOffset);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(QinglanCameraObstruction), /*bTraceComplex=*/false, this);
	FHitResult Hit;
	float DesiredArmLength = QinglanCameraArmLength;
	const bool bBlocked = World->SweepSingleByChannel(
		Hit, PivotLocation, DesiredCameraLocation, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(QinglanCameraProbeRadius), Params);
	if (bBlocked)
	{
		DesiredArmLength = FMath::Clamp(Hit.Distance - QinglanCameraObstructionPadding, QinglanCameraMinArmLength, QinglanCameraArmLength);
		// Fade of eligible foliage obstructors is a material-side concern owned by the
		// camera/VFX module; geometry pulls in here.
	}

	// Interpolate inward quickly, outward slowly (docs/02 §2).
	const float CurrentArmLength = CameraBoom->TargetArmLength;
	const float InterpSpeed = (DesiredArmLength < CurrentArmLength) ? QinglanCameraPullInSpeed : QinglanCameraPushOutSpeed;
	CameraBoom->TargetArmLength = FMath::FInterpConstantTo(CurrentArmLength, DesiredArmLength, DeltaSeconds, InterpSpeed);
}

FVector AQinglanCharacter::GetAimPoint() const
{
	// docs/02 §2: free aiming traces from the camera to derive an aim point, then from
	// the casting socket toward that point so shoulder-camera offsets do not cause
	// wall penetration.
	if (!FollowCamera || !GetWorld())
	{
		return GetActorLocation() + GetActorForwardVector() * QinglanAimTraceDistance;
	}
	static const FName CastSocketName(TEXT("cast_source")); // socket name not authored; rig-side tunable

	const FVector CameraLocation = FollowCamera->GetComponentLocation();
	const FVector CameraForward = FollowCamera->GetComponentRotation().Vector();
	const FVector TraceEnd = CameraLocation + CameraForward * QinglanAimTraceDistance;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(QinglanAimCameraLeg), /*bTraceComplex=*/false, this);
	FHitResult CameraHit;
	FVector AimPoint = TraceEnd;
	if (GetWorld()->LineTraceSingleByChannel(CameraHit, CameraLocation, TraceEnd, ECC_Visibility, Params))
	{
		AimPoint = CameraHit.Location;
	}

	// Second leg from the casting socket toward the camera-derived point; accept a
	// socket-leg hit only when it is strictly closer (otherwise keep the camera point).
	if (const USkeletalMeshComponent* const MeshComp = GetMesh())
	{
		const FVector SocketLocation = MeshComp->DoesSocketExist(CastSocketName)
			? MeshComp->GetSocketLocation(CastSocketName)
			: MeshComp->GetComponentLocation();
		FVector SocketDirection = AimPoint - SocketLocation;
		if (!SocketDirection.Normalize())
		{
			return AimPoint;
		}
		FHitResult SocketHit;
		FCollisionQueryParams SocketParams(SCENE_QUERY_STAT(QinglanAimSocketLeg), /*bTraceComplex=*/false, this);
		if (GetWorld()->LineTraceSingleByChannel(SocketHit, SocketLocation, SocketLocation + SocketDirection * QinglanAimTraceDistance, ECC_Visibility, SocketParams))
		{
			const float SocketHitDistance = FVector::Dist(SocketLocation, SocketHit.Location);
			const float CameraPointDistance = FVector::Dist(SocketLocation, AimPoint);
			if (SocketHitDistance < CameraPointDistance)
			{
				return SocketHit.Location;
			}
		}
	}
	return AimPoint;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

EQinglanCombatState AQinglanCharacter::GetCombatState() const
{
	// docs/02 §3 priority: Dead > forced HitReaction > dodge > committed action.
	if (CombatComponent && !CombatComponent->IsAlive())
	{
		return EQinglanCombatState::Dead;
	}
	// FIXME-CONTRACT: no scaffold API exposes a forced hit reaction; when one lands,
	// return EQinglanCombatState::HitReaction here.
	if (const UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this); Dodge && Dodge->IsDodging())
	{
		return EQinglanCombatState::Dodging;
	}
	if (TechniqueComponent && TechniqueComponent->IsCasting())
	{
		return EQinglanCombatState::Casting;
	}
	if (const UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		const EQinglanCombatState SwordState = Sword->GetCombatState();
		if (SwordState != EQinglanCombatState::Idle)
		{
			return SwordState; // AttackWindup / AttackActive / AttackRecovery
		}
	}
	return (GetVelocity().SizeSquared2D() > FMath::Square(QinglanMoveStateSpeedThreshold))
		? EQinglanCombatState::Move
		: EQinglanCombatState::Idle;
}

// ---------------------------------------------------------------------------
// Playable-loop glue: HUD presentation, key menus, story bindings, defeat
// (docs/dev/level-layout.md interaction contract; docs/03 sections 4-6)
// ---------------------------------------------------------------------------

void AQinglanCharacter::EnsureHudCreated()
{
	if (HudWidget.IsValid())
	{
		return;
	}
	if (UUserWidget* Widget = UQinglanHUDStatics::CreateWidgetById(this, FName(TEXT("HUD"))))
	{
		Widget->AddToViewport();
		HudWidget = Widget;
	}
	else
	{
		// WBP_HUD is not authored yet (UI pass); ResolveWidgetClass logged why.
		UE_LOG(LogQinglanCombat, Log, TEXT("HUD widget unavailable; combat vitals skip presentation"));
	}
}

UUserWidget* AQinglanCharacter::EnsureDialogueWidget()
{
	if (UUserWidget* Widget = DialogueWidget.Get())
	{
		return Widget;
	}
	UUserWidget* Widget = UQinglanHUDStatics::CreateWidgetById(this, FName(TEXT("Dialogue")));
	if (Widget)
	{
		DialogueWidget = Widget;
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget))
		{
			Dialogue->OnChoiceSelected.AddDynamic(this, &AQinglanCharacter::HandleDialogueChoiceSelected);
		}
	}
	return Widget;
}

UQinglanEndingWidget* AQinglanCharacter::EnsureEndingWidget()
{
	if (UQinglanEndingWidget* Widget = EndingWidget.Get()) { return Widget; }
	APlayerController* PC = Cast<APlayerController>(Controller);
	UQinglanEndingWidget* Widget = PC ? CreateWidget<UQinglanEndingWidget>(PC, UQinglanEndingWidget::StaticClass()) : nullptr;
	if (Widget)
	{
		EndingWidget = Widget;
		Widget->OnEndingClosed.AddDynamic(this, &AQinglanCharacter::HandleEndingClosed);
		Widget->OnEndingCommitted.AddDynamic(this, &AQinglanCharacter::HandleEndingCommitted);
	}
	return Widget;
}

void AQinglanCharacter::UpdateMenuInputMode()
{
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC) { return; }
	UUserWidget* Focus = nullptr;
	if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport()) { Focus = Ending; }
	else if (!PendingSceneChoices.IsEmpty()) { Focus = DialogueWidget.Get(); }
	PC->bShowMouseCursor = Focus != nullptr;
	if (Focus)
	{
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(Focus->TakeWidget());
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
		if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport())
		{
			Ending->FocusDefaultControl();
		}
		else if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Focus))
		{
			Dialogue->SetSelectedChoiceIndex(MenuSelectionIndex);
		}
	}
	else { PC->SetInputMode(FInputModeGameOnly()); }
}

void AQinglanCharacter::HandleDialogueChoiceSelected(FName ChoiceId)
{
	const int32 Index = PendingSceneChoices.IndexOfByPredicate([ChoiceId](const FQinglanSceneChoice& Choice) { return Choice.ChoiceId == ChoiceId; });
	if (Index != INDEX_NONE) { ConsumeMenuSelection(Index); }
}

void AQinglanCharacter::MoveMenuSelection(int32 Direction)
{
	if (PendingSceneChoices.IsEmpty()) { return; }
	MenuSelectionIndex = (MenuSelectionIndex + Direction + PendingSceneChoices.Num()) % PendingSceneChoices.Num();
	if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(DialogueWidget.Get()))
	{
		Dialogue->SetSelectedChoiceIndex(MenuSelectionIndex);
	}
}

void AQinglanCharacter::ConfirmMenuSelection()
{
	if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport())
	{
		Ending->ConfirmCurrentEnding();
		return;
	}
	if (!PendingSceneChoices.IsEmpty()) { ConsumeMenuSelection(MenuSelectionIndex); }
}

void AQinglanCharacter::CancelMenu()
{
	if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport())
	{
		Ending->CancelPreview();
		return;
	}
	if (UGameplayStatics::GetCurrentLevelName(this, true).Contains(TEXT("TribulationArena")))
	{
		HandleStayAtHub(NAME_None, NAME_None);
		return;
	}
	PendingShopChoices.Reset();
	// A mandatory blessing/scene choice stays pending; dismissal cannot erase it.
	if (PendingSceneChoices.IsEmpty() && PendingBlessingChoices.IsEmpty())
	{
		if (UUserWidget* Dialogue = DialogueWidget.Get()) { Dialogue->RemoveFromParent(); }
	}
	UpdateMenuInputMode();
}

void AQinglanCharacter::HandleEndingPreviewed(FName EndingId, FName SceneId)
{
	if (UQinglanEndingWidget* Ending = EnsureEndingWidget())
	{
		if (!Ending->IsInViewport()) { Ending->AddToViewport(100); }
		UpdateMenuInputMode();
		Ending->ShowEnding(EndingId, false);
	}
}

void AQinglanCharacter::HandleConfirmEndingRequested(FName EndingId, FName SceneId)
{
	if (UQinglanEndingWidget* Ending = EnsureEndingWidget())
	{
		if (!Ending->IsInViewport()) { Ending->AddToViewport(100); }
		UpdateMenuInputMode();
		Ending->ShowEnding(EndingId, true);
	}
}

void AQinglanCharacter::HandleCancelConfirmation(FName SceneId)
{
	if (UQinglanEndingWidget* Ending = EndingWidget.Get()) { Ending->RemoveFromParent(); }
	UpdateMenuInputMode();
}

void AQinglanCharacter::HandleEndingClosed()
{
	UpdateMenuInputMode();
}

void AQinglanCharacter::HandleEndingCommitted(FName EndingId)
{
	PendingSceneChoices.Reset();
	UpdateMenuInputMode();
	QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
}

void AQinglanCharacter::HandleSceneLine(const FQinglanScriptLine& Line)
{
	if (Line.SceneId == LastEnteredSceneId)
	{
		bSawLineForEnteredScene = true; // objective-proof tracking
	}
	if (UUserWidget* Widget = EnsureDialogueWidget())
	{
		if (!Widget->IsInViewport())
		{
			Widget->AddToViewport();
		}
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget))
		{
			Dialogue->SetSourceKind(Line.SourceKind);
			if (Dialogue->SetCurrentLine(Line.SpeakerName, Line.Text))
			{
				return; // rendered into Txt_Speaker/Txt_Line
			}
		}
	}
	// Unauthored-tree fallback: the keyed on-screen message stays the
	// last-resort presenter when the widget lacks the named blocks.
	ShowOnScreenMessage(FString::Printf(TEXT("%s: %s"), *Line.SpeakerName.ToString(), *Line.Text.ToString()));
}

void AQinglanCharacter::HandleChoicesPresented(const TArray<FQinglanSceneChoice>& Choices)
{
	PendingSceneChoices = Choices;
	MenuSelectionIndex = 0;
	if (UUserWidget* Widget = EnsureDialogueWidget(); Widget && !Widget->IsInViewport()) { Widget->AddToViewport(); }
	UpdateMenuInputMode();
	// Widget path first (stored entries + Box_Choices rebuild); the keyed
	// on-screen list is the unauthored-frontend fallback only.
	if (UUserWidget* Widget = DialogueWidget.Get())
	{
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget);
			Dialogue && Widget->IsInViewport() && Dialogue->SetChoiceEntries(Choices))
		{
			Dialogue->SetSelectedChoiceIndex(MenuSelectionIndex);
			UpdateMenuInputMode(); // Keep an ending modal above re-presented choices.
			return;
		}
	}
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Choices.Num(); ++Index)
	{
		Lines.Add(FString::Printf(TEXT("[%d] %s"), Index + 1, *Choices[Index].Label.ToString()));
	}
	ShowOnScreenMessage(FString::Join(Lines, TEXT("  ")));
}

void AQinglanCharacter::HandleSceneCompleted(FName SceneId)
{
	PendingSceneChoices.Reset();
	if (SceneId == LastEnteredSceneId)
	{
		LastEnteredSceneId = NAME_None; // committed: no proof pending
		bSawLineForEnteredScene = false;
	}
	if (UUserWidget* Widget = DialogueWidget.Get())
	{
		Widget->RemoveFromParent(); // keep the widget for the next scene
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget))
		{
			Dialogue->SetChoiceEntries(TArray<FQinglanSceneChoice>());
		}
	}
	UpdateMenuInputMode();
	if (SceneId.ToString().StartsWith(TEXT("CH07")) && GetWorld()
		&& Cast<AQinglanHubGameMode>(GetWorld()->GetAuthGameMode()))
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (UQinglanStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
			{
				QinglanInteraction::BeginFirstSceneOfChapters(Story, { FName(TEXT("CH07")) });
			}
		}));
	}
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Scene %s completed; dialogue presenter closed"), *SceneId.ToString());
}

void AQinglanCharacter::HandleSceneEntered(FName SceneId)
{
	// Objective-proof tracking: choice-free scenes stop at script end without
	// committing (their proof is a gameplay event); choice scenes commit from
	// the selection itself.
	UGameInstance* const Instance = GetGameInstance();
	UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	LastEnteredSceneId = SceneId;
	bLastEnteredSceneChoiceDriven = QinglanInteraction::SceneHasChoices(Story, SceneId);
	bSawLineForEnteredScene = false;
}

bool AQinglanCharacter::WasSceneScriptExhausted(FName SceneId) const
{
	if (SceneId == NAME_None || SceneId != LastEnteredSceneId
		|| bLastEnteredSceneChoiceDriven || !bSawLineForEnteredScene)
	{
		return false;
	}
	// The script ran out only when playback has ended without a commit.
	const UGameInstance* const Instance = GetGameInstance();
	const UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	return Story == nullptr || !Story->IsScenePlaying();
}

void AQinglanCharacter::HandleRoomCompleted(int32 RoomIndex, int64 ShardsAwarded)
{
	// Boss-room clear is the combat proof for the realm's current choice-free
	// chapter scene (CH02_S02 Bamboo Guardian, CH05_S01 Ashen Guardian; the
	// encounter director already ran CompleteCurrentRoom -> this broadcast).
	UGameInstance* const Instance = GetGameInstance();
	UQinglanRunSubsystem* const Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (!Run || !Run->IsInExpedition() || !Story)
	{
		return;
	}
	const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
	if (Realm == nullptr || Realm->GetRoomKind(RoomIndex) != EQinglanRoomKind::Boss)
	{
		return;
	}
	// Dispatch the exact defeated encounter; never scan for the first eligible
	// chapter scene (late chapters deliberately reuse local objective IDs).
	if (Realm->RealmId == FName(UQinglanEconomyLibrary::BambooRealmIdText))
	{
		Story->NotifySceneObjectiveCompleted(FName(TEXT("CH02_S02")), FName(TEXT("CH02_S02_OBJECTIVE")));
	}
	else if (Realm->RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
	{
		Story->NotifySceneObjectiveCompleted(FName(TEXT("CH05_S01")), FName(TEXT("s01.objective")));
	}
}

void AQinglanCharacter::HandleRecordOpened(FName RecordId, const FText& Title, const FText& Body)
{
	if (UUserWidget* Widget = EnsureDialogueWidget())
	{
		if (!Widget->IsInViewport())
		{
			Widget->AddToViewport();
		}
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget))
		{
			if (Dialogue->ShowRecordEntry(Title, Body))
			{
				return; // rendered into the speaker/line blocks
			}
		}
	}
	ShowOnScreenMessage(FString::Printf(TEXT("%s\n%s"), *Title.ToString(), *Body.ToString()));
}

void AQinglanCharacter::HandleEnterTrialRequested(FName IdempotencyKey, FName SceneId)
{
	// Choice actions dispatch before their scene transaction finishes. Defer
	// entry until that transaction has committed its preparation/story flags.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this, SceneId]()
		{
			UGameInstance* Instance = GetGameInstance();
			UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
			UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
			const QinglanTrialAdmission::ETrial Trial = SceneId.ToString().StartsWith(TEXT("CH03"))
				? QinglanTrialAdmission::ETrial::Foundation : QinglanTrialAdmission::ETrial::GoldenCore;
			const QinglanTrialAdmission::FResult Admission = QinglanTrialAdmission::EvaluateForTrial(Trial, Profile, Story);
			if (!Admission.IsAllowed())
			{
				ShowGameMessage(UQinglanHUDStatics::GameText(Admission.ReasonKey));
				return;
			}
			const FString Map = UGameplayStatics::GetCurrentLevelName(this, true);
			if (!Map.Contains(TEXT("TribulationArena")))
			{
				QinglanInteraction::TravelToMap(this, QinglanInteraction::ArenaMapPath);
			}
		}));
	}
}

void AQinglanCharacter::HandleStayAtHub(FName IdempotencyKey, FName SceneId)
{
	// Canceling preparation never completes a trial or grants a breakthrough.
	if (UWorld* World = GetWorld())
	{
		TActorIterator<AQinglanTrialDirector> It(World);
		if (It)
		{
			It->CancelTrialAndReturnToHub();
			return;
		}
		if (UGameplayStatics::GetCurrentLevelName(this, true).Contains(TEXT("TribulationArena")))
		{
			QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
		}
	}
}

void AQinglanCharacter::HandleSaveFailed(FName ProfileId, FText Reason)
{
	ShowGameMessage(Reason);
}

void AQinglanCharacter::HandleBankingFailed()
{
	ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("run.banking_failed"))));
}

void AQinglanCharacter::HandleReturnToHub(FName IdempotencyKey, FName SceneId)
{
	// CH06_S04 "return_to_hub": the post-decision sanctuary route.
	QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
}

void AQinglanCharacter::HandleCampaignComplete(FName IdempotencyKey, FName SceneId)
{
	// CH07_S04 "complete_campaign": back to the sanctuary, where the hub entry
	// pass begins the epilogue chapter (see AQinglanHubGameMode::BeginPlay).
	UE_LOG(LogQinglanCombat, Log, TEXT("Campaign complete (scene %s)"), *SceneId.ToString());
	// FIXME-CONTRACT: no string-table key for the campaign-complete notice.
	ShowGameMessage(NSLOCTEXT("Qinglan", "Campaign.Complete", "The first winter begins."));
	QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
}

void AQinglanCharacter::HandleDeliveryArrived(FName QuestId, FName Version)
{
	// The arrival notice has exactly one shower: this binding (the rest
	// station displays only the rested hours). Version names the answered
	// draft for diagnostics; the player-facing line is the authored one.
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Courier reply arrived for %s (version %s)"),
		*QuestId.ToString(), *Version.ToString());
	UGameInstance* const Instance = GetGameInstance();
	UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (!Story)
	{
		return;
	}
	const FText Arrival = QinglanStoryRuntime::DeliveryTimeText(Story, QuestId);
	if (!Arrival.IsEmpty())
	{
		ShowGameMessage(Arrival);
	}
}

void AQinglanCharacter::HandleHealthChanged(float Current, float Maximum)
{
	// Defeat (docs/03 section 6: defeat banks 50%; blueprint section 1.6 hub =
	// safe checkpoint): stop input, bank, then reload the hub after a short
	// death beat. One-shot per pawn; test worlds without a combat component
	// never bind this handler.
	if (bDefeatHandled || Current > 0.f || !IsPlayerControlled())
	{
		return;
	}
	bDefeatHandled = true;
	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		Movement->DisableMovement();
	}
	UGameInstance* const Instance = GetGameInstance();
	if (UQinglanRunSubsystem* const Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr)
	{
		if (Run->IsInExpedition())
		{
			Run->EndExpedition(/*bVictory=*/false, /*bVoluntaryRetreat=*/false); // 50% banking
		}
	}
	// FIXME-CONTRACT: no string-table key for the defeat notice.
	ShowGameMessage(NSLOCTEXT("Qinglan", "Combat.Defeated", "Defeated."));
	if (UWorld* const World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			DefeatTravelTimer, this, &AQinglanCharacter::HandleDefeatTravel, QinglanDefeatTravelDelay, /*bLoop=*/false);
	}
}

void AQinglanCharacter::HandleDefeatTravel()
{
	// Timer callback: OpenLevel runs off the input/damage call stack.
	if (UQinglanRunSubsystem* Run = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanRunSubsystem>() : nullptr)
	{
		if (Run->IsInExpedition())
		{
			Run->EndExpedition(/*bVictory=*/false, /*bVoluntaryRetreat=*/false);
			if (Run->IsInExpedition())
			{
				// TODO(AUD-07/51): expose retry/recovery from the defeat screen.
				// Never travel with a failed settlement's loot still unbanked.
				return;
			}
		}
	}
	QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
}

void AQinglanCharacter::BindStoryDelegates()
{
	if (bStoryDelegatesBound)
	{
		return;
	}
	UGameInstance* const Instance = GetGameInstance();
	UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (!Story)
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Story subsystem unavailable; narrative display inert"));
		return;
	}
	Story->OnSceneLine.AddDynamic(this, &AQinglanCharacter::HandleSceneLine);
	Story->OnChoicesPresented.AddDynamic(this, &AQinglanCharacter::HandleChoicesPresented);
	Story->OnSceneEntered.AddDynamic(this, &AQinglanCharacter::HandleSceneEntered);
	Story->OnSceneCompleted.AddDynamic(this, &AQinglanCharacter::HandleSceneCompleted);
	// Choice-action payloads (Narrative/QinglanStoryRuntime.h typed delegates).
	FQinglanStoryDelegates& Delegates = QinglanStoryRuntime::Delegates(Story);
	Delegates.OnRecordOpened.AddDynamic(this, &AQinglanCharacter::HandleRecordOpened);
	Delegates.OnEnterTrial.AddDynamic(this, &AQinglanCharacter::HandleEnterTrialRequested);
	Delegates.OnStayAtHub.AddDynamic(this, &AQinglanCharacter::HandleStayAtHub);
	Delegates.OnEndingPreviewed.AddDynamic(this, &AQinglanCharacter::HandleEndingPreviewed);
	Delegates.OnConfirmEndingRequested.AddDynamic(this, &AQinglanCharacter::HandleConfirmEndingRequested);
	Delegates.OnCancelConfirmation.AddDynamic(this, &AQinglanCharacter::HandleCancelConfirmation);
	Delegates.OnReturnToHub.AddDynamic(this, &AQinglanCharacter::HandleReturnToHub);
	Delegates.OnCompleteCampaign.AddDynamic(this, &AQinglanCharacter::HandleCampaignComplete);
	Delegates.OnDeliveryArrived.AddDynamic(this, &AQinglanCharacter::HandleDeliveryArrived);
	// Boss-room clears prove the realm's choice-free chapter scenes.
	if (UQinglanRunSubsystem* const Run = Instance->GetSubsystem<UQinglanRunSubsystem>())
	{
		Run->OnRoomCompleted.AddDynamic(this, &AQinglanCharacter::HandleRoomCompleted);
		Run->OnBankingFailed.AddDynamic(this, &AQinglanCharacter::HandleBankingFailed);
	}
	if (UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>())
	{
		Save->OnSaveFailed.AddDynamic(this, &AQinglanCharacter::HandleSaveFailed);
	}
	bStoryDelegatesBound = true;
}

void AQinglanCharacter::UnbindStoryDelegates()
{
	if (!bStoryDelegatesBound)
	{
		return;
	}
	if (UGameInstance* const Instance = GetGameInstance())
	{
		if (UQinglanStorySubsystem* const Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
		{
			Story->OnSceneLine.RemoveDynamic(this, &AQinglanCharacter::HandleSceneLine);
			Story->OnChoicesPresented.RemoveDynamic(this, &AQinglanCharacter::HandleChoicesPresented);
			Story->OnSceneEntered.RemoveDynamic(this, &AQinglanCharacter::HandleSceneEntered);
			Story->OnSceneCompleted.RemoveDynamic(this, &AQinglanCharacter::HandleSceneCompleted);
			FQinglanStoryDelegates& Delegates = QinglanStoryRuntime::Delegates(Story);
			Delegates.OnRecordOpened.RemoveDynamic(this, &AQinglanCharacter::HandleRecordOpened);
			Delegates.OnEnterTrial.RemoveDynamic(this, &AQinglanCharacter::HandleEnterTrialRequested);
			Delegates.OnStayAtHub.RemoveDynamic(this, &AQinglanCharacter::HandleStayAtHub);
			Delegates.OnEndingPreviewed.RemoveDynamic(this, &AQinglanCharacter::HandleEndingPreviewed);
			Delegates.OnConfirmEndingRequested.RemoveDynamic(this, &AQinglanCharacter::HandleConfirmEndingRequested);
			Delegates.OnCancelConfirmation.RemoveDynamic(this, &AQinglanCharacter::HandleCancelConfirmation);
			Delegates.OnReturnToHub.RemoveDynamic(this, &AQinglanCharacter::HandleReturnToHub);
			Delegates.OnCompleteCampaign.RemoveDynamic(this, &AQinglanCharacter::HandleCampaignComplete);
			Delegates.OnDeliveryArrived.RemoveDynamic(this, &AQinglanCharacter::HandleDeliveryArrived);
		}
		if (UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>())
		{
			Save->OnSaveFailed.RemoveDynamic(this, &AQinglanCharacter::HandleSaveFailed);
		}
		if (UQinglanRunSubsystem* const Run = Instance->GetSubsystem<UQinglanRunSubsystem>())
		{
			Run->OnRoomCompleted.RemoveDynamic(this, &AQinglanCharacter::HandleRoomCompleted);
			Run->OnBankingFailed.RemoveDynamic(this, &AQinglanCharacter::HandleBankingFailed);
		}
	}
	bStoryDelegatesBound = false;
}

void AQinglanCharacter::PresentBlessingOffer(const TArray<FQinglanBlessing>& Offers)
{
	PendingBlessingChoices = Offers;
	// FIXME-CONTRACT: menu layout belongs to WBP_Dialogue; keyed on-screen list
	// until the UMG frontend lands. Names resolve through the string table
	// (blessing.<id>.name); unauthored keys fall back to the stable id.
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Offers.Num(); ++Index)
	{
		const FText Name = Offers[Index].DisplayName.IsEmpty()
			? FText::FromString(Offers[Index].BlessingId.ToString())
			: Offers[Index].DisplayName;
		Lines.Add(FString::Printf(TEXT("[%d] %s"), Index + 1, *Name.ToString()));
	}
	ShowGameMessage(FText::FromString(FString::Join(Lines, TEXT("  "))));
}

void AQinglanCharacter::PresentShopOffer(const TArray<FQinglanShopOfferEntry>& Entries)
{
	PendingShopChoices = Entries;
	// FIXME-CONTRACT: shop layout belongs to WBP_Dialogue; keyed list fallback.
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		Lines.Add(FString::Printf(TEXT("[%d] %s (%s)"),
			Index + 1, *Entries[Index].Title.ToString(), *Entries[Index].CostSummary.ToString()));
	}
	ShowGameMessage(FText::FromString(FString::Join(Lines, TEXT("  "))));
}

void AQinglanCharacter::ShowGameMessage(const FText& Message)
{
	if (UUserWidget* Widget = DialogueWidget.Get())
	{
		if (Widget->IsInViewport())
		{
			if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(Widget))
			{
				if (Dialogue->SetCurrentLine(FText::GetEmpty(), Message))
				{
					return; // notice rendered through the dialogue line block
				}
			}
		}
	}
	ShowOnScreenMessage(Message.ToString());
}

bool AQinglanCharacter::ConsumeMenuSelection(int32 Slot)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return true; }
	if (Slot < 0)
	{
		return false;
	}
	// Priority: story choices (frozen scene) > blessing draft > shop offer.
	if (PendingSceneChoices.Num() > 0)
	{
		if (!PendingSceneChoices.IsValidIndex(Slot))
		{
			return true; // swallow invalid keys while choices freeze the scene
		}
		const TArray<FQinglanSceneChoice> PreviousChoices = PendingSceneChoices;
		const FName ChoiceId = PendingSceneChoices[Slot].ChoiceId;
		PendingSceneChoices.Reset();
		if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(DialogueWidget.Get()))
		{
			Dialogue->SetChoiceEntries({});
		}
		UpdateMenuInputMode();
		if (!UQinglanHUDStatics::SelectDialogueChoice(this, ChoiceId))
		{
			HandleChoicesPresented(PreviousChoices);
		}
		// Successful selection may synchronously present another group/menu.
		// Do not clear that new list after returning from the story subsystem.
		return true;
	}
	if (PendingBlessingChoices.Num() > 0)
	{
		if (!PendingBlessingChoices.IsValidIndex(Slot))
		{
			return true;
		}
		const FQinglanBlessing Blessing = PendingBlessingChoices[Slot];
		PendingBlessingChoices.Reset();
		if (UGameInstance* const Instance = GetGameInstance())
		{
			if (UQinglanRunSubsystem* const Run = Instance->GetSubsystem<UQinglanRunSubsystem>())
			{
				Run->AcceptBlessing(Blessing.BlessingId); // exactly 1 of 3 (docs/03 section 5)
			}
		}
		ShowGameMessage(Blessing.DisplayName.IsEmpty()
			? FText::FromString(Blessing.BlessingId.ToString())
			: Blessing.DisplayName);
		return true;
	}
	if (PendingShopChoices.Num() > 0)
	{
		if (!PendingShopChoices.IsValidIndex(Slot))
		{
			return true;
		}
		const FQinglanShopOfferEntry Entry = PendingShopChoices[Slot];
		PendingShopChoices.RemoveAt(Slot);
		BuyShopEntry(Entry);
		if (PendingShopChoices.Num() > 0)
		{
			PresentShopOffer(PendingShopChoices); // re-present the shortened list
		}
		return true;
	}
	return false;
}

void AQinglanCharacter::BuyShopEntry(const FQinglanShopOfferEntry& Entry)
{
	UGameInstance* const Instance = GetGameInstance();
	UQinglanProfileSubsystem* const Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (Profile == nullptr)
	{
		return;
	}
	const FName TransactionId = *FString::Printf(TEXT("shop.%s.%s"),
		Entry.bRelic ? TEXT("relic") : TEXT("passive"), *Entry.ItemId.ToString());
	const bool bCommitted = Entry.bRelic
		? Profile->TryPurchaseRelic(TransactionId, Entry.ItemId)
		: Profile->TryPurchasePassive(TransactionId, Entry.ItemId);
	if (!bCommitted)
	{
		// FIXME-CONTRACT: no string-table key for the declined purchase.
		ShowGameMessage(NSLOCTEXT("Qinglan", "Shop.Declined", "The keeper declines."));
		return;
	}
	// Auto-equip into a free slot so purchases take effect without a separate
	// equip UI (docs/03 sections 4-5: 1 relic, 2 passives equipped).
	if (Entry.bRelic)
	{
		if (Profile->GetEquippedRelic() == NAME_None)
		{
			Profile->TryEquipRelic(Entry.ItemId);
		}
	}
	else
	{
		TArray<FName> Equipped = Profile->GetEquippedPassives();
		if (Equipped.Num() < QinglanProgression::EquippedPassiveLimit && !Equipped.Contains(Entry.ItemId))
		{
			Equipped.Add(Entry.ItemId);
			Profile->TrySetEquippedPassives(Equipped);
		}
	}
	ShowGameMessage(Entry.Title);
}
