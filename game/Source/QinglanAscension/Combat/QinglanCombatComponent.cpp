// Combat component implementation: attributes, damage pipeline, barriers,
// resource regen, hit audit, statuses. Authoritative docs:
// docs/02-combat-and-camera.md sections 3-7, docs/05-technical-architecture.md
// component table, docs/08-quality-and-testing.md section 2.
//
// NOTE (fixed scaffold): this class declares no extra members or helpers, so
// every behavior lives inside the declared function bodies below. Private
// member state is reachable only from these functions.
#include "Combat/QinglanCombatComponent.h"
#include "Abilities/QinglanBlessingLibrary.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanDamageMath.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"
#include "HAL/ThreadSafeCounter.h"

namespace
{
	// Safety valve: per-swing victim list and hit-key set are reset well before
	// they could grow unbounded (pool reuse, marathon fights).
	constexpr int32 MaxSwordSwingVictims = 64;
	constexpr int32 MaxTrackedHitKeys = 1024;

	FName BurnTickTechniqueId() { return FName("status.burn"); }

	FQinglanBlessingModifiers ResolvePlayerModifiers(UQinglanCombatComponent* Combat)
	{
		FQinglanBlessingModifiers Modifiers;
		if (Cast<AQinglanCharacter>(Combat->GetOwner()))
		{
			TArray<FName> Owned;
			UQinglanBlessingLibrary::GetOwnedBlessings(Combat, Owned);
			UQinglanBlessingLibrary::ComputeModifiers(Owned, Modifiers);
			UQinglanEconomyLibrary::AppendPermanentModifiers(Combat, Modifiers);
		}
		return Modifiers;
	}
}

UQinglanCombatComponent::UQinglanCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UQinglanCombatComponent::BeginPlay()
{
	Super::BeginPlay();

	// AUD-34: bind the dodge component's start notification instead of
	// polling the character state once per frame. The notification is
	// frame-exact and fires for every dodge, including consecutive ones whose
	// starts are closer than the old 0.5 s observation guard.
	if (AActor* Owner = GetOwner())
	{
		if (UQinglanDodgeComponent* Dodge = Owner->FindComponentByClass<UQinglanDodgeComponent>())
		{
			Dodge->OnDodgeStarted.AddUniqueDynamic(this, &UQinglanCombatComponent::HandleDodgeStarted);
		}
	}

	// AUD-37: fold permanent-equipment maxima on spawn and whenever the
	// profile commits (purchase/equip/unequip/load). Only the player pawn has
	// a profile; enemies never bind.
	if (Cast<AQinglanCharacter>(GetOwner()) != nullptr)
	{
		if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
		{
			if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
			{
				Profile->OnProfileCommitted.AddUniqueDynamic(this, &UQinglanCombatComponent::HandleProfileCommitted);
			}
			if (UQinglanRunSubsystem* Run = GameInstance->GetSubsystem<UQinglanRunSubsystem>())
			{
				Run->OnBlessingsChanged.AddUniqueDynamic(this, &UQinglanCombatComponent::HandleProfileCommitted);
			}
		}
		const bool bSpawnedAtFullHealth = Attributes.Health >= Attributes.MaxHealth;
		const bool bSpawnedAtFullEnergy = Attributes.SpiritualEnergy >= Attributes.MaxSpiritualEnergy;
		const bool bSpawnedAtFullStamina = Attributes.Stamina >= Attributes.MaxStamina;
		RefreshEquipmentAttributeBonuses();
		// Fresh pawns start full at their derived maxima. Mid-session equipment
		// changes use the delta path and cannot turn equipping into free healing.
		if (bSpawnedAtFullHealth) { Attributes.Health = Attributes.MaxHealth; }
		if (bSpawnedAtFullEnergy) { Attributes.SpiritualEnergy = Attributes.MaxSpiritualEnergy; }
		if (bSpawnedAtFullStamina) { Attributes.Stamina = Attributes.MaxStamina; }
		OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);
		OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
		OnStaminaChanged.Broadcast(Attributes.Stamina, Attributes.MaxStamina);
	}
}

void UQinglanCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (UQinglanProfileSubsystem* Profile = GameInstance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			Profile->OnProfileCommitted.RemoveDynamic(this, &UQinglanCombatComponent::HandleProfileCommitted);
		}
		if (UQinglanRunSubsystem* Run = GameInstance->GetSubsystem<UQinglanRunSubsystem>())
		{
			Run->OnBlessingsChanged.RemoveDynamic(this, &UQinglanCombatComponent::HandleProfileCommitted);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UQinglanCombatComponent::HandleDodgeStarted(float Duration)
{
	(void)Duration;
	const UWorld* World = GetWorld();
	NotifyDodgeStarted(World ? World->GetTimeSeconds() : 0.f);
}

void UQinglanCombatComponent::NotifyDodgeStarted(float StartTimeSeconds)
{
	// Unconditional re-anchor: each dodge owns its own immunity clock. A
	// second dodge starting before the first one's tail must not inherit the
	// old start (AUD-34 acceptance: 0.10-0.28 s from *this* dodge's start).
	DodgeStartTime = StartTimeSeconds;
}

void UQinglanCombatComponent::HandleProfileCommitted()
{
	RefreshEquipmentAttributeBonuses();
}

void UQinglanCombatComponent::RefreshEquipmentAttributeBonuses(bool bSnapshotIncludesBonuses)
{
	// Only the player pawn folds permanent equipment maxima; enemies have no
	// profile and keep their archetype-derived values.
	if (Cast<AQinglanCharacter>(GetOwner()) == nullptr)
	{
		AppliedEquipmentMaxHealthBonus = 0.f;
		AppliedEquipmentMaxStaminaBonus = 0.f;
		return;
	}

	float NewHealthBonus = 0.f;
	float NewStaminaBonus = 0.f;
	float NewEnergyBonus = 0.f;
	float NewPowerBonus = 0.f;
	UQinglanEconomyLibrary::GetPermanentAttributeBonuses(this, NewHealthBonus, NewStaminaBonus);
	if (UGameInstance* Instance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (const UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			float CultivationHealth = 0.f;
			UQinglanEconomyLibrary::GetCultivationBonuses(Profile->GetCultivationStage(),
				Profile->GetCultivationRank(), CultivationHealth, NewEnergyBonus, NewPowerBonus);
			NewHealthBonus += CultivationHealth;
		}
	}
	TArray<FName> OwnedBlessings;
	UQinglanBlessingLibrary::GetOwnedBlessings(this, OwnedBlessings);
	FQinglanBlessingModifiers Blessings;
	UQinglanBlessingLibrary::ComputeModifiers(OwnedBlessings, Blessings);
	NewHealthBonus += Blessings.MaxHealthBonus;

	const float HealthDelta = NewHealthBonus - AppliedEquipmentMaxHealthBonus;
	const float StaminaDelta = NewStaminaBonus - AppliedEquipmentMaxStaminaBonus;
	const float EnergyDelta = NewEnergyBonus - AppliedCultivationMaxEnergyBonus;
	const float PowerDelta = NewPowerBonus - AppliedCultivationPowerBonus;
	AppliedEquipmentMaxHealthBonus = NewHealthBonus;
	AppliedEquipmentMaxStaminaBonus = NewStaminaBonus;
	AppliedCultivationMaxEnergyBonus = NewEnergyBonus;
	AppliedCultivationPowerBonus = NewPowerBonus;

	// Checkpoint maxima already contain these bonuses. Rebase the ledger
	// without adding again so the next equip/rank change applies one delta.
	if (bSnapshotIncludesBonuses || (FMath::IsNearlyZero(HealthDelta)
		&& FMath::IsNearlyZero(StaminaDelta) && FMath::IsNearlyZero(EnergyDelta)
		&& FMath::IsNearlyZero(PowerDelta)))
	{
		return;
	}

	// Delta application keeps recomputation idempotent: equip/unequip adds or
	// removes its own contribution once, never stacks on repeated commits.
	Attributes.MaxHealth = FMath::Max(1.f, Attributes.MaxHealth + HealthDelta);
	Attributes.MaxStamina = FMath::Max(1.f, Attributes.MaxStamina + StaminaDelta);
	Attributes.MaxSpiritualEnergy = FMath::Max(1.f, Attributes.MaxSpiritualEnergy + EnergyDelta);
	Attributes.TechniquePower = FMath::Max(0.f, Attributes.TechniquePower + PowerDelta);
	Attributes.Health = FMath::Clamp(Attributes.Health, 0.f, Attributes.MaxHealth);
	Attributes.Stamina = FMath::Clamp(Attributes.Stamina, 0.f, Attributes.MaxStamina);
	Attributes.SpiritualEnergy = FMath::Clamp(Attributes.SpiritualEnergy, 0.f, Attributes.MaxSpiritualEnergy);

	OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);
	OnStaminaChanged.Broadcast(Attributes.Stamina, Attributes.MaxStamina);
	OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
}

void UQinglanCombatComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Paused-game-time semantics (docs/dev/code-contracts.md): world time
	// seconds freeze under pause and TickComponent does not run, so both
	// countdowns and timestamps are pause-safe.
	const float Now = World->GetTimeSeconds();

	// Dodge i-frames are re-anchored by UQinglanDodgeComponent::OnDodgeStarted
	// (BeginPlay binding); no per-frame observation is needed (AUD-34).

	// --- Resource regen (docs/02 §5) --------------------------------------
	RegenResources(DeltaTime);

	// --- Burn ticks: three 4-damage ticks across 3 s (docs/02 §7) ----------
	// Ticks drive the Burn lifecycle (tick 3 lands exactly at duration end);
	// ticks run before the generic expiry pass to avoid float-epsilon races.
	while (BurnTicksRemaining > 0 && ActiveStatuses.Contains(EQinglanStatus::Burn)
		&& Now >= BurnNextTickAt && IsAlive())
	{
		const int32 TickOrdinal = QinglanCombatTunables::BurnTickCount - BurnTicksRemaining + 1;

		float TickDamage = QinglanCombatTunables::BurnTickDamage; // 4 dmg per tick
		bool bBlockedByBarrier = false;

		if (BarrierAmount > 0.f)
		{
			const float Absorbed = FMath::Min(BarrierAmount, TickDamage); // barrier absorbs before health (docs/02 §6)
			BarrierAmount = FMath::Max(0.f, BarrierAmount - Absorbed);
			TickDamage -= Absorbed;
			bBlockedByBarrier = Absorbed > 0.f;
			if (BarrierAmount <= 0.f)
			{
				BarrierRemaining = 0.f;
				OnBarrierChanged.Broadcast(0.f, 0.f);
			}
			else
			{
				OnBarrierChanged.Broadcast(BarrierAmount, BarrierAmount + Absorbed);
			}
		}

		Attributes.Health = FMath::Clamp(Attributes.Health - TickDamage, 0.f, Attributes.MaxHealth);
		OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);

		FQinglanDamageEvent TickEvent;
		TickEvent.Attacker = nullptr; // source-agnostic: the scaffold ApplyStatus carries no attacker
		TickEvent.Target = GetOwner();
		TickEvent.CastId = FQinglanCastId(); // status ticks are not technique releases; no dedupe key
		TickEvent.HitOrdinal = TickOrdinal;  // multi-tick effects increment the ordinal (docs/02 §3)
		TickEvent.TechniqueId = BurnTickTechniqueId();
		TickEvent.Element = EQinglanElement::Fire;
		TickEvent.RawMagnitude = QinglanCombatTunables::BurnTickDamage;
		TickEvent.FinalMagnitude = QinglanCombatTunables::BurnTickDamage;
		TickEvent.Timestamp = Now;
		TickEvent.bBlockedByBarrier = bBlockedByBarrier;
		OnDamageResolved.Broadcast(TickEvent); // cue stage last

		// Accepted burn tick: same resolved-result notification as direct hits
		// so Burn kills drive the victim's death lifecycle too (AUD-28).
		FQinglanResolvedDamage TickResult;
		TickResult.Victim = GetOwner();
		TickResult.HitOrdinal = TickOrdinal;
		TickResult.AppliedMagnitude = QinglanCombatTunables::BurnTickDamage;
		TickResult.bBlockedByBarrier = bBlockedByBarrier;
		TickResult.bLethal = Attributes.Health <= 0.f;
		OnDamageResultResolved.Broadcast(TickResult);

		--BurnTicksRemaining;
		// AUD-37: the requested duration is spread across the three-tick
		// contract, so a longer (bonus) Burn keeps exactly three ticks.
		BurnNextTickAt = BurnNextTickAt + BurnTickInterval;
	}
	if (BurnTicksRemaining <= 0 && ActiveStatuses.Contains(EQinglanStatus::Burn))
	{
		ActiveStatuses.Remove(EQinglanStatus::Burn);
		OnStatusCleared.Broadcast(EQinglanStatus::Burn);
	}
	if (!IsAlive())
	{
		// Death by burn tick: same cleanup as the ApplyDamage death path.
		ActiveStatuses.Reset();
		BurnTicksRemaining = 0;
		BurnNextTickAt = -1.f;
		BarrierAmount = 0.f;
		BarrierRemaining = 0.f;
		BarrierElement = EQinglanElement::None;
	}

	// --- Status countdown ---------------------------------------------------
	TArray<EQinglanStatus, TInlineAllocator<8>> Expired;
	for (TPair<EQinglanStatus, float>& Pair : ActiveStatuses)
	{
		Pair.Value = FMath::Max(0.f, Pair.Value - DeltaTime);
		// Burn's three tick deadlines own expiry. Countdown rounding must not
		// delete the final tick a frame before its timestamp is reached.
		if (Pair.Value <= 0.f && Pair.Key != EQinglanStatus::Burn)
		{
			Expired.Add(Pair.Key);
		}
	}
	for (EQinglanStatus Status : Expired)
	{
		if (Status == EQinglanStatus::Burn)
		{
			BurnTicksRemaining = 0; // defensive: the tick loop above normally clears Burn first
			BurnNextTickAt = -1.f;
		}
		ActiveStatuses.Remove(Status);
		OnStatusCleared.Broadcast(Status);
	}

	// --- Barrier countdown --------------------------------------------------
	if (BarrierAmount > 0.f)
	{
		BarrierRemaining = FMath::Max(0.f, BarrierRemaining - DeltaTime);
		if (BarrierRemaining <= 0.f)
		{
			BarrierAmount = 0.f;
			BarrierElement = EQinglanElement::None;
			OnBarrierChanged.Broadcast(0.f, 0.f);
		}
	}

	// --- Per-swing sword energy context reset (docs/02 §4) ------------------
	// FIXME-CONTRACT: the scaffold lacks BeginSwordSwing()/EndSwordSwing();
	// the per-swing victim list and 8-energy cap reset while the owning
	// character is outside AttackActive (each sword cut has its own active
	// window: 0.10/0.10/0.12 s, docs/02 §4), so every cut re-arms the grant.
	if (AQinglanCharacter* PlayerOwner = Cast<AQinglanCharacter>(GetOwner()))
	{
		if (PlayerOwner->GetCombatState() != EQinglanCombatState::AttackActive
			&& (SwordSwingVictims.Num() > 0 || SwordSwingEnergyGranted > 0))
		{
			SwordSwingVictims.Reset();
			SwordSwingEnergyGranted = 0;
		}
	}
	else if (SwordSwingVictims.Num() > MaxSwordSwingVictims)
	{
		SwordSwingVictims.Reset();
		SwordSwingEnergyGranted = 0;
	}

	// --- Hit-key pruning (pool reuse safety, docs/05 pooling contract) ------
	// FIXME-CONTRACT: the scaffold exposes no ResetForPool() for this
	// component; stale keys from a pooled actor's previous life could reject
	// legitimate hits ("no stale hit" contract), so the audit set is bounded.
	if (AppliedHitKeys.Num() > MaxTrackedHitKeys)
	{
		AppliedHitKeys.Reset();
	}
}

