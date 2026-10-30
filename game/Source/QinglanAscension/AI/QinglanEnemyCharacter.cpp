// Enemy character implementation: archetype init, FSM core, damage routing,
// telegraphs, pooling reset. FSM states are exactly EQinglanAIState; decision
// transitions run on the 5-10 Hz accumulator, attack timing runs every tick
// (docs/02 section 8; docs/05 section 5).
//
// The scaffold header is frozen: every FSM sub-step lives inside the declared
// member bodies as local lambdas (which keep protected-member access), and
// shared pure computations are free functions that use only public APIs.
#include "AI/QinglanEnemyCharacter.h"

// UQinglanStatusComponent ships in the Combat folder (the combat module's
// deliverable); the Abilities spelling stays as a tolerated fallback from the
// parallel-module handoff. Compile with or without it (the scaffold declares
// the pointer either way).
// FIXME-CONTRACT: keep the probe until the include location is contractual.
#if __has_include("Combat/QinglanStatusComponent.h")
#include "Combat/QinglanStatusComponent.h"
#define QINGLAN_HAS_STATUS_COMPONENT 1
#elif __has_include("Abilities/QinglanStatusComponent.h")
#include "Abilities/QinglanStatusComponent.h"
#define QINGLAN_HAS_STATUS_COMPONENT 1
#else
#define QINGLAN_HAS_STATUS_COMPONENT 0
#endif

#include "AI/QinglanAIController.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCatalog.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCharacterAnimInstance.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Core/QinglanAudioSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "QinglanAscension.h" // LogQinglanCombat / LogQinglan
#include "VFX/QinglanVFXCueComponent.h"

namespace
{
	// Authored values (docs give no numbers); doc-mandated behavior noted.
	constexpr float IdleBreakSeconds = 0.5f;          // idle beat before re-approach
	constexpr float PhaseTwoIdleBreakFactor = 0.6f;   // docs/02 section 9: bosses shorten idle breaks at 50% HP
	constexpr float MeleeConeHalfAngleDeg = 35.f;     // Stone Sentinel "ground wedge" arc
	constexpr float BossChargeSpeed = 1400.f;         // Ashen charging strike velocity (cm/s)
	constexpr float SpiritBoltSecondShotYawDeg = 4.f; // "two aimed spirit bolts" spread (docs/02 section 9)
	constexpr float SafeArcHalfWidthDeg = 30.f;       // Ashen ring shockwave safe gap (docs/02 section 9)
	constexpr float EnemyTechniquePowerBaseline = 20.f; // enemy side of AttackCoefficient math

	// AUD-41: Ashen phase-two ember volley, "intermittent embers outside
	// marked safe lanes" (docs/02 section 9). All values authored.
	constexpr float EmberVolleyIntervalSeconds = 6.f;  // "intermittent" cadence in phase two
	constexpr float EmberVolleyWarningSeconds = 0.9f;  // matches the boss's shortest telegraph (fire lance)
	constexpr float EmberVolleyDamage = 18.f;          // below every pattern's damage
	constexpr float EmberVolleyRangeCm = 1400.f;       // readable radial spread reach
	constexpr float EmberVolleySpeed = 1600.f;         // slow enough to read and enter the lane
	constexpr int32 EmberVolleySectorCount = 8;        // radial sectors; the lane swallows its sectors
	constexpr float EmberSafeLaneHalfWidthDeg = 30.f;  // mirrors the ring-shockwave gap width

	AQinglanAIController* GetQinglanAI(APawn* Pawn)
	{
		return Pawn ? Cast<AQinglanAIController>(Pawn->GetController()) : nullptr;
	}

	/** Raw = flat + coefficient * attacker power (mirrors technique math, docs/02 section 6). */
	float ComputeRawDamage(AQinglanEnemyCharacter* Attacker, AQinglanAIController* AI,
		const FQinglanBossPattern* Pattern)
	{
		const FQinglanArchetypeSpec* Spec = AI ? AI->GetSpec() : nullptr;
		const float Flat = Pattern ? Pattern->Damage : (Spec ? Spec->AttackDamage : 0.f);
		const float Coefficient = Spec ? Spec->AttackCoefficient : 0.f;
		const float Power = (Attacker && Attacker->EnemyCombat) ? Attacker->EnemyCombat->GetTechniquePower() : 20.f;
		return Flat + Coefficient * Power;
	}

	float WindupSeconds(AQinglanAIController* AI)
	{
		const FQinglanBossPattern* Pattern = AI ? AI->GetCurrentBossPattern() : nullptr;
		if (Pattern)
		{
			return Pattern->TelegraphDuration;
		}
		const FQinglanArchetypeSpec* Spec = AI ? AI->GetSpec() : nullptr;
		return Spec ? Spec->AttackWindup : QinglanAITunables::DecisionIntervalSeconds;
	}

	float ActiveSeconds(AQinglanAIController* AI)
	{
		const FQinglanArchetypeSpec* Spec = AI ? AI->GetSpec() : nullptr;
		return Spec ? Spec->AttackActive : 0.2f;
	}

	float RecoverSeconds(AQinglanAIController* AI)
	{
		const FQinglanArchetypeSpec* Spec = AI ? AI->GetSpec() : nullptr;
		return Spec ? Spec->AttackRecover : 0.5f;
	}

	float IdleBreakFor(AQinglanAIController* AI)
	{
		// Docs/02 section 9: bosses shorten idle breaks below 50% HP; telegraphs
		// and damage stay unchanged.
		return IdleBreakSeconds * ((AI && AI->IsPhaseTwo()) ? PhaseTwoIdleBreakFactor : 1.f);
	}

