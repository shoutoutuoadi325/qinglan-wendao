// Player character implementation: Enhanced Input wiring, third-person camera with
// sphere-sweep obstruction, lock-on, dodge, and routing into the sword/technique
// components. Numbers from docs/02-combat-and-camera.md §2-§4.
#include "Combat/QinglanCharacter.h"

#include "QinglanAscension.h"
#include "Combat/QinglanCharacterAnimInstance.h"
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
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
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
#include "Framework/Application/SlateApplication.h"
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
	// The camera sits a full arm length behind the player, so a station at the
	// player's feet is already ~350 cm from the camera. Capping the camera leg
	// at the proximity range made every nearby station unfocusable (AUD-26):
	// allow the arm length plus the proximity reach.
	constexpr float QinglanInteractCameraReach = QinglanInteractRange + 350.f + 100.f;
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

	struct FQinglanInteractCandidate
	{
		AQinglanInteractable* Actor = nullptr;
		float PlayerDistance = 0.f;
		float Dot = 0.f;
	};

	/**
	 * AUD-26: aimed interactable. All of the following must hold:
	 *  - player -> interactable distance <= 300 cm,
	 *  - camera -> interactable distance <= 300 cm,
	 *  - the interactable lies inside the camera-aim cone (dot >= 0.5),
	 *  - the camera has ECC_Visibility line of sight (player ignored).
	 * Overlaps are resolved deterministically: closer to the player first,
	 * then larger aim dot, then actor name as the stable tie-break.
	 */
	AQinglanInteractable* QinglanFindAimedInteractable(AQinglanCharacter* Self)
	{
		UWorld* const World = Self ? Self->GetWorld() : nullptr;
		if (!World)
		{
			return nullptr;
		}
		const FVector PlayerLocation = Self->GetActorLocation();
		const FVector AimOrigin = Self->FollowCamera ? Self->FollowCamera->GetComponentLocation() : PlayerLocation;
		const FVector AimDirection = Self->FollowCamera
			? Self->FollowCamera->GetComponentRotation().Vector()
			: Self->GetActorForwardVector();

		TArray<FQinglanInteractCandidate> Candidates;
		for (TActorIterator<AQinglanInteractable> It(World); It; ++It)
		{
			AQinglanInteractable* const Candidate = *It;
			const FVector AnchorLocation = Candidate->GetActorLocation();
			const float PlayerDistance = FVector::Dist(PlayerLocation, AnchorLocation);
			if (PlayerDistance > QinglanInteractRange)
			{
				continue;
			}
			// Authored anchors sit on the walking surface. Aim/visibility use a
			// point above that surface so the floor cannot occlude the E prompt.
			const FVector TargetLocation = AnchorLocation + FVector(0.f, 0.f, 80.f);
			const FVector CameraDelta = TargetLocation - AimOrigin;
			if (CameraDelta.Size() > QinglanInteractCameraReach)
			{
				continue;
			}
			const float Dot = FVector::DotProduct(AimDirection, CameraDelta.GetSafeNormal());
			if (Dot < QinglanInteractAimDot)
			{
				continue;
			}
			// Visibility: the interaction anchor itself has no collision, so any
			// blocking hit between the camera and the anchor obscures it.
			FCollisionQueryParams Params(SCENE_QUERY_STAT(QinglanInteractAimTrace), /*bTraceComplex=*/false, Self);
			Params.AddIgnoredActor(Self);
			FHitResult Hit;
			const bool bBlocked = World->LineTraceSingleByChannel(Hit, AimOrigin, TargetLocation, ECC_Visibility, Params);
			if (bBlocked && Hit.GetActor() != Candidate)
			{
				continue;
			}
			Candidates.Add({ Candidate, PlayerDistance, Dot });
		}

		Candidates.Sort([](const FQinglanInteractCandidate& A, const FQinglanInteractCandidate& B)
		{
			if (!FMath::IsNearlyEqual(A.PlayerDistance, B.PlayerDistance))
			{
				return A.PlayerDistance < B.PlayerDistance; // closer to the player wins
			}
			if (!FMath::IsNearlyEqual(A.Dot, B.Dot))
			{
				return A.Dot > B.Dot; // better aim wins
			}
			return A.Actor->GetName() < B.Actor->GetName(); // deterministic tie-break
		});
		return Candidates.IsEmpty() ? nullptr : Candidates[0].Actor;
	}

	/** AUD-26: "action.interact" + the spawner's DisplayNameKey when authored. */
	FText QinglanMakeInteractPrompt(const AQinglanInteractable* Target)
	{
		const FName ActionKey(TEXT("action.interact"));
		const FText Action = UQinglanHUDStatics::HasGameText(ActionKey)
			? UQinglanHUDStatics::GameText(ActionKey)
			: NSLOCTEXT("Qinglan", "Interact.Action", "Interact");
		if (!Target || Target->DisplayNameKey.IsNone()
			|| !UQinglanHUDStatics::HasGameText(Target->DisplayNameKey))
		{
			return Action;
		}
		const FText Name = UQinglanHUDStatics::GameText(Target->DisplayNameKey);
		if (Name.IsEmpty())
		{
			return Action;
		}
		// Ordered-format fallback pattern (no new string-table key authored).
		return FText::Format(NSLOCTEXT("Qinglan", "Interact.Prompt", "{0}: {1}"), Action, Name);
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

	// --- Gameplay-proof event producers (AUD-11) ---

	/**
	 * Character -> proof relay. The actor owns the relay through a UPROPERTY,
	 * so it survives while the actor is alive without rooting its world. Dynamic
	 * delegates bind UFUNCTIONs only; the relay forwards stable proof event
	 * names to QinglanStoryRuntime::NotifyGameplayEvent.
	 */
	void QinglanBindGameplayProofEvents(AQinglanCharacter* Character, UQinglanStorySubsystem* Story,
		TObjectPtr<UQinglanGameplayProofRelay>& RelaySlot)
	{
		if (Character == nullptr || Story == nullptr)
		{
			return;
		}
		if (!RelaySlot)
		{
			RelaySlot = NewObject<UQinglanGameplayProofRelay>(Character);
		}
		UQinglanGameplayProofRelay* Relay = RelaySlot.Get();
		Relay->StorySubsystem = Story;
		Relay->OwnerActor = Character;
		if (UQinglanCombatComponent* Combat = Character->GetCombat())
		{
			Combat->OnDamageResultResolved.AddUniqueDynamic(Relay, &UQinglanGameplayProofRelay::HandleDamageResultResolved);
		}
		if (UQinglanDodgeComponent* Dodge = QinglanFindComponent<UQinglanDodgeComponent>(Character))
		{
			Dodge->OnDodgeStarted.AddUniqueDynamic(Relay, &UQinglanGameplayProofRelay::HandleDodgeStarted);
		}
		if (UQinglanTechniqueComponent* Techniques = Character->GetTechniques())
		{
			Techniques->OnCastReleased.AddUniqueDynamic(Relay, &UQinglanGameplayProofRelay::HandleCastReleased);
		}
	}

	void QinglanUnbindGameplayProofEvents(AQinglanCharacter* Character,
		TObjectPtr<UQinglanGameplayProofRelay>& RelaySlot)
	{
		if (Character == nullptr)
		{
			return;
		}
		UQinglanGameplayProofRelay* Relay = RelaySlot.Get();
		if (Relay == nullptr)
		{
			return;
		}
		if (UQinglanCombatComponent* Combat = Character->GetCombat())
		{
			Combat->OnDamageResultResolved.RemoveDynamic(Relay, &UQinglanGameplayProofRelay::HandleDamageResultResolved);
		}
		if (UQinglanDodgeComponent* Dodge = QinglanFindComponent<UQinglanDodgeComponent>(Character))
		{
			Dodge->OnDodgeStarted.RemoveDynamic(Relay, &UQinglanGameplayProofRelay::HandleDodgeStarted);
		}
		if (UQinglanTechniqueComponent* Techniques = Character->GetTechniques())
		{
			Techniques->OnCastReleased.RemoveDynamic(Relay, &UQinglanGameplayProofRelay::HandleCastReleased);
		}
		Relay->StorySubsystem.Reset();
		Relay->OwnerActor.Reset();
		RelaySlot = nullptr;
	}

	/** Authored room-clear realm token ("tutorial_guided" -> "tutorial"). */
	FString QinglanRoomClearRealmToken(FName RealmId)
	{
		const FString Id = RealmId.ToString();
		if (Id.StartsWith(TEXT("tutorial"))) { return TEXT("tutorial"); }
		if (Id.StartsWith(TEXT("bamboo"))) { return TEXT("bamboo"); }
		if (Id.StartsWith(TEXT("ashen"))) { return TEXT("ashen"); }
		return Id;
	}

	// AUD-44: ABP_Human's parent is the native state machine; its compiled
	// Root+DefaultSlot graph supplies the pose output that native slot montages
	// require. Missing/incompatible assets fall back to the native class.
	const TCHAR* const HumanAnimBlueprintPackage = TEXT("/Game/Qinglan/Characters/Human/ABP_Human");
	const TCHAR* const HumanAnimBlueprintClassPath =
		TEXT("/Game/Qinglan/Characters/Human/ABP_Human.ABP_Human_C");

	UClass* ResolveHumanoidAnimInstanceClass(const USkeletalMesh* Mesh)
	{
		const USkeleton* const MeshSkeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
		if (MeshSkeleton && FPackageName::DoesPackageExist(HumanAnimBlueprintPackage))
		{
			if (UClass* const AnimBlueprintClass =
					LoadClass<UQinglanCharacterAnimInstance>(nullptr, HumanAnimBlueprintClassPath))
			{
				const UAnimBlueprintGeneratedClass* const GeneratedClass =
					Cast<UAnimBlueprintGeneratedClass>(AnimBlueprintClass);
				if (GeneratedClass && GeneratedClass->GetTargetSkeleton() == MeshSkeleton)
				{
					return AnimBlueprintClass;
				}
			}
		}
		return UQinglanCharacterAnimInstance::StaticClass();
	}
}

