// AUD-44: native locomotion/action anim instance. See the header for the asset
// gaps that still block a fully authored AnimBP presentation.
#include "Combat/QinglanCharacterAnimInstance.h"

#include "QinglanAscension.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/**
	 * Dynamic slot montages have no "infinite" loop flag; their loop count is
	 * baked into the segment length. A large count behaves like a looping clip
	 * and is stopped explicitly when the locomotion band changes.
	 */
	constexpr int32 LocomotionMontageLoops = 1000000;

	const TCHAR* const HumanIdlePath = TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Idle");
	const TCHAR* const HumanWalkPath = TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Walk");
	const TCHAR* const HumanRunPath = TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Run");
	const TCHAR* const HumanAirbornePath = TEXT("/Game/Qinglan/Characters/Human/SK_HumanHuman_Armature_Jump");
}

UQinglanCharacterAnimInstance::UQinglanCharacterAnimInstance()
{
	LocomotionSlotName = FName(TEXT("DefaultSlot"));

	// Convention paths authored by the mesh import. The Human pack has Idle
	// and Jump; Walk/Run currently resolve to nothing (no authored take), and
	// ResolveLocomotionAnimation falls back to Idle in that case.
	DefaultIdleAnimationPath = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(HumanIdlePath));
	DefaultWalkAnimationPath = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(HumanWalkPath));
	DefaultRunAnimationPath = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(HumanRunPath));
	DefaultAirborneAnimationPath = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(HumanAirbornePath));
}

void UQinglanCharacterAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	LoadDefaultAnimations();
	UpdateOwnerKinematics();

	LocomotionState = SelectLocomotionState(Speed, bIsAirborne);
	// Playback starts on the first NativeUpdateAnimation: the owning mesh has
	// finished binding the instance by then, and slot montages are safe to use.
	bLocomotionPlaybackDirty = true;

	UE_LOG(LogQinglanCombat, Verbose,
		TEXT("Qinglan anim instance initialized (idle=%s walk=%s run=%s airborne=%s)"),
		*GetNameSafe(IdleAnimation), *GetNameSafe(WalkAnimation), *GetNameSafe(RunAnimation),
		*GetNameSafe(AirborneAnimation));
}

void UQinglanCharacterAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	UpdateOwnerKinematics();

	// Action layer owns the pose while it is active; keep Speed/airborne fresh
	// so returning to Idle immediately selects the right band.
	if (ActionState != EQinglanAnimActionState::Idle)
	{
		return;
	}

	const EQinglanLocomotionState NewState = SelectLocomotionState(Speed, bIsAirborne);
	if (NewState != LocomotionState)
	{
		LocomotionState = NewState;
		bLocomotionPlaybackDirty = true;
	}
	if (bLocomotionPlaybackDirty)
	{
		PlayLocomotionAnimation();
	}
}

EQinglanLocomotionState UQinglanCharacterAnimInstance::SelectLocomotionState(float InSpeed, bool bAirborne)
{
	if (bAirborne)
	{
		return EQinglanLocomotionState::Airborne;
	}
	if (InSpeed < IdleSpeedThreshold)
	{
		return EQinglanLocomotionState::Idle;
	}
	if (InSpeed < WalkSpeedThreshold)
	{
		return EQinglanLocomotionState::Walk;
	}
	return EQinglanLocomotionState::Run;
}

void UQinglanCharacterAnimInstance::SetActionState(EQinglanAnimActionState NewState)
{
	if (ActionState == NewState)
	{
		return;
	}
	ActionState = NewState;
	// On the next Idle update, replay the locomotion band: the action montage
	// (or a missing one) has replaced the transient slot montage in the meantime.
	bLocomotionPlaybackDirty = true;
}

void UQinglanCharacterAnimInstance::SetLocomotionAnimations(
	UAnimSequence* InIdle, UAnimSequence* InWalk, UAnimSequence* InRun)
{
	// Null entries leave the current take untouched, so callers can override
	// only the idle clip (enemy archetypes) without discarding convention loads.
	if (InIdle)
	{
		IdleAnimation = InIdle;
	}
	if (InWalk)
	{
		WalkAnimation = InWalk;
	}
	if (InRun)
	{
		RunAnimation = InRun;
	}
	bLocomotionPlaybackDirty = true;
}

void UQinglanCharacterAnimInstance::LoadDefaultAnimations()
{
	auto LoadSoft = [](const TSoftObjectPtr<UAnimSequence>& Soft) -> UAnimSequence*
	{
		return Soft.IsNull() ? nullptr : Soft.LoadSynchronous();
	};

	if (!IdleAnimation)
	{
		IdleAnimation = LoadSoft(DefaultIdleAnimationPath);
	}
	if (!WalkAnimation)
	{
		WalkAnimation = LoadSoft(DefaultWalkAnimationPath);
	}
	if (!RunAnimation)
	{
		RunAnimation = LoadSoft(DefaultRunAnimationPath);
	}
	if (!AirborneAnimation)
	{
		AirborneAnimation = LoadSoft(DefaultAirborneAnimationPath);
	}

	if (!IdleAnimation)
	{
		// Missing content is non-fatal: gameplay/state machine tests must run in
		// headless worlds without the imported rig. No pose is produced until an
		// asset is supplied (SetLocomotionAnimations) or imported.
		UE_LOG(LogQinglanCombat, Warning,
			TEXT("Qinglan anim instance: no idle sequence at %s; locomotion will hold the ref pose"),
			*DefaultIdleAnimationPath.ToString());
	}
}

