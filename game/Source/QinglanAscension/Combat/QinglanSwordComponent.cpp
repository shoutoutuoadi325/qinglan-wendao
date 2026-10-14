// Sword combo implementation. All timings from docs/02-combat-and-camera.md §4:
// Cut1 0.14/0.10/0.22 x0.8, Cut2 0.16/0.10/0.24 x0.9, Cut3 0.22/0.12/0.32 x1.3;
// combo window 0.35 s after recovery begins; single-slot buffer 0.15 s (§3);
// sweeps between previous and current blade sockets with substeps; one damage
// per victim per swing; energy restore hooks into UQinglanCombatComponent.
#include "Combat/QinglanSwordComponent.h"

#include "QinglanAscension.h"
#include "Abilities/QinglanBlessingLibrary.h"
#include "Combat/QinglanCombatComponent.h"
#include "Core/QinglanAudioSubsystem.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Engine/World.h"
#include "Progression/QinglanEconomyLibrary.h"

namespace
{
	// Stable ASCII swing ids used in damage events; presentation names resolve from
	// /Game/Qinglan/Data/ST_Game keys if a UI ever needs them (code-contracts).
	const FName* QinglanSwingIdTable()
	{
		static const FName SwingIdTable[3] = { FName(TEXT("sword_cut_1")), FName(TEXT("sword_cut_2")), FName(TEXT("sword_cut_3")) };
		return SwingIdTable;
	}
}

UQinglanSwordComponent::UQinglanSwordComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(false);

	// docs/02 §4 combo table: windup / active / recovery / power multiplier.
	Cuts = {
		FQinglanSwordCut{ 0.14f, 0.10f, 0.22f, 0.8f }, // Cut 1
		FQinglanSwordCut{ 0.16f, 0.10f, 0.24f, 0.9f }, // Cut 2
		FQinglanSwordCut{ 0.22f, 0.12f, 0.32f, 1.3f }  // Cut 3
	};

	// Convention paths authored by QinglanAnimBootstrap (config-overridable).
	SwingMontageSoftPaths = {
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_SwordCut_1")),
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_SwordCut_2")),
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_SwordCut_3")),
	};
}

UAnimMontage* UQinglanSwordComponent::ResolveSwingMontageSoft(int32 CutIndex)
{
	if (!SwingMontageSoftPaths.IsValidIndex(CutIndex) || !SwingMontageSoftPaths[CutIndex].IsValid())
	{
		return nullptr;
	}
	if (UObject* Resolved = SwingMontageSoftPaths[CutIndex].TryLoad())
	{
		return Cast<UAnimMontage>(Resolved);
	}
	return nullptr;
}

void UQinglanSwordComponent::BeginPlay()
{
	Super::BeginPlay();
	// Reset the paused-game-time clock; it accumulates only in TickComponent.
	Now = 0.f;
}

float UQinglanSwordComponent::GetPhaseDuration() const
{
	if (!Cuts.IsValidIndex(ComboIndex))
	{
		return 0.f;
	}
	const FQinglanSwordCut& Cut = Cuts[ComboIndex];
	switch (Phase)
	{
	case EQinglanSwordPhase::Windup:  return Cut.WindupSeconds;
	case EQinglanSwordPhase::Active:  return Cut.ActiveSeconds;
	case EQinglanSwordPhase::Recovery:return Cut.RecoverySeconds;
	default:                           return 0.f;
	}
}

EQinglanCombatState UQinglanSwordComponent::GetCombatState() const
{
	switch (Phase)
	{
	case EQinglanSwordPhase::Windup:   return EQinglanCombatState::AttackWindup;
	case EQinglanSwordPhase::Active:   return EQinglanCombatState::AttackActive;
	case EQinglanSwordPhase::Recovery: return EQinglanCombatState::AttackRecovery;
	default:                           return EQinglanCombatState::Idle;
	}
}

FName UQinglanSwordComponent::GetSwingId() const
{
	const FName* const Table = QinglanSwingIdTable();
	return (ComboIndex >= 0 && ComboIndex < 3) ? Table[ComboIndex] : Table[0];
}

