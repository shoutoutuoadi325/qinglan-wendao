// Technique component implementation: the fixed cast pipeline.
//
// Pipeline (blueprint §1.2; docs/02 §3): validate alive/state/resources/target
// -> anticipation (cancellable, zero cost) -> release ONCE (unique
// FQinglanCastId; commits energy+cooldown) -> execute by EQinglanTargeting ->
// resolve hits/statuses through the combat components -> publish cues ->
// recovery. Interrupted pre-release costs nothing; released techniques are
// never refunded (docs/02 §3).
#include "Combat/QinglanTechniqueComponent.h"

#include "Abilities/QinglanBlessingLibrary.h"
#include "Abilities/QinglanTechniqueExecutors.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanCombatState.h"
#include "Core/QinglanAudioSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"
#include "UnrealClient.h" // GFrameNumber

namespace
{
	/** Loadout size: four techniques (docs/03 §3 "Equip up to four
	 *  techniques"; blueprint §1.5 loadout). */
	constexpr int32 KLoadoutSlotCount = 4;

	/** Max distance from the caster to the ground-strike aim point.
	 *  FIXME-CONTRACT: docs/02 §6 gives the 300 cm radius but no cast range;
	 *  tunable default 2,000 cm matches the lock-on range (docs/02 §2). */
	constexpr float KMaxGroundStrikeAimRangeCm = 2000.f;

	/** Cast socket height above the actor origin for direction resolution.
	 *  docs/02 §2: aim traces run "from the casting socket toward that point"
	 *  so shoulder offsets do not cause wall penetration. */
	constexpr float KCastSocketHeightCm = 50.f;

	/** Paused-game-time clock: world time seconds stop advancing while the
	 *  game is paused, so phases and cooldowns freeze with it (code-contracts
	 *  "Timers use paused game time"; timestamps use the same clock). */
	float PausedGameTimeSeconds(const UWorld* World)
	{
		return World ? World->GetTimeSeconds() : 0.f;
	}

	/** docs/03 §4: apply cost percent once, then clamp to a minimum of 1
	 *  rounded up for any energy-spending technique. */
	int32 ComputeEnergyCost(const UQinglanTechniqueDataAsset* Technique, const FQinglanBlessingModifiers& Blessings)
	{
		const float Modified = static_cast<float>(Technique->EnergyCost) * (1.f + Blessings.TechniqueCostPercent / 100.f);
		return FMath::Max(1, FMath::CeilToInt(Modified));
	}

	/** Cooldown with blessing modifier applied once (docs/03 §4 category
	 *  rule; casting-economy group). */
	float ComputeCooldownSeconds(const UQinglanTechniqueDataAsset* Technique, const FQinglanBlessingModifiers& Blessings)
	{
		return FMath::Max(0.f, Technique->CooldownSeconds * (1.f + Blessings.TechniqueCooldownPercent / 100.f));
	}

	/** Character-state gating: casting may start from idle/move/attack
	 *  recovery, never from a committed attack, dodge, forced reaction, or
	 *  death (docs/02 §3 priority order). */
	bool IsCastBlockedByState(EQinglanCombatState State)
	{
		switch (State)
		{
		case EQinglanCombatState::Idle:
		case EQinglanCombatState::Move:
		case EQinglanCombatState::AttackRecovery:
			return false;
		case EQinglanCombatState::AttackWindup:
		case EQinglanCombatState::AttackActive:
		case EQinglanCombatState::Casting:
		case EQinglanCombatState::Dodging:
		case EQinglanCombatState::HitReaction:
		case EQinglanCombatState::Dead:
		default:
			return true;
		}
	}
}

UQinglanTechniqueComponent::UQinglanTechniqueComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(false); // offline single-process game
}

// --- Aim resolution (target validation step) ---