// ---------------------------------------------------------------------------
// UQinglanGameplayProofRelay
// ---------------------------------------------------------------------------

void UQinglanGameplayProofRelay::HandleDamageResultResolved(const FQinglanResolvedDamage& Result)
{
	UQinglanStorySubsystem* Story = StorySubsystem.Get();
	AActor* Owner = OwnerActor.Get();
	if (Story == nullptr || Owner == nullptr || Result.Attacker.Get() != Owner)
	{
		return;
	}
	// The resolved result carries only the cast id; technique proofs are
	// emitted by HandleCastReleased, whose delegate carries TechniqueId.
	QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("combat.sword_hit")), {});
}

void UQinglanGameplayProofRelay::HandleDodgeStarted(float /*Duration*/)
{
	UQinglanStorySubsystem* Story = StorySubsystem.Get();
	if (Story != nullptr)
	{
		QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("combat.dodge")), {});
	}
}

void UQinglanGameplayProofRelay::HandleCastReleased(FName TechniqueId, FQinglanCastId /*CastId*/)
{
	UQinglanStorySubsystem* Story = StorySubsystem.Get();
	if (Story == nullptr || TechniqueId.IsNone())
	{
		return;
	}
	QinglanStoryRuntime::NotifyGameplayEvent(Story,
		FName(*FString::Printf(TEXT("cast.%s"), *TechniqueId.ToString())), {});
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
	//
	// AUD-44: the mesh runs UQinglanCharacterAnimInstance instead of the old
	// single-node looping idle. The native instance derives planar speed and
	// selects Idle/Walk/Run/Airborne, while the action layer (Attack/Cast/
	// Dodge/Hit/Death) pauses locomotion selection for montage-driven states.
	// Montage playback stays on DefaultSlot. If creating the instance fails
	// (unregistered mesh or class failure), fall back to the single-node idle.
	//
	// Walk/Run and ABP_Human are authored by QinglanAnimBootstrap (the Human rig
	// ships Idle/Jump/Action only), falling back to native/Idle until that
	// commandlet runs. ABP_Human's parent is the native class and its graph is
	// Root + DefaultSlot, so native state machine logic is preserved while slot
	// montages finally produce pose. Still missing from the art side: aim
	// offsets, a layered combat slot, richer transitions, and
	// sword_base/sword_tip/cast_source socket validation.
	if (USkeletalMeshComponent* const MeshComp = GetMesh())
	{
		if (USkeletalMesh* const HeroMesh = Cast<USkeletalMesh>(
				FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/SK_Human")).TryLoad()))
		{
			MeshComp->SetSkeletalMesh(HeroMesh);
			// The Quaternius FBX rigs import at ~5.3 m tall (SK_Human bounds
			// extent Z ~263 cm) against the 1.76 m capsule, which put the
			// third-person camera inside the mesh when rendered. Normalize the
			// component scale so the mesh half-height matches the capsule, and
			// re-seat the feet at the capsule bottom (origin may not be at the
			// feet in the export).
			const FBoxSphereBounds MeshBounds = HeroMesh->GetBounds();
			const float MeshHalfHeight = FMath::Max(1.f, static_cast<float>(MeshBounds.BoxExtent.Z));
			const float MeshScale = QinglanCapsuleHalfHeight / MeshHalfHeight;
			MeshComp->SetRelativeScale3D(FVector(MeshScale));
			const float MeshMinZ = static_cast<float>(MeshBounds.Origin.Z - MeshBounds.BoxExtent.Z);
			MeshComp->SetRelativeLocation(FVector(0.f, 0.f, -QinglanCapsuleHalfHeight - MeshMinZ * MeshScale));
			MeshComp->SetRelativeRotation(FRotator(0.f, -90.f, 0.f)); // rig faces +Y in the export
			// AUD-55: hero skin material imported from the Quaternius pack; the
			// engine grey default read as an untextured blockout in renders.
			if (UMaterialInterface* Skin = Cast<UMaterialInterface>(
					FSoftObjectPath(TEXT("/Game/Qinglan/Materials/MIC_HumanCharacter.MIC_HumanCharacter")).TryLoad()))
			{
				MeshComp->SetMaterial(0, Skin);
			}

			MeshComp->SetAnimInstanceClass(ResolveHumanoidAnimInstanceClass(HeroMesh));
			if (UQinglanCharacterAnimInstance* const AnimInstance =
					Cast<UQinglanCharacterAnimInstance>(MeshComp->GetAnimInstance()))
			{
				AnimInstance->SetActionState(EQinglanAnimActionState::Idle);
			}
			else if (UAnimSequence* const Idle = Cast<UAnimSequence>(
					FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Idle.SK_HumanHuman_Armature_Idle")).TryLoad()))
			{
				// AUD-44 fallback: retain the old visible idle when no anim
				// instance could be created, rather than showing the ref pose.
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
		// AUD-32: accepted hits force an invulnerable-to-input hit reaction.
		CombatComponent->OnDamageResolved.AddUniqueDynamic(this, &AQinglanCharacter::HandleDamageResolved);
	}
	BindStoryDelegates();
	// AUD-27: a freshly spawned pawn (including every map travel) re-imports
	// the profile's equipped techniques, then keeps tracking profile commits.
	BindProfileDelegates();
	if (TechniqueComponent)
	{
		TechniqueComponent->SyncLoadoutFromProfile();
	}

	// AUD-01: cold start opens the slot selector instead of silently importing
	// slot 0 over the session. The hub game mode defers its arrival work until
	// a slot is chosen (HandleMenuClosed resumes it).
	OpenProfileFrontendIfNeeded();
}

void AQinglanCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindStoryDelegates();
	UnbindProfileDelegates();
	if (CombatComponent)
	{
		CombatComponent->OnDamageResolved.RemoveDynamic(this, &AQinglanCharacter::HandleDamageResolved);
	}
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
	// AUD-27: the profile may have finished loading between spawn and possession.
	BindProfileDelegates();
	if (TechniqueComponent)
	{
		TechniqueComponent->SyncLoadoutFromProfile();
	}

	// AUD-01: BeginPlay may run before possession on some travel flows; retry
	// the cold-start selector here so it is never skipped for lack of a PC.
	OpenProfileFrontendIfNeeded();

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
	RefreshFinishedSceneDialogue();

	// AUD-32: paused-game-time action clock; DeltaSeconds stops while paused.
	ActionClock += DeltaSeconds;

	// Forced hit reaction ends on its own; death keeps the gate closed.
	if (bInHitReaction && (ActionClock >= HitReactionEndsAt || (CombatComponent && !CombatComponent->IsAlive())))
	{
		bInHitReaction = false;
	}
	// A buffered dodge fires as soon as its action window opens (0.15 s max).
	ProcessBufferedDodge();

	// AUD-44: keep the native anim instance's action layer in sync with the
	// authoritative gameplay components (idle/attack/cast/dodge/hit/death).
	UpdateAnimationActionState();

	// HUD lands on the first player-controlled tick (possession may complete
	// after BeginPlay on dedicated-client flows; UI/QinglanHUDStatics ids).
	if (!bHudCreated && IsPlayerControlled())
	{
		bHudCreated = true;
		EnsureHudCreated();
	}

	UpdateLockOn(DeltaSeconds);
	UpdateCameraObstruction(DeltaSeconds);
	if (IsPlayerControlled())
	{
		UpdateInteractionFocus();
	}

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
	// AUD-51: Escape is the explicit menu back/close key (journal included).
	BindMenuKey(EKeys::Escape, [this]() { CancelMenu(); });
	BindMenuKey(EKeys::Gamepad_FaceButton_Right, [this]() { CancelMenu(); });
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
	// AUD-32: movement is frozen while a scene choice is pending, during the
	// forced hit reaction, and while the dodge owns the velocity.
	if (!CanStartAction(EQinglanActionKind::Move))
	{
		return;
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y), Axis.X);
}

void AQinglanCharacter::InputLook(const FInputActionValue& Value)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	// AUD-51: an open menu/journal owns the camera as well as the action gate.
	if (bMenuOpen) { return; }
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
	// AUD-32: casts (including post-release recovery) cannot be canceled by a
	// normal attack; scene choices/hit reaction/dodge also close this gate.
	if (!CanStartAction(EQinglanActionKind::Sword))
	{
		return;
	}
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->RequestAttack(); // buffering and combo-window rules live in the sword component
		UpdateAnimationActionState();
	}
}

void AQinglanCharacter::InputDodge(const FInputActionValue& /*Value*/)
{
	// A shares the gameplay dodge action, but confirms native menu/choice
	// selections while those overlays own input. Check the physical gamepad
	// key so Space keeps its separate ending-warning acknowledgment behavior.
	const APlayerController* PC = Cast<APlayerController>(Controller);
	const bool bEndingOpen = EndingWidget.IsValid() && EndingWidget->IsInViewport();
	if (PC && PC->IsInputKeyDown(EKeys::Gamepad_FaceButton_Bottom)
		&& (bMenuOpen || !PendingSceneChoices.IsEmpty() || !PendingBlessingChoices.IsEmpty()
			|| !PendingShopChoices.IsEmpty() || bEndingOpen))
	{
		ConfirmMenuSelection();
		return;
	}
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	TryDodge();
}

void AQinglanCharacter::InputLockTarget(const FInputActionValue& /*Value*/)
{
	// AUD-32: scene-choice/menu/death/reaction states own the input focus.
	if (!CanStartAction(EQinglanActionKind::LockOn))
	{
		return;
	}
	ToggleLockOn();
}

void AQinglanCharacter::InputCycleTarget(const FInputActionValue& /*Value*/)
{
	if (!CanStartAction(EQinglanActionKind::LockOn))
	{
		return;
	}
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
	if (!CanStartAction(EQinglanActionKind::Technique))
	{
		return; // AUD-32: committed actions and pending choices freeze casting
	}
	if (TechniqueComponent)
	{
		TechniqueComponent->TryCast(Slot); // validation/anticipation/release pipeline is there
		UpdateAnimationActionState();
	}
}

void AQinglanCharacter::InputTalismanSlot(int32 Slot)
{
	if (EndingWidget.IsValid() && EndingWidget->IsInViewport()) { return; }
	// AUD-51: menus freeze every gameplay action, talismans included.
	if (bMenuOpen) { return; }
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
	// AUD-51: menu overlays capture interaction; Enter/row buttons drive them.
	if (bMenuOpen)
	{
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
		RefreshFinishedSceneDialogue();
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
	if (bMenuOpen)
	{
		CancelMenu();
		return;
	}
	OpenGameMenu();
}

// ---------------------------------------------------------------------------
// Dodge (docs/02 §2: ~450 cm over 0.45 s, 25 stamina, i-frames 0.10-0.28 s,
// no wall pass-through; cancel rules docs/02 §3)
// ---------------------------------------------------------------------------

// --- AUD-32: unified action priority gate ---

bool AQinglanCharacter::IsActionAllowed(EQinglanActionKind Kind, const FQinglanActionGateState& State)
{
	// Priority: Dead > forced HitReaction > menu/scene-choice freeze.
	if (!State.bAlive || State.bHitReaction)
	{
		return false;
	}
	if (State.bSceneChoicePending || State.bMenuOpen)
	{
		return false;
	}

	switch (Kind)
	{
	case EQinglanActionKind::Move:
		return !State.bDodging && !State.bRooted;

	case EQinglanActionKind::Sword:
		// Committed casts (anticipation and post-release recovery) cannot be
		// canceled by a normal attack. The sword's own state machine handles
		// buffering/chaining; dodge and reaction are excluded above.
		return !State.bDodging && !State.bCasting;

	case EQinglanActionKind::Dodge:
		if (State.bRooted || State.bDodging)
		{
			return false; // one dodge at a time; root refuses outright
		}
		// Dodge cancels zero-cost anticipation and the pre-hit sword windup;
		// post-release cast recovery and the active hit window are committed.
		if (State.bCasting)
		{
			return State.bCastAnticipation;
		}
		return !State.bSwordSwinging || State.bSwordCanDodgeCancel;

	case EQinglanActionKind::Technique:
		// Existing docs/02 §3 rule: casting may start from idle/move/attack
		// recovery, but never from a dodge, another cast, or sword
		// windup/active (attack recovery stays legal).
		return !State.bDodging && !State.bCasting && !State.bSwordWindupOrActive;

	case EQinglanActionKind::LockOn:
		return true;

	default:
		return false;
	}
}

EQinglanDodgeRequest AQinglanCharacter::EvaluateDodgeRequest(const FQinglanActionGateState& State, bool bHasStamina)
{
	if (!State.bAlive || State.bHitReaction || State.bSceneChoicePending
		|| State.bMenuOpen || State.bRooted || !bHasStamina)
	{
		return EQinglanDodgeRequest::Reject;
	}
	if (IsActionAllowed(EQinglanActionKind::Dodge, State))
	{
		return EQinglanDodgeRequest::Start;
	}
	// A committed action (sword active/early recovery, post-release cast
	// recovery, another dodge tail) blocks now but will open inside the
	// 0.15 s buffer window if the player pressed early.
	return EQinglanDodgeRequest::Buffer;
}

bool AQinglanCharacter::IsDodgeBufferFresh(float BufferedAtSeconds, float NowSeconds, float WindowSeconds)
{
	return NowSeconds >= BufferedAtSeconds && (NowSeconds - BufferedAtSeconds) <= WindowSeconds;
}

void AQinglanCharacter::GetActionGateState(FQinglanActionGateState& OutState) const
{
	OutState = FQinglanActionGateState();
	OutState.bAlive = CombatComponent && CombatComponent->IsAlive();
	OutState.bRooted = CombatComponent && CombatComponent->HasStatus(EQinglanStatus::Root);
	OutState.bHitReaction = bInHitReaction;

	if (const UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this))
	{
		OutState.bDodging = Dodge->IsDodging();
	}
	if (TechniqueComponent)
	{
		OutState.bCasting = TechniqueComponent->IsCasting();
		OutState.bCastAnticipation = TechniqueComponent->IsInAnticipation();
		OutState.bCastRecovery = TechniqueComponent->IsInRecovery();
	}
	if (const UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		const EQinglanSwordPhase Phase = Sword->GetPhase();
		OutState.bSwordSwinging = Phase != EQinglanSwordPhase::Idle;
		OutState.bSwordWindupOrActive = Phase == EQinglanSwordPhase::Windup
			|| Phase == EQinglanSwordPhase::Active;
		OutState.bSwordActive = Phase == EQinglanSwordPhase::Active;
		OutState.bSwordCanDodgeCancel = Sword->CanDodgeCancelNow();
	}
	OutState.bSceneChoicePending = PendingSceneChoices.Num() > 0;
	// AUD-51: pause/profile/settings/equipment/journal/offer overlays share the
	// same freeze as scene choices; the ending modal remains an additional gate.
	OutState.bMenuOpen = bMenuOpen
		|| (EndingWidget.IsValid() && EndingWidget->IsInViewport());
}

bool AQinglanCharacter::CanStartAction(EQinglanActionKind Kind) const
{
	FQinglanActionGateState State;
	GetActionGateState(State);
	return IsActionAllowed(Kind, State);
}

bool AQinglanCharacter::CanDodge() const
{
	return CanStartAction(EQinglanActionKind::Dodge);
}

void AQinglanCharacter::TryDodge()
{
	FQinglanActionGateState State;
	GetActionGateState(State);
	const bool bHasStamina = CombatComponent
		&& CombatComponent->GetAttributes().Stamina + KINDA_SMALL_NUMBER >= DodgeStaminaCost;

	switch (EvaluateDodgeRequest(State, bHasStamina))
	{
	case EQinglanDodgeRequest::Start:
		StartDodgeNow();
		break;
	case EQinglanDodgeRequest::Buffer:
		// Early press: retained for 0.15 s and fired by ProcessBufferedDodge
		// as soon as the committed action reaches a cancellable boundary.
		bHasBufferedDodge = true;
		BufferedDodgeAt = ActionClock;
		break;
	case EQinglanDodgeRequest::Reject:
	default:
		break;
	}
}

void AQinglanCharacter::StartDodgeNow()
{
	UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this);
	if (!Dodge || !CanDodge())
	{
		return;
	}
	// Defensive ordering: never spend stamina for a dodge the gate must reject.
	if (TechniqueComponent && TechniqueComponent->IsCasting() && !TechniqueComponent->IsInAnticipation())
	{
		return; // released/recoving casts are not cancellable
	}
	// Stamina commits only when the dodge actually starts (buffering is free).
	if (!CombatComponent || !CombatComponent->TrySpendStamina(DodgeStaminaCost))
	{
		return;
	}

	// Cancel lower-priority committed actions (docs/02 §3). Post-release cast
	// recovery is not cancellable and was already excluded above/gate.
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->InterruptSwing();
	}
	if (TechniqueComponent && TechniqueComponent->IsCasting())
	{
		TechniqueComponent->InterruptCast(); // pre-release: zero cost, never refunded
	}

	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		// Keep MaxWalkSpeed at dodge speed so the movement component never clamps the
		// driven velocity; HandleDodgeFinished restores WalkSpeed.
		Movement->MaxWalkSpeed = DodgeDistance / DodgeDuration;
	}

	bHasBufferedDodge = false;
	Dodge->StartDodge(QinglanComputeDodgeDirection(this), DodgeDistance, DodgeDuration, GetDodgeIFrameStart(), GetDodgeIFrameEnd());
	UpdateAnimationActionState();
}