bool UQinglanSwordComponent::CanStartSwing() const
{
	UQinglanCombatComponent* const OwnerCombat = GetOwnerCombat();
	if (!OwnerCombat || !OwnerCombat->IsAlive())
	{
		return false; // Dead outranks everything (docs/02 §3 priority).
	}
	return !Cuts.IsEmpty();
}

bool UQinglanSwordComponent::RequestAttack()
{
	if (!CanStartSwing())
	{
		return false;
	}

	switch (Phase)
	{
	case EQinglanSwordPhase::Windup:
	case EQinglanSwordPhase::Active:
		// Single-slot buffer, 0.15 s (docs/02 §3); the newest press replaces any older one.
		bHasBufferedAttack = true;
		BufferedAttackAt = Now;
		return false;

	case EQinglanSwordPhase::Recovery:
	{
		// Recovery is always inside the 0.35 s combo window for every authored cut
		// (max recovery 0.32 s), so a press here chains directly (docs/02 §4).
		const int32 NextIndex = (ComboIndex + 1) % Cuts.Num();
		StartSwing(NextIndex);
		return true;
	}

	case EQinglanSwordPhase::Idle:
	default:
	{
		const bool bWithinComboWindow = (Now - RecoveryBeganAt) <= ComboWindowSeconds;
		int32 NextIndex = 0;
		if (bWithinComboWindow)
		{
			NextIndex = (ComboIndex + 1) % Cuts.Num(); // after Cut 3 the chain restarts at Cut 1
		}
		StartSwing(NextIndex);
		return true;
	}
	}
}

void UQinglanSwordComponent::EnterPhase(EQinglanSwordPhase NewPhase)
{
	Phase = NewPhase;
	PhaseElapsed = 0.f;
}

void UQinglanSwordComponent::StartSwing(int32 CutIndex)
{
	if (!Cuts.IsValidIndex(CutIndex))
	{
		return;
	}
	ComboIndex = CutIndex;
	++SwingSerialCounter; // deterministic serial; no gameplay RNG anywhere
	SwingCastId = FQinglanCastId{ SwingSerialCounter, SwingSerialCounter };
	SwingVictims.Reset();          // one damage per victim per *swing* (docs/02 §4)
	if (UQinglanCombatComponent* const Combat = GetOwnerCombat())
	{
		Combat->NotifySwordSwingStarted(); // per-swing energy ledger resets with the swing
	}
	bHavePreviousSocketLocations = false;
	CloseWindow();
	bHasBufferedAttack = false;    // the buffer is consumed by starting this swing
	// Resolve the swing's sword-damage percent once here: loadout swaps are
	// rejected mid-expedition and blessings change only between rooms, so the
	// committed swing can safely carry one stable total (docs/03 section 4
	// "add within category, apply once").
	SwingSwordDamagePercent = ResolveSwingSwordDamagePercent();
	EnterPhase(EQinglanSwordPhase::Windup);
	PlaySwingMontage();
	// Swing whoosh (docs/04 audio pass); null-safe without imported audio.
	if (UGameInstance* GameInstance = GetOwner() ? GetOwner()->GetGameInstance() : nullptr)
	{
		if (UQinglanAudioSubsystem* Audio = GameInstance->GetSubsystem<UQinglanAudioSubsystem>())
		{
			Audio->PlaySfx(FName(TEXT("sfx_sword_swing")));
		}
	}
	OnSwingStarted.Broadcast(ComboIndex, GetSwingId());
}

float UQinglanSwordComponent::ResolveSwingSwordDamagePercent()
{
	// Sword-damage category total: expedition blessings plus the permanent
	// equipment fold (Sword Discipline +10%; relic bonuses/tradeoffs,
	// docs/03 sections 4-5). Headless contexts without a profile fold the
	// blessing part only.
	FQinglanBlessingModifiers Mods;
	TArray<FName> Owned;
	UQinglanBlessingLibrary::GetOwnedBlessings(this, Owned);
	UQinglanBlessingLibrary::ComputeModifiers(Owned, Mods);
	UQinglanEconomyLibrary::AppendPermanentModifiers(this, Mods);
	return Mods.SwordDamagePercent;
}