namespace
{
	bool ResolveAim(const UQinglanTechniqueComponent* Component, const UQinglanTechniqueDataAsset* Technique,
		FVector& OutAimPoint, FVector& OutAimDirection)
	{
		const AActor* Owner = Component->GetOwner();
		if (!Owner || !Technique)
		{
			return false;
		}

		// docs/02 §2: free-aim point from the camera; the technique then fires
		// from the cast socket toward that point.
		const AQinglanCharacter* Character = Cast<AQinglanCharacter>(Owner);
		const FVector AimPoint = Character ? Character->GetAimPoint()
			: Owner->GetActorLocation() + Owner->GetActorForwardVector() * 1000.f;
		const FVector SocketOrigin = Owner->GetActorLocation() + FVector(0.f, 0.f, KCastSocketHeightCm);

		OutAimPoint = AimPoint;
		OutAimDirection = (AimPoint - SocketOrigin).GetSafeNormal();

		switch (Technique->Targeting)
		{
		case EQinglanTargeting::SelfBarrier:
		case EQinglanTargeting::SelfHeal:
		case EQinglanTargeting::SelfDash:
		case EQinglanTargeting::SelfPulses:
			return true; // self techniques need no external target
		case EQinglanTargeting::GroundStrike:
			return FVector::Dist(SocketOrigin, AimPoint) <= KMaxGroundStrikeAimRangeCm;
		case EQinglanTargeting::Projectile:
		case EQinglanTargeting::SelfArc:
		default:
			return !OutAimDirection.IsNearlyZero();
		}
	}
}

// --- Cast pipeline entry ---

bool UQinglanTechniqueComponent::TryCast(int32 Slot)
{
	// -- Validation, fixed order: alive -> state -> resources -> target --
	if (Slot < 0 || Slot >= KLoadoutSlotCount || Slot >= Loadout.Num())
	{
		FailValidation();
		return false;
	}
	UQinglanTechniqueDataAsset* Technique = Loadout[Slot];
	if (!Technique || Technique->TechniqueId == NAME_None)
	{
		FailValidation();
		return false;
	}

	AActor* Owner = GetOwner();
	UQinglanCombatComponent* Combat = Owner ? Owner->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
	if (!Owner || !Combat)
	{
		FailValidation();
		return false;
	}

	// 1) alive
	if (!Combat->IsAlive())
	{
		FailValidation();
		return false;
	}

	// 2) state: one cast at a time and the character state machine must allow it
	if (CurrentCastId.IsValid())
	{
		FailValidation();
		return false;
	}
	if (const AQinglanCharacter* Character = Cast<AQinglanCharacter>(Owner))
	{
		if (IsCastBlockedByState(Character->GetCombatState()))
		{
			FailValidation();
			return false;
		}
	}

	// 3) resources: energy and cooldown (committed only at release)
	FQinglanBlessingModifiers Blessings;
	{
		TArray<FName> Owned;
		UQinglanBlessingLibrary::GetOwnedBlessings(this, Owned);
		UQinglanBlessingLibrary::ComputeModifiers(Owned, Blessings);
		// Permanent equipment (equipped relic + passives) folds onto the same
		// surface (docs/03 sections 4-5): the cost/cooldown maths below applies
		// the combined category totals exactly once.
		UQinglanEconomyLibrary::AppendPermanentModifiers(this, Blessings);
	}
	const int32 EnergyCost = ComputeEnergyCost(Technique, Blessings);
	if (Combat->GetAttributes().SpiritualEnergy + KINDA_SMALL_NUMBER < static_cast<float>(EnergyCost))
	{
		FailValidation();
		return false;
	}
	if (GetCooldownRemaining(Technique->TechniqueId) > 0.f)
	{
		FailValidation();
		return false;
	}

	// 4) target: per-targeting aim requirements
	FVector AimPoint = FVector::ZeroVector;
	FVector AimDirection = FVector::ForwardVector;
	if (!ResolveAim(this, Technique, AimPoint, AimDirection))
	{
		FailValidation();
		return false;
	}

	// -- Anticipation: cancellable, zero cost (docs/02 §3) --
	// The cast id is minted here so IsCasting()/GetCurrentCastId() cover the
	// whole pipeline and reentrant casts are impossible; it is spent exactly
	// once at release.
	++CastSerialCounter;
	CurrentCastId = FQinglanCastId();
	CurrentCastId.Serial = CastSerialCounter;
	CurrentCastId.FrameNumber = static_cast<int32>(GFrameNumber & 0x7fffffff);
	CurrentTechnique = Technique;
	bInAnticipation = true;
	CastPhaseEndTime = PausedGameTimeSeconds(GetWorld()) + Technique->Phases.Anticipation;

	// Anticipation cue (telegraph; warning-relevant so it survives every
	// quality tier, code-contracts "Every shipped technique keeps telegraph").
	FQinglanTechniqueCastContext CueContext;
	CueContext.Caster = Owner;
	CueContext.Technique = Technique;
	CueContext.CastId = CurrentCastId;
	CueContext.Blessings = Blessings;
	CueContext.AimPoint = AimPoint;
	CueContext.AimDirection = AimDirection;
	FQinglanCueEvent Cue;
	Cue.CastId = CurrentCastId;
	Cue.SourceTransform = Owner->GetActorTransform();
	Cue.TargetPoint = AimPoint;
	Cue.Element = Technique->Element;
	Cue.StageId = FName("anticipation");
	Cue.FeatureId = FName("vfx.spell.anticipation");
	Cue.bWarningRelevant = true;
	UQinglanTechniqueExecutors::PublishCue(CueContext, Cue);

	return true;
}