void UQinglanCombatComponent::RegenResources(float DeltaTime)
{
	// Energy 4/s after 1 s without releasing a technique; stamina 20/s after
	// 0.8 s without spending stamina (docs/02 §5). Failed spends do not reset
	// the idle timers; only successful ones do (in TrySpend*).
	const float Elapsed = FMath::Max(0.f, DeltaTime);
	TimeSinceEnergySpend += Elapsed;
	TimeSinceStaminaSpend += Elapsed;
	const float EnergyRegenTime = FMath::Clamp(TimeSinceEnergySpend - EnergyRegenDelay, 0.f, Elapsed);
	const float StaminaRegenTime = FMath::Clamp(TimeSinceStaminaSpend - StaminaRegenDelay, 0.f, Elapsed);

	// Permanent equipment folds onto the base rates for the player pawn only
	// (Energy Recovery +1/s, Measured Breath +10% recovery, docs/03 section 4);
	// enemies keep the authored constants since the profile is the player's.
	float EnergyRegen = EnergyRegenPerSecond;
	float StaminaRegen = StaminaRegenPerSecond;
	if (Cast<AQinglanCharacter>(GetOwner()) != nullptr)
	{
		float HealingPercent = 0.f; // heal paths consume this output; regen does not
		UQinglanEconomyLibrary::AppendPermanentRegen(this, EnergyRegen, StaminaRegen, HealingPercent);
	}

	if (TimeSinceEnergySpend >= EnergyRegenDelay
		&& Attributes.SpiritualEnergy < Attributes.MaxSpiritualEnergy)
	{
		Attributes.SpiritualEnergy = FMath::Min(Attributes.MaxSpiritualEnergy,
			Attributes.SpiritualEnergy + EnergyRegen * EnergyRegenTime);
		OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
	}

	if (TimeSinceStaminaSpend >= StaminaRegenDelay
		&& Attributes.Stamina < Attributes.MaxStamina)
	{
		Attributes.Stamina = FMath::Min(Attributes.MaxStamina,
			Attributes.Stamina + StaminaRegen * StaminaRegenTime);
		OnStaminaChanged.Broadcast(Attributes.Stamina, Attributes.MaxStamina);
	}
}

bool UQinglanCombatComponent::IsInvulnerable() const
{
	// Dodge i-frames [0.10, 0.28] s of the 0.45 s dodge (docs/02 §2), measured
	// in paused game time so the window is identical at 30/60/120 FPS
	// (docs/08 §2 "Dodge" acceptance).
	const UWorld* World = GetWorld();
	return World ? IsInvulnerableAtTime(World->GetTimeSeconds()) : false;
}

bool UQinglanCombatComponent::IsInvulnerableAtTime(float TimeSeconds) const
{
	const float Elapsed = TimeSeconds - DodgeStartTime;
	return Elapsed >= InvulnerableWindowStart && Elapsed <= InvulnerableWindowEnd;
}

float UQinglanCombatComponent::ResolveReactionMultiplier(EQinglanElement IncomingElement, AActor* Attacker)
{
	// Conduct: Thunder vs Wet -> one 1.25x application, consumes Wet once
	// (docs/02 §7). Consuming Wet here makes the reaction one-shot per Wet
	// application: it cannot recursively trigger itself, and by design it never
	// chains between targets.
	if (IncomingElement == EQinglanElement::Lightning
		&& ActiveStatuses.Contains(EQinglanStatus::Wet)
		&& ActiveStatuses[EQinglanStatus::Wet] > 0.f)
	{
		RemoveStatus(EQinglanStatus::Wet);

		// AUD-37: Elemental Insight / Thunder Charm / expedition blessings
		// raise the reaction once. Only the player's own attacks may read the
		// player profile (attacker == the caster character for sword,
		// technique, and projectile damage); enemy sources keep the base 1.25
		// so the player's gear never buffs enemy Lightning.
		float ConductPercent = 0.f;
		if (Cast<AQinglanCharacter>(Attacker) != nullptr)
		{
			FQinglanBlessingModifiers Modifiers;
			TArray<FName> OwnedBlessings;
			UQinglanBlessingLibrary::GetOwnedBlessings(Attacker, OwnedBlessings);
			UQinglanBlessingLibrary::ComputeModifiers(OwnedBlessings, Modifiers);
			UQinglanEconomyLibrary::AppendPermanentModifiers(Attacker, Modifiers);
			ConductPercent = FMath::Max(0.f, Modifiers.ConductDamagePercent);
		}
		return QinglanCombatTunables::ConductMultiplier * (1.f + ConductPercent / 100.f);
	}
	return 1.f;
}

