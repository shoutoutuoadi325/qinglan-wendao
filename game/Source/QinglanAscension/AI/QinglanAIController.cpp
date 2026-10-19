// AI controller implementation: sensing, movement execution, arbitration,
// stagger immunity, portal timing, boss pattern picks, pooled projectiles.
// The character's EnemyData is protected, so the controller keeps its own
// weak mirror (EnemyDataRef) plus a cached catalog spec for behavior flags.
#include "AI/QinglanAIController.h"

#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEnemyProjectile.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Navigation/PathFollowingComponent.h" // full EPathFollowingRequestResult enumerators (AIController.h only forward-declares the enum)
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY(LogQinglanAI);

namespace
{
	// Placeholder cue assets (docs/05 section 3: original placeholder
	// alternatives until the Audio pass). Null-safe everywhere: a missing
	// asset degrades to no sound, never to a crash or gameplay change.
	const TCHAR* GOffscreenAttackCuePath = TEXT("/Game/Qinglan/Audio/S_EnemyAttackOffscreen");

	USoundBase* LoadSound(const TCHAR* Path)
	{
		return Cast<USoundBase>(FSoftObjectPath(Path).TryLoad());
	}
}

AQinglanAIController::AQinglanAIController()
{
	// No blackboard/behavior tree: the FSM is explicit (docs/05 section 5).
	bWantsPlayerState = false;
	PrimaryActorTick.bCanEverTick = true;
}

void AQinglanAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	SetActorTickEnabled(true);
}

void AQinglanAIController::OnUnPossess()
{
	ReleaseHeldSlots();
	StopMoving();
	Super::OnUnPossess();
}

void AQinglanAIController::Initialize(AQinglanEncounterDirector* InDirector, int32 InDeterministicSeed)
{
	Director = InDirector;
	BehaviorStream.Initialize(InDeterministicSeed);
	// Deterministic orbit direction per enemy (docs/01 section 6 seeded runs).
	StrafeSign = BehaviorStream.GetFraction() < 0.5f ? -1.f : 1.f;
}

void AQinglanAIController::OnEnemyDataInitialized(UQinglanEnemyDataAsset* InData)
{
	EnemyDataRef = InData;
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	CachedSpec = (Enemy && InData) ? QinglanEnemyCatalog::FindSpec(InData->EnemyId) : nullptr;
}

void AQinglanAIController::ResetForPool()
{
	ReleaseHeldSlots();
	TargetPawn = nullptr;
	EnemyDataRef = nullptr;
	SecondsSinceTargetSeen = 999.f;
	bHasLineOfSight = false;
	SensingAccumulator = 0.f;
	StaggerImmunityRemaining = 0.f;
	bPortalPending = false;
	PortalElapsed = 0.f;
	PortalDuration = QinglanAITunables::PortalWarningSeconds;
	ResetCombo();
	bAttackHitApplied = false;
	bChargeLaunched = false;
	CurrentPattern = nullptr;
	PendingStrikeCenters.Empty();
	GroundMarkerCenter = FVector::ZeroVector;
	RetreatRemaining = 0.f;
	DeathWorldSeconds = -1.f;
	CachedSpec = nullptr;
	bDirectMovement = false;
	bOrbiting = false;
	OrbitRemaining = 0.f;
	StopMoving();
	ClearFocus(EAIFocusPriority::Gameplay);
}

AQinglanEnemyCharacter* AQinglanAIController::GetEnemy() const
{
	return Cast<AQinglanEnemyCharacter>(GetPawn());
}

void AQinglanAIController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy)
	{
		return;
	}

	// Hidden behind an unopened portal: only the warning timer advances.
	if (bPortalPending)
	{
		UpdatePortal(DeltaSeconds);
		return;
	}

	if (Enemy->GetAIState() == EQinglanAIState::Dead)
	{
		return; // death pool timing is polled by the director
	}

	UpdateStaggerImmunity(DeltaSeconds);

	// Sensing rides the same 5-10 Hz decision cadence (docs/02 section 8).
	SensingAccumulator += DeltaSeconds;
	if (SensingAccumulator >= QinglanAITunables::DecisionIntervalSeconds)
	{
		SensingAccumulator = 0.f;
		UpdateSensing();
	}

	UpdateMovement(DeltaSeconds);
}

