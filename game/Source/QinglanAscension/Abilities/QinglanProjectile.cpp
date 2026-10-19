// Pooled effect actor implementation (see QinglanProjectile.h).
#include "Abilities/QinglanProjectile.h"

#include "Abilities/QinglanTechniqueExecutors.h"
#include "QinglanAscension.h" // LogQinglanCombat
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/OverlapResult.h" // FOverlapResult full type (UE 5.8 split from EngineTypes)
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

namespace
{
	// Sweep sphere radius in cm; also the substep length cap so fast shots
	// cannot tunnel through a target capsule. FIXME-CONTRACT: projectile body
	// radius is not specified in docs/02; tunable default.
	constexpr float KSweepRadiusCm = 30.f;
	constexpr int32 KMaxSubsteps = 16;
	// Forward spawn offset from the casting origin (cast socket offset;
	// FIXME-CONTRACT: socket name unspecified in scaffold data).
	constexpr float KSpawnOffsetCm = 60.f;
	// Gap kept in front of a blocker when the spawn placement is clamped.
	constexpr float KSpawnWallStandoffCm = 5.f;

	/** True when the hit component is static/dynamic level geometry: the
	 *  blocker class projectiles must stop at (they never carry a combat
	 *  component, so the victim filter alone would silently skip them). */
	bool IsWorldBlocker(const FHitResult& Hit)
	{
		const ECollisionChannel ObjectType = Hit.Component.IsValid()
			? Hit.Component->GetCollisionObjectType()
			: ECC_MAX;
		return ObjectType == ECC_WorldStatic || ObjectType == ECC_WorldDynamic;
	}
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

	// AUD-42 placeholder body: the shot previously had no visible representation
	// at all. Components are created here (assets are loaded in BeginPlay, never
	// in the constructor/CDO), and collision stays disabled so presentation can
	// never alter the CPU-authoritative sweep.
	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(CollisionSphere);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	VisualMesh->SetGenerateOverlapEvents(false);
	VisualMesh->SetRelativeScale3D(FVector(0.35f));

	VisualLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("VisualLight"));
	VisualLight->SetupAttachment(CollisionSphere);
	VisualLight->SetIntensity(1200.f);
	VisualLight->SetAttenuationRadius(300.f);
	VisualLight->SetCastShadows(false);
}

void AQinglanProjectile::BeginPlay()
{
	Super::BeginPlay();
	EnsureVisualPlaceholders();
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
}

void AQinglanProjectile::EnsureVisualPlaceholders()
{
	// Engine basic shapes are always cooked; a missing asset only logs and the
	// cue remains logical (no gameplay dependency, docs/05 section 4).
	if (VisualMesh && VisualMesh->GetStaticMesh() == nullptr)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
		VisualMesh->SetStaticMesh(Mesh);
		if (!Mesh)
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("Projectile placeholder mesh missing; continuing without a body"));
		}
	}
	if (VisualMesh)
	{
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial")))
		{
			VisualMesh->SetMaterial(0, Material);
		}
		VisualMesh->SetVisibility(true, true);
	}
	if (VisualLight)
	{
		VisualLight->SetVisibility(true, true);
	}
}