bool UQinglanCombatComponent::ApplyDamage(const FQinglanDamageEvent& Request)
{
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return false;
	}

	// Health reaches zero -> no further input or duplicate reward (docs/08 §2).
	if (!IsAlive())
	{
		return false;
	}
	// This entry point only ever damages; healing is bounded elsewhere.
	if (Request.RawMagnitude < 0.f)
	{
		return false;
	}

	FQinglanDamageEvent Event = Request;
	const float Now = World->GetTimeSeconds();
	Event.Timestamp = Now; // paused-game-time timestamp (docs/dev/code-contracts.md)
	if (!Event.Target.IsValid())
	{
		Event.Target = Owner; // component owner is the resolved victim
	}

	// --- Audit gate: reject duplicate (CastId, victim, ordinal) -------------
	// A damage event is resolved once; a visual emitter never applies damage
	// (docs/02 §3, §5). The victim is this component's owner; the key binds
	// cast id + ordinal to it. Status ticks carry invalid CastIds and are not
	// keyed. The attacker joins the key (AUD-33) so independent attackers
	// whose cast ids collide both resolve, while one attack's repeats still
	// dedupe: same attacker + cast + victim + ordinal is still one key.
	uint64 HitKey = 0;
	if (Event.CastId.IsValid())
	{
		const uint32 VictimKey = static_cast<uint32>(Owner->GetUniqueID());
		const uint32 AttackerKey = Event.Attacker.IsValid()
			? static_cast<uint32>(Event.Attacker->GetUniqueID()) : 0u;
		HitKey = QinglanDamageMath::MakeHitKey(Event.CastId,
			HashCombine(VictimKey, AttackerKey), Event.HitOrdinal);
		if (AppliedHitKeys.Contains(HitKey))
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("Rejected duplicate hit key (ordinal %d)"), Event.HitOrdinal);
			return false;
		}
		AppliedHitKeys.Add(HitKey);
	}

	// --- Stage 1: immunity (precedence: immunity first, docs/02 §7) ---------
	if (IsInvulnerable())
	{
		// The hit was consumed by the dodge: its key stays registered so the
		// same (cast, ordinal) cannot be re-processed, but nothing changes.
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Hit dodged inside i-frame window"));
		return false;
	}

	// --- Stage 2: reaction consumption (pre-damage, docs/02 §7) -------------
	// Fire removes Wet before its normal damage calculation and gains no
	// bonus; Water removes Burn, consuming one application; Thunder vs Wet is
	// Conduct and multiplies the damage instead. Precedence vs. the damage
	// number: the multiplier must be known before the math, while barrier and
	// health changes happen on the next stage.
	const float ReactionMultiplier = ResolveReactionMultiplier(Event.Element, Event.Attacker.Get());
	if (Event.Element == EQinglanElement::Fire && HasStatus(EQinglanStatus::Wet))
	{
		RemoveStatus(EQinglanStatus::Wet);
	}
	else if (Event.Element == EQinglanElement::Water && HasStatus(EQinglanStatus::Burn))
	{
		RemoveStatus(EQinglanStatus::Burn);
	}

	// --- Stage 3: barrier/health damage (docs/02 §5, §6) --------------------
	const float Mitigated = QinglanDamageMath::ComputeMitigatedDamage(Event.RawMagnitude, Attributes.Defense);
	const float DamageTakenMultiplier = FMath::Max(0.f,
		1.f + ResolvePlayerModifiers(this).DamageTakenPercent / 100.f);
	const float Final = QinglanDamageMath::ComputeFinalDamage(
		Mitigated * DamageTakenMultiplier, ReactionMultiplier, QinglanCombatTunables::DefaultDifficultyMultiplier);

	float Remaining = Final;
	if (BarrierAmount > 0.f && Remaining > 0.f)
	{
		// Barriers absorb before health; a fully absorbed hit still counts as
		// a resolved event flagged bBlockedByBarrier (docs/02 §6).
		const float Absorbed = FMath::Min(BarrierAmount, Remaining);
		BarrierAmount = FMath::Max(0.f, BarrierAmount - Absorbed);
		Remaining -= Absorbed;
		Event.bBlockedByBarrier = true;
		if (BarrierAmount <= 0.f)
		{
			BarrierRemaining = 0.f;
			BarrierElement = EQinglanElement::None;
			OnBarrierChanged.Broadcast(0.f, 0.f);
		}
		else
		{
			// Maximum = the barrier amount as of this absorption (current +
			// absorbed); the scaffold stores no separate max field.
			OnBarrierChanged.Broadcast(BarrierAmount, BarrierAmount + Absorbed);
		}
	}

	if (Remaining > 0.f)
	{
		Attributes.Health = FMath::Clamp(Attributes.Health - Remaining, 0.f, Attributes.MaxHealth);
		OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);
	}

	Event.FinalMagnitude = Final;

	if (Attributes.Health <= 0.f)
	{
		// Death mid-resolution: clear transient combat state so nothing keeps
		// ticking a dead actor (docs/08 §2 "Death" acceptance).
		ActiveStatuses.Reset();
		BurnTicksRemaining = 0;
		BurnNextTickAt = -1.f;
		BarrierAmount = 0.f;
		BarrierRemaining = 0.f;
		BarrierElement = EQinglanElement::None;
	}

	// --- Stage 4/5: status application then cue ------------------------------
	// New statuses (e.g. Burn from ember_lance) are applied by the delivery
	// layer through ApplyStatus AFTER ApplyDamage returns, keeping the
	// precedence order immunity -> barrier/health -> reaction -> status -> cue
	// (docs/02 §7). The cue stage is this broadcast.
	OnDamageResolved.Broadcast(Event);

	// Resolved-result notification for accepted hits only (AUD-28): victims
	// drive stagger/death from this, and delivery layers gate trailing status
	// application on ApplyDamage's return while presentation reads the final
	// magnitude here.
	FQinglanResolvedDamage Result;
	Result.Attacker = Event.Attacker;
	Result.Victim = Event.Target;
	Result.CastId = Event.CastId;
	Result.HitOrdinal = Event.HitOrdinal;
	Result.AppliedMagnitude = Final;
	Result.bBlockedByBarrier = Event.bBlockedByBarrier;
	Result.bLethal = Attributes.Health <= 0.f;
	Result.HitKey = static_cast<int64>(HitKey); // bit-exact reinterpret for the Blueprint-visible field
	OnDamageResultResolved.Broadcast(Result);
	return true;
}