void AQinglanCharacter::ProcessBufferedDodge()
{
	if (!bHasBufferedDodge)
	{
		return;
	}
	if (!IsDodgeBufferFresh(BufferedDodgeAt, ActionClock, DodgeInputBufferSeconds))
	{
		bHasBufferedDodge = false; // the 0.15 s window elapsed unused
		return;
	}

	FQinglanActionGateState State;
	GetActionGateState(State);
	const bool bHasStamina = CombatComponent
		&& CombatComponent->GetAttributes().Stamina + KINDA_SMALL_NUMBER >= DodgeStaminaCost;
	switch (EvaluateDodgeRequest(State, bHasStamina))
	{
	case EQinglanDodgeRequest::Start:
		bHasBufferedDodge = false;
		StartDodgeNow();
		break;
	case EQinglanDodgeRequest::Reject:
		bHasBufferedDodge = false; // death/reaction/menu/root: drop the press
		break;
	case EQinglanDodgeRequest::Buffer:
	default:
		break; // still inside the window, keep waiting
	}
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
	// Authored on SK_Human_Skeleton at RightHand by QinglanAnimBootstrap (AUD-44).
	static const FName CastSocketName(TEXT("cast_source"));

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
	// AUD-32: forced hit reaction outranks every committed action.
	if (bInHitReaction)
	{
		return EQinglanCombatState::HitReaction;
	}
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

AQinglanInteractable* AQinglanCharacter::GetFocusedInteractable() const
{
	return FocusedInteractable.Get();
}

FText AQinglanCharacter::GetInteractPromptText() const
{
	const AQinglanInteractable* const Focus = FocusedInteractable.Get();
	return Focus ? QinglanMakeInteractPrompt(Focus) : FText::GetEmpty();
}

void AQinglanCharacter::UpdateInteractionFocus()
{
	// AUD-26: selection is recomputed every frame so the hint disappears as soon
	// as the player/camera moves out of range or the line of sight is blocked.
	if (bDefeatHandled || bInHitReaction)
	{
		FocusedInteractable = nullptr;
		return;
	}
	AQinglanInteractable* const NewFocus = QinglanFindAimedInteractable(this);
	AQinglanInteractable* const OldFocus = FocusedInteractable.Get();
	if (NewFocus == OldFocus)
	{
		return;
	}
	FocusedInteractable = NewFocus;
	if (!NewFocus)
	{
		return;
	}
	// Never stomp a dialogue/menu presentation with the proximity hint.
	if (!PendingSceneChoices.IsEmpty()
		|| (EndingWidget.IsValid() && EndingWidget->IsInViewport()))
	{
		return;
	}
	if (UGameInstance* const Instance = GetGameInstance())
	{
		if (const UQinglanStorySubsystem* const Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
			Story && Story->IsScenePlaying())
		{
			return;
		}
	}
	// Localized suggestion: existing "action.interact" key plus the spawner's
	// DisplayNameKey when the string table resolves it. Uses the established
	// ShowGameMessage HUD channel (dialogue widget when open, on-screen text
	// otherwise); no new localization keys are introduced.
	ShowGameMessage(QinglanMakeInteractPrompt(NewFocus));
}

void AQinglanCharacter::HandleProfileCommitted()
{
	// AUD-27: every equipment/unlock commit re-imports the profile order.
	if (TechniqueComponent)
	{
		TechniqueComponent->SyncLoadoutFromProfile();
	}
}

void AQinglanCharacter::BindProfileDelegates()
{
	if (bProfileDelegatesBound)
	{
		return;
	}
	UGameInstance* const Instance = GetGameInstance();
	UQinglanProfileSubsystem* const Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (!Profile)
	{
		return; // possession retries once the profile session exists
	}
	Profile->OnProfileCommitted.AddUniqueDynamic(this, &AQinglanCharacter::HandleProfileCommitted);
	bProfileDelegatesBound = true;
}

void AQinglanCharacter::UnbindProfileDelegates()
{
	if (!bProfileDelegatesBound)
	{
		return;
	}
	UGameInstance* const Instance = GetGameInstance();
	if (UQinglanProfileSubsystem* const Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr)
	{
		Profile->OnProfileCommitted.RemoveDynamic(this, &AQinglanCharacter::HandleProfileCommitted);
	}
	bProfileDelegatesBound = false;
}

void AQinglanCharacter::HandleDamageResolved(const FQinglanDamageEvent& Event)
{
	// AUD-32: the victim's own accepted hit forces a short reaction. Death is
	// handled by HandleHealthChanged (OnHealthChanged fires first); a lethal
	// event therefore never enters a reaction window.
	if (Event.Target.Get() != this || bDefeatHandled)
	{
		return;
	}
	if (!CombatComponent || !CombatComponent->IsAlive())
	{
		return;
	}
	if (Event.FinalMagnitude <= 0.f && !Event.bBlockedByBarrier)
	{
		return;
	}

	bInHitReaction = true;
	HitReactionEndsAt = ActionClock + FMath::Max(0.01f, HitReactionSeconds);
	bHasBufferedDodge = false;

	// A forced reaction outranks committed actions: cancel the sword and any
	// cast (released techniques keep their committed cost).
	if (UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this))
	{
		Sword->InterruptSwing();
	}
	if (TechniqueComponent && TechniqueComponent->IsCasting())
	{
		TechniqueComponent->InterruptCast();
	}
	// While no authored reaction montage exists, the action gate alone at least
	// guarantees an immobile, unactionable window (docs/02 §3 "forced").
	if (UCharacterMovementComponent* const Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}
	PlayHitReactionMontage();
	UpdateAnimationActionState();
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Forced hit reaction for %.2fs"), HitReactionSeconds);
}