void UQinglanCharacterAnimInstance::UpdateOwnerKinematics()
{
	Speed = 0.f;
	bIsMoving = false;
	bIsAirborne = false;

	// Unchecked accessor: constructed/external anim instances in automation have
	// no skeletal-mesh outer, while the checked variant would assert.
	const USkeletalMeshComponent* MeshComp = GetSkelMeshComponentUnchecked();
	const APawn* OwnerPawn = MeshComp ? Cast<APawn>(MeshComp->GetOwner()) : nullptr;
	if (!OwnerPawn)
	{
		return;
	}

	Speed = static_cast<float>(OwnerPawn->GetVelocity().Size2D());
	bIsMoving = Speed >= IdleSpeedThreshold;

	if (const ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerPawn))
	{
		const UCharacterMovementComponent* Movement = OwnerCharacter->GetCharacterMovement();
		bIsAirborne = Movement != nullptr && Movement->IsFalling();
	}
}

UAnimSequence* UQinglanCharacterAnimInstance::ResolveLocomotionAnimation(EQinglanLocomotionState State) const
{
	// A take from another rig (for example the Human jump on a monster mesh)
	// cannot play here; treat it as absent so the fallback chain wins.
	const USkeletalMeshComponent* MeshComp = GetSkelMeshComponentUnchecked();
	const USkeletalMesh* Mesh = MeshComp ? MeshComp->GetSkeletalMeshAsset() : nullptr;
	const USkeleton* MeshSkeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
	auto Compatible = [MeshSkeleton](const UAnimSequence* Sequence) -> bool
	{
		if (!Sequence)
		{
			return false;
		}
		return MeshSkeleton == nullptr
			|| Sequence->GetSkeleton() == nullptr
			|| Sequence->GetSkeleton() == MeshSkeleton;
	};

	switch (State)
	{
	case EQinglanLocomotionState::Walk:
		return Compatible(WalkAnimation) ? WalkAnimation.Get()
			: (Compatible(IdleAnimation) ? IdleAnimation.Get() : nullptr);

	case EQinglanLocomotionState::Run:
		if (Compatible(RunAnimation))
		{
			return RunAnimation.Get();
		}
		return Compatible(WalkAnimation) ? WalkAnimation.Get()
			: (Compatible(IdleAnimation) ? IdleAnimation.Get() : nullptr);

	case EQinglanLocomotionState::Airborne:
		if (Compatible(AirborneAnimation))
		{
			return AirborneAnimation.Get();
		}
		if (Compatible(RunAnimation))
		{
			return RunAnimation.Get();
		}
		return Compatible(WalkAnimation) ? WalkAnimation.Get()
			: (Compatible(IdleAnimation) ? IdleAnimation.Get() : nullptr);

	case EQinglanLocomotionState::Idle:
	default:
		return Compatible(IdleAnimation) ? IdleAnimation.Get() : nullptr;
	}
}

float UQinglanCharacterAnimInstance::ResolveLocomotionPlayRate() const
{
	switch (LocomotionState)
	{
	case EQinglanLocomotionState::Walk:
		// WalkSpeedThreshold is the authored walk pace; slower starts still play
		// at a readable minimum, faster walks cap before the run band.
		return FMath::Clamp(Speed / WalkSpeedThreshold, 0.5f, 1.25f);

	case EQinglanLocomotionState::Run:
		// RunSpeedThreshold is the full-run reference speed (500 cm/s).
		return FMath::Clamp(Speed / RunSpeedThreshold, 0.75f, 1.5f);

	case EQinglanLocomotionState::Idle:
	case EQinglanLocomotionState::Airborne:
	default:
		return 1.f;
	}
}

void UQinglanCharacterAnimInstance::PlayLocomotionAnimation()
{
	if (!IsInitialized() || LocomotionSlotName.IsNone())
	{
		return;
	}

	// A real (non-transient) montage is authoritative presentation even if the
	// action-state mirror has already fallen back to Idle (for example during a
	// montage blend-out). Keep the dirty flag so the locomotion band replays
	// once that montage finishes; never cut an authored attack/cast montage.
	if (const UAnimMontage* const ActiveMontage = GetCurrentActiveMontage())
	{
		if (ActiveMontage->GetOuter() != GetTransientPackage())
		{
			return;
		}
	}

	bLocomotionPlaybackDirty = false;

	UAnimSequence* Sequence = ResolveLocomotionAnimation(LocomotionState);
	if (!Sequence)
	{
		return;
	}

	// Stop the previous transient slot montage before starting the new band so
	// Idle -> Walk -> Run cannot stack weights. StopSlotAnimation only touches
	// dynamic (transient) montages, so authored attack/cast montages are safe.
	StopSlotAnimation(LocomotionBlendOutTime, LocomotionSlotName);

	UAnimMontage* const DynamicMontage = PlaySlotAnimationAsDynamicMontage(
		Sequence,
		LocomotionSlotName,
		LocomotionBlendInTime,
		LocomotionBlendOutTime,
		ResolveLocomotionPlayRate(),
		LocomotionMontageLoops);
	if (!DynamicMontage)
	{
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Qinglan anim instance: could not start locomotion slot montage for %s"),
			*GetNameSafe(Sequence));
	}
}