FQinglanCastId UQinglanCombatComponent::NextCastId()
{
	// One process-wide counter: independent attackers can never mint
	// colliding serials, and pooled actors reuse without resets, so an id
	// stays unique for the whole session (AUD-33).
	static FThreadSafeCounter GlobalCastSerial;
	FQinglanCastId Id;
	Id.Serial = GlobalCastSerial.Increment();
	Id.FrameNumber = static_cast<int32>(GFrameNumber);
	return Id;
}

void UQinglanCombatComponent::SetDerivedAttributes(const FQinglanAttributes& NewAttributes)
{
	// This API receives complete derived values, such as GetAttributes().
	// Raw player checkpoint maxima are rebuilt by RestoreCheckpointAttributes.
	FQinglanAttributes Next = NewAttributes;
	Next.MaxHealth = FMath::Max(1.f, Next.MaxHealth);
	Next.MaxSpiritualEnergy = FMath::Max(1.f, Next.MaxSpiritualEnergy);
	Next.MaxStamina = FMath::Max(1.f, Next.MaxStamina);
	Next.TechniquePower = FMath::Max(0.f, Next.TechniquePower);
	Next.Defense = FMath::Max(0.f, Next.Defense);
	Next.Health = FMath::Clamp(Next.Health, 0.f, Next.MaxHealth);
	Next.SpiritualEnergy = FMath::Clamp(Next.SpiritualEnergy, 0.f, Next.MaxSpiritualEnergy);
	Next.Stamina = FMath::Clamp(Next.Stamina, 0.f, Next.MaxStamina);

	Attributes = Next;

	// Rebase the ledger to the already included bonuses rather than adding
	// them a second time after restoring a complete snapshot.
	RefreshEquipmentAttributeBonuses(/*bSnapshotIncludesBonuses=*/true);

	OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);
	OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
	OnStaminaChanged.Broadcast(Attributes.Stamina, Attributes.MaxStamina);
	OnBarrierChanged.Broadcast(BarrierAmount, BarrierAmount);
}

void UQinglanCombatComponent::RestoreCheckpointAttributes(const FQinglanAttributes& Snapshot)
{
	if (!Cast<AQinglanCharacter>(GetOwner()))
	{
		SetDerivedAttributes(Snapshot);
		return;
	}
	// Entrance vitals can predate a blessing chosen in the cleared room.
	// Maxima and power are derived from the loaded profile/run, never from
	// that older entrance snapshot (which also supports pre-fix saves).
	Attributes = FQinglanAttributes();
	Attributes.Health = Snapshot.Health;
	Attributes.SpiritualEnergy = Snapshot.SpiritualEnergy;
	Attributes.Stamina = Snapshot.Stamina;
	AppliedEquipmentMaxHealthBonus = 0.f;
	AppliedEquipmentMaxStaminaBonus = 0.f;
	AppliedCultivationMaxEnergyBonus = 0.f;
	AppliedCultivationPowerBonus = 0.f;
	RefreshEquipmentAttributeBonuses();
	SetDerivedAttributes(Attributes);
}

