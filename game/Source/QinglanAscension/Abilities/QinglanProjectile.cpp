// Pooled effect actor implementation (see QinglanProjectile.h).
#include "Abilities/QinglanProjectile.h"

#include "Abilities/QinglanTechniqueExecutors.h"
#include "QinglanAscension.h" // LogQinglanCombat
#include "Combat/QinglanCombatComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/OverlapResult.h" // FOverlapResult full type (UE 5.8 split from EngineTypes)
#include "Engine/World.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

namespace
{
	// Sweep sphere radius in cm; also the substep length cap so fast shots
	// cannot tunnel through a target capsule. FIXME-CONTRACT: projectile body
	// radius is not specified in docs/02; tunable default.
	constexpr float KSweepRadiusCm = 30.f;
	constexpr int32 KMaxSubsteps = 16;
}

// --- Pool (per-world free list; never destroys actors mid-flight) ---
namespace
{
	TMap<TWeakObjectPtr<UWorld>, TArray<TWeakObjectPtr<AQinglanProjectile>>> GPool;
}

AQinglanProjectile* FQinglanProjectilePool::Acquire(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	TArray<TWeakObjectPtr<AQinglanProjectile>>& FreeList = GPool.FindOrAdd(World);
	while (FreeList.Num() > 0)
	{
		AQinglanProjectile* Candidate = FreeList.Pop().Get();
		if (IsValid(Candidate) && !Candidate->IsActive())
		{
			return Candidate;
		}
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	return World->SpawnActor<AQinglanProjectile>(AQinglanProjectile::StaticClass(), Params);
}

void FQinglanProjectilePool::Release(AQinglanProjectile* Effect)
{
	if (!IsValid(Effect))
	{
		return;
	}
	Effect->ResetForPool();
	UWorld* World = Effect->GetWorld();
	if (World)
	{
		GPool.FindOrAdd(World).Add(Effect);
	}
}

void FQinglanProjectilePool::ResetPoolForWorld(UWorld* World)
{
	if (!World)
	{
		return;
	}
	if (TArray<TWeakObjectPtr<AQinglanProjectile>>* Entries = GPool.Find(World))
	{
		// Pooled actors are RF_Transient; the world destroys them on cleanup.
		Entries->Empty();
	}
}

AQinglanProjectile::AQinglanProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = false; // offline game, single process
	SetNetUpdateFrequency(0.f); // UE 5.8: direct member access deprecated

	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	CollisionSphere->InitSphereRadius(KSweepRadiusCm);
	// Query-only, and in fact disabled: all hit detection is the manual sweep
	// below, so the collision body can never block movement or generate
	// physics (code-contracts "CPU-authoritative"; stone_aegis "no geometry
	// blocker", docs/02 §6, applies to all module effects).
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionSphere->SetGenerateOverlapEvents(false);
	RootComponent = CollisionSphere;
}

void AQinglanProjectile::BeginPlay()
{
	Super::BeginPlay();
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
}

void AQinglanProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bActive)
	{
		return;
	}

	// Paused-game-time semantics: Tick does not run while the game is paused,
	// so lifetime/countdowns freeze (code-contracts "Timers use paused game
	// time"; never wall clock).
	LifetimeRemaining -= DeltaSeconds;
	if (LifetimeRemaining <= 0.f)
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Effect %s expired by safety lifetime"), *TechniqueId.ToString());
		ReturnToPool();
		return;
	}
	if (EffectOwner.IsValid() == false)
	{
		ReturnToPool(); // stale owner: stop cleanly, no gameplay mutation
		return;
	}

	switch (Mode)
	{
	case EQinglanEffectMode::LinearProjectile:
		TickLinear(DeltaSeconds);
		break;
	case EQinglanEffectMode::OrbitPulses:
		TickOrbitPulses(DeltaSeconds);
		break;
	case EQinglanEffectMode::HealOverTime:
		TickHealOverTime(DeltaSeconds);
		break;
	default:
		ReturnToPool();
		break;
	}
}

// --- Configuration ---

