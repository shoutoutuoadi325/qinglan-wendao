// Sword combo implementation. All timings from docs/02-combat-and-camera.md §4:
// Cut1 0.14/0.10/0.22 x0.8, Cut2 0.16/0.10/0.24 x0.9, Cut3 0.22/0.12/0.32 x1.3;
// combo window 0.35 s after recovery begins; single-slot buffer 0.15 s (§3);
// blade sockets are sampled every active tick (moving or stationary) with
// substeps and per-swing dedupe; one damage per victim per swing; energy
// restore hooks into UQinglanCombatComponent.
//
// AUD-44 STATUS: OPEN. The swing montage assets are authored by
// QinglanAnimBootstrap as /Game/Qinglan/Characters/Human/Montages/
// AM_Human_SwordCut_1..3 (slot "DefaultSlot"); the rig itself is still the
// shared AnimationSingleNode idle loop plus montage/notify overlays. A full
// AnimBP locomotion/aim/combat state machine is NOT implemented. Editor
// verification: montage slot DefaultSlot, sword sockets sword_base/sword_tip,
// cast socket cast_source (see AQinglanCharacter::ResolveAim).
#include "Combat/QinglanSwordComponent.h"

#include "QinglanAscension.h"
#include "Abilities/QinglanBlessingLibrary.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Core/QinglanAudioSubsystem.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Engine/OverlapResult.h" // FOverlapResult full type (UE 5.8 split from EngineTypes)
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
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

	// Convention paths authored by QinglanAnimBootstrap: object name matches the
	// package file name, and the package lives in its own asset path (AUD-45).
	// Full object-path form is used so a stray package/object mismatch is
	// reported by the loader instead of silently resolving another package.
	SwingMontageSoftPaths = {
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_Human_SwordCut_1.AM_Human_SwordCut_1")),
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_Human_SwordCut_2.AM_Human_SwordCut_2")),
		FSoftObjectPath(TEXT("/Game/Qinglan/Characters/Human/Montages/AM_Human_SwordCut_3.AM_Human_SwordCut_3")),
	};
}