	bool HasTargetContactAlongPath(AQinglanEnemyCharacter* Enemy, AQinglanAIController* AI, const FVector& From)
	{
		ACharacter* Target = AI ? AI->GetTargetCharacter() : nullptr;
		UCapsuleComponent* Capsule = Enemy ? Enemy->GetCapsuleComponent() : nullptr;
		UWorld* World = Enemy ? Enemy->GetWorld() : nullptr;
		if (!World || !Capsule || !Target) { return false; }
		// AttackRange selects when a lunge may begin; it is not its contact
		// volume. Sweep the actual capsule path so sidestepping works and a
		// fast charge cannot tunnel past its victim between actor ticks.
		constexpr float ContactToleranceCm = 2.f;
		FCollisionQueryParams Query(SCENE_QUERY_STAT(QinglanChargeContact), false, Enemy);
		TArray<FHitResult> Hits;
		World->SweepMultiByObjectType(Hits, From, Enemy->GetActorLocation(), Capsule->GetComponentQuat(),
			FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeCapsule(
				Capsule->GetScaledCapsuleRadius() + ContactToleranceCm,
				Capsule->GetScaledCapsuleHalfHeight() + ContactToleranceCm), Query);
		return Hits.ContainsByPredicate([Target](const FHitResult& Hit) { return Hit.GetActor() == Target; });
	}

	bool IsTargetInCone(AQinglanAIController* AI, const FVector& Origin, const FVector& Forward,
		float RangeCm, float HalfAngleDeg)
	{
		ACharacter* Target = AI ? AI->GetTargetCharacter() : nullptr;
		if (!Target)
		{
			return false;
		}
		const FVector ToTarget = (Target->GetActorLocation() - Origin).GetSafeNormal2D();
		const float Dot = FVector::DotProduct(Forward.GetSafeNormal2D(), ToTarget);
		const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)));
		return FVector::Dist2D(Origin, Target->GetActorLocation()) <= RangeCm && AngleDeg <= HalfAngleDeg;
	}

	/** Enemy->player hit: audited damage + trailing status through the victim's combat component. */
	void ApplyHitToTarget(AQinglanEnemyCharacter* Attacker, AQinglanAIController* AI, float RawMagnitude)
	{
		AQinglanCharacter* PlayerVictim = AI ? Cast<AQinglanCharacter>(AI->GetTargetCharacter()) : nullptr;
		UQinglanCombatComponent* VictimCombat = PlayerVictim ? PlayerVictim->GetCombat() : nullptr;
		UWorld* World = Attacker ? Attacker->GetWorld() : nullptr;
		if (!Attacker || !AI || !VictimCombat || !World)
		{
			return;
		}

		FQinglanDamageEvent Event;
		Event.Attacker = Attacker;
		Event.Target = PlayerVictim;
		Event.CastId = AI->NextAttackCastId(); // globally-unique release identity (AUD-33)
		Event.HitOrdinal = 0;
		Event.TechniqueId = Attacker->GetEnemyId();
		if (const FQinglanArchetypeSpec* Spec = AI->GetSpec())
		{
			Event.Element = Spec->Element;
		}
		Event.RawMagnitude = RawMagnitude;
		Event.Timestamp = static_cast<float>(World->GetTimeSeconds());

		const bool bAccepted = VictimCombat->ApplyDamage(Event);

		// Status follows damage in the central precedence chain
		// (immunity -> barrier/health -> reaction -> status -> cue), and only an
		// accepted hit may apply it: dodged hits and duplicate keys never apply
		// or refresh Burn/Root (AUD-31).
		if (bAccepted)
		{
			if (const FQinglanArchetypeSpec* Spec = AI->GetSpec())
			{
				if (Spec->StatusOnHit != EQinglanStatus::None)
				{
					VictimCombat->ApplyStatus(Spec->StatusOnHit);
				}
			}
		}
	}

	// AUD-44: every rig gets a sibling slot graph beside its mesh (ABP_Human for
	// the player rig, ABP_Knight / ABP_Slime / ABP_Skeleton / ABP_Bat /
	// ABP_Dragon for enemy rigs), authored by QinglanAnimBootstrap. A compiled
	// anim graph is skeleton-specific, so the target skeleton is verified
	// before use; a missing or mismatched graph keeps the native class (the
	// native state machine still drives locomotion, montage slots go unused).
	UClass* ResolveEnemyAnimInstanceClass(const USkeletalMesh* Mesh)
	{
		const USkeleton* const MeshSkeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
		if (!MeshSkeleton)
		{
			return UQinglanCharacterAnimInstance::StaticClass();
		}

		// Derive the sibling ABP from the mesh package: SK_<Name> -> ABP_<Name>
		// in the same folder (e.g. .../Monsters/SK_Slime -> .../Monsters/ABP_Slime).
		const FString MeshPackage = Mesh->GetOutermost()->GetName();
		FString MeshBase = FPackageName::GetShortName(MeshPackage);
		MeshBase.RemoveFromStart(TEXT("SK_"), ESearchCase::CaseSensitive);
		const FString AbpName = FString::Printf(TEXT("ABP_%s"), *MeshBase);
		const FString AbpPackage = FPackageName::GetLongPackagePath(MeshPackage) / AbpName;
		const FString AbpClassPath = FString::Printf(TEXT("%s.%s_C"), *AbpPackage, *AbpName);

		if (FPackageName::DoesPackageExist(AbpPackage))
		{
			if (UClass* const AnimBlueprintClass =
					LoadClass<UQinglanCharacterAnimInstance>(nullptr, *AbpClassPath))
			{
				const UAnimBlueprintGeneratedClass* const GeneratedClass =
					Cast<UAnimBlueprintGeneratedClass>(AnimBlueprintClass);
				if (GeneratedClass && GeneratedClass->GetTargetSkeleton() == MeshSkeleton)
				{
					return AnimBlueprintClass;
				}
			}
			UE_LOG(LogQinglan, Warning,
				TEXT("Enemy anim graph %s exists but does not target %s; native class used"),
				*AbpClassPath, *MeshSkeleton->GetName());
		}
		return UQinglanCharacterAnimInstance::StaticClass();
	}
}

AQinglanEnemyCharacter::AQinglanEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	EnemyCombat = CreateDefaultSubobject<UQinglanCombatComponent>(TEXT("EnemyCombat"));
#if QINGLAN_HAS_STATUS_COMPONENT
	EnemyStatus = CreateDefaultSubobject<UQinglanStatusComponent>(TEXT("EnemyStatus"));