void AQinglanAIController::UpdateSensing()
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	UQinglanEnemyDataAsset* Data = EnemyDataRef.Get();
	ACharacter* PlayerPawn = Cast<ACharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));
	if (PlayerPawn == nullptr)
	{
		// Headless worlds (automation fixtures) register no local player;
		// the possessed player character in the world is the target.
		if (UWorld* World = GetWorld())
		{
			TActorIterator<ACharacter> It(World);
			while (It)
			{
				if (AController* PC = It->GetController())
				{
					if (PC->IsPlayerController())
					{
						PlayerPawn = *It;
						break;
					}
				}
				++It;
			}
		}
	}
	if (!Enemy || !Data || !PlayerPawn || !Enemy->IsAlive())
	{
		TargetPawn = nullptr;
		bHasLineOfSight = false;
		return;
	}

	const float Distance = FVector::Dist2D(Enemy->GetActorLocation(), PlayerPawn->GetActorLocation());
	const float AggroRadius = Data->AggroRadius; // radius sensing, blueprint section 1.3
	const float LeashRadius = AggroRadius * QinglanAITunables::LeashRadiusFactor;

	bHasLineOfSight = HasLineOfSightToTarget(PlayerPawn);
	if (bHasLineOfSight)
	{
		SecondsSinceTargetSeen = 0.f;
	}
	else
	{
		SecondsSinceTargetSeen += QinglanAITunables::DecisionIntervalSeconds;
	}

	if (!TargetPawn.IsValid())
	{
		// Acquire: inside radius AND visible (radius+LOS sensing).
		if (Distance <= AggroRadius && bHasLineOfSight)
		{
			TargetPawn = PlayerPawn;
		}
		return;
	}

	// Keep: short memory after losing sight, hard leash otherwise.
	if (Distance > LeashRadius ||
		(!bHasLineOfSight && SecondsSinceTargetSeen > QinglanAITunables::TargetMemorySeconds))
	{
		TargetPawn = nullptr;
		ClearFocus(EAIFocusPriority::Gameplay);
	}
}

bool AQinglanAIController::HasLineOfSightToTarget() const
{
	ACharacter* Target = GetTargetCharacter();
	return Target ? HasLineOfSightToTarget(Target) : false;
}

bool AQinglanAIController::HasLineOfSightToTarget(ACharacter* Target) const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	UWorld* World = GetWorld();
	if (!Enemy || !Target || !World)
	{
		return false;
	}
	const FVector Start = Enemy->GetActorLocation() + FVector(0.f, 0.f, QinglanAITunables::EyeHeightCm);
	const FVector End = Target->GetActorLocation() + FVector(0.f, 0.f, QinglanAITunables::EyeHeightCm * 0.5f);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(QinglanAILineOfSight), /*bTraceComplex*/ true, this);
	Params.AddIgnoredActor(Enemy);
	FHitResult Hit;
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	return !bBlocked || Hit.GetActor() == Target;
}

bool AQinglanAIController::HasTarget() const
{
	return TargetPawn.IsValid();
}

ACharacter* AQinglanAIController::GetTargetCharacter() const
{
	return TargetPawn.Get();
}

float AQinglanAIController::GetDistanceToTarget() const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	const ACharacter* Target = GetTargetCharacter();
	return (Enemy && Target) ? FVector::Dist2D(Enemy->GetActorLocation(), Target->GetActorLocation())
		: TNumericLimits<float>::Max();
}

bool AQinglanAIController::IsInAttackRange() const
{
	const UQinglanEnemyDataAsset* Data = EnemyDataRef.Get();
	if (!Data || !GetTargetCharacter())
	{
		return false;
	}
	// Melee swings get a small authored tolerance so the release check cannot
	// whiff against a target that stepped one centimeter out.
	const float Tolerance = Data->bRanged ? 1.f : 1.1f;
	return GetDistanceToTarget() <= Data->AttackRange * Tolerance;
}