void UQinglanCombatComponent::ApplyBarrier(EQinglanElement Element, float Amount, float Duration)
{
	// Barriers absorb before health; the stronger barrier replaces a weaker
	// one and barriers never stack (docs/02 §6, docs/08 §2 "Barrier").
	// An equal re-cast refreshes duration and element without stacking.
	if (Amount <= 0.f)
	{
		return;
	}
	if (Amount < BarrierAmount)
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Weaker barrier ignored (existing %f, new %f)"), BarrierAmount, Amount);
		return;
	}
	BarrierElement = Element;
	BarrierAmount = Amount;
	BarrierRemaining = FMath::Max(0.f, Duration);
	OnBarrierChanged.Broadcast(BarrierAmount, BarrierAmount);
}

bool UQinglanCombatComponent::TrySpendStamina(float Amount)
{
	if (Amount < 0.f || Attributes.Stamina < Amount)
	{
		return false; // atomic: insufficient means no state change
	}
	Attributes.Stamina -= Amount;
	TimeSinceStaminaSpend = 0.f; // restarts the 0.8 s regen delay (docs/02 §5)
	OnStaminaChanged.Broadcast(Attributes.Stamina, Attributes.MaxStamina);
	return true;
}

bool UQinglanCombatComponent::TrySpendEnergy(float Amount)
{
	if (Amount < 0.f || Attributes.SpiritualEnergy < Amount)
	{
		return false; // atomic: insufficient means no state change
	}
	Attributes.SpiritualEnergy -= Amount;
	TimeSinceEnergySpend = 0.f; // restarts the 1 s regen delay (docs/02 §5)
	OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
	return true;
}

void UQinglanCombatComponent::GrantEnergy(float Amount)
{
	// Focus Talisman-style restore; bounded by maximum (docs/03 talismans).
	if (Amount <= 0.f)
	{
		return;
	}
	const float Before = Attributes.SpiritualEnergy;
	Attributes.SpiritualEnergy = FMath::Clamp(Attributes.SpiritualEnergy + Amount,
		0.f, Attributes.MaxSpiritualEnergy);
	if (!FMath::IsNearlyEqual(Before, Attributes.SpiritualEnergy))
	{
		OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
	}
}