void AQinglanCharacter::PlayHitReactionMontage()
{
	// Convention path authored by QinglanAnimBootstrap when available. Missing
	// animation is non-fatal: the gate above still disables every action.
	UAnimMontage* const Montage = Cast<UAnimMontage>(
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_Human_HitReact.AM_Human_HitReact")).TryLoad());
	USkeletalMeshComponent* const MeshComp = GetMesh();
	UAnimInstance* const AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (AnimInstance && Montage)
	{
		AnimInstance->Montage_Play(Montage, 1.f);
	}
	else
	{
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Hit-reaction montage unavailable; action gate supplies the forced reaction"));
	}
}

void AQinglanCharacter::UpdateAnimationActionState()
{
	// AUD-44: presentation-only mirror of the authoritative action gates. The
	// animation instance never feeds back into gameplay; if the instance or the
	// rig is absent this is a no-op (headless/automation worlds).
	USkeletalMeshComponent* const MeshComp = GetMesh();
	UQinglanCharacterAnimInstance* const AnimInstance =
		MeshComp ? Cast<UQinglanCharacterAnimInstance>(MeshComp->GetAnimInstance()) : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	EQinglanAnimActionState DesiredState = EQinglanAnimActionState::Idle;
	const UQinglanDodgeComponent* const Dodge = QinglanFindComponent<UQinglanDodgeComponent>(this);
	const UQinglanSwordComponent* const Sword = QinglanFindComponent<UQinglanSwordComponent>(this);
	if (bDefeatHandled || (CombatComponent && !CombatComponent->IsAlive()))
	{
		DesiredState = EQinglanAnimActionState::Death;
	}
	else if (bInHitReaction)
	{
		DesiredState = EQinglanAnimActionState::Hit;
	}
	else if (Dodge && Dodge->IsDodging())
	{
		DesiredState = EQinglanAnimActionState::Dodge;
	}
	else if (TechniqueComponent && TechniqueComponent->IsCasting())
	{
		DesiredState = EQinglanAnimActionState::Cast;
	}
	else if (Sword && Sword->IsSwinging())
	{
		DesiredState = EQinglanAnimActionState::Attack;
	}

	AnimInstance->SetActionState(DesiredState);
}

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

void AQinglanCharacter::RefreshFinishedSceneDialogue()
{
	if (UQinglanDialogueWidget* Dialogue = Cast<UQinglanDialogueWidget>(DialogueWidget.Get()))
	{
		const UGameInstance* Instance = GetGameInstance();
		const UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
		Dialogue->RefreshScenePlayback(Story && Story->IsScenePlaying());
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

UQinglanMenuWidget* AQinglanCharacter::EnsureMenuWidget()
{
	if (UQinglanMenuWidget* Widget = MenuWidget.Get())
	{
		return Widget;
	}
	APlayerController* PC = Cast<APlayerController>(Controller);
	UQinglanMenuWidget* Widget = PC
		? CreateWidget<UQinglanMenuWidget>(PC, UQinglanMenuWidget::StaticClass()) : nullptr;
	if (Widget)
	{
		MenuWidget = Widget;
		Widget->OnMenuClosed.AddDynamic(this, &AQinglanCharacter::HandleMenuClosed);
	}
	return Widget;
}

UQinglanJournalWidget* AQinglanCharacter::EnsureJournalWidget()
{
	if (UQinglanJournalWidget* Widget = JournalWidget.Get())
	{
		return Widget;
	}
	APlayerController* PC = Cast<APlayerController>(Controller);
	UQinglanJournalWidget* Widget = PC
		? CreateWidget<UQinglanJournalWidget>(PC, UQinglanJournalWidget::StaticClass()) : nullptr;
	if (Widget)
	{
		JournalWidget = Widget;
		Widget->OnJournalClosed.AddDynamic(this, &AQinglanCharacter::HandleJournalClosed);
	}
	return Widget;
}

void AQinglanCharacter::OpenGameMenu()
{
	if (bDefeatHandled || (EndingWidget.IsValid() && EndingWidget->IsInViewport()))
	{
		return;
	}
	if (!PendingSceneChoices.IsEmpty())
	{
		return; // scene choices keep the dialogue presenter
	}
	UQinglanMenuWidget* Menu = EnsureMenuWidget();
	if (!Menu)
	{
		return;
	}
	bMenuOpen = true;
	Menu->ShowScreen(EQinglanMenuScreen::Pause);
	if (!Menu->IsInViewport())
	{
		Menu->AddToViewport(50);
	}
	// Pending offers persist across menu closes; re-present them rather than
	// hiding the decision behind the pause screen.
	if (PendingBlessingChoices.Num() > 0)
	{
		PresentBlessingOffer(PendingBlessingChoices);
	}
	else if (PendingShopChoices.Num() > 0)
	{
		PresentShopOffer(PendingShopChoices);
	}
	RefreshMenuInputState();
}

void AQinglanCharacter::OpenProfileFrontendIfNeeded()
{
	if (bProfileFrontendOpen)
	{
		return;
	}
	// QA capture (`-QinglanTrial=`) renders arena hazards without a slot; the
	// selector would cover the frame and the trial does not need a profile.
	FString TrialCaptureValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("QinglanTrial="), TrialCaptureValue))
	{
		return;
	}
	UGameInstance* Instance = GetGameInstance();
	UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	if (!Save || Save->IsProfileSessionActive())
	{
		return; // ordinary travel keeps the in-memory profile; no selector
	}
	// Widgets can only be owned by a local player controller. Headless test
	// worlds and server-side controllers have no UI: they continue without a
	// profile session rather than logging a widget-assignment error.
	const APlayerController* const PC = Cast<APlayerController>(Controller);
	if (!PC || !PC->IsLocalController())
	{
		UE_LOG(LogQinglan, Verbose,
			TEXT("Profile frontend skipped: no local player controller (headless world)"));
		return;
	}
	UQinglanMenuWidget* Menu = EnsureMenuWidget();
	if (!Menu)
	{
		UE_LOG(LogQinglan, Warning, TEXT("Profile frontend unavailable (no menu widget); boot continues without a profile"));
		return;
	}
	bProfileFrontendOpen = true;
	bMenuOpen = true;
	Menu->ShowScreen(EQinglanMenuScreen::Profile);
	if (!Menu->IsInViewport())
	{
		Menu->AddToViewport(60);
	}
	RefreshMenuInputState();
	UE_LOG(LogQinglan, Log, TEXT("Profile frontend open: select a slot to begin (AUD-01)"));

	// QA capture aid: -QinglanAutoSlot=<0-2> confirms a slot through the real
	// menu rows after a short settle, so an unattended capture can reach the
	// hub. It never bypasses save validation; interactive selection acceptance
	// still requires real input.
	FString AutoSlotValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("QinglanAutoSlot="), AutoSlotValue))
	{
		const int32 AutoSlot = FMath::Clamp(FCString::Atoi(*AutoSlotValue), 0,
			UQinglanSaveSubsystem::ProfileSlotCount - 1);
		FTimerHandle AutoSlotHandle;
		GetWorld()->GetTimerManager().SetTimer(AutoSlotHandle, FTimerDelegate::CreateWeakLambda(this, [this, AutoSlot]()
		{
			UQinglanMenuWidget* ActiveMenu = MenuWidget.Get();
			if (!ActiveMenu || !ActiveMenu->IsMenuOpen())
			{
				return;
			}
			for (int32 Step = 0; Step < AutoSlot; ++Step)
			{
				ActiveMenu->MoveSelection(1);
			}
			ActiveMenu->ConfirmSelection();
		}), 2.f, /*bLoop=*/false);
	}
}