bool AQinglanAIController::IsMovementSuppressed() const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	const UQinglanEnemyDataAsset* Data = EnemyDataRef.Get();
	if (!Enemy)
	{
		return true;
	}
	if (Enemy->GetAIState() == EQinglanAIState::Stagger)
	{
		return true;
	}
	// Root 1 s on ordinary enemies; the data asset marks who ignores it
	// (docs/02 section 7).
	const bool bRootImmune = Data && Data->bRootImmune;
	return !bRootImmune && Enemy->EnemyCombat && Enemy->EnemyCombat->HasStatus(EQinglanStatus::Root);
}

void AQinglanAIController::UpdateStaggerImmunity(float DeltaSeconds)
{
	if (StaggerImmunityRemaining > 0.f)
	{
		StaggerImmunityRemaining -= DeltaSeconds; // paused-game-time accumulation
	}
}

bool AQinglanAIController::CanBeStaggeredBy(float IncomingDamage) const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy || !Enemy->IsAlive())
	{
		return false;
	}
	// Bosses never stagger (they still take the Root->slow downgrade,
	// docs/02 section 7).
	if (IsBoss())
	{
		return false;
	}
	if (StaggerImmunityRemaining > 0.f)
	{
		return false; // stun-lock prevention, docs/02 section 8
	}
	return IncomingDamage >= QinglanAITunables::StaggerMinimumDamage;
}

void AQinglanAIController::NotifyStaggerBegan()
{
	StaggerImmunityRemaining = QinglanAITunables::StaggerImmunitySeconds;
	ReleaseHeldSlots();
	ResetCombo();
	bAttackHitApplied = false;
	bChargeLaunched = false;
	CurrentPattern = nullptr;
	RetreatRemaining = 0.f;
	StopMoving();
}

void AQinglanAIController::UpdatePortal(float DeltaSeconds)
{
	PortalElapsed += DeltaSeconds; // paused-game-time accumulation
	if (PortalElapsed >= PortalDuration)
	{
		MaterializeFromPortal();
	}
}

void AQinglanAIController::BeginPortalWarning(float Duration)
{
	bPortalPending = true;
	PortalElapsed = 0.f;
	PortalDuration = Duration > 0.f ? Duration : QinglanAITunables::PortalWarningSeconds;
}

void AQinglanAIController::MaterializeFromPortal()
{
	bPortalPending = false;
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy)
	{
		return;
	}
	Enemy->SetActorHiddenInGame(false);
	Enemy->SetActorEnableCollision(true);
	Enemy->SetActorTickEnabled(true);
}

bool AQinglanAIController::IsBoss() const
{
	return Cast<UQinglanBossDataAsset>(EnemyDataRef.Get()) != nullptr;
}

bool AQinglanAIController::IsPhaseTwo() const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	const UQinglanBossDataAsset* BossData = Cast<UQinglanBossDataAsset>(EnemyDataRef.Get());
	if (!Enemy || !BossData || !Enemy->EnemyCombat || Enemy->EnemyCombat->GetMaxHealth() <= 0.f)
	{
		return false;
	}
	// "At 50% HP, shorten idle breaks rather than hide telegraphs"
	// (docs/02 section 9).
	return Enemy->EnemyCombat->GetHealth() / Enemy->EnemyCombat->GetMaxHealth()
		<= BossData->PhaseTwoAtHealthFraction;
}

const FQinglanBossPattern* AQinglanAIController::PickBossPattern(const UQinglanBossDataAsset* BossData) const
{
	if (!BossData || BossData->Patterns.IsEmpty())
	{
		return nullptr;
	}
	float TotalWeight = 0.f;
	for (const FQinglanBossPattern& P : BossData->Patterns)
	{
		TotalWeight += FMath::Max(0.f, P.Weight);
	}
	if (TotalWeight <= 0.f)
	{
		return &BossData->Patterns[0];
	}
	// Deterministic weighted pick from the room-seeded stream (docs/01 section 6).
	float Pick = BehaviorStream.GetFraction() * TotalWeight;
	for (const FQinglanBossPattern& P : BossData->Patterns)
	{
		Pick -= FMath::Max(0.f, P.Weight);
		if (Pick <= 0.f)
		{
			return &P;
		}
	}
	return &BossData->Patterns.Last();
}