#endif
	// AUD-42: every enemy owns the cue subscriber for its own telegraphs.
	VFXCues = CreateDefaultSubobject<UQinglanVFXCueComponent>(TEXT("VFXCues"));

	// One shared humanoid rig (docs/02 section 8): archetypes differ by data,
	// so all enemies use the same base class + the explicit-FSM controller.
	AIControllerClass = AQinglanAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	bUseControllerRotationYaw = false;
	if (GetCharacterMovement())
	{
		GetCharacterMovement()->bOrientRotationToMovement = true;
		GetCharacterMovement()->bUseControllerDesiredRotation = true;
		GetCharacterMovement()->RotationRate = FRotator(0.f, 270.f, 0.f); // authored turn speed
	}
}

void AQinglanEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();
	// AUD-42: bind the presenter exactly once. AddUniqueDynamic keeps pooled
	// re-initialization idempotent; gameplay never checks whether this bound.
	OnTelegraph.AddUniqueDynamic(this, &AQinglanEnemyCharacter::HandleTelegraphPresented);
	OnEmberVolleyTelegraph.AddUniqueDynamic(this, &AQinglanEnemyCharacter::HandleEmberVolleyTelegraphPresented);
}

void AQinglanEnemyCharacter::HandleTelegraphPresented(AQinglanEnemyCharacter* Enemy, float Duration)
{
	if (Enemy != this || bDead)
	{
		return;
	}
	if (!VFXCues)
	{
		// Presentation failure is logged and never interrupts the attack.
		UE_LOG(LogQinglanCombat, Warning, TEXT("Enemy %s telegraph: no VFX cue component; warning still broadcast"),
			*GetEnemyId().ToString());
		return;
	}

	EQinglanElement Element = EQinglanElement::None;
	float Radius = 180.f;
	const FQinglanArchetypeSpec* Spec = QinglanEnemyCatalog::FindSpec(GetEnemyId());
	if (Spec)
	{
		Element = Spec->Element;
		Radius = Spec->GroundRingRadius > 0.f
			? Spec->GroundRingRadius
			: FMath::Max(Spec->AttackRange, 180.f);
	}
	// Component minimum warning is 0.9 s (docs/06); a shorter authored windup
	// keeps its marker slightly longer than the mechanic, which is harmless.
	const float MarkerSeconds = FMath::Max(Duration, 0.9f);
	const AQinglanAIController* AI = GetQinglanAI(this);
	const FQinglanBossPattern* Pattern = AI ? AI->GetCurrentBossPattern() : nullptr;
	const EQinglanAttackShape Shape = Pattern ? QinglanEnemyCatalog::ShapeOfPattern(*Pattern)
		: (Spec ? Spec->AttackShape : EQinglanAttackShape::MeleeArc);
	const auto PresentRing = [&](const FVector& Center, float RingRadius)
	{
		VFXCues->PlayTelegraph(Center, RingRadius, MarkerSeconds, Element);
		FTransform Source;
		Source.SetLocation(Center);
		VFXCues->PlayCue(QinglanVFXCues::TelegraphRing, Source, Center, Element, 1.f);
	};
	// The release tests the positions frozen at windup start. Drawing under
	// the caster instead made roots/volley damage land outside the warning.
	if (AI && Shape == EQinglanAttackShape::MultiGroundStrikes)
	{
		const float StrikeRadius = Spec ? Spec->GroundRingRadius : 300.f;
		for (const FVector& Center : AI->GetPendingStrikeCenters())
		{
			PresentRing(Center, StrikeRadius);
		}
	}
	else if (AI && (Shape == EQinglanAttackShape::GroundRing || Shape == EQinglanAttackShape::GroundRingGaps))
	{
		const float Range = Pattern ? Pattern->Range : (Spec ? Spec->AttackRange : 150.f);
		const float RingRadius = Shape == EQinglanAttackShape::GroundRingGaps && Pattern && Pattern->Range > 0.f
			? Pattern->Range : (Spec ? Spec->GroundRingRadius : Range);
		PresentRing(AI->GetGroundMarkerCenter(), RingRadius);
		if (Shape == EQinglanAttackShape::GroundRingGaps)
		{
			VFXCues->PlaySafeLane(AI->GetGroundMarkerCenter(), RingRadius, AI->GetSafeArcCenterDegrees(),
				SafeArcHalfWidthDeg, MarkerSeconds, Element);
		}
	}
	else
	{
		PresentRing(GetActorLocation(), Radius);
	}
	// Placeholder note (AUD-42 open): center disc/ring is engine-shape geometry,
	// not authored Niagara; audio for the warning is a named SFX key only.
}

void AQinglanEnemyCharacter::HandleEmberVolleyTelegraphPresented(AQinglanEnemyCharacter* Enemy,
	float SafeLaneDegrees, float WarningSeconds)
{
	if (Enemy != this || bDead)
	{
		return;
	}
	if (!VFXCues)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("Enemy %s ember telegraph: no VFX cue component; warning still broadcast"),
			*GetEnemyId().ToString());
		return;
	}

	EQinglanElement Element = EQinglanElement::None;
	if (const FQinglanArchetypeSpec* Spec = QinglanEnemyCatalog::FindSpec(GetEnemyId()))
	{
		Element = Spec->Element;
	}
	// AUD-42: visible ground center + safe arc. The safe-lane value is the same
	// one the mechanic uses; drawing it never changes the mechanic.
	const float MarkerSeconds = FMath::Max(WarningSeconds, 0.9f);
	VFXCues->PlayTelegraph(GetActorLocation(), EmberVolleyRangeCm, MarkerSeconds, Element);
	VFXCues->PlaySafeLane(GetActorLocation(), EmberVolleyRangeCm, SafeLaneDegrees,
		EmberSafeLaneHalfWidthDeg, MarkerSeconds, Element);
	FTransform Source;
	Source.SetLocation(GetActorLocation());
	VFXCues->PlayCue(QinglanVFXCues::BossGroundMarker, Source, GetActorLocation(), Element, 1.f);
}

void AQinglanEnemyCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (AIState == EQinglanAIState::Dead)
	{
		return;
	}

	AQinglanAIController* AI = GetQinglanAI(this);
	if (AI && AI->IsPortalPending())
	{
		return; // still behind the spawn portal warning
	}

	// Paused-game-time semantics: accumulate DeltaTime, never wall clock.
	StateTimer += DeltaSeconds;
	UpdateAttack(DeltaSeconds);
	UpdateDecisions(DeltaSeconds);
	UpdatePhaseTwoEmbers(DeltaSeconds);
	UpdateAnimationActionState();
}

FName AQinglanEnemyCharacter::GetEnemyId() const
{
	return EnemyData != nullptr ? EnemyData->EnemyId : NAME_None;
}

bool AQinglanEnemyCharacter::IsAlive() const
{
	return !bDead && EnemyCombat && EnemyCombat->IsAlive() && AIState != EQinglanAIState::Dead;
}

bool AQinglanEnemyCharacter::IsBoss() const
{
	// The data asset is the source of truth; the tag is the gameplay-visible
	// mirror InitializeFromData installs for status/Root/Shock consumers.
	return Cast<UQinglanBossDataAsset>(EnemyData.Get()) != nullptr
		|| ActorHasTag(FName(QinglanCombatTunables::QinglanBossTag));
}

void AQinglanEnemyCharacter::HandleDamageResolved(const FQinglanResolvedDamage& Result)
{
	// AUD-28: the combat authority's accepted-hit notification drives every
	// damage reaction. Direct technique damage, projectiles, ground areas, and
	// Burn ticks all arrive here no matter which delivery layer resolved them.
	if (bDead || Result.Victim.Get() != this)
	{
		return;
	}

	if (Result.bLethal)
	{
		Die();
		return;
	}

	AQinglanAIController* AI = GetQinglanAI(this);
	if (AI && AI->CanBeStaggeredBy(Result.AppliedMagnitude))
	{
		EnterStagger();
	}
}

void AQinglanEnemyCharacter::EnterStagger()
{
	AIState = EQinglanAIState::Stagger;
	StateTimer = 0.f;
	TelegraphTimer = 0.f;
	bAttackCommitted = false;
	bProximityActivePhase = false;
	StopAnimMontage();
	if (VFXCues) { VFXCues->ResetForPool(); }
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->ClearAccumulatedForces();
		Movement->StopMovementImmediately();
	}

	AQinglanAIController* AI = GetQinglanAI(this);
	if (AI)
	{
		AI->NotifyStaggerBegan(); // releases held slots; immunity window prevents stun locks (docs/02 section 8)
	}
	if (EnemyData != nullptr)
	{
		if (UAnimMontage* Montage = EnemyData->HitReactMontage.LoadSynchronous())
		{
			PlayAnimMontage(Montage);
		}
	}
	UpdateAnimationActionState();
}

void AQinglanEnemyCharacter::Die()
{
	// Exactly-once death guard: pool reuse re-arms through InitializeFromData.
	if (bDead)
	{
		return;
	}
	bDead = true;

	// Death event drives the encounter director's completion check.
	AIState = EQinglanAIState::Dead;
	StateTimer = 0.f;
	TelegraphTimer = 0.f;
	bAttackCommitted = false;
	bProximityActivePhase = false;
	bEmberTelegraphActive = false;
	UpdateAnimationActionState(); // AUD-44: Death freezes locomotion selection
	StopAnimMontage();
	if (VFXCues)
	{
		VFXCues->ResetForPool(); // death cancels every unreleased warning
	}

	if (GetCharacterMovement())
	{
		GetCharacterMovement()->ClearAccumulatedForces();
		GetCharacterMovement()->StopMovementImmediately();
		GetCharacterMovement()->DisableMovement();
	}
	if (GetCapsuleComponent())
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (EnemyData != nullptr)
	{
		// Death cue placeholder (docs/05 section 3); null asset degrades safely.
		if (UNiagaraSystem* DeathCue = EnemyData->DeathCue.LoadSynchronous())
		{
			UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, DeathCue, GetActorLocation());
		}
	}

	AQinglanAIController* AI = GetQinglanAI(this);
	if (AI)
	{
		AI->NotifyDeathBegan(); // stops movement, releases held slots immediately
		if (AQinglanEncounterDirector* Director = AI->GetDirector())
		{
			Director->NotifyEnemyDied(this);
		}
	}

	OnDied.Broadcast(this, GetEnemyId());
}

float AQinglanEnemyCharacter::TakeDamage(float Damage, const FDamageEvent& DamageEvent,
	AController* EventInstigator, AActor* DamageCauser)
{
	if (!IsAlive() || !EnemyCombat || !GetWorld() || Damage <= 0.f)
	{
		return 0.f;
	}

	// Single audited damage path: everything routes through UQinglanCombatComponent
	// (code-contracts "Combat semantics"). Primary attacker systems call
	// ApplyDamage directly; this engine path covers native damage sources.
	// Stagger/death are no longer decided here: the resolved-result delegate
	// (HandleDamageResolved) fires inside ApplyDamage for accepted hits
	// (AUD-28). The serial keys repeated engine hits from one causer in one
	// frame so the duplicate-hit contract still holds.
	// FIXME-CONTRACT: ApplyDamage() returns bool only, so the final magnitude
	// is not available here; the return below is an upper bound.
	FQinglanDamageEvent Request;
	Request.Attacker = DamageCauser ? DamageCauser
		: (EventInstigator ? EventInstigator->GetPawn() : nullptr);
	Request.Target = this;
	Request.CastId.Serial = DamageCauser ? static_cast<int32>(DamageCauser->GetUniqueID()) : 0;
	Request.CastId.FrameNumber = static_cast<int32>(GFrameNumber);
	Request.HitOrdinal = 0;
	Request.TechniqueId = GetEnemyId();
	Request.Element = EQinglanElement::None; // engine-routed damage carries no element yet
	Request.RawMagnitude = Damage;
	Request.Timestamp = static_cast<float>(GetWorld()->GetTimeSeconds());

	const bool bChanged = EnemyCombat->ApplyDamage(Request);
	return bChanged ? Damage : 0.f;
}