void UQinglanSwordComponent::OpenWindow()
{
	if (bWindowOpen)
	{
		return;
	}
	bWindowOpen = true;
	// Fresh reference frame: the first sweep tick only records socket positions so a
	// swing cannot inherit stale blade positions from the previous swing.
	bHavePreviousSocketLocations = false;
}

void UQinglanSwordComponent::CloseWindow()
{
	bWindowOpen = false;
}

void UQinglanSwordComponent::NotifyWindowOpen()
{
	// Animation notifies open the hit window (docs/02 §4); allow an early open during
	// windup but ignore notifies when no swing is in flight.
	if (Phase == EQinglanSwordPhase::Windup || Phase == EQinglanSwordPhase::Active)
	{
		OpenWindow();
	}
}

void UQinglanSwordComponent::NotifyWindowClosed()
{
	CloseWindow();
}

void UQinglanSwordComponent::ConsumeBufferedInput()
{
	if (!bHasBufferedAttack)
	{
		return;
	}
	bHasBufferedAttack = false;
	// A buffered press executes as soon as the state machine would accept a new one:
	// while the combo window is open it chains, otherwise it starts from Cut 1.
	if (CanStartSwing())
	{
		const bool bWithinComboWindow = (Now - RecoveryBeganAt) <= ComboWindowSeconds;
		StartSwing(bWithinComboWindow ? (ComboIndex + 1) % Cuts.Num() : 0);
	}
}

void UQinglanSwordComponent::InterruptSwing()
{
	if (Phase != EQinglanSwordPhase::Idle)
	{
		OnSwingEnded.Broadcast(ComboIndex, GetSwingId());
	}
	Phase = EQinglanSwordPhase::Idle;
	PhaseElapsed = 0.f;
	RecoveryBeganAt = -1.e9f; // an interrupted chain restarts at Cut 1
	bHasBufferedAttack = false;
	CloseWindow();
	SwingVictims.Reset();
	bHavePreviousSocketLocations = false;
	StopSwingMontage();
}

bool UQinglanSwordComponent::CanDodgeCancelNow() const
{
	switch (Phase)
	{
	case EQinglanSwordPhase::Idle:
	case EQinglanSwordPhase::Windup:
		return true;  // windup is cancellable before the active hit window (docs/02 §3)
	case EQinglanSwordPhase::Active:
		return false; // active hits cannot be canceled (docs/02 §3)
	case EQinglanSwordPhase::Recovery:
		return PhaseElapsed >= RecoveryCancelAfterSeconds; // recovery after first 0.10 s (docs/02 §3)
	default:
		return true;
	}
}

void UQinglanSwordComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Paused-game-time semantics: the clock only advances with real (unpaused) ticks.
	Now += DeltaTime;

	UQinglanCombatComponent* const OwnerCombat = GetOwnerCombat();
	if (OwnerCombat && !OwnerCombat->IsAlive())
	{
		// Dead outranks the committed action (docs/02 §3 priority).
		InterruptSwing();
		return;
	}

	// Single-slot buffer expiry (docs/02 §3: 0.15 s).
	if (bHasBufferedAttack && (Now - BufferedAttackAt) > InputBufferSeconds)
	{
		bHasBufferedAttack = false;
	}

	switch (Phase)
	{
	case EQinglanSwordPhase::Idle:
		break;

	case EQinglanSwordPhase::Windup:
		PhaseElapsed += DeltaTime;
		if (PhaseElapsed >= GetPhaseDuration())
		{
			EnterPhase(EQinglanSwordPhase::Active);
			// Timer fallback when the montage/notify is absent (headless automation,
			// -nullrhi specs): open at the authored active start. A notify that already
			// opened the window is honored; NotifyWindowClosed can still close it early.
			OpenWindow();
		}
		break;

	case EQinglanSwordPhase::Active:
		PhaseElapsed += DeltaTime;
		if (bWindowOpen)
		{
			TickSweep();
		}
		if (PhaseElapsed >= GetPhaseDuration())
		{
			CloseWindow();
			EnterPhase(EQinglanSwordPhase::Recovery);
			RecoveryBeganAt = Now; // 0.35 s combo continuation window starts here (docs/02 §4)
			ConsumeBufferedInput(); // buffered press chains into the next cut immediately
		}
		break;

	case EQinglanSwordPhase::Recovery:
		PhaseElapsed += DeltaTime;
		if (PhaseElapsed >= GetPhaseDuration())
		{
			const int32 FinishedIndex = ComboIndex;
			const FName FinishedSwingId = GetSwingId();
			EnterPhase(EQinglanSwordPhase::Idle);
			OnSwingEnded.Broadcast(FinishedIndex, FinishedSwingId);
			ConsumeBufferedInput(); // still-fresh buffered press fires at the Idle boundary
		}
		break;
	}
}