void AQinglanAIController::NotifyWindupBegan(float /*TelegraphDuration*/, const FQinglanBossPattern* Pattern)
{
	bAttackHitApplied = false;
	bChargeLaunched = false;
	PendingStrikeCenters.Empty();

	AQinglanEnemyCharacter* Enemy = GetEnemy();
	ACharacter* Target = GetTargetCharacter();
	if (!Enemy || !Target)
	{
		return;
	}

	// Audible cue for attacks the player cannot see (docs/02 section 8).
	PlayOffscreenAttackAudioCue();

	// Record shape data at windup start so the release envelope is fixed and
	// dodgeable (delayed ground effects, docs/02 section 8).
	EQinglanAttackShape Shape = CachedSpec ? CachedSpec->AttackShape : EQinglanAttackShape::MeleeArc;
	if (Pattern)
	{
		Shape = QinglanEnemyCatalog::ShapeOfPattern(*Pattern);
	}

	if (Shape == EQinglanAttackShape::GroundRing || Shape == EQinglanAttackShape::GroundRingGaps)
	{
		RecordGroundMarker(Target->GetActorLocation());
		if (Shape == EQinglanAttackShape::GroundRingGaps)
		{
			// Deterministic safe-gap direction (Ashen ring shockwave).
			SafeArcCenterDeg = BehaviorStream.FRand() * 360.f;
		}
	}
	else if (Shape == EQinglanAttackShape::MultiGroundStrikes)
	{
		// Three persistent outlines at windup start, each >= 0.9 s warning
		// (Golden Core trial rule, docs/02 section 9).
		const float BaseRotationDeg = BehaviorStream.FRand() * 360.f;
		const float SpacingCm = CachedSpec ? CachedSpec->GroundRingRadius * 1.5f : 450.f;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const float AngleDeg = FMath::DegreesToRadians(BaseRotationDeg + Index * 120.f);
			const FVector Offset(FMath::Cos(AngleDeg) * SpacingCm, FMath::Sin(AngleDeg) * SpacingCm, 0.f);
			PendingStrikeCenters.Add(Target->GetActorLocation() + Offset);
		}
	}
	else
	{
		RecordGroundMarker(Target->GetActorLocation());
	}
}

void AQinglanAIController::NotifyAttackCommitted(float LungeSpeed)
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	ACharacter* Target = GetTargetCharacter();
	if (!Enemy || !Target || LungeSpeed <= 0.f || bChargeLaunched)
	{
		return;
	}
	// Wolf lunge / boss charge: fixed-direction launch at the release notify.
	const FVector Direction = (Target->GetActorLocation() - Enemy->GetActorLocation()).GetSafeNormal2D();
	Enemy->LaunchCharacter(Direction * LungeSpeed, /*bXYOverride*/ true, /*bZOverride*/ false);
	bChargeLaunched = true;
}

void AQinglanAIController::NotifyAttackCycleEnded()
{
	// Budget release on attack end (task contract; docs/05 section 5).
	ReleaseHeldSlots();
	ComboIndex++;
}

bool AQinglanAIController::ConsumeAttackHit()
{
	if (bAttackHitApplied)
	{
		return false;
	}
	bAttackHitApplied = true;
	return true;
}

bool AQinglanAIController::IsTargetInsideGroundRing(float RingRadius) const
{
	const ACharacter* Target = GetTargetCharacter();
	if (!Target)
	{
		return false;
	}
	return FVector::Dist2D(Target->GetActorLocation(), GroundMarkerCenter) <= RingRadius;
}

bool AQinglanAIController::IsTargetOutsideSafeArc(float SafeArcHalfWidthDeg) const
{
	const ACharacter* Target = GetTargetCharacter();
	if (!Target)
	{
		return false;
	}
	const FVector Diff = Target->GetActorLocation() - GroundMarkerCenter;
	const float TargetDeg = FMath::RadiansToDegrees(FMath::Atan2(Diff.Y, Diff.X));
	const float Delta = FMath::Abs(FMath::FindDeltaAngleDegrees(TargetDeg, SafeArcCenterDeg));
	return Delta <= SafeArcHalfWidthDeg; // inside the gap: safe, no damage
}

// GetPendingStrikeCenters is inline in QinglanAIController.h; no out-of-line
// definition here (a second definition would be a redefinition error).

FQinglanCastId AQinglanAIController::NextAttackCastId()
{
	// Globally-unique release identity (AUD-33): two enemies attacking on the
	// same frame can never collide, and pooled reuse keeps ids unique because
	// the authority counter never resets.
	return UQinglanCombatComponent::NextCastId();
}