void AQinglanEnemyCharacter::InitializeFromData(UQinglanEnemyDataAsset* InData)
{
	if (!InData || !EnemyCombat)
	{
		return;
	}
	EnemyData = InData;

	// AUD-28: subscribe to the combat authority's resolved-result delegate;
	// every accepted hit (direct, projectile, area, Burn tick) drives
	// stagger/death from here. AddUniqueDynamic keeps pool reuse idempotent.
	EnemyCombat->OnDamageResultResolved.AddUniqueDynamic(this, &AQinglanEnemyCharacter::HandleDamageResolved);

	// AUD-42: defensive re-bind for placed/pooled actors whose BeginPlay binding
	// may have run before this data init; AddUniqueDynamic is idempotent.
	OnTelegraph.AddUniqueDynamic(this, &AQinglanEnemyCharacter::HandleTelegraphPresented);
	OnEmberVolleyTelegraph.AddUniqueDynamic(this, &AQinglanEnemyCharacter::HandleEmberVolleyTelegraphPresented);

	// AUD-36: authoritative boss identity. The catalog marks bosses via
	// UQinglanBossDataAsset (Spec.bBoss); the gameplay tag carries that identity
	// to the Root authority (downgrade to slow), Shock exclusion, and status
	// semantics (docs/02 section 7). ResetForPool clears Tags; re-init re-adds.
	if (Cast<UQinglanBossDataAsset>(InData))
	{
		Tags.AddUnique(FName(QinglanCombatTunables::QinglanBossTag));
	}

	// Presentation wiring (mesh/pose conventions from the enemy catalog);
	// every step null-safe so tests without imported content still run.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		if (USkeletalMesh* SkelMesh = InData->Mesh.LoadSynchronous())
		{
			MeshComp->SetSkeletalMesh(SkelMesh);
			// Pivot the rig onto the capsule: root at capsule base, facing
			// the movement forward axis (Quaternius exports face +Y).
			// The FBX rigs import ~3x human height, so normalize the component
			// scale to the capsule (same contract as the player pawn).
			const float CapsuleHalfHeight = GetCapsuleComponent() ? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 88.f;
			const FBoxSphereBounds MeshBounds = SkelMesh->GetBounds();
			const float MeshHalfHeight = FMath::Max(1.f, static_cast<float>(MeshBounds.BoxExtent.Z));
			const float MeshScale = CapsuleHalfHeight / MeshHalfHeight;
			MeshComp->SetRelativeScale3D(FVector(MeshScale));
			const float MeshMinZ = static_cast<float>(MeshBounds.Origin.Z - MeshBounds.BoxExtent.Z);
			MeshComp->SetRelativeLocation(FVector(0.f, 0.f, -CapsuleHalfHeight - MeshMinZ * MeshScale));
			MeshComp->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));

			// AUD-44: the shared native anim instance replaces the single-node
			// idle. Enemy archetypes carry their own take (different Quaternius
			// skeletons), so pass it as the idle override; Walk/Run fall back to
			// that idle until authored takes exist. If instance creation fails
			// (e.g. an unregistered pooled mesh during tests), keep the old
			// single-node idle path as the visible fallback.
			UAnimSequence* const Idle = InData->IdleAnimation.LoadSynchronous();
			MeshComp->SetAnimInstanceClass(ResolveEnemyAnimInstanceClass(SkelMesh));
			if (UQinglanCharacterAnimInstance* const AnimInstance =
					Cast<UQinglanCharacterAnimInstance>(MeshComp->GetAnimInstance()))
			{
				if (Idle)
				{
					AnimInstance->SetLocomotionAnimations(Idle, /*InWalk=*/nullptr, /*InRun=*/nullptr);
				}
				AnimInstance->SetActionState(EQinglanAnimActionState::Idle);
			}
			else if (Idle)
			{
				MeshComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
				MeshComp->SetAnimation(Idle);
				MeshComp->PlayAnimation(Idle, /*bLooping=*/true);
			}
		}
	}

	// Fresh attributes from the archetype (HP/Defense per docs/02 sections 8-9).
	FQinglanAttributes Base;
	Base.Health = InData->Health;
	Base.MaxHealth = InData->Health;
	Base.SpiritualEnergy = 0.f;
	Base.MaxSpiritualEnergy = 0.f;
	Base.Stamina = 0.f;
	Base.MaxStamina = 0.f;
	Base.TechniquePower = EnemyTechniquePowerBaseline;
	Base.Defense = InData->Defense;
	EnemyCombat->SetDerivedAttributes(Base);

	if (GetCharacterMovement())
	{
		GetCharacterMovement()->MaxWalkSpeed = InData->MoveSpeed;
	}

	AIState = EQinglanAIState::Idle;
	StateTimer = 0.f;
	TelegraphTimer = 0.f;
	DecisionAccumulator = 0.f;
	bAttackCommitted = false;
	bProximityActivePhase = false;
	bDead = false;
	PreviousProximityLocation = GetActorLocation();
	bPhaseTwoEmbersArmed = false;
	bEmberTelegraphActive = false;
	EmberVolleyTimer = 0.f;
	EmberTelegraphRemaining = 0.f;
	EmberSafeLaneDeg = 0.f;

	if (AQinglanAIController* AI = GetQinglanAI(this))
	{
		AI->OnEnemyDataInitialized(InData);
	}
}