void AQinglanProjectile::SetupLinear(const FQinglanCastId& InCastId, FName InTechniqueId, EQinglanElement InElement,
	AActor* InOwner, const FVector& InDirection, float SpeedCmPerSec, float RangeCm,
	float InRawMagnitude, bool bInAppliesBurn, bool bInPiercing, float RawDamageMultiplier,
	float InBurnDurationSeconds)
{
	const FVector SafeDirection = InDirection.GetSafeNormal();
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	Element = InElement;
	EffectOwner = InOwner;
	Mode = EQinglanEffectMode::LinearProjectile;
	VelocityCmPerSec = SafeDirection * SpeedCmPerSec;
	RemainingRangeCm = RangeCm;
	RawMagnitude = InRawMagnitude;
	DamageMultiplier = RawDamageMultiplier;
	bAppliesBurn = bInAppliesBurn;
	bPiercing = bInPiercing;
	BurnDurationSeconds = FMath::Max(0.f, InBurnDurationSeconds);

	bActive = true;
	SetActorHiddenInGame(false);
	SetActorTickEnabled(true);
	if (InOwner)
	{
		// Spawn ahead of the caster along the fire direction (cast socket
		// offset; FIXME-CONTRACT: socket name unspecified in scaffold data).
		const FVector SpawnLocation = InOwner->GetActorLocation() + SafeDirection * 60.f;
		SetActorLocation(SpawnLocation, false);
	}
}

void AQinglanProjectile::SetupOrbitPulses(const FQinglanCastId& InCastId, FName InTechniqueId, EQinglanElement InElement,
	AActor* InOwner, float InRawMagnitude, int32 InPulseCount, float InPulseInterval,
	float InPulseRadiusCm, int32 InMaxTargetsPerPulse, float RawDamageMultiplier)
{
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	Element = InElement;
	EffectOwner = InOwner;
	Mode = EQinglanEffectMode::OrbitPulses;
	RawMagnitude = InRawMagnitude;
	DamageMultiplier = RawDamageMultiplier;
	PulsesRemaining = InPulseCount;
	PulseInterval = FMath::Max(0.05f, InPulseInterval);
	PulseRadiusCm = InPulseRadiusCm;
	MaxTargetsPerPulse = InMaxTargetsPerPulse;
	PulseTimer = PulseInterval; // first pulse fires immediately on the next tick

	bActive = true;
	SetActorHiddenInGame(true); // orbit pulses use cue markers, not a body
	SetActorTickEnabled(true);
	if (InOwner)
	{
		SetActorLocation(InOwner->GetActorLocation(), false);
	}
}

void AQinglanProjectile::SetupHealOverTime(const FQinglanCastId& InCastId, FName InTechniqueId,
	AActor* InOwner, float InTotalHeal, float InDurationSeconds, float HealMultiplier)
{
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	Element = EQinglanElement::Wood; // verdant_pulse, docs/02 §6
	EffectOwner = InOwner;
	Mode = EQinglanEffectMode::HealOverTime;
	HealRemaining = InTotalHeal * HealMultiplier;
	// docs/02 §6: heal 20 over 4 s -> 4 one-second ticks of 5.
	const int32 TickCount = FMath::Max(1, FMath::RoundToInt(InDurationSeconds));
	HealTickInterval = InDurationSeconds / static_cast<float>(TickCount);
	HealTimer = HealTickInterval;

	bActive = true;
	SetActorHiddenInGame(true);
	SetActorTickEnabled(true);
	if (InOwner)
	{
		SetActorLocation(InOwner->GetActorLocation(), false);
	}
}

// --- Pooling contract ---

void AQinglanProjectile::ResetForPool()
{
	// Order matches code-contracts "Pools": timers, victims, owner, tags,
	// collision, lifetime.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this); // defensive: no world timers are used, but the contract is explicit
	}

	VictimHitOrdinals.Empty();
	ProcessedVictims.Empty();

	EffectOwner = nullptr;

	Tags.Empty();

	if (CollisionSphere)
	{
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CollisionSphere->SetGenerateOverlapEvents(false);
	}
	SetActorEnableCollision(false);

	LifetimeRemaining = UQinglanTechniqueExecutors::EffectLifetimeSafety;
	VelocityCmPerSec = FVector::ZeroVector;
	RemainingRangeCm = 0.f;
	RawMagnitude = 0.f;
	DamageMultiplier = 1.f;
	PulsesRemaining = 0;
	PulseTimer = 0.f;
	HealRemaining = 0.f;
	HealTimer = 0.f;
	bAppliesBurn = false;
	CastId = FQinglanCastId();
	TechniqueId = NAME_None;
	Element = EQinglanElement::None;

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetActorHiddenInGame(true);
	SetActorTickEnabled(false);
	bActive = false;
}

void AQinglanProjectile::ReturnToPool()
{
	FQinglanProjectilePool::Release(this);
}

// --- Tick drivers ---