AQinglanEnemyProjectile* AQinglanAIController::AcquirePooledProjectile()
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	UWorld* World = GetWorld();
	if (!Enemy || !World)
	{
		return nullptr;
	}
	for (const TWeakObjectPtr<AQinglanEnemyProjectile>& Pooled : ProjectilePool)
	{
		AQinglanEnemyProjectile* Candidate = Pooled.Get();
		if (Candidate && !Candidate->IsInFlight())
		{
			return Candidate;
		}
	}
	if (ProjectilePool.Num() < QinglanAITunables::ProjectilePoolSize)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.Owner = Enemy;
		AQinglanEnemyProjectile* Shot = World->SpawnActor<AQinglanEnemyProjectile>(
			AQinglanEnemyProjectile::StaticClass(), Enemy->GetActorLocation(),
			FRotator::ZeroRotator, Params);
		if (Shot)
		{
			ProjectilePool.Add(Shot);
		}
		return Shot;
	}
	return nullptr;
}

bool AQinglanAIController::SpawnPooledProjectileAtTarget(float Damage, float Range, FName TechniqueId,
	FName CueId, float AimYawOffsetDeg)
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	ACharacter* Target = GetTargetCharacter();
	if (!Enemy || !Target)
	{
		return false;
	}
	AQinglanEnemyProjectile* Shot = AcquirePooledProjectile();
	if (!Shot)
	{
		return false;
	}

	const float ProjectileSpeed = (CachedSpec && CachedSpec->ProjectileSpeed > 0.f)
		? CachedSpec->ProjectileSpeed : 3200.f; // authored fallback speed
	const FVector Muzzle = Enemy->GetActorLocation() + FVector(0.f, 0.f, QinglanAITunables::EyeHeightCm);
	// Aimed at the target's current location: dodgeable, deterministic.
	FVector Aim = (Target->GetActorLocation() + FVector(0.f, 0.f, 40.f) - Muzzle).GetSafeNormal();
	if (AimYawOffsetDeg != 0.f)
	{
		Aim = Aim.RotateAngleAxis(AimYawOffsetDeg, FVector::UpVector);
	}
	Shot->SetOwner(Enemy);
	Shot->LaunchFrom(Enemy, Aim, ProjectileSpeed, Damage, Range, NextAttackCastId(), TechniqueId, CueId);
	return true;
}

bool AQinglanAIController::SpawnPooledProjectileInDirection(float Damage, float Range, FName TechniqueId,
	FName CueId, const FVector& Direction, float Speed)
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy)
	{
		return false;
	}
	AQinglanEnemyProjectile* Shot = AcquirePooledProjectile();
	if (!Shot)
	{
		return false;
	}
	Shot->SetOwner(Enemy);
	Shot->LaunchFrom(Enemy, Direction.GetSafeNormal(), Speed, Damage, Range,
		NextAttackCastId(), TechniqueId, CueId);
	return true;
}

float AQinglanAIController::GetDeathAgeSeconds() const
{
	if (DeathWorldSeconds < 0.f)
	{
		return -1.f;
	}
	const UWorld* World = GetWorld();
	return World ? static_cast<float>(World->GetTimeSeconds()) - DeathWorldSeconds : -1.f;
}

void AQinglanAIController::NotifyDeathBegan()
{
	if (DeathWorldSeconds < 0.f)
	{
		const UWorld* World = GetWorld();
		DeathWorldSeconds = World ? static_cast<float>(World->GetTimeSeconds()) : 0.f;
	}
	ReleaseHeldSlots(); // budget release on death (task contract)
	ResetCombo();
	StopMoving();
	ClearFocus(EAIFocusPriority::Gameplay);
}

void AQinglanAIController::PlayOffscreenAttackAudioCue()
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy || !IsTargetOffscreenFromPlayerCamera())
	{
		return;
	}
	// Audio cues are never quality-stripped (docs/02 section 9: cast audio
	// retained at low quality/low-flash; docs/06 section 6).
	if (USoundBase* Cue = LoadSound(GOffscreenAttackCuePath))
	{
		UGameplayStatics::PlaySoundAtLocation(this, Cue, Enemy->GetActorLocation(),
			QinglanAITunables::OffscreenCueVolumeMultiplier);
	}
	UE_LOG(LogQinglanAI, Verbose, TEXT("Offscreen attack cue fired for %s"), *Enemy->GetEnemyId().ToString());
}