void AQinglanEnemyCharacter::ResetForPool()
{
	// Pooling contract (docs/05 section 5): clears timers, victims, owner,
	// tags, collision, and remaining lifetime.
	StopAnimMontage();
	if (VFXCues)
	{
		VFXCues->ResetForPool();
	}
	AIState = EQinglanAIState::Idle;
	StateTimer = 0.f;
	TelegraphTimer = 0.f;
	DecisionAccumulator = 0.f;
	bAttackCommitted = false;
	bProximityActivePhase = false;
	bDead = false;
	PreviousProximityLocation = GetActorLocation();
	bPhaseTwoEmbersArmed = false;
	bEmberTelegraphActive = false;
	EmberVolleyTimer = 0.f;
	EmberTelegraphRemaining = 0.f;
	EmberSafeLaneDeg = 0.f;
	EnemyData = nullptr;
	UpdateAnimationActionState(); // AIState is Idle again: resume native locomotion
	if (EnemyCombat)
	{
		EnemyCombat->ResetForPool(); // stale hit keys could reject fresh hits
	}

	Tags.Empty();
	if (GetCapsuleComponent())
	{
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SetActorEnableCollision(false);
	SetActorHiddenInGame(true);
	SetActorTickEnabled(false);

	if (GetCharacterMovement())
	{
		GetCharacterMovement()->ClearAccumulatedForces();
		GetCharacterMovement()->StopMovementImmediately();
		GetCharacterMovement()->DisableMovement();
	}

	// Clear lingering statuses (best effort: the combat API exposes
	// RemoveStatus per status rather than a bulk clear).
	if (EnemyCombat)
	{
		EnemyCombat->RemoveStatus(EQinglanStatus::Wet);
		EnemyCombat->RemoveStatus(EQinglanStatus::Burn);
		EnemyCombat->RemoveStatus(EQinglanStatus::Root);
		EnemyCombat->RemoveStatus(EQinglanStatus::Conduct);
	}

	if (AQinglanAIController* AI = GetQinglanAI(this))
	{
		AI->ResetForPool();
	}
}

void AQinglanEnemyCharacter::UpdateDecisions(float DeltaSeconds)
{
	// Decision tick at 5-10 Hz (docs/02 section 8).
	DecisionAccumulator += DeltaSeconds;
	if (DecisionAccumulator < QinglanAITunables::DecisionIntervalSeconds)
	{
		return;
	}
	DecisionAccumulator = 0.f;

	AQinglanAIController* AI = GetQinglanAI(this);

	// --- Shared windup entry (lambdas keep protected-member access) ---
	auto BeginWindup = [this, AI]()
	{
		if (!AI || EnemyData == nullptr)
		{
			return;
		}
		// Bosses pick a weighted pattern per cycle from the room's seeded stream.
		const UQinglanBossDataAsset* BossData = Cast<UQinglanBossDataAsset>(EnemyData.Get());
		const FQinglanBossPattern* Pattern = BossData ? AI->PickBossPattern(BossData) : nullptr;
		AI->SetCurrentBossPattern(Pattern);

		const float Windup = Pattern ? Pattern->TelegraphDuration : EnemyData->AttackWindup;
		AIState = EQinglanAIState::Windup;
		StateTimer = 0.f;
		TelegraphTimer = Windup;
		bAttackCommitted = false;
		bProximityActivePhase = false;

		AI->NotifyWindupBegan(Windup, Pattern);
		if (UAnimMontage* Montage = EnemyData->AttackMontage.LoadSynchronous())
		{
			PlayAnimMontage(Montage);
		}
		// NotifyWindupBegan owns the single audio warning. Presentation keeps
		// the telegraph silhouette at Low quality (docs/06 section 6).
		OnTelegraph.Broadcast(this, Windup);
	};

	auto AbortWindup = [this, AI]()
	{
		// Interrupted pre-release: nothing was consumed (code-contracts).
		AIState = EQinglanAIState::Recover;
		StateTimer = 0.f;
		TelegraphTimer = 0.f;
		bAttackCommitted = false;
		bProximityActivePhase = false;
		if (AI)
		{
			AI->ReleaseHeldSlots();
			AI->ResetCombo();
			AI->SetCurrentBossPattern(nullptr);
		}
	};

	switch (AIState)
	{
	case EQinglanAIState::Idle:
	{
		if (AI && AI->HasTarget() && StateTimer >= IdleBreakFor(AI))
		{
			AIState = EQinglanAIState::Acquire;
			StateTimer = 0.f;
		}
		break;
	}

	case EQinglanAIState::Acquire:
	{
		if (!AI || !AI->HasTarget())
		{
			AIState = EQinglanAIState::Idle;
			StateTimer = 0.f;
		}
		else if (StateTimer >= QinglanAITunables::AcquireHoldSeconds)
		{
			AIState = EQinglanAIState::Approach;
			StateTimer = 0.f;
		}
		break;
	}

	case EQinglanAIState::Approach:
	{
		if (!AI || !AI->HasTarget())
		{
			AIState = EQinglanAIState::Idle;
			StateTimer = 0.f;
			break;
		}
		if (!AI->IsInAttackRange())
		{
			break;
		}
		// Shared attack arbitration: melee windup slots (<= 3 simultaneous)
		// or the separate ranged budget (docs/05 section 5).
		const bool bRanged = EnemyData != nullptr && EnemyData->bRanged;
		const bool bGranted = bRanged ? AI->TryAcquireRangedAttackSlot()
			: AI->TryAcquireMeleeWindupSlot();
		if (bGranted)
		{
			BeginWindup();
		}
		break;
	}

	case EQinglanAIState::Windup:
	{
		// Target lost mid-windup: un-committed attacks abort cleanly.
		if (AI && !AI->HasTarget() && !bAttackCommitted)
		{
			AbortWindup();
		}
		break;
	}

	case EQinglanAIState::Attack:
	case EQinglanAIState::Recover:
		break; // per-frame transitions live in UpdateAttack

	case EQinglanAIState::Stagger:
		if (StateTimer >= QinglanAITunables::StaggerSeconds)
		{
			AIState = EQinglanAIState::Idle;
			StateTimer = 0.f;
		}
		break;

	case EQinglanAIState::Dead:
	default:
		break;
	}
}

void AQinglanEnemyCharacter::UpdateAttack(float DeltaSeconds)
{
	AQinglanAIController* AI = GetQinglanAI(this);

	// --- Release: commit the attack at the notify (code-contracts) ---
	auto CommitAttack = [this, AI]()
	{
		if (!AI)
		{
			return;
		}
		// Interrupted windups consumed nothing; commit happens exactly once
		// per cycle here.
		bAttackCommitted = true;

		const FQinglanArchetypeSpec* Spec = AI->GetSpec();
		const FQinglanBossPattern* Pattern = AI->GetCurrentBossPattern();
		const EQinglanAttackShape Shape = Pattern
			? QinglanEnemyCatalog::ShapeOfPattern(*Pattern)
			: (Spec ? Spec->AttackShape : EQinglanAttackShape::MeleeArc);
		const float Range = Pattern ? Pattern->Range
			: (Spec ? Spec->AttackRange : 150.f);
		const float Raw = ComputeRawDamage(this, AI, Pattern);
		const FName CueId = (Pattern && !Pattern->CueIds.IsEmpty())
			? Pattern->CueIds[0]
			: (Spec ? Spec->TelegraphCueId : NAME_None);

		// AUD-29: only the authored proximity shapes (wolf lunge, boss charge)
		// keep hitting through the active window; every release-shaped attack
		// resolves exclusively at this commit instant.
		const bool bLungeShape = Shape == EQinglanAttackShape::MeleeArc && Spec && Spec->LungeSpeed > 0.f;
		bProximityActivePhase = bLungeShape || Shape == EQinglanAttackShape::ChargeDash;
		PreviousProximityLocation = GetActorLocation();

		switch (Shape)
		{
		case EQinglanAttackShape::MeleeArc:
			if (Spec && Spec->LungeSpeed > 0.f)
			{
				// Spirit Wolf: fixed-direction lunge; the hit lands on
				// proximity during the active window ("dodge sideways").
				AI->NotifyAttackCommitted(Spec->LungeSpeed);
			}
			else if (AI->IsInAttackRange())
			{
				ApplyHitToTarget(this, AI, Raw);
			}
			break;

		case EQinglanAttackShape::MeleeCone:
			// Stone Sentinel wedge / boss sweeping blade: "move behind".
			if (IsTargetInCone(AI, GetActorLocation(), GetActorForwardVector(),
				Range, MeleeConeHalfAngleDeg))
			{
				ApplyHitToTarget(this, AI, Raw);
			}
			break;

		case EQinglanAttackShape::ChargeDash:
			// Ashen charging strike: dash now; proximity hit during active.
			AI->NotifyAttackCommitted(BossChargeSpeed);
			break;

		case EQinglanAttackShape::AimedProjectile:
		{
			AI->SpawnPooledProjectileAtTarget(Raw, Range, GetEnemyId(), CueId);
			if (Pattern && Pattern->PatternId == QinglanPatternIds::SpiritBolts())
			{
				// "Two aimed spirit bolts" (docs/02 section 9): slight spread
				// so each carries its own hit key.
				AI->SpawnPooledProjectileAtTarget(Raw, Range, GetEnemyId(), CueId,
					SpiritBoltSecondShotYawDeg);
			}
			break;
		}

		case EQinglanAttackShape::GroundRing:
			// Delayed roots: leave the marked area or be caught (docs/02 section 8).
			if (AI->IsTargetInsideGroundRing(Spec ? Spec->GroundRingRadius : Range))
			{
				ApplyHitToTarget(this, AI, Raw);
			}
			break;

		case EQinglanAttackShape::GroundRingGaps:
		{
			// Ring shockwave: safe gaps absorb the hit (docs/02 section 9).
			const float RingRadius = (Pattern && Pattern->Range > 0.f) ? Pattern->Range
				: (Spec ? Spec->GroundRingRadius : Range);
			if (AI->IsTargetInsideGroundRing(RingRadius) &&
				!AI->IsTargetOutsideSafeArc(SafeArcHalfWidthDeg)) // in the gap = safe
			{
				ApplyHitToTarget(this, AI, Raw);
			}
			break;
		}

		case EQinglanAttackShape::MultiGroundStrikes:
		{
			// Broken Guardian storm volley: persistent outlines recorded at
			// windup start (docs/02 section 9).
			const float StrikeRadius = Spec ? Spec->GroundRingRadius : 300.f;
			bool bCaught = false;
			ACharacter* Target = AI->GetTargetCharacter();
			for (const FVector& Center : AI->GetPendingStrikeCenters())
			{
				if (Target && FVector::Dist2D(Target->GetActorLocation(), Center) <= StrikeRadius)
				{
					bCaught = true;
					break;
				}
			}
			if (bCaught)
			{
				ApplyHitToTarget(this, AI, Raw);
			}
			break;
		}

		default:
			break;
		}

		// AUD-29: release-shaped commits spend the attack's one proximity hit
		// whether they connected or missed, so a missed circle/cone/gap/projection
		// can never hit later through the active-window range branch.
		if (!bProximityActivePhase)
		{
			AI->ConsumeAttackHit();
		}

		AIState = EQinglanAIState::Attack;
		StateTimer = 0.f;
	};

	// --- Attack end: slot/budget release, then recover ---
	auto EnterRecover = [this, AI]()
	{
		AIState = EQinglanAIState::Recover;
		StateTimer = 0.f;
		if (AI)
		{
			// "Budget release on attack end" (task contract; docs/05 section 5).
			AI->NotifyAttackCycleEnded();
		}
	};

	switch (AIState)
	{
	case EQinglanAIState::Windup:
	{
		TelegraphTimer = FMath::Max(0.f, TelegraphTimer - DeltaSeconds);
		if (StateTimer >= WindupSeconds(AI))
		{
			CommitAttack();
		}
		break;
	}

	case EQinglanAIState::Attack:
	{
		// AUD-29: only authored proximity shapes (lunge/charge) may hit through
		// this range branch, once during the active window; release-shaped
		// attacks resolved at commit and spent their hit flag there.
		if (AI && bProximityActivePhase && HasTargetContactAlongPath(this, AI, PreviousProximityLocation)
			&& AI->ConsumeAttackHit())
		{
			ApplyHitToTarget(this, AI, ComputeRawDamage(this, AI, AI->GetCurrentBossPattern()));
		}
		PreviousProximityLocation = GetActorLocation();
		if (StateTimer >= ActiveSeconds(AI))
		{
			EnterRecover();
		}
		break;
	}

	case EQinglanAIState::Recover:
	{
		if (StateTimer >= RecoverSeconds(AI))
		{
			// Combo finish: "two sword hits then retreats" (docs/02 section 8).
			AIState = EQinglanAIState::Idle;
			StateTimer = 0.f;
			if (AI)
			{
				AI->SetCurrentBossPattern(nullptr);
				const FQinglanArchetypeSpec* Spec = AI->GetSpec();
				const int32 ComboCount = Spec ? FMath::Max(1, Spec->ComboCount) : 1;
				if (AI->GetComboIndex() >= ComboCount)
				{
					AI->ResetCombo();
					if (Spec && Spec->bRetreatAfterCombo && Spec->RetreatSeconds > 0.f)
					{
						AI->NotifyRetreatBegan(Spec->RetreatSeconds);
					}
				}
			}
		}
		break;
	}

	default:
		break;
	}
}

void AQinglanEnemyCharacter::UpdatePhaseTwoEmbers(float DeltaSeconds)
{
	// AUD-41: Ashen Guardian phase two - "At 50%, add intermittent embers
	// outside marked safe lanes" (docs/02 section 9). The volley is a radial
	// ember spread with one deterministic safe lane; the warning uses the same
	// telegraph broadcast/audio path as regular windups, so the minimum
	// warning time (0.9 s, the boss's shortest telegraph) and quality tiers
	// are unchanged. Mechanic is logical (no visuals required to function);
	// presentation subscribes to OnTelegraph as for any windup.
	if (bDead)
	{
		return;
	}
	AQinglanAIController* AI = GetQinglanAI(this);
	if (!AI || EnemyData == nullptr
		|| Cast<UQinglanBossDataAsset>(EnemyData.Get()) == nullptr // only a boss archetype owns this mechanic (AUD-41)
		|| EnemyData->EnemyId != FName(TEXT("ashen_guardian")))
	{
		return;
	}

	if (!bPhaseTwoEmbersArmed)
	{
		// Crossing 50% HP arms the mechanic exactly once per life; pool reuse
		// re-arms through InitializeFromData.
		if (AI->IsPhaseTwo())
		{
			bPhaseTwoEmbersArmed = true;
			EmberVolleyTimer = EmberVolleyIntervalSeconds;
		}
		return;
	}

	if (bEmberTelegraphActive)
	{
		EmberTelegraphRemaining -= DeltaSeconds; // paused-game-time accumulation
		if (EmberTelegraphRemaining > 0.f)
		{
			return;
		}
		bEmberTelegraphActive = false;

		// Launch: radial sectors minus the safe lane; each ember carries its
		// own globally-unique cast id, so lane reading is the only dodge
		// requirement and duplicate contacts still resolve once.
		const float BaseDeg = AI->GetBehaviorStream().FRand() * 360.f;
		const float SectorDeg = 360.f / EmberVolleySectorCount;
		for (int32 Index = 0; Index < EmberVolleySectorCount; ++Index)
		{
			const float ShotDeg = BaseDeg + Index * SectorDeg;
			if (FMath::Abs(FMath::FindDeltaAngleDegrees(ShotDeg, EmberSafeLaneDeg)) <=
				EmberSafeLaneHalfWidthDeg + SectorDeg * 0.5f)
			{
				continue; // safe lane sector never fires
			}
			const float Rad = FMath::DegreesToRadians(ShotDeg);
			AI->SpawnPooledProjectileInDirection(EmberVolleyDamage, EmberVolleyRangeCm,
				GetEnemyId(), FName(TEXT("cue.boss.ashen.ember")),
				FVector(FMath::Cos(Rad), FMath::Sin(Rad), 0.f), EmberVolleySpeed);
		}
		return;
	}

	EmberVolleyTimer -= DeltaSeconds; // paused-game-time accumulation
	if (EmberVolleyTimer <= 0.f && AIState == EQinglanAIState::Idle)
	{
		// Intermittent cadence, started from idle so embers never stack onto a
		// mid-cycle pattern release.
		EmberVolleyTimer = EmberVolleyIntervalSeconds;
		bEmberTelegraphActive = true;
		EmberTelegraphRemaining = EmberVolleyWarningSeconds;
		EmberSafeLaneDeg = AI->GetBehaviorStream().FRand() * 360.f;
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			if (UQinglanAudioSubsystem* Audio = GameInstance->GetSubsystem<UQinglanAudioSubsystem>())
			{
				Audio->PlaySfx(FName(TEXT("sfx_telegraph_warn")));
			}
		}
		OnTelegraph.Broadcast(this, EmberVolleyWarningSeconds);
		// AUD-41: carry the safe-lane bearing so the presenter can draw the
		// visible gap. This broadcast is independent of graphics quality; Low
		// and Metal still receive it (the minimum 0.9 s warning is unchanged).
		OnEmberVolleyTelegraph.Broadcast(this, EmberSafeLaneDeg, EmberVolleyWarningSeconds);
	}
}