void AQinglanCharacter::CloseGameMenu()
{
	if (UQinglanMenuWidget* Menu = MenuWidget.Get())
	{
		Menu->CloseMenu(); // broadcasts; HandleMenuClosed restores input mode
	}
	else
	{
		bMenuOpen = false;
		RefreshMenuInputState();
	}
}

void AQinglanCharacter::OpenJournal(FName RecordId)
{
	if (bDefeatHandled || (EndingWidget.IsValid() && EndingWidget->IsInViewport()))
	{
		return;
	}
	if (!PendingSceneChoices.IsEmpty())
	{
		return; // scene choices own the input focus (AUD-32 gate compatibility)
	}
	UQinglanJournalWidget* Journal = EnsureJournalWidget();
	if (!Journal)
	{
		return;
	}
	if (MenuWidget.IsValid() && MenuWidget->IsInViewport())
	{
		MenuWidget->RemoveFromParent(); // journal is the top overlay
	}
	bMenuOpen = true;
	if (!Journal->IsInViewport())
	{
		Journal->AddToViewport(60);
	}
	Journal->OpenJournal();
	if (!RecordId.IsNone())
	{
		Journal->OpenRecord(RecordId);
	}
	RefreshMenuInputState();
}

bool AQinglanCharacter::SelectMenuOption(int32 Index)
{
	if (!MenuWidget.IsValid() || !MenuWidget->IsMenuOpen())
	{
		return false;
	}
	return ConsumeMenuSelection(Index);
}