UAnimMontage* UQinglanSwordComponent::ResolveSwingMontageSoft(int32 CutIndex)
{
	if (!SwingMontageSoftPaths.IsValidIndex(CutIndex) || !SwingMontageSoftPaths[CutIndex].IsValid())
	{
		return nullptr;
	}
	const FString PathString = SwingMontageSoftPaths[CutIndex].ToString();
	if (UObject* Resolved = SwingMontageSoftPaths[CutIndex].TryLoad())
	{
		if (UAnimMontage* Montage = Cast<UAnimMontage>(Resolved))
		{
			return Montage;
		}
		UE_LOG(LogQinglanCombat, Warning, TEXT("Swing montage path %s resolved to a non-montage; phase timers continue"), *PathString);
		return nullptr;
	}
	// Missing assets degrade to "no animation": phase timers and blade sweeps
	// stay authoritative, and the log is emitted once per path to avoid spam.
	if (!MissingSwingMontagePathsLogged.Contains(PathString))
	{
		MissingSwingMontagePathsLogged.Add(PathString);
		UE_LOG(LogQinglanCombat, Warning,
			TEXT("Swing montage missing at %s (cut %d); running without animation. Run QinglanAnimBootstrap."),
			*PathString, CutIndex + 1);
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
	// AUD-32: committed casts cannot be canceled by a normal attack (this
	// includes post-release recovery); dodge, hit reaction, and open scene
	// choices are also excluded by the character's unified action gate.
	if (const AQinglanCharacter* const Character = Cast<AQinglanCharacter>(GetOwner()))
	{
		if (!Character->CanStartAction(EQinglanActionKind::Sword))
		{
			return false;
		}
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
	if ((Now - BufferedAttackAt) > InputBufferSeconds)
	{
		return;
	}
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

	// Advance to each phase boundary before checking the buffer. Expiring it at
	// the frame endpoint discards presses that were still fresh when recovery
	// actually began during a slow frame.
	float RemainingTime = FMath::Max(0.f, DeltaTime);
	const float FrameEndTime = Now + RemainingTime;

	UQinglanCombatComponent* const OwnerCombat = GetOwnerCombat();
	if (OwnerCombat && !OwnerCombat->IsAlive())
	{
		// Dead outranks the committed action (docs/02 §3 priority).
		InterruptSwing();
		Now = FrameEndTime;
		return;
	}

	// Carry overshoot through windup, active and recovery. The active phase is
	// sampled even when one frame spans its entire duration.
	while (RemainingTime > 0.f && Phase != EQinglanSwordPhase::Idle)
	{
		const float Duration = FMath::Max(0.f, GetPhaseDuration());
		const float Step = FMath::Min(RemainingTime, FMath::Max(0.f, Duration - PhaseElapsed));
		Now += Step;
		PhaseElapsed += Step;
		RemainingTime -= Step;

		if (Phase == EQinglanSwordPhase::Active && bWindowOpen)
		{
			TickSweep();
		}
		if (PhaseElapsed < Duration)
		{
			break;
		}

		switch (Phase)
		{
		case EQinglanSwordPhase::Windup:
			EnterPhase(EQinglanSwordPhase::Active);
			// Timer fallback when the montage/notify is absent (headless automation,
			// -nullrhi specs): open at the authored active start. A notify that already
			// opened the window is honored; NotifyWindowClosed can still close it early.
			OpenWindow();
			break;
		case EQinglanSwordPhase::Active:
			CloseWindow();
			EnterPhase(EQinglanSwordPhase::Recovery);
			RecoveryBeganAt = Now; // 0.35 s combo continuation window starts here (docs/02 §4)
			ConsumeBufferedInput(); // buffered press chains into the next cut immediately
			break;
		case EQinglanSwordPhase::Recovery:
		{
			const int32 FinishedIndex = ComboIndex;
			const FName FinishedSwingId = GetSwingId();
			EnterPhase(EQinglanSwordPhase::Idle);
			OnSwingEnded.Broadcast(FinishedIndex, FinishedSwingId);
			ConsumeBufferedInput(); // still-fresh buffered press fires at the Idle boundary
			break;
		}
		default:
			break;
		}
	}
	Now = FrameEndTime;
	if (bHasBufferedAttack && (Now - BufferedAttackAt) > InputBufferSeconds)
	{
		bHasBufferedAttack = false;
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
	if (USkeletalMeshComponent* const MeshComp = GetOwnerMesh())
	{
		if (MeshComp->DoesSocketExist(SocketName))
		{
			return MeshComp->GetSocketLocation(SocketName);
		}
	}

	// AUD-44 fallback: sockets are not authored in the scaffold rig. Prefer a
	// weapon/sword-named scene component (the intended "weapon component"
	// position), then the owner mesh origin, then the actor origin. Every
	// fallback is logged; none of them can crash the sweep.
	AActor* const OwnerActor = GetOwner();
	if (OwnerActor)
	{
		TArray<UActorComponent*> Components;
		OwnerActor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			USceneComponent* const SceneComp = Cast<USceneComponent>(Component);
			const FString ComponentName = Component ? Component->GetName() : FString();
			if (SceneComp && SceneComp != GetOwnerMesh()
				&& (ComponentName.Contains(TEXT("weapon"), ESearchCase::IgnoreCase)
					|| ComponentName.Contains(TEXT("sword"), ESearchCase::IgnoreCase)))
			{
				UE_LOG(LogQinglanCombat, Verbose,
					TEXT("Sword socket '%s' missing; using weapon component '%s' position"),
					*SocketName.ToString(), *ComponentName);
				return SceneComp->GetComponentLocation();
			}
		}
	}
	if (USkeletalMeshComponent* const MeshComp = GetOwnerMesh())
	{
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Sword socket '%s' missing and no weapon component found; using owner mesh origin"),
			*SocketName.ToString());
		return MeshComp->GetComponentLocation();
	}
	return OwnerActor ? OwnerActor->GetActorLocation() : FVector::ZeroVector;
}

void UQinglanSwordComponent::TickSweep()
{
	// Sample the blade at its current position every active tick. Moving sockets
	// additionally sweep the segment from the previous tick so fast cuts cannot
	// tunnel; stationary sockets still sample (AUD-44) and dedupe through the
	// per-swing victim set.
	const FVector TipLocation = GetBladeSocketLocation(BladeTipSocketName);
	const FVector BaseLocation = GetBladeSocketLocation(BladeStartSocketName);
	// The full blade can strike a capsule between its endpoints. Sampling
	// only the hilt and tip leaves a gap through the middle of a standing cut.
	SweepSocketSegment(BaseLocation, TipLocation);
	if (bHavePreviousSocketLocations)
	{
		SweepSocketSegment(PreviousTipLocation, TipLocation);
		SweepSocketSegment(PreviousBaseLocation, BaseLocation);
	}
	else
	{
		// First active tick: the previous reference frame is intentionally not
		// inherited from the last swing, so sample the current blade directly.
		SampleSocketAt(TipLocation);
		SampleSocketAt(BaseLocation);
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
		// AUD-44: a socket that did not move still samples its position; the old
		// early-return silently skipped hits on a held/stationary blade.
		SampleSocketAt(To);
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

		TArray<FHitResult> Hits;
		World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectQuery, SweepShape, QueryParams);
		// Deterministic ordering: sweep time first, then stable actor name.
		Hits.Sort([](const FHitResult& A, const FHitResult& B)
		{
			if (!FMath::IsNearlyEqual(A.Time, B.Time))
			{
				return A.Time < B.Time;
			}
			const AActor* ActorA = A.GetActor();
			const AActor* ActorB = B.GetActor();
			return ActorA && ActorB ? ActorA->GetName() < ActorB->GetName() : ActorA != nullptr;
		});
		for (const FHitResult& Hit : Hits)
		{
			AActor* const Victim = Hit.GetActor();
			if (Victim && !SwingVictims.Contains(Victim))
			{
				ApplyHit(Victim);
			}
		}
	}
}

