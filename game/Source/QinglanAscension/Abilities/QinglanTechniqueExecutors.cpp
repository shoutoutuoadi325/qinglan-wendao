// Technique effect executors implementation (see QinglanTechniqueExecutors.h).
#include "Abilities/QinglanTechniqueExecutors.h"

#include "Abilities/QinglanProjectile.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDodgeComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/OverlapResult.h" // FOverlapResult full type (UE 5.8 split from EngineTypes)
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Platform/QinglanGraphicsSettingsService.h"
#include "QinglanAscension.h"

namespace
{
	UQinglanGraphicsSettingsService* GetGraphicsService(const UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UQinglanGraphicsSettingsService>() : nullptr;
	}

	/** Cue feature ids; "Low quality must retain spell telegraphs and impact
	 *  silhouettes" (code-contracts; blueprint §1.7), so warning-relevant
	 *  stages never pass through the budget gate. */
	bool IsCueAllowed(const FQinglanCueEvent& Cue, UWorld* World)
	{
		if (Cue.bWarningRelevant)
		{
			return true;
		}
		UQinglanGraphicsSettingsService* Graphics = GetGraphicsService(World);
		return !Graphics || Graphics->IsFeatureAllowed(Cue.FeatureId);
	}

	UNiagaraSystem* ResolveStageCue(const UQinglanTechniqueDataAsset* Technique, const FName& StageId)
	{
		if (!Technique)
		{
			return nullptr;
		}
		TSoftObjectPtr<UNiagaraSystem> Selected;
		if (StageId == FName("anticipation"))
		{
			Selected = Technique->AnticipationCue;
		}
		else if (StageId == FName("release"))
		{
			Selected = Technique->ReleaseCue;
		}
		else if (StageId == FName("impact"))
		{
			Selected = Technique->ImpactCue;
		}
		return Selected.IsNull() ? nullptr : Selected.LoadSynchronous(); // refs are cook-validated (blueprint §3)
	}

	/**
	 * AUD-44: the release notify is the moment the cast identity is committed, so
	 * the technique's cosmetic CastMontage belongs here (it was authored into the
	 * data assets by QinglanAnimBootstrap but never played before). Missing
	 * montages, mesh animation instances, or sockets only log; gameplay has
	 * already resolved or will resolve regardless of whether the pose plays.
	 */
	void PlayReleaseCastMontage(const FQinglanTechniqueCastContext& Context)
	{
		const UQinglanTechniqueDataAsset* Technique = Context.Technique;
		AActor* Caster = Context.Caster.Get();
		if (!Technique || !Caster)
		{
			return;
		}
		if (Technique->CastMontage.IsNull())
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("Technique %s has no CastMontage; release pose skipped"),
				*Technique->TechniqueId.ToString());
			return;
		}
		UAnimMontage* Montage = Technique->CastMontage.LoadSynchronous();
		ACharacter* Character = Cast<ACharacter>(Caster);
		USkeletalMeshComponent* MeshComp = Character ? Character->GetMesh() : nullptr;
		UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
		if (!Montage || !AnimInstance)
		{
			// AUD-44 open gap: a pure-C++ pawn without an AnimInstance cannot
			// montage-blend. The shared idle/montage overlay is the whole rig
			// presentation; no full AnimBP state machine exists yet.
			UE_LOG(LogQinglanCombat, Warning,
				TEXT("Technique %s CastMontage could not play (montage=%s animinstance=%s); gameplay continues"),
				*Technique->TechniqueId.ToString(), Montage ? TEXT("ok") : TEXT("missing"),
				AnimInstance ? TEXT("ok") : TEXT("missing"));
			return;
		}
		if (AnimInstance->Montage_Play(Montage, 1.f) <= 0.f)
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("Technique %s CastMontage failed to start; gameplay continues"),
				*Technique->TechniqueId.ToString());
		}
	}

	/**
	 * AUD-42 placeholder presentation: when a stage has no Niagara system (the
	 * current content set has none), spawn a short-lived engine-shape marker so
	 * the cue is still visible. No dynamic light is created, so low-flash and
	 * Low quality keep the same silhouette; this is explicitly a placeholder
	 * until authored Niagara/audio assets land.
	 */
	void SpawnFallbackCueMarker(UWorld* World, const FQinglanCueEvent& Cue)
	{
		if (!World)
		{
			return;
		}
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transient;
		AActor* Marker = World->SpawnActor<AActor>(AActor::StaticClass(),
			Cue.SourceTransform.GetLocation(), FRotator::ZeroRotator, SpawnParams);
		if (!Marker)
		{
			UE_LOG(LogQinglanCombat, Warning, TEXT("Cue placeholder spawn failed for stage %s"),
				*Cue.StageId.ToString());
			return;
		}

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(Marker);
		Marker->SetRootComponent(MeshComp);
		MeshComp->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere")));
		MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComp->SetGenerateOverlapEvents(false);
		MeshComp->SetWorldLocation(Cue.SourceTransform.GetLocation());
		MeshComp->SetWorldScale3D(FVector(0.4f));
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial")))
		{
			MeshComp->SetMaterial(0, Material);
		}
		MeshComp->RegisterComponent();
		Marker->SetLifeSpan(0.6f);
		UE_LOG(LogQinglanCombat, Warning,
			TEXT("Cue %s stage %s has no Niagara system; using placeholder marker (AUD-42 open)"),
			*Cue.TechniqueId.ToString(), *Cue.StageId.ToString());
	}
}