bool AQinglanCharacter::DismissMenuOffer()
{
	UQinglanMenuWidget* Menu = MenuWidget.Get();
	// Blessing drafts are mandatory: leaving would strand the proof event.
	if (PendingBlessingChoices.Num() > 0)
	{
		if (Menu)
		{
			Menu->SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.offer_required"))));
		}
		return false;
	}
	PendingShopChoices.Reset();
	if (Menu)
	{
		Menu->CloseOffer();
	}
	RefreshMenuInputState();
	return true;
}

void AQinglanCharacter::QuitToSanctuary()
{
	UGameInstance* const Instance = GetGameInstance();
	UQinglanSaveSubsystem* const Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	if (!Save)
	{
		// Safe-exit contract: without the save API the only available action is
		// Resume (closing the menu), never an unsaved map travel.
		CloseGameMenu();
		return;
	}
	FText SaveError;
	if (!Save->SaveActiveSession(SaveError))
	{
		if (UQinglanMenuWidget* Menu = MenuWidget.Get())
		{
			Menu->SetNotice(SaveError.IsEmpty()
				? UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.save_failed")))
				: SaveError);
		}
		return;
	}
	CloseGameMenu();
	if (!UGameplayStatics::GetCurrentLevelName(this, true).Contains(TEXT("Hub")))
	{
		QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
	}
}