bool AQinglanAIController::IsTargetOffscreenFromPlayerCamera() const
{
	const AQinglanEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	if (!Enemy || !World)
	{
		return false;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager)
	{
		return false;
	}
	FVector2D ScreenPosition;
	// UE 5.8: projection lives on APlayerController, not APlayerCameraManager.
	const bool bProjected = PC->ProjectWorldLocationToScreen(
		Enemy->GetActorLocation(), ScreenPosition, /*bPlayerViewportPercentage*/ false);
	if (!bProjected)
	{
		return true; // behind the camera or off-frustum
	}
	FVector2D ViewportSize(0.f, 0.f);
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);
	}
	return ScreenPosition.X < 0.f || ScreenPosition.Y < 0.f ||
		(ViewportSize.X > 0 && ScreenPosition.X > ViewportSize.X) ||
		(ViewportSize.Y > 0 && ScreenPosition.Y > ViewportSize.Y);
}

bool AQinglanAIController::TryAcquireMeleeWindupSlot()
{
	if (bMeleeSlotHeld)
	{
		return true;
	}
	if (Director.IsValid() && Director->RequestMeleeWindupSlot())
	{
		bMeleeSlotHeld = true;
		return true;
	}
	return false;
}

bool AQinglanAIController::TryAcquireRangedAttackSlot()
{
	if (bRangedSlotHeld)
	{
		return true;
	}
	if (Director.IsValid() && Director->RequestRangedAttackSlot())
	{
		bRangedSlotHeld = true;
		return true;
	}
	return false;
}

void AQinglanAIController::ReleaseHeldSlots()
{
	bMeleeSlotHeld = false;
	bRangedSlotHeld = false;
}

void AQinglanAIController::StopMoving()
{
	StopMovement();
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (Enemy && Enemy->GetCharacterMovement())
	{
		Enemy->GetCharacterMovement()->StopActiveMovement();
	}
}