void AQinglanEnemyCharacter::UpdateAnimationActionState()
{
	// AUD-44: presentation-only mirror of the AI state machine. Locomotion
	// (Idle/Walk/Run/Airborne) is driven natively from the pawn velocity while
	// ActionState is Idle; committed attack/stagger/death states freeze that
	// selection so the montage owns the pose.
	USkeletalMeshComponent* const MeshComp = GetMesh();
	UQinglanCharacterAnimInstance* const AnimInstance =
		MeshComp ? Cast<UQinglanCharacterAnimInstance>(MeshComp->GetAnimInstance()) : nullptr;
	if (!AnimInstance)
	{
		return;
	}

	EQinglanAnimActionState DesiredState = EQinglanAnimActionState::Idle;
	switch (AIState)
	{
	case EQinglanAIState::Windup:
	case EQinglanAIState::Attack:
	case EQinglanAIState::Recover:
		DesiredState = EQinglanAnimActionState::Attack;
		break;

	case EQinglanAIState::Stagger:
		DesiredState = EQinglanAnimActionState::Hit;
		break;

	case EQinglanAIState::Dead:
		DesiredState = EQinglanAnimActionState::Death;
		break;

	case EQinglanAIState::Idle:
	case EQinglanAIState::Acquire:
	case EQinglanAIState::Approach:
	default:
		DesiredState = EQinglanAnimActionState::Idle;
		break;
	}

	AnimInstance->SetActionState(DesiredState);
}