void AQinglanCharacter::RefreshMenuInputState()
{
	const bool bJournalShowing = JournalWidget.IsValid() && JournalWidget->IsInViewport();
	const bool bMenuShowing = MenuWidget.IsValid() && MenuWidget->IsInViewport();
	bMenuOpen = bJournalShowing || bMenuShowing;
	UpdateMenuInputMode();
}

void AQinglanCharacter::HandleMenuClosed()
{
	bMenuOpen = JournalWidget.IsValid() && JournalWidget->IsInViewport();

	// AUD-01: leaving the cold-start selector is what starts the session. If a
	// slot was chosen, run the hub arrival work that BeginPlay deferred;
	// otherwise keep the selector up rather than entering a session-less hub.
	if (bProfileFrontendOpen)
	{
		UGameInstance* Instance = GetGameInstance();
		UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
		if (Save && Save->IsProfileSessionActive())
		{
			bProfileFrontendOpen = false;
			if (AQinglanHubGameMode* Hub = GetWorld()
					? Cast<AQinglanHubGameMode>(GetWorld()->GetAuthGameMode()) : nullptr)
			{
				Hub->CompleteHubArrivalAfterProfileSelection();
			}
		}
		else
		{
			bProfileFrontendOpen = false;
			OpenProfileFrontendIfNeeded();
			return; // stay in UI-only input mode
		}
	}
	UpdateMenuInputMode();
}

void AQinglanCharacter::HandleJournalClosed()
{
	// Closing the record viewer restores gameplay even when it was reached
	// from the pause menu; the pause menu reopens through its own Journal row.
	if (UQinglanMenuWidget* Menu = MenuWidget.Get(); Menu && Menu->IsMenuOpen())
	{
		Menu->CloseMenu(); // broadcasts HandleMenuClosed
	}
	bMenuOpen = false;
	UpdateMenuInputMode();
}

void AQinglanCharacter::UpdateMenuInputMode()
{
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC) { return; }
	UUserWidget* Focus = nullptr;
	// AUD-51: native menu/journal overlays keep keyboard focus on the game
	// viewport so the Character's Up/Down/Enter/Escape/number bindings drive
	// them; mouse clicks still resolve through Slate hit-testing.
	bool bGameViewportKeyboard = false;
	if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport()) { Focus = Ending; }
	else if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		Focus = Journal;
		bGameViewportKeyboard = true;
	}
	else if (UQinglanMenuWidget* Menu = MenuWidget.Get(); Menu && Menu->IsInViewport())
	{
		Focus = Menu;
		bGameViewportKeyboard = true;
	}
	else if (!PendingSceneChoices.IsEmpty()) { Focus = DialogueWidget.Get(); }
	PC->bShowMouseCursor = Focus != nullptr;
	if (Focus)
	{
		FInputModeGameAndUI Mode;
		if (!bGameViewportKeyboard)
		{
			Mode.SetWidgetToFocus(Focus->TakeWidget());
		}
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
		if (bGameViewportKeyboard)
		{
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().SetAllUserFocusToGameViewport();
			}
		}
		else if (UQinglanEndingWidget* Ending = EndingWidget.Get(); Ending && Ending->IsInViewport())
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
	// AUD-51: native menus take Up/Down while open; journal navigation moves
	// the record list instead.
	if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		Journal->MoveRecordSelection(Direction);
		return;
	}
	if (UQinglanMenuWidget* Menu = MenuWidget.Get(); Menu && Menu->IsMenuOpen())
	{
		Menu->MoveSelection(Direction);
		return;
	}
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
	if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		if (Journal->IsRecordOpen())
		{
			// Enter on an open record returns to the list; the list itself
			// opens the highlighted record.
			Journal->CloseRecord();
		}
		else
		{
			Journal->SelectCurrentRecord();
		}
		return;
	}
	if (UQinglanMenuWidget* Menu = MenuWidget.Get(); Menu && Menu->IsMenuOpen())
	{
		Menu->ConfirmSelection();
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
	if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		Journal->Back(); // record -> list -> closed (OnJournalClosed restores mode)
		return;
	}
	if (UQinglanMenuWidget* Menu = MenuWidget.Get(); Menu && Menu->IsMenuOpen())
	{
		Menu->Back(); // offer dismisses, sub-screens return to Pause, Pause closes
		return;
	}
	if (bMenuOpen)
	{
		// Defensive: a stale overlay flag must never trap gameplay input.
		bMenuOpen = false;
		RefreshMenuInputState();
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
			if (Dialogue->SetScenePlaybackLine(Line.SpeakerName, Line.Text))
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
	// Defensive (e.g. resumed playback): choices outrank any open overlay.
	if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		Journal->CloseJournal();
	}
	else if (bMenuOpen)
	{
		CloseGameMenu();
	}
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
	// AUD-51/AUD-32: narrative playback owns the input focus. Close menu or
	// journal overlays so scene choices never share the numeric/confirm keys.
	if (UQinglanJournalWidget* Journal = JournalWidget.Get(); Journal && Journal->IsInViewport())
	{
		Journal->CloseJournal(); // broadcast restores gameplay/menu state
	}
	else if (bMenuOpen)
	{
		CloseGameMenu();
	}
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

bool AQinglanCharacter::HasSceneScriptExhaustedForInspection(FName SceneId) const
{
	const UGameInstance* const Instance = GetGameInstance();
	const UQinglanStorySubsystem* const Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (!Story || SceneId.IsNone())
	{
		return false;
	}
	if (Story->IsSceneCompleted(SceneId))
	{
		return true;
	}
	return SceneId == LastEnteredSceneId && bSawLineForEnteredScene && !Story->IsScenePlaying();
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
	if (Realm == nullptr)
	{
		return;
	}
	// AUD-11: every non-boss clear proves the authored room event
	// encounter.room_cleared.<realm>.<room> (main-early CH01, main-late CH04).
	if (Realm->GetRoomKind(RoomIndex) != EQinglanRoomKind::Boss)
	{
		QinglanStoryRuntime::NotifyGameplayEvent(Story,
			FName(*FString::Printf(TEXT("encounter.room_cleared.%s.%d"),
				*QinglanRoomClearRealmToken(Realm->RealmId), RoomIndex)), {});
		return;
	}
	// Dispatch the exact defeated encounter; never scan for the first eligible
	// chapter scene (late chapters deliberately reuse local objective IDs).
	// AUD-11: emit the authored boss-defeat proof event first; the guard-based
	// auto-completion then proves the exact objective, and the direct notify
	// below stays idempotent.
	if (Realm->RealmId == FName(UQinglanEconomyLibrary::BambooRealmIdText))
	{
		QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("encounter.boss_defeated.bamboo")), {});
		Story->NotifySceneObjectiveCompleted(FName(TEXT("CH02_S02")), FName(TEXT("CH02_S02_OBJECTIVE")));
	}
	else if (Realm->RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
	{
		QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("encounter.boss_defeated.ashen")), {});
		Story->NotifySceneObjectiveCompleted(FName(TEXT("CH05_S01")), FName(TEXT("s01.objective")));
	}
}