void UQinglanTechniqueComponent::InterruptCast()
{
	if (!CurrentCastId.IsValid())
	{
		return;
	}
	const bool bPreRelease = bInAnticipation;
	// Pre-release interruption consumed nothing (no energy spent, no cooldown
	// started). Post-release interruption keeps the committed cost: released
	// techniques are never refunded (docs/02 §3).
	CurrentCastId = FQinglanCastId();
	CurrentTechnique = nullptr;
	bInAnticipation = false;
	CastPhaseEndTime = 0.f;
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Cast interrupted (pre-release=%d)"), bPreRelease ? 1 : 0);
	OnCastInterrupted.Broadcast();
}

float UQinglanTechniqueComponent::GetCooldownRemaining(FName TechniqueId) const
{
	const float* CooldownEnd = CooldownEndsAt.Find(TechniqueId);
	if (!CooldownEnd)
	{
		return 0.f;
	}
	return FMath::Max(0.f, *CooldownEnd - PausedGameTimeSeconds(GetWorld()));
}

bool UQinglanTechniqueComponent::SetLoadout(const TArray<UQinglanTechniqueDataAsset*>& NewLoadout)
{
	// docs/03 §3: "changes only at sanctuary or entrance shrine ... not
	// mid-combat ... cooldown-preserving" (blueprint §1.5). Never during a
	// cast and never inside an expedition room loop.
	if (CurrentCastId.IsValid())
	{
		return false;
	}
	if (NewLoadout.Num() > KLoadoutSlotCount)
	{
		return false;
	}
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			UQinglanRunSubsystem* Run = GameInstance->GetSubsystem<UQinglanRunSubsystem>();
			if (Run && Run->IsInExpedition())
			{
				return false;
			}
		}
	}

	UQinglanProfileSubsystem* Profile = nullptr;
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>();
		}
	}

	TSet<FName> SeenIds;
	SeenIds.Reserve(NewLoadout.Num());
	for (const UQinglanTechniqueDataAsset* Technique : NewLoadout)
	{
		if (!Technique || Technique->TechniqueId == NAME_None)
		{
			return false; // no empty slots mid-array; pad by passing fewer entries
		}
		if (SeenIds.Contains(Technique->TechniqueId))
		{
			return false; // duplicates rejected: stable ids are unique (code-contracts)
		}
		SeenIds.Add(Technique->TechniqueId);
		if (Profile && !Profile->IsTechniqueUnlocked(Technique->TechniqueId))
		{
			return false; // locked techniques cannot be equipped (docs/03 §3 unlock order)
		}
	}

	// CooldownEndsAt untouched: cooldowns are preserved across equipment
	// changes (docs/03 §3). Element-wise conversion TObjectPtr <- raw ptr.
	Loadout.Reset(NewLoadout.Num());
	for (UQinglanTechniqueDataAsset* Technique : NewLoadout)
	{
		Loadout.Add(Technique);
	}
	return true;
}

void UQinglanTechniqueComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!CurrentCastId.IsValid())
	{
		return;
	}
	const float Now = PausedGameTimeSeconds(GetWorld());

	if (bInAnticipation)
	{
		// Anticipation finished: release exactly once (docs/02 §3 "commits
		// energy and cooldown at the release notify, not at button press").
		if (Now >= CastPhaseEndTime)
		{
			ReleaseCurrentCast();
		}
	}

	// Recovery finished: the cast ends and the next cast may begin. Ticks stop
	// while paused, and the paused world clock does not advance, so both phase
	// deadlines freeze with paused game time (code-contracts "Timers").
	if (!bInAnticipation && CurrentCastId.IsValid() && Now >= CastPhaseEndTime)
	{
		CurrentCastId = FQinglanCastId();
		CurrentTechnique = nullptr;
	}
}