bool UQinglanTechniqueExecutors::Execute(const FQinglanTechniqueCastContext& Context)
{
	AActor* Caster = Context.Caster.Get();
	const UQinglanTechniqueDataAsset* Technique = Context.Technique;
	UWorld* World = Caster ? Caster->GetWorld() : nullptr;
	if (!Caster || !Technique || !World)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("Execute rejected: missing caster/technique/world"));
		return false;
	}

	// AUD-44: release pose (cosmetic only; never gates execution).
	PlayReleaseCastMontage(Context);

	switch (Technique->Targeting)
	{
	case EQinglanTargeting::SelfArc:
		ExecuteSelfArc(Context);
		return true;
	case EQinglanTargeting::Projectile:
		ExecuteProjectile(Context);
		return true;
	case EQinglanTargeting::SelfBarrier:
		ExecuteSelfBarrier(Context);
		return true;
	case EQinglanTargeting::SelfHeal:
		ExecuteSelfHeal(Context);
		return true;
	case EQinglanTargeting::GroundStrike:
		ExecuteGroundStrike(Context);
		return true;
	case EQinglanTargeting::SelfDash:
		ExecuteSelfDash(Context);
		return true;
	case EQinglanTargeting::SelfPulses:
		ExecuteSelfPulses(Context);
		return true;
	default:
		UE_LOG(LogQinglanCombat, Warning, TEXT("Execute: unsupported targeting mode %d"), static_cast<int32>(Technique->Targeting));
		return false;
	}
}

float UQinglanTechniqueExecutors::ComputeRawDamage(const FQinglanTechniqueCastContext& Context)
{
	const AActor* Caster = Context.Caster.Get();
	const UQinglanCombatComponent* Combat = Caster ? Caster->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
	const UQinglanTechniqueDataAsset* Technique = Context.Technique;
	if (!Combat || !Technique)
	{
		return 0.f;
	}
	// docs/02 §5: Raw = TechniquePower * Coefficient + FlatDamage. Tier-2 and
	// blessing raw-damage percent apply to the complete raw expression before
	// mitigation, exactly once (docs/03 §3-4).
	const float BaseRaw = Combat->GetTechniquePower() * Technique->PowerCoefficient + static_cast<float>(Technique->FlatDamage);
	const float Scaled = BaseRaw * Context.TierMultiplier * (1.f + Context.Blessings.TechniqueRawDamagePercent / 100.f);
	return FMath::Max(0.f, Scaled);
}