void UQinglanSwordComponent::SampleSocketAt(const FVector& Location)
{
	UWorld* const World = GetWorld();
	AActor* const OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanBladeSample), /*bTraceComplex=*/false, OwnerActor);
	QueryParams.AddIgnoredActor(OwnerActor);
	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity,
		FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeSphere(BladeSweepRadius), QueryParams);

	// Deterministic nearest-first ordering, then stable name (no RNG anywhere).
	Overlaps.Sort([Location](const FOverlapResult& A, const FOverlapResult& B)
	{
		const AActor* ActorA = A.GetActor();
		const AActor* ActorB = B.GetActor();
		const float DistA = FVector::DistSquared(ActorA ? ActorA->GetActorLocation() : Location, Location);
		const float DistB = FVector::DistSquared(ActorB ? ActorB->GetActorLocation() : Location, Location);
		if (!FMath::IsNearlyEqual(DistA, DistB))
		{
			return DistA < DistB;
		}
		return ActorA && ActorB ? ActorA->GetName() < ActorB->GetName() : ActorA != nullptr;
	});
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* const Victim = Overlap.GetActor();
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

	if (!VictimCombat->ApplyDamage(Event))
	{
		return; // rejected/invulnerable damage grants neither proof nor energy
	}
	// The resolved-damage delegate belongs to the victim's Combat component;
	// the player's own component therefore cannot observe outgoing sword
	// hits. Publish proof here, after an accepted real sword collision, rather
	// than from a dummy target or a test-only event injection.
	if (const AQinglanCharacter* Player = Cast<AQinglanCharacter>(GetOwner()))
	{
		if (UGameInstance* Instance = Player->GetGameInstance())
		{
			if (UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
			{
				QinglanStoryRuntime::NotifyGameplayEvent(Story, FName(TEXT("combat.sword_hit")), {});
			}
		}
	}

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
		// Each StartSwing call (including buffered chains into the next cut) plays
		// the montage for that cut; Montage_Play replaces the previous cut's pose.
		const FQinglanSwordCut& Cut = Cuts[ComboIndex];
		const float CutDuration = Cut.WindupSeconds + Cut.ActiveSeconds + Cut.RecoverySeconds;
		const float PlayRate = Montage->GetPlayLength() / FMath::Max(CutDuration, UE_SMALL_NUMBER);
		const float Duration = AnimInstance->Montage_Play(Montage, PlayRate);
		if (Duration <= 0.f)
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("Sword montage %s failed to start for %s; phase timers continue"),
				*Montage->GetName(), *GetNameSafe(OwnerCharacter));
		}
	}
	else if (Montage && !AnimInstance)
	{
		// AUD-44 open gap: AnimationSingleNode/no AnimInstance rigs cannot blend
		// the montage; the timer-driven swing still resolves all damage.
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Sword montage %s skipped: owner has no animation instance (AUD-44 state-machine gap)"),
			*Montage->GetName());
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