// --- Release (private) ---

void UQinglanTechniqueComponent::ReleaseCurrentCast()
{
	AActor* Owner = GetOwner();
	UQinglanTechniqueDataAsset* Technique = CurrentTechnique;
	UQinglanCombatComponent* Combat = Owner ? Owner->FindComponentByClass<UQinglanCombatComponent>() : nullptr;

	// Caster died or state collapsed mid-anticipation: cancel with zero cost.
	if (!Owner || !Technique || !Combat || !Combat->IsAlive())
	{
		InterruptCast();
		return;
	}

	FQinglanBlessingModifiers Blessings;
	{
		TArray<FName> Owned;
		UQinglanBlessingLibrary::GetOwnedBlessings(this, Owned);
		UQinglanBlessingLibrary::ComputeModifiers(Owned, Blessings);
		// Same fold as the validation site: permanent equipment joins the
		// blessing totals so the release commit and the pre-check agree.
		UQinglanEconomyLibrary::AppendPermanentModifiers(this, Blessings);
	}

	// Release chime rides the commit (docs/04 audio pass); null-safe.
	if (UGameInstance* GameInstance = Owner->GetGameInstance())
	{
		if (UQinglanAudioSubsystem* Audio = GameInstance->GetSubsystem<UQinglanAudioSubsystem>())
		{
			Audio->PlaySfx(FName(TEXT("sfx_technique_release")));
		}
	}

	// Commit energy exactly once; if energy dropped below cost during
	// anticipation the cast still costs nothing (pre-release failure,
	// docs/02 §3).
	const int32 EnergyCost = ComputeEnergyCost(Technique, Blessings);
	if (!Combat->TrySpendEnergy(EnergyCost))
	{
		InterruptCast();
		return;
	}

	// Flip the phase first: release happens exactly once per cast id and can
	// never reenter (docs/02 §3 "Prevent reentrant ability release with a
	// unique cast ID").
	bInAnticipation = false;

	// Commit cooldown exactly once.
	const float Cooldown = ComputeCooldownSeconds(Technique, Blessings);
	CooldownEndsAt.Add(Technique->TechniqueId, PausedGameTimeSeconds(GetWorld()) + Cooldown);
	OnCooldownStarted.Broadcast(Technique->TechniqueId, Cooldown);

	// Execute by targeting -> resolve hits/statuses through the combat
	// components -> publish cues (all inside the executor tail).
	FQinglanTechniqueCastContext Context;
	Context.Caster = Owner;
	Context.Technique = Technique;
	Context.CastId = CurrentCastId;
	Context.Blessings = Blessings;
	// docs/03 §3 tier-2 (+15% raw damage / heal / barrier). The profile owns
	// the purchased tier; headless/no-profile contexts keep the base 1.0
	// (AUD-37).
	Context.TierMultiplier = 1.f;
	if (UGameInstance* GameInstance = Owner->GetGameInstance())
	{
		if (const UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			Context.TierMultiplier = Profile->GetTechniqueTierMultiplier(Technique->TechniqueId);
		}
	}
	ResolveAim(this, Technique, Context.AimPoint, Context.AimDirection);
	UQinglanTechniqueExecutors::Execute(Context);

	OnCastReleased.Broadcast(Technique->TechniqueId, CurrentCastId);

	// Recovery window (plus the instantaneous release window) blocks the next
	// cast; docs/02 §6 "explicit targeting rule, release time, animation, cue
	// set" - phase durations come from the data asset.
	CastPhaseEndTime = PausedGameTimeSeconds(GetWorld()) + Technique->Phases.Release + Technique->Phases.Recovery;
}

void UQinglanTechniqueComponent::FailValidation()
{
	// Validation never leaves partial state: no energy is spent, no cooldown
	// started, no cast id minted before every check passes. Kept as a seam for
	// rejection telemetry/UI feedback (code-contracts logs; no player-facing
	// text here by design).
	UE_LOG(LogQinglanCombat, Verbose, TEXT("TryCast rejected at validation"));
}