void AQinglanCharacter::HandleRecordOpened(FName RecordId, const FText& Title, const FText& Body)
{
	// AUD-52: the record title also lands in the HUD text region so a player
	// who continues after the dialogue notices the collection.
	if (UQinglanHUDWidget* HUD = Cast<UQinglanHUDWidget>(HudWidget.Get()))
	{
		HUD->ShowRecordNotice(Title);
	}
	// AUD-11 producer: the read itself is the proof event; authored proofs use
	// the stable record id (record.opened.<id>).
	if (RecordId != NAME_None)
	{
		if (UGameInstance* const Instance = GetGameInstance())
		{
			if (UQinglanStorySubsystem* const Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
			{
				QinglanStoryRuntime::NotifyGameplayEvent(Story,
					FName(*FString::Printf(TEXT("record.opened.%s"), *RecordId.ToString())), {});
			}
		}
	}
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
				// AUD-52: the registry has no authored record source, so the
				// dialogue tag uses the same id-prefix inference as the journal.
				Dialogue->SetSourceKind(FQinglanJournalState::InferRecordSourceKind(RecordId));
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
	UpdateAnimationActionState(); // AUD-44: Death action state, locomotion paused
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
	// AUD-11: bind the dynamic combat/dodge/technique delegates through the
	// actor-owned proof relay, which shares the character's GC lifetime.
	QinglanBindGameplayProofEvents(this, Story, GameplayProofRelay);
	bStoryDelegatesBound = true;
}

void AQinglanCharacter::UnbindStoryDelegates()
{
	if (!bStoryDelegatesBound)
	{
		return;
	}
	QinglanUnbindGameplayProofEvents(this, GameplayProofRelay);
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
	TArray<FQinglanOfferChoice> MenuOffers;
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Offers.Num(); ++Index)
	{
		const FText Name = Offers[Index].DisplayName.IsEmpty()
			? FText::FromString(Offers[Index].BlessingId.ToString())
			: Offers[Index].DisplayName;
		FQinglanOfferChoice Choice;
		Choice.OfferId = Offers[Index].BlessingId;
		Choice.Title = Name;
		Choice.bBlessing = true;
		MenuOffers.Add(Choice);
		Lines.Add(FString::Printf(TEXT("[%d] %s"), Index + 1, *Name.ToString()));
	}
	// AUD-51: native offer menu first; the keyed on-screen list remains the
	// fallback when a scene choice currently owns the numeric row or the menu
	// widget cannot be created.
	if (PendingSceneChoices.IsEmpty())
	{
		if (UQinglanMenuWidget* Menu = EnsureMenuWidget())
		{
			bMenuOpen = true;
			Menu->ShowOffers(MenuOffers);
			if (!Menu->IsInViewport()) { Menu->AddToViewport(50); }
			RefreshMenuInputState();
			return;
		}
	}
	ShowGameMessage(FText::FromString(FString::Join(Lines, TEXT("  "))));
}

void AQinglanCharacter::PresentShopOffer(const TArray<FQinglanShopOfferEntry>& Entries)
{
	PendingShopChoices = Entries;
	TArray<FQinglanOfferChoice> MenuOffers;
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		FQinglanOfferChoice Choice;
		Choice.OfferId = Entries[Index].ItemId;
		Choice.Title = Entries[Index].Title;
		Choice.Cost = Entries[Index].CostSummary;
		Choice.bBlessing = false;
		MenuOffers.Add(Choice);
		Lines.Add(FString::Printf(TEXT("[%d] %s (%s)"),
			Index + 1, *Entries[Index].Title.ToString(), *Entries[Index].CostSummary.ToString()));
	}
	if (PendingSceneChoices.IsEmpty())
	{
		if (UQinglanMenuWidget* Menu = EnsureMenuWidget())
		{
			bMenuOpen = true;
			Menu->ShowOffers(MenuOffers);
			if (!Menu->IsInViewport()) { Menu->AddToViewport(50); }
			RefreshMenuInputState();
			return;
		}
	}
	ShowGameMessage(FText::FromString(FString::Join(Lines, TEXT("  "))));
}

void AQinglanCharacter::ShowGameMessage(const FText& Message)
{
	// Shipping builds do not draw the engine's debug-message fallback. Keep
	// interaction and blocked-action notices in the normal HUD, without
	// reopening a finished conversation over the player's view.
	if (UQinglanHUDWidget* HUD = Cast<UQinglanHUDWidget>(HudWidget.Get()))
	{
		HUD->ShowBarkLine(FText::GetEmpty(), Message);
		return;
	}
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
	// AUD-51: every numeric slot routed while a menu overlay is open is
	// captured on the menu state; CloseMenu() clears that capture list.
	UQinglanMenuWidget* Menu = MenuWidget.Get();
	const bool bMenuOpenNow = Menu && Menu->IsMenuOpen();
	if (bMenuOpenNow && Slot >= 0)
	{
		Menu->CaptureNumberKey(Slot);
	}
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
		bool bAccepted = false;
		if (UGameInstance* const Instance = GetGameInstance())
		{
			if (UQinglanRunSubsystem* const Run = Instance->GetSubsystem<UQinglanRunSubsystem>())
			{
				// exactly 1 of 3 (docs/03 section 5)
				bAccepted = Run->AcceptBlessing(Blessing.BlessingId);
				if (bAccepted)
				{
					// AUD-11 producer: accepted draft proof (CH01_S02).
					if (UQinglanStorySubsystem* const Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
					{
						QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("blessing.taken")), {});
					}
				}
			}
		}
		if (!bAccepted)
		{
			// Offer persistence (AUD-51): a failed selection leaves the draft.
			if (Menu) { Menu->SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.offer_failed")))); }
			return true;
		}
		PendingBlessingChoices.Reset();
		if (Menu) { Menu->CloseOffer(); }
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
		if (!BuyShopEntry(Entry))
		{
			// Keep the entry selectable after a failed purchase (AUD-51).
			if (Menu) { Menu->SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.offer_failed")))); }
			return true;
		}
		PendingShopChoices.RemoveAt(Slot);
		if (PendingShopChoices.Num() > 0)
		{
			PresentShopOffer(PendingShopChoices); // re-present the shortened list
		}
		else if (Menu)
		{
			Menu->CloseOffer();
		}
		return true;
	}
	// While a menu overlay is open the number row belongs to the menu even when
	// no offer is active (techniques must not fire behind it). Outside the
	// offer screen the digit is a direct row shortcut.
	if (bMenuOpenNow)
	{
		if (Menu->GetScreen() != EQinglanMenuScreen::Offer)
		{
			Menu->ActivateRow(Slot);
		}
		return true;
	}
	return false;
}

bool AQinglanCharacter::BuyShopEntry(const FQinglanShopOfferEntry& Entry)
{
	UGameInstance* const Instance = GetGameInstance();
	UQinglanProfileSubsystem* const Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (Profile == nullptr)
	{
		return false;
	}
	const FName TransactionId = *FString::Printf(TEXT("shop.%s.%s"),
		Entry.bRelic ? TEXT("relic") : TEXT("passive"), *Entry.ItemId.ToString());
	const bool bCommitted = Entry.bRelic
		? Profile->TryPurchaseRelic(TransactionId, Entry.ItemId)
		: Profile->TryPurchasePassive(TransactionId, Entry.ItemId);
	if (!bCommitted)
	{
		ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.offer_failed"))));
		return false;
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
	return true;
}