void AQinglanProjectile::RefreshVisualPresentation()
{
	// Element colour is cosmetic; UPointLightComponent accepts a linear colour.
	const FLinearColor Color =
		Element == EQinglanElement::Fire ? FLinearColor(1.f, 0.35f, 0.1f) :
		Element == EQinglanElement::Water ? FLinearColor(0.3f, 0.6f, 1.f) :
		Element == EQinglanElement::Wood ? FLinearColor(0.4f, 0.9f, 0.5f) :
		Element == EQinglanElement::Lightning ? FLinearColor(0.8f, 0.7f, 1.f) :
		Element == EQinglanElement::Earth ? FLinearColor(0.9f, 0.75f, 0.4f) :
		Element == EQinglanElement::Metal ? FLinearColor(0.75f, 0.95f, 1.f) :
		FLinearColor::White;
	if (VisualLight)
	{
		VisualLight->SetLightColor(Color);
	}
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
	case EQinglanEffectMode::WetTrail:
		TickWetTrail(DeltaSeconds);
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
	EnsureVisualPlaceholders();
	RefreshVisualPresentation();
	SetActorHiddenInGame(false);
	SetActorTickEnabled(true);
	if (InOwner)
	{
		// Spawn ahead of the caster along the fire direction (cast socket
		// offset; FIXME-CONTRACT: socket name unspecified in scaffold data).
		// The placement segment is validated against world blockers so a
		// point-blank or shoulder-aimed shot can never materialize across a
		// wall: clamp to just short of the first blocking hit.
		const FVector CastingOrigin = InOwner->GetActorLocation();
		const FVector UncheckedSpawn = CastingOrigin + SafeDirection * KSpawnOffsetCm;
		float SpawnDistanceCm = KSpawnOffsetCm;
		if (UWorld* World = InOwner->GetWorld())
		{
			FCollisionObjectQueryParams ObjectParams;
			ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
			ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
			FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanProjectileSpawn), false, InOwner);
			FHitResult BlockHit;
			if (World->SweepSingleByObjectType(BlockHit, CastingOrigin, UncheckedSpawn, FQuat::Identity,
				ObjectParams, FCollisionShape::MakeSphere(KSweepRadiusCm), QueryParams))
			{
				SpawnDistanceCm = FMath::Max(0.f, BlockHit.Time * KSpawnOffsetCm - KSpawnWallStandoffCm);
			}
		}
		SetActorLocation(CastingOrigin + SafeDirection * SpawnDistanceCm, false);
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
	// docs/02 §6: heal 20 over 4 s -> 4 one-second ticks of 5. Total magnitude
	// divides across the authored tick count, so tier/blessing bonuses raise
	// the per-tick amount inside the same window; the final tick carries any
	// float remainder.
	const int32 TickCount = FMath::Max(1, FMath::RoundToInt(InDurationSeconds));
	HealTickInterval = InDurationSeconds / static_cast<float>(TickCount);
	HealPerTick = HealRemaining / static_cast<float>(TickCount);
	HealTimer = 0.f; // no heal at cast time: the first tick lands after one full interval

	bActive = true;
	SetActorHiddenInGame(true);
	SetActorTickEnabled(true);
	if (InOwner)
	{
		SetActorLocation(InOwner->GetActorLocation(), false);
	}
}

void AQinglanProjectile::SetupWetTrail(const FQinglanCastId& InCastId, FName InTechniqueId,
	AActor* InOwner, float InTrailRadiusCm, float InWetDurationBonus)
{
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	Element = EQinglanElement::Water; // mist_step, docs/02 §7
	EffectOwner = InOwner;
	Mode = EQinglanEffectMode::WetTrail;
	WetTrailRadiusCm = FMath::Max(50.f, InTrailRadiusCm);
	WetTrailDurationBonus = FMath::Max(0.f, InWetDurationBonus);

	bActive = true;
	SetActorHiddenInGame(true);
	SetActorTickEnabled(true);
	if (InOwner)
	{
		SetActorLocation(InOwner->GetActorLocation(), false);
	}
}

void AQinglanProjectile::SetCueContext(const FQinglanTechniqueCastContext& InContext)
{
	// AUD-42: the old impact path published an empty cast context, losing the
	// technique id and the authored impact system. Keep the complete release
	// identity; it is cosmetic-only state and is cleared by ResetForPool.
	CueContext = InContext;
}