void AQinglanProjectile::TickLinear(float DeltaSeconds)
{
	const FVector PreviousLocation = GetActorLocation();
	const FVector ProposedLocation = PreviousLocation + VelocityCmPerSec * DeltaSeconds;
	const float StepDistance = FVector::Dist(PreviousLocation, ProposedLocation);
	if (StepDistance <= 0.f)
	{
		return;
	}

	if (!SweepProjectilePath(ProposedLocation))
	{
		return; // consumed
	}

	SetActorLocation(ProposedLocation, false);
	RemainingRangeCm -= StepDistance;
	if (RemainingRangeCm <= 0.f)
	{
		ReturnToPool();
	}
}

bool AQinglanProjectile::SweepProjectilePath(const FVector& ProposedLocation)
{
	const FVector PreviousLocation = GetActorLocation();
	const FVector Delta = ProposedLocation - PreviousLocation;
	const float Distance = Delta.Size();

	const int32 SubstepCount = FMath::Clamp(FMath::CeilToInt(Distance / KSweepRadiusCm), 1, KMaxSubsteps);
	const FVector Step = Delta / static_cast<float>(SubstepCount);

	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanProjectileSweep), false /*bTraceComplex*/);
	QueryParams.AddIgnoredActor(EffectOwner.Get());

	for (int32 StepIndex = 0; StepIndex < SubstepCount; ++StepIndex)
	{
		const FVector Start = PreviousLocation + Step * static_cast<float>(StepIndex);
		const FVector End = Start + Step;

		TArray<FHitResult> Hits;
		GetWorld()->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectParams,
			FCollisionShape::MakeSphere(KSweepRadiusCm), QueryParams);

		// Deterministic ordering: sweep time first, then stable name order
		// (docs: no random selection in gameplay).
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
			AActor* Victim = Hit.GetActor();
			if (!Victim || Victim == EffectOwner.Get() || ProcessedVictims.Contains(Victim))
			{
				continue;
			}
			UQinglanCombatComponent* VictimCombat = ResolveVictimCombat(Victim);
			if (!VictimCombat || !VictimCombat->IsAlive())
			{
				continue;
			}

			const int32 HitOrdinal = ++VictimHitOrdinals.FindOrAdd(Victim);
			if (!ApplyHitToVictim(Victim, RawMagnitude * DamageMultiplier, HitOrdinal))
			{
				continue;
			}
			ProcessedVictims.Add(Victim);
			PublishImpactCue(Hit.ImpactPoint, Victim);

			if (!bPiercing)
			{
				ReturnToPool(); // ember_lance-style single-target shot
				return false;
			}
		}
	}
	return true;
}

void AQinglanProjectile::TickOrbitPulses(float DeltaSeconds)
{
	AActor* Owner = EffectOwner.Get();
	if (!Owner)
	{
		ReturnToPool();
		return;
	}
	SetActorLocation(Owner->GetActorLocation(), false);

	PulseTimer += DeltaSeconds;
	while (PulseTimer >= PulseInterval && PulsesRemaining > 0)
	{
		PulseTimer -= PulseInterval;
		ExecuteOrbitPulse();
		if (Mode != EQinglanEffectMode::OrbitPulses || !bActive)
		{
			return; // pool reset happened inside
		}
	}
	if (PulsesRemaining <= 0)
	{
		ReturnToPool();
	}
}

void AQinglanProjectile::ExecuteOrbitPulse()
{
	AActor* Owner = EffectOwner.Get();
	if (!Owner)
	{
		return;
	}

	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanOrbitPulse), false, Owner);

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByObjectType(Overlaps, Owner->GetActorLocation(), FQuat::Identity,
		ObjectParams, FCollisionShape::MakeSphere(PulseRadiusCm), QueryParams);

	// Deterministic nearest-first ordering: distance, then name.
	Overlaps.Sort([Center = Owner->GetActorLocation()](const FOverlapResult& A, const FOverlapResult& B)
	{
		const AActor* ActorA = A.GetActor();
		const AActor* ActorB = B.GetActor();
		const float DistA = FVector::DistSquared(ActorA ? ActorA->GetActorLocation() : Center, Center);
		const float DistB = FVector::DistSquared(ActorB ? ActorB->GetActorLocation() : Center, Center);
		if (!FMath::IsNearlyEqual(DistA, DistB))
		{
			return DistA < DistB;
		}
		return ActorA && ActorB ? ActorA->GetName() < ActorB->GetName() : ActorA != nullptr;
	});

	int32 TargetsThisPulse = 0;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		if (TargetsThisPulse >= MaxTargetsPerPulse)
		{
			break; // docs/02 §6: "at most 2 targets/pulse"
		}
		AActor* Victim = Overlap.GetActor();
		if (!Victim || Victim == Owner)
		{
			continue;
		}
		UQinglanCombatComponent* VictimCombat = ResolveVictimCombat(Victim);
		if (!VictimCombat || !VictimCombat->IsAlive())
		{
			continue;
		}

		const int32 HitOrdinal = ++VictimHitOrdinals.FindOrAdd(Victim);
		if (ApplyHitToVictim(Victim, RawMagnitude * DamageMultiplier, HitOrdinal))
		{
			++TargetsThisPulse;
			PublishImpactCue(Victim->GetActorLocation(), Victim);
		}
	}
	--PulsesRemaining;
}