float UQinglanTechniqueExecutors::ComputeMagnitude(const FQinglanTechniqueCastContext& Context)
{
	// docs/03 §3: tier-2 grants +15% to damage/heal magnitude or barrier
	// amount; cast timing unchanged.
	return FMath::Max(0.f, (Context.Technique ? Context.Technique->Magnitude : 0.f) * Context.TierMultiplier);
}

void UQinglanTechniqueExecutors::ExecuteSelfArc(const FQinglanTechniqueCastContext& Context)
{
	// jade_sword_arc: 1,200 cm arc projectile (docs/02 §6). The pierce path is
	// the authoritative hit volume; the blade-sweep arc width is a cosmetic
	// cue parameter (ArcHalfAngleDeg, unspecified in docs - tunable).
	AActor* Caster = Context.Caster.Get();
	UWorld* World = Caster->GetWorld();

	AQinglanProjectile* Shot = FQinglanProjectilePool::Acquire(World);
	if (!Shot)
	{
		return;
	}
	const float Raw = ComputeRawDamage(Context);
	Shot->SetupLinear(Context.CastId, Context.Technique->TechniqueId, Context.Technique->Element,
		Caster, Context.AimDirection, ArcSpeedCmPerSec, ArcRangeCm, Raw,
		false /*bAppliesBurn*/, true /*bPiercing*/, 1.f /*multiplier already applied*/);
	Shot->SetCueContext(Context); // AUD-42: impact cues keep the technique identity

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.TargetPoint = Caster->GetActorLocation() + Context.AimDirection * ArcRangeCm;
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.spell.release");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteProjectile(const FQinglanTechniqueCastContext& Context)
{
	// ember_lance: single projectile; applies Burn (docs/02 §6-7).
	AActor* Caster = Context.Caster.Get();
	UWorld* World = Caster->GetWorld();

	AQinglanProjectile* Shot = FQinglanProjectilePool::Acquire(World);
	if (!Shot)
	{
		return;
	}
	const float Raw = ComputeRawDamage(Context);
	const float BurnDuration = 3.f + Context.Blessings.BurnDurationBonus; // docs/02 §7 + blessing bonus
	Shot->SetupLinear(Context.CastId, Context.Technique->TechniqueId, Context.Technique->Element,
		Caster, Context.AimDirection, ProjectileSpeedCmPerSec, ProjectileMaxRangeCm, Raw,
		true /*bAppliesBurn*/, false /*single target*/, 1.f, BurnDuration);
	Shot->SetCueContext(Context); // AUD-42: impact cues keep the technique identity

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.TargetPoint = Caster->GetActorLocation() + Context.AimDirection * ProjectileMaxRangeCm;
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.spell.release");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteSelfBarrier(const FQinglanTechniqueCastContext& Context)
{
	// flowing_ward: 25-pt barrier 5 s; stone_aegis: 35-pt barrier, no geometry
	// blocker (docs/02 §6). Neither spawns collision - barriers are point
	// shields absorbed before health, stronger replaces weaker, never stack
	// (code-contracts "Combat semantics").
	AActor* Caster = Context.Caster.Get();
	UQinglanCombatComponent* Combat = Caster->FindComponentByClass<UQinglanCombatComponent>();
	if (!Combat)
	{
		return;
	}
	const float Amount = ComputeMagnitude(Context) + Context.Blessings.BarrierPointsBonus;
	const bool bIsStoneAegis = Context.Technique->TechniqueId == FName("stone_aegis");
	const float Duration = bIsStoneAegis ? BarrierDurationStoneAegis : BarrierDurationFlowingWard;
	Combat->ApplyBarrier(Context.Technique->Element, Amount, Duration);

	// docs/02 §7: "Water removes Burn" - Flowing Ward (Water) cleanses one
	// Burn application from the caster when the barrier forms.
	if (Context.Technique->Element == EQinglanElement::Water && Combat->HasStatus(EQinglanStatus::Burn))
	{
		Combat->RemoveStatus(EQinglanStatus::Burn);
	}

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.barrier");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteSelfHeal(const FQinglanTechniqueCastContext& Context)
{
	// verdant_pulse: heal 20 over 4 s bounded by max health (docs/02 §6).
	AActor* Caster = Context.Caster.Get();
	UWorld* World = Caster->GetWorld();
	AQinglanProjectile* Effect = FQinglanProjectilePool::Acquire(World);
	if (!Effect)
	{
		return;
	}
	const float TotalHeal = ComputeMagnitude(Context); // tier-2 applies to heal magnitude, docs/03 §3
	// Duration rides the Range field (heal has no range); docs default 4 s.
	const float Duration = Context.Technique->Range > 0.f ? Context.Technique->Range : 4.f;
	Effect->SetupHealOverTime(Context.CastId, Context.Technique->TechniqueId, Caster, TotalHeal, Duration, 1.f);
	Effect->SetCueContext(Context);

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.heal");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteGroundStrike(const FQinglanTechniqueCastContext& Context)
{
	// thunder_seal: 300 cm radius ground strike at the aim point (docs/02 §6).
	// Conduct (Thunder vs Wet -> one 1.25x, consumes Wet) resolves inside the
	// victim combat component's reaction step (docs/02 §7; code-contracts).
	AActor* Caster = Context.Caster.Get();
	const float Radius = Context.Technique->Range > 0.f ? Context.Technique->Range : GroundStrikeRadiusCm;
	const float Raw = ComputeRawDamage(Context);

	TArray<AActor*> Victims;
	GatherVictims(Context, Context.AimPoint, Radius, Victims);

	int32 StrikeOrdinal = 1;
	for (AActor* Victim : Victims)
	{
		ApplyDamageToVictim(Context, Victim, Raw, StrikeOrdinal);
	}

	// Persistent ground outline is a warning-relevant cue: always published
	// regardless of quality tier (docs/02 §"trials" outline requirement;
	// blueprint §1.7 budgets never apply to warnings).
	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform.SetLocation(Context.AimPoint);
	Cue.TargetPoint = Context.AimPoint;
	Cue.Element = Context.Technique->Element;
	Cue.Intensity = Radius;
	Cue.StageId = FName("impact");
	Cue.FeatureId = FName("vfx.spell.impact");
	Cue.bWarningRelevant = true;
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteSelfDash(const FQinglanTechniqueCastContext& Context)
{
	// mist_step: enhanced dodge, 600 cm travel (docs/02 §6). Travel reuses the
	// character's dodge machinery (UQinglanDodgeComponent): constant-speed
	// capsule sweeps through the movement component stop at blockers, and the
	// authored i-frame window (0.10-0.28 s of 0.45 s) flows through the same
	// dodge-state observation the regular dodge's immunity uses (docs/02 §2).
	AActor* Caster = Context.Caster.Get();
	ACharacter* Character = Cast<ACharacter>(Caster);
	if (!Character)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("SelfDash rejected: caster is not a character"));
		return;
	}
	const float DashDistance = ComputeMagnitude(Context) > 0.f ? ComputeMagnitude(Context) : DashDistanceCm; // docs/02 §6
	const FVector HorizontalDirection = Context.AimDirection.GetSafeNormal2D().GetSafeNormal();
	if (HorizontalDirection.IsNearlyZero())
	{
		return;
	}

	AQinglanCharacter* QinglanCharacter = Cast<AQinglanCharacter>(Character);
	UQinglanDodgeComponent* Dodge = QinglanCharacter ? QinglanCharacter->FindComponentByClass<UQinglanDodgeComponent>() : nullptr;
	if (Dodge)
	{
		// The bounded root-motion override supplies dash speed without changing
		// ordinary locomotion settings, including when a start is rejected.
		if (!Dodge->StartDodge(HorizontalDirection, DashDistance, DashDurationSeconds,
				QinglanCharacter->GetDodgeIFrameStart(), QinglanCharacter->GetDodgeIFrameEnd()))
		{
			UE_LOG(LogQinglanCombat, Verbose, TEXT("SelfDash: movement rejected"));
			return;
		}
	}
	else
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("SelfDash: caster has no dodge component; travel skipped"));
		return;
	}

	// Water technique: cleanse one Burn application from self (docs/02 §7).
	UQinglanCombatComponent* Combat = Caster->FindComponentByClass<UQinglanCombatComponent>();
	if (Combat && Context.Technique->Element == EQinglanElement::Water && Combat->HasStatus(EQinglanStatus::Burn))
	{
		Combat->RemoveStatus(EQinglanStatus::Burn);
	}

	// mist_step is the "specified Water attack" that applies Wet (docs/02 §7):
	// the pooled Wet trail samples the owner's actual swept position every
	// tick with one deduplicated victim set, so enemies at the path's start,
	// middle, and end soak exactly once and wall-separated or unreachable
	// targets never do. The blessing WetDurationBonus folds in through
	// ApplyStatus' magnitude (reaction mods affect Conduct only).
	if (Context.Technique->Element == EQinglanElement::Water)
	{
		if (AQinglanProjectile* Trail = FQinglanProjectilePool::Acquire(Caster->GetWorld()))
		{
			Trail->SetupWetTrail(Context.CastId, Context.Technique->TechniqueId, Caster,
				DashWetRadiusCm, Context.Blessings.WetDurationBonus);
			Trail->SetCueContext(Context);
		}
	}

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.dash");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::ExecuteSelfPulses(const FQinglanTechniqueCastContext& Context)
{
	// orbiting_blades: six pulses, at most 2 targets per pulse (docs/02 §6).
	// The pooled effect actor drives the cadence with per-victim hit
	// ordinals, so a victim can be hit once per pulse but never twice in one
	// pulse (duplicate hit keys rejected downstream, docs/02 §3).
	AActor* Caster = Context.Caster.Get();
	UWorld* World = Caster->GetWorld();
	AQinglanProjectile* Effect = FQinglanProjectilePool::Acquire(World);
	if (!Effect)
	{
		return;
	}
	const float Raw = ComputeRawDamage(Context);
	const int32 PulseCount = Context.Technique->Magnitude > 0.f ? FMath::RoundToInt(Context.Technique->Magnitude) : OrbitPulseCount;
	Effect->SetupOrbitPulses(Context.CastId, Context.Technique->TechniqueId, Context.Technique->Element,
		Caster, Raw, PulseCount, OrbitPulseInterval, OrbitPulseRadiusCm, 2 /*max targets/pulse*/, 1.f);
	Effect->SetCueContext(Context);

	FQinglanCueEvent Cue;
	Cue.CastId = Context.CastId;
	Cue.SourceTransform = Caster->GetActorTransform();
	Cue.Element = Context.Technique->Element;
	Cue.StageId = FName("release");
	Cue.FeatureId = FName("vfx.pulses");
	PublishCue(Context, Cue);
}

void UQinglanTechniqueExecutors::GatherVictims(const FQinglanTechniqueCastContext& Context, const FVector& Center,
	float RadiusCm, TArray<AActor*>& OutVictims)
{
	AActor* Caster = Context.Caster.Get();
	UWorld* World = Caster ? Caster->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	FCollisionObjectQueryParams ObjectParams(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(QinglanGatherVictims), false, Caster);

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, ObjectParams,
		FCollisionShape::MakeSphere(RadiusCm), QueryParams);

	OutVictims.Reset(Overlaps.Num());
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Victim = Overlap.GetActor();
		if (!Victim || Victim == Caster)
		{
			continue;
		}
		const UQinglanCombatComponent* VictimCombat = Victim->FindComponentByClass<UQinglanCombatComponent>();
		if (!VictimCombat || !VictimCombat->IsAlive())
		{
			continue;
		}
		OutVictims.Add(Victim);
	}
	// Deterministic: nearest first, then stable name order (no RNG).
	// TArray::Sort wraps the predicate in TDereferenceWrapper, so the lambda
	// receives dereferenced elements (AActor&), not the stored pointers.
	OutVictims.Sort([Center](AActor& A, AActor& B)
	{
		const FVector LocA = A.GetActorLocation();
		const FVector LocB = B.GetActorLocation();
		const float DistA = FVector::DistSquared(LocA, Center);
		const float DistB = FVector::DistSquared(LocB, Center);
		if (!FMath::IsNearlyEqual(DistA, DistB))
		{
			return DistA < DistB;
		}
		return A.GetName() < B.GetName(); // stable name tiebreak (FString lexical)
	});
}