UQinglanCombatComponent* UQinglanSwordComponent::GetOwnerCombat() const
{
	AActor* const OwnerActor = GetOwner();
	return OwnerActor ? OwnerActor->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
}

USkeletalMeshComponent* UQinglanSwordComponent::GetOwnerMesh() const
{
	ACharacter* const OwnerCharacter = Cast<ACharacter>(GetOwner());
	return OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
}

FVector UQinglanSwordComponent::GetBladeSocketLocation(FName SocketName) const
{
	USkeletalMeshComponent* const MeshComp = GetOwnerMesh();
	if (MeshComp)
	{
		if (!MeshComp->DoesSocketExist(SocketName))
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("Sword socket '%s' missing on owner mesh; using component origin"), *SocketName.ToString());
			return MeshComp->GetComponentLocation();
		}
		return MeshComp->GetSocketLocation(SocketName);
	}
	return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

void UQinglanSwordComponent::TickSweep()
{
	// Sweep between previous and current blade socket positions (docs/02 §4).
	const FVector TipLocation = GetBladeSocketLocation(BladeTipSocketName);
	const FVector BaseLocation = GetBladeSocketLocation(BladeStartSocketName);
	if (bHavePreviousSocketLocations)
	{
		SweepSocketSegment(PreviousTipLocation, TipLocation);
		SweepSocketSegment(PreviousBaseLocation, BaseLocation);
	}
	PreviousTipLocation = TipLocation;
	PreviousBaseLocation = BaseLocation;
	bHavePreviousSocketLocations = true;
}

void UQinglanSwordComponent::SweepSocketSegment(const FVector& From, const FVector& To)
{
	UWorld* const World = GetWorld();
	AActor* const OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return;
	}

	const float SegmentLength = FVector::Dist(From, To);
	if (SegmentLength < KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Substeps when movement distance is large so fast swings cannot tunnel (docs/02 §4).
	const int32 SubstepCount = FMath::Max(1, FMath::CeilToInt(SegmentLength / FMath::Max(1.f, MaxSubstepDistance)));

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanBladeSweep), /*bTraceComplex=*/false, OwnerActor);
	QueryParams.AddIgnoredActor(OwnerActor);
	const FCollisionObjectQueryParams ObjectQuery(ECC_Pawn);
	const FCollisionShape SweepShape = FCollisionShape::MakeSphere(BladeSweepRadius);

	for (int32 Substep = 0; Substep < SubstepCount; ++Substep)
	{
		const float AlphaStart = static_cast<float>(Substep) / static_cast<float>(SubstepCount);
		const float AlphaEnd = static_cast<float>(Substep + 1) / static_cast<float>(SubstepCount);
		const FVector Start = FMath::Lerp(From, To, AlphaStart);
		const FVector End = FMath::Lerp(From, To, AlphaEnd);

		FHitResult Hit;
		const bool bBlocking = World->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity, ObjectQuery, SweepShape, QueryParams);
		AActor* Victim = bBlocking ? Hit.GetActor() : nullptr;
		if (Victim && !SwingVictims.Contains(Victim))
		{
			ApplyHit(Victim);
		}
	}
}