void AQinglanProjectile::TickHealOverTime(float DeltaSeconds)
{
	AActor* Owner = EffectOwner.Get();
	if (!Owner)
	{
		ReturnToPool();
		return;
	}

	HealTimer += DeltaSeconds;
	while (HealTimer >= HealTickInterval && HealRemaining > 0.f)
	{
		HealTimer -= HealTickInterval;
		// Evenly split ticks; the final tick clears any float remainder.
		const float TickAmount = FMath::Min(HealRemaining, 5.f); // 20 over 4 s -> 5/tick (docs/02 §6)
		ExecuteHealTick(TickAmount);
	}
	if (HealRemaining <= 0.f)
	{
		ReturnToPool();
	}
}

void AQinglanProjectile::ExecuteHealTick(float TickAmount)
{
	UQinglanCombatComponent* Combat = ResolveVictimCombat(EffectOwner.Get());
	if (!Combat)
	{
		HealRemaining = 0.f;
		return;
	}

	// Authoritative heal through the combat component (docs/02 §6 "healing
	// is bounded by maximum health"); the tick stops early at full health
	// with no refund semantics.
	const FQinglanAttributes Attributes = Combat->GetAttributes();
	const float Healed = FMath::Min(TickAmount, Attributes.MaxHealth - Attributes.Health);
	if (Healed <= 0.f)
	{
		HealRemaining = 0.f; // already at full health: stop ticking, no refund semantics
		return;
	}
	Combat->Heal(Healed);
	HealRemaining -= Healed;
}

UQinglanCombatComponent* AQinglanProjectile::ResolveVictimCombat(AActor* Victim) const
{
	return Victim ? Victim->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
}

bool AQinglanProjectile::ApplyHitToVictim(AActor* Victim, float EffectiveRawMagnitude, int32 HitOrdinal)
{
	UQinglanCombatComponent* VictimCombat = ResolveVictimCombat(Victim);
	if (!VictimCombat)
	{
		return false;
	}

	// Authoritative resolution through the victim's combat component; it owns
	// mitigation, difficulty, reactions (Conduct), barriers, and duplicate hit
	// key rejection (docs/02 §5, §7; code-contracts "Combat semantics").
	FQinglanDamageEvent Event;
	Event.Attacker = EffectOwner.Get();
	Event.Target = Victim;
	Event.CastId = CastId;
	Event.HitOrdinal = HitOrdinal;
	Event.TechniqueId = TechniqueId;
	Event.Element = Element;
	Event.RawMagnitude = EffectiveRawMagnitude;
	Event.Timestamp = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f; // paused-game-time seconds
	const bool bChanged = VictimCombat->ApplyDamage(Event);

	if (bAppliesBurn && Element == EQinglanElement::Fire && VictimCombat->IsAlive())
	{
		// docs/02 §7 Burn: 3 s, three 4-damage ticks, refresh not stack.
		VictimCombat->ApplyStatus(EQinglanStatus::Burn, BurnDurationSeconds);
	}
	return bChanged;
}

void AQinglanProjectile::PublishImpactCue(const FVector& Location, AActor* Victim)
{
	// Cosmetic only; cues never gate gameplay (blueprint §1.7 "missing GPU
	// emitter never suppresses damage/warnings"). The full cue event is
	// published by the executors with the technique's cue set; this marker
	// exists so audio/VFX can key off the impact point deterministically.
	FQinglanCueEvent Cue;
	Cue.CastId = CastId;
	Cue.SourceTransform.SetLocation(Location);
	Cue.Element = Element;
	Cue.StageId = FName("impact"); // docs/04 five-stage contract
	Cue.FeatureId = FName("vfx.spell.impact");
	UQinglanTechniqueExecutors::PublishCue(FQinglanTechniqueCastContext(), Cue, this);
}