FQinglanCueEvent AQinglanProjectile::BuildImpactCueEvent(const FVector& Location) const
{
	// Pure construction (no world access) so automation can assert that the
	// technique identity survives the pooled delivery path (AUD-42).
	FQinglanCueEvent Cue;
	Cue.CastId = CastId;
	Cue.TechniqueId = TechniqueId;
	Cue.SourceTransform.SetLocation(Location);
	Cue.TargetPoint = Location;
	Cue.Element = Element;
	Cue.Intensity = 1.f;
	Cue.StageId = FName("impact"); // docs/04 five-stage contract
	Cue.FeatureId = FName("vfx.spell.impact");
	return Cue;
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
	HealPerTick = 0.f;
	HealTimer = 0.f;
	WetTrailRadiusCm = 220.f;
	WetTrailDurationBonus = 0.f;
	WetTrailVictims.Empty();
	bAppliesBurn = false;
	CastId = FQinglanCastId();
	TechniqueId = NAME_None;
	Element = EQinglanElement::None;
	CueContext = FQinglanTechniqueCastContext(); // AUD-42: no stale technique identity on reuse

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

	// Pawns plus world blockers in one query: static/dynamic geometry stops
	// the shot (docs/02 §2 projectiles never pass through level geometry),
	// while piercing may skip pawns only.
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
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
			// Level geometry consumes the shot regardless of piercing: pierce
			// may skip pawns, never blockers.
			if (IsWorldBlocker(Hit))
			{
				PublishImpactCue(Hit.ImpactPoint, nullptr);
				ReturnToPool();
				return false;
			}

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
		const float TickAmount = FMath::Min(HealRemaining, HealPerTick); // 20 over 4 s -> 5/tick (docs/02 §6)
		ExecuteHealTick(TickAmount);
	}
	if (HealRemaining <= 0.f)
	{
		ReturnToPool();
	}
}

void AQinglanProjectile::TickWetTrail(float DeltaSeconds)
{
	AActor* Owner = EffectOwner.Get();
	if (!Owner)
	{
		ReturnToPool();
		return;
	}
	// The trail follows the owner's actual dodge: capsule-driven movement
	// stops at blockers, so sampling the real position each tick can never
	// soak wall-separated or unreachable targets (docs/02 §2 dodge rule).
	const UQinglanDodgeComponent* Dodge = Owner->FindComponentByClass<UQinglanDodgeComponent>();
	if (!Dodge || !Dodge->IsDodging())
	{
		ReturnToPool(); // dash finished (or wall-stopped early)
		return;
	}

	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanWetTrail), false, Owner);

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByObjectType(Overlaps, Owner->GetActorLocation(), FQuat::Identity,
		ObjectParams, FCollisionShape::MakeSphere(WetTrailRadiusCm), QueryParams);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Victim = Overlap.GetActor();
		if (!Victim || Victim == Owner || WetTrailVictims.Contains(Victim))
		{
			continue;
		}
		UQinglanCombatComponent* VictimCombat = ResolveVictimCombat(Victim);
		if (!VictimCombat || !VictimCombat->IsAlive())
		{
			continue;
		}
		// One deduplicated accumulated set across every sample: start, middle,
		// and end targets soak exactly once, none beyond actual movement.
		VictimCombat->ApplyStatus(EQinglanStatus::Wet, WetTrailDurationBonus);
		WetTrailVictims.Add(Victim);
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

	// Statuses ride only an accepted authoritative result: the pipeline's
	// precedence runs immunity/barrier/duplicate rejection before status, so a
	// dodged or duplicate-rejected hit must never apply or refresh Burn
	// (docs/02 §5, §7).
	if (bChanged && bAppliesBurn && Element == EQinglanElement::Fire && VictimCombat->IsAlive())
	{
		// docs/02 §7 Burn: 3 s, three 4-damage ticks, refresh not stack.
		VictimCombat->ApplyStatus(EQinglanStatus::Burn, BurnDurationSeconds);
	}
	return bChanged;
}

void AQinglanProjectile::PublishImpactCue(const FVector& Location, AActor* Victim)
{
	// Cosmetic only; cues never gate gameplay (blueprint §1.7 "missing GPU
	// emitter never suppresses damage/warnings"). AUD-42: the cue is built from
	// the retained release context, so it carries the technique id/cast id and
	// can resolve the technique's authored impact stage. A null Context.Technique
	// still publishes with Cue.TechniqueId; only the fallback marker exists.
	const FQinglanCueEvent Cue = BuildImpactCueEvent(Location);
	UQinglanTechniqueExecutors::PublishCue(CueContext, Cue, this);
}