void UQinglanSwordComponent::ApplyHit(AActor* Victim)
{
	if (!Victim)
	{
		return;
	}

	UQinglanCombatComponent* const VictimCombat = Victim->FindComponentByClass<UQinglanCombatComponent>();
	if (!VictimCombat || !VictimCombat->IsAlive())
	{
		return;
	}

	UQinglanCombatComponent* const OwnerCombat = GetOwnerCombat();
	if (!OwnerCombat || !Cuts.IsValidIndex(ComboIndex))
	{
		return;
	}

	// A single swing damages a victim once (docs/02 §4); the victim-side hit-key
	// dedupe (CastId serial, victim, ordinal) is the second guard.
	SwingVictims.AddUnique(Victim);

	// Raw = Power * Coefficient + Flat (docs/02 §5); the sword's coefficient is 1 and
	// the per-cut multiplier comes from the §4 table. Sword-damage percents
	// (blessings + relic + passives) apply to the complete raw expression once
	// (docs/03 section 4 "add within category, apply once"), resolved per swing
	// in StartSwing.
	const FQinglanSwordCut& Cut = Cuts[ComboIndex];
	FQinglanDamageEvent Event;
	Event.Attacker = GetOwner();
	Event.Target = Victim;
	Event.CastId = SwingCastId;
	Event.HitOrdinal = 1; // one hit per victim per swing; multi-tick effects would increment deliberately
	Event.TechniqueId = GetSwingId();
	Event.Element = EQinglanElement::None;
	Event.RawMagnitude = (OwnerCombat->GetTechniquePower() * SwordBaseCoefficient * Cut.PowerMultiplier + SwordFlatDamage)
		* (1.f + SwingSwordDamagePercent / 100.f);
	Event.Timestamp = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f; // paused-game-time timestamp

	VictimCombat->ApplyDamage(Event);

	// Energy restore hook (docs/02 §4): 4 energy per unique target, capped 8 per swing;
	// training targets cannot generate campaign resources. The cap bookkeeping lives in
	// UQinglanCombatComponent::NotifySwordHitTarget.
	// FIXME-CONTRACT: the scaffold UQinglanCombatComponent header exposes no per-swing
	// reset API for SwordSwingVictims/SwordSwingEnergyGranted; NotifySwordHitTarget must
	// reset its swing ledger at swing boundaries or restore the cap per CastId.
	OwnerCombat->NotifySwordHitTarget(Victim);
}

void UQinglanSwordComponent::PlaySwingMontage()
{
	ACharacter* const OwnerCharacter = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* const MeshComp = GetOwnerMesh();
	if (!OwnerCharacter || !MeshComp)
	{
		return;
	}
	UAnimInstance* const AnimInstance = MeshComp->GetAnimInstance();
	UAnimMontage* Montage = SwingMontages.IsValidIndex(ComboIndex) ? SwingMontages[ComboIndex].Get() : nullptr;
	if (Montage == nullptr)
	{
		// Pure-C++ pawn fallback: resolve the bootstrapped montage by well-known
		// package path (QinglanAnimBootstrap authors these); null-safe.
		Montage = ResolveSwingMontageSoft(ComboIndex);
	}
	if (AnimInstance && Montage)
	{
		// Cosmetic only: phase timers above stay authoritative; notifies refine the window.
		const float Duration = AnimInstance->Montage_Play(Montage, 1.f);
		if (Duration <= 0.f)
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("Sword montage failed to start for %s"), *GetNameSafe(OwnerCharacter));
		}
	}
}

void UQinglanSwordComponent::StopSwingMontage()
{
	USkeletalMeshComponent* const MeshComp = GetOwnerMesh();
	UAnimInstance* const AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (AnimInstance && AnimInstance->IsAnyMontagePlaying())
	{
		AnimInstance->Montage_Stop(0.1f);
	}
}