void AQinglanAIController::UpdateMovement(float DeltaSeconds)
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	ACharacter* Target = GetTargetCharacter();
	if (!Enemy)
	{
		return;
	}
	UQinglanEnemyDataAsset* Data = EnemyDataRef.Get();
	if (!Data)
	{
		return;
	}

	// Status-driven locomotion speed (AUD-36 AI side): the boss Root downgrade
	// (10% slow) and any future slow actually change movement. Hard Root
	// suppression stays in IsMovementSuppressed (preserved); the multiplier is
	// layered on top so the two paths agree.
	if (UQinglanStatusComponent* StatusComp = Enemy->FindComponentByClass<UQinglanStatusComponent>())
	{
		if (UCharacterMovementComponent* MoveComp = Enemy->GetCharacterMovement())
		{
			const float TargetSpeed = FMath::Max(StatusComp->GetMoveSpeedMultiplier() * Data->MoveSpeed, 1.f);
			if (!FMath::IsNearlyEqual(MoveComp->MaxWalkSpeed, TargetSpeed))
			{
				MoveComp->MaxWalkSpeed = TargetSpeed;
			}
		}
	}

	switch (Enemy->GetAIState())
	{
	case EQinglanAIState::Idle:
	case EQinglanAIState::Acquire:
	{
		if (RetreatRemaining > 0.f)
		{
			// Corrupted Disciple's post-combo retreat (docs/02 section 8).
			RetreatRemaining -= DeltaSeconds; // paused-game-time accumulation
			if (Target)
			{
				const FVector Away = (Enemy->GetActorLocation() - Target->GetActorLocation()).GetSafeNormal2D();
				StopMoving();
				Enemy->AddMovementInput(Away, QinglanAITunables::RangedBackupSpeedFraction);
			}
		}
		else
		{
			StopMoving();
			if (Target)
			{
				SetFocus(Target, EAIFocusPriority::Gameplay);
			}
		}
		break;
	}

	case EQinglanAIState::Approach:
	{
		if (!Target)
		{
			StopMoving();
			break;
		}
		SetFocus(Target, EAIFocusPriority::Gameplay);
		if (IsMovementSuppressed())
		{
			StopMoving();
			break;
		}
		if (CachedSpec && CachedSpec->bFixedPerch)
		{
			break; // Ember Wisp: launches from fixed perches, never moves.
		}

		const float Distance = GetDistanceToTarget();
		const bool bRanged = CachedSpec ? CachedSpec->bRanged : Data->bRanged;

		if (bRanged)
		{
			const float MinComfort = CachedSpec ? CachedSpec->MinComfortRange : 0.f;
			const float Preferred = (CachedSpec && CachedSpec->PreferredRange > 0.f)
				? CachedSpec->PreferredRange : Data->AttackRange * 0.5f;
			if (MinComfort > 0.f && Distance < MinComfort)
			{
				// Rushed archer backs away (docs/02 section 8 "strafe or close distance").
				StopMoving();
				const FVector Away = (Enemy->GetActorLocation() - Target->GetActorLocation()).GetSafeNormal2D();
				Enemy->AddMovementInput(Away, QinglanAITunables::RangedBackupSpeedFraction);
				break;
			}
			if (Distance > Preferred)
			{
				ApproachTarget(Target, Preferred * 0.9f);
			}
			else
			{
				StopMoving(); // comfort band: hold ground and shoot
			}
			break;
		}

		// Melee approach, with the Spirit Wolf's circle-then-lunge flavor.
		if (CachedSpec && CachedSpec->bCircleBeforeAttack && CachedSpec->OrbitRadius > 0.f &&
			Distance < CachedSpec->OrbitRadius && Distance > Data->AttackRange)
		{
			// Bounded orbit phase (AUD-40): circling is an approach flavor, so
			// it gets a finite budget; after MaxOrbitSeconds the enemy turns
			// inward and the normal windup->lunge cycle takes over, keeping the
			// time-to-attack bounded for a stationary target too.
			if (!bOrbiting)
			{
				bOrbiting = true;
				OrbitRemaining = QinglanAITunables::MaxOrbitSeconds;
			}
			if (OrbitRemaining > 0.f)
			{
				OrbitRemaining -= DeltaSeconds; // paused-game-time accumulation
				StopMoving();
				const FVector ToTarget = (Target->GetActorLocation() - Enemy->GetActorLocation()).GetSafeNormal2D();
				const FVector Tangent = FVector(-ToTarget.Y, ToTarget.X, 0.f) * StrafeSign;
				Enemy->AddMovementInput(Tangent, QinglanAITunables::OrbitSpeedFraction);
				break;
			}
			ApproachTarget(Target, Data->AttackRange * 0.5f); // expired: commit inward
			break;
		}
		bOrbiting = false;
		ApproachTarget(Target, Data->AttackRange * 0.8f);
		break;
	}

	case EQinglanAIState::Windup:
	case EQinglanAIState::Attack:
		// Stand and face while committed; lunges/charges travel on their
		// launch velocity.
		if (!bChargeLaunched)
		{
			StopMoving();
			if (Target)
			{
				SetFocus(Target, EAIFocusPriority::Gameplay);
			}
		}
		break;

	case EQinglanAIState::Recover:
	case EQinglanAIState::Stagger:
	case EQinglanAIState::Dead:
	default:
		StopMoving();
		break;
	}
}

void AQinglanAIController::ApproachTarget(ACharacter* Target, float AcceptanceRadius)
{
	AQinglanEnemyCharacter* Enemy = GetEnemy();
	if (!Enemy || !Target)
	{
		return;
	}
	// UE 5.8: EPathFollowingRequestResult is a namespaced enum (::Type, ::Failed...).
	const EPathFollowingRequestResult::Type Result =
		MoveToLocation(Target->GetActorLocation(), AcceptanceRadius,
			/*bStopOnOverlap*/ true, /*bUsePathfinding*/ true);
	if (Result == EPathFollowingRequestResult::Failed)
	{
		// No navmesh (headless automation): fall back to direct steering.
		bDirectMovement = true;
	}
	if (bDirectMovement)
	{
		const FVector Toward = (Target->GetActorLocation() - Enemy->GetActorLocation()).GetSafeNormal2D();
		Enemy->AddMovementInput(Toward, 1.f);
	}
}
