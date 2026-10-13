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
#include "Engine/SkeletalMesh.h"
#include "UObject/SoftObjectPath.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/ConstructorHelpers.h"

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
					FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/AS_Idle")).TryLoad()))
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
}

void AQinglanCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

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
}

void AQinglanCharacter::InputMove(const FInputActionValue& Value)
{
	if (!Controller)
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
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->RequestAttack(); // buffering and combo-window rules live in the sword component
	}
}

void AQinglanCharacter::InputDodge(const FInputActionValue& /*Value*/)
{
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
	if (TechniqueComponent)
	{
		TechniqueComponent->TryCast(Slot); // validation/anticipation/release pipeline is there
	}
}

void AQinglanCharacter::InputTalismanSlot(int32 Slot)
{
	// docs/02 §1 slots Q/R; charge accounting and effect execution live in the
	// talisman component (docs/03 §5).
	if (UQinglanTalismanComponent* const Talisman = QinglanFindComponent<UQinglanTalismanComponent>(this))
	{
		Talisman->UseSlot(Slot);
	}
}

void AQinglanCharacter::InputInteract(const FInputActionValue& /*Value*/)
{
	// FIXME-CONTRACT: interactables (realm doors, shrines, records) have no interface or
	// subsystem in the scaffold; interaction routing lands with the realm module.
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Interact pressed; no interactable subsystem in scaffold"));
}

void AQinglanCharacter::InputPause(const FInputActionValue& /*Value*/)
{
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