bool UQinglanTechniqueExecutors::ApplyDamageToVictim(const FQinglanTechniqueCastContext& Context, AActor* Victim,
	float RawMagnitude, int32 HitOrdinal)
{
	AActor* Caster = Context.Caster.Get();
	UQinglanCombatComponent* VictimCombat = Victim ? Victim->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
	if (!Caster || !Victim || !VictimCombat)
	{
		return false;
	}

	// One audited event per application; the victim component owns mitigation,
	// difficulty, reaction consumption (Conduct), barrier/health precedence,
	// and duplicate hit-key rejection (docs/02 §5, §7; code-contracts).
	FQinglanDamageEvent Event;
	Event.Attacker = Caster;
	Event.Target = Victim;
	Event.CastId = Context.CastId;
	Event.HitOrdinal = HitOrdinal;
	Event.TechniqueId = Context.Technique ? Context.Technique->TechniqueId : NAME_None;
	Event.Element = Context.Technique ? Context.Technique->Element : EQinglanElement::None;
	Event.RawMagnitude = RawMagnitude;
	Event.Timestamp = Caster->GetWorld() ? Caster->GetWorld()->GetTimeSeconds() : 0.f; // paused-game-time seconds
	return VictimCombat->ApplyDamage(Event);
}

void UQinglanTechniqueExecutors::PublishCue(const FQinglanTechniqueCastContext& Context, const FQinglanCueEvent& Cue, const UObject* WorldContextObject)
{
	// Cosmetic publication only - a missing, culled, or failed cue never
	// suppresses or cancels gameplay (code-contracts; blueprint §1.7).
	AActor* Caster = Context.Caster.Get();
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : (Caster ? Caster->GetWorld() : nullptr);
	if (!World)
	{
		return;
	}

	const FName TechniqueId = Context.Technique ? Context.Technique->TechniqueId : Cue.TechniqueId;
	UE_LOG(LogQinglanCombat, Verbose, TEXT("Cue %s stage=%s element=%d cast_serial=%d"),
		*TechniqueId.ToString(), *Cue.StageId.ToString(), static_cast<int32>(Cue.Element), Cue.CastId.Serial);

	if (!IsCueAllowed(Cue, World))
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Cue %s stage %s culled by the quality feature gate"),
			*TechniqueId.ToString(), *Cue.StageId.ToString());
		return;
	}
	UNiagaraSystem* System = ResolveStageCue(Context.Technique, Cue.StageId);
	if (System)
	{
		// UE 5.8: SpawnSystemAtWorld is SpawnSystemAtLocation (same argument order).
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, System, Cue.SourceTransform.GetLocation(),
			Cue.SourceTransform.GetRotation().Rotator(), FVector::OneVector, false /*bAutoDestroy*/,
			true /*bAutoActivate*/, ENCPoolMethod::AutoRelease, true /*bPreCullCheck*/);
		return;
	}

	// AUD-42: a cue must never fail silently. The placeholder marker is
	// gameplay-neutral, visible at every quality tier, and light-free so
	// low-flash mode stays compliant. Replaced when authored Niagara exists.
	FQinglanCueEvent PlaceholderCue = Cue;
	if (PlaceholderCue.TechniqueId.IsNone())
	{
		PlaceholderCue.TechniqueId = TechniqueId;
	}
	SpawnFallbackCueMarker(World, PlaceholderCue);
}