void UQinglanCombatComponent::NotifySwordHitTarget(AActor* Victim)
{
	// Sword hits restore 4 Spiritual Energy per unique target, capped at 8 per
	// swing (two targets); hitting training targets generates no campaign
	// resources (docs/02 §4, docs/03).
	if (!Victim)
	{
		return;
	}
	if (SwordSwingVictims.Contains(Victim))
	{
		return; // one grant per unique victim per swing
	}
	SwordSwingVictims.Add(Victim);

	if (Victim->ActorHasTag(FName(QinglanCombatTunables::QinglanTrainingTargetTag)))
	{
		return; // recorded for dedupe, but never grants energy
	}

	const FQinglanBlessingModifiers Modifiers = ResolvePlayerModifiers(this);
	const float Cap = QinglanCombatTunables::SwordEnergyCapPerSwing + Modifiers.SwordEnergyCapBonus;
	if (SwordSwingEnergyGranted >= Cap)
	{
		return;
	}
	const float Grant = FMath::Min(QinglanCombatTunables::SwordEnergyPerTarget
		+ Modifiers.SwordEnergyPerHitBonus, Cap - SwordSwingEnergyGranted);
	SwordSwingEnergyGranted += Grant;

	Attributes.SpiritualEnergy = FMath::Clamp(Attributes.SpiritualEnergy + Grant,
		0.f, Attributes.MaxSpiritualEnergy);
	OnEnergyChanged.Broadcast(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
}

void UQinglanCombatComponent::NotifySwordSwingStarted()
{
	SwordSwingVictims.Reset();
	SwordSwingEnergyGranted = 0;
}

void UQinglanCombatComponent::ResetForPool()
{
	AppliedHitKeys.Reset();
	NotifySwordSwingStarted();
	// No stale immunity clock or burn cadence from a pooled actor's prior life.
	DodgeStartTime = -1000.f;
	BurnTicksRemaining = 0;
	BurnNextTickAt = -1.f;
	BurnTickInterval = QinglanCombatTunables::BurnTickIntervalSeconds;
}

void UQinglanCombatComponent::Heal(float Amount)
{
	if (Amount <= 0.f || !IsAlive())
	{
		return;
	}

	// AUD-37: every player heal path (Verdant Pulse's heal-over-time, Healing
	// Talisman, future relics) resolves permanent healing percent once here:
	// Herbal Knowledge (+10%) and Verdant Seed. Enemies keep raw amounts.
	float EffectiveAmount = Amount;
	if (Cast<AQinglanCharacter>(GetOwner()) != nullptr)
	{
		float UnusedEnergyRegen = 0.f;
		float UnusedStaminaRegen = 0.f;
		float HealingPercent = 0.f;
		UQinglanEconomyLibrary::AppendPermanentRegen(this, UnusedEnergyRegen, UnusedStaminaRegen, HealingPercent);
		EffectiveAmount *= (1.f + FMath::Max(0.f, HealingPercent) / 100.f);
	}

	Attributes.Health = FMath::Clamp(Attributes.Health + EffectiveAmount, 0.f, Attributes.MaxHealth);
	OnHealthChanged.Broadcast(Attributes.Health, Attributes.MaxHealth);
}

void UQinglanCombatComponent::ApplyStatus(EQinglanStatus Status, float Magnitude)
{
	if (!IsAlive())
	{
		return;
	}
	UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	switch (Status)
	{
	case EQinglanStatus::Wet:
		// Wet: 5 s base plus the accumulated WetDurationBonus, refreshes on
		// reapplication (docs/02 §7). Only authored Water attacks or explicit
		// gameplay puddles reach this call; cosmetic rain never does. The
		// delivered magnitude is the bonus (mist_step passes the blessing /
		// relic total), so the base duration is never replaced by it.
		ActiveStatuses.Add(EQinglanStatus::Wet, QinglanCombatTunables::WetDurationSeconds
			+ FMath::Max(0.f, Magnitude));
		OnStatusApplied.Broadcast(EQinglanStatus::Wet);
		break;

	case EQinglanStatus::Burn:
		// Burn: requested duration (base 3 s + BurnDurationBonus), exactly three
		// 4-damage ticks spread across it, refresh duration without stacking,
		// maximum three ticks after the latest refresh (docs/02 §7). The tick
		// counter RESETS to three; it never accumulates past the authored max.
		{
			const float Duration = Magnitude > 0.f
				? Magnitude : QinglanCombatTunables::BurnDurationSeconds;
			ActiveStatuses.Add(EQinglanStatus::Burn, FMath::Max(0.01f, Duration));
			BurnTicksRemaining = QinglanCombatTunables::BurnTickCount;
			BurnTickInterval = FMath::Max(0.01f, Duration / static_cast<float>(QinglanCombatTunables::BurnTickCount));
			BurnNextTickAt = Now + BurnTickInterval;
			OnStatusApplied.Broadcast(EQinglanStatus::Burn);
		}
		break;

	case EQinglanStatus::Root:
		{
			// Root: 1 s ordinary enemies only; bosses receive a 10% slow for 1 s
			// instead and are never rooted (docs/02 §7). This component is the
			// single Root entry point: non-boss owners get the ActiveStatuses
			// entry (AQinglanCharacter gates CanDodge on HasStatus(Root)) and
			// the movement effect goes to UQinglanStatusComponent's loop-free
			// ApplyRootMovement sink.
			const float Duration = Magnitude > 0.f ? Magnitude : QinglanCombatTunables::RootDurationSeconds;
			AActor* const OwnerActor = GetOwner();
			const bool bBoss = OwnerActor
				&& OwnerActor->ActorHasTag(FName(QinglanCombatTunables::QinglanBossTag));
			UQinglanStatusComponent* const StatusComp = OwnerActor
				? OwnerActor->FindComponentByClass<UQinglanStatusComponent>() : nullptr;
			if (StatusComp)
			{
				StatusComp->ApplyRootMovement(Duration, bBoss);
			}
			if (!bBoss)
			{
				ActiveStatuses.Add(EQinglanStatus::Root, Duration);
				if (!StatusComp)
				{
					OnStatusApplied.Broadcast(EQinglanStatus::Root); // with a sibling, its own delegates already fired
				}
			}
		}
		break;

	case EQinglanStatus::Conduct:
		// Not a stored status: Conduct resolves one-shot inside
		// ResolveReactionMultiplier during ApplyDamage (docs/02 §7). Cue
		// publication happens on the damage-resolved event.
		break;

	case EQinglanStatus::None:
	default:
		break;
	}
}

bool UQinglanCombatComponent::HasStatus(EQinglanStatus Status) const
{
	if (Status == EQinglanStatus::Conduct || Status == EQinglanStatus::None)
	{
		return false; // never stored
	}
	const float* Remaining = ActiveStatuses.Find(Status);
	return Remaining != nullptr && *Remaining > 0.f;
}

float UQinglanCombatComponent::GetStatusRemaining(EQinglanStatus Status) const
{
	const float* Remaining = ActiveStatuses.Find(Status);
	return Remaining ? FMath::Max(0.f, *Remaining) : 0.f;
}

void UQinglanCombatComponent::RemoveStatus(EQinglanStatus Status)
{
	// Water removes Burn (consuming one application); Fire removes Wet
	// pre-damage; Cleansing Talisman removes Burn/Root (docs/02 §7, docs/03).
	const bool bWasStored = ActiveStatuses.Remove(Status) > 0;
	if (Status == EQinglanStatus::Root)
	{
		// Root also lives in the sibling status component (movement effect);
		// clear both stores so queries and movement agree. ClearRootMovement
		// never calls back, so this is loop-free.
		if (UQinglanStatusComponent* StatusComp = GetOwner() ? GetOwner()->FindComponentByClass<UQinglanStatusComponent>() : nullptr)
		{
			StatusComp->ClearRootMovement();
		}
	}
	if (bWasStored)
	{
		if (Status == EQinglanStatus::Burn)
		{
			BurnTicksRemaining = 0;
			BurnNextTickAt = -1.f;
		}
		OnStatusCleared.Broadcast(Status);
	}
}
