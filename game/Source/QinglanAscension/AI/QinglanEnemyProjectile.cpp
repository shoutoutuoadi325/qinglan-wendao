// Pooled enemy projectile implementation. Damage is routed through the
// victim's UQinglanCombatComponent (single audited path, code-contracts
// "Combat semantics"); the (CastId, victim, ordinal) key dedupes repeats.
#include "AI/QinglanEnemyProjectile.h"

#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"

namespace
{
	// Authored hard cap so a pooled shot can never outlive its room; range is
	// the primary limit (docs/05 section 5 stale-object safety).
	constexpr float MaxFlightSeconds = 6.f;
}

AQinglanEnemyProjectile::AQinglanEnemyProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	CollisionSphere->InitSphereRadius(12.f); // authored arrow/bolt caliber
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CollisionSphere->SetGenerateOverlapEvents(true);
	CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &AQinglanEnemyProjectile::HandleBeginOverlap);
	RootComponent = CollisionSphere;

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bAutoActivate = false;
	Movement->ProjectileGravityScale = 0.f; // aimed shots fly straight
	Movement->bRotationFollowsVelocity = true;
	Movement->bInitialVelocityInLocalSpace = false;
	Movement->bShouldBounce = false;

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void AQinglanEnemyProjectile::LaunchFrom(AQinglanEnemyCharacter* InOwner, const FVector& Direction,
	float Speed, float InDamage, float InRange, const FQinglanCastId& InCastId, FName InTechniqueId, FName InCueId)
{
	OwningEnemy = InOwner;
	Damage = InDamage;
	RemainingRange = InRange;
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	CueId = InCueId;
	Victim = nullptr;
	bInFlight = true;
	FlightSeconds = 0.f;

	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	if (InOwner)
	{
		CollisionSphere->IgnoreActorWhenMoving(InOwner, true);
		SetActorLocation(InOwner->GetActorLocation() + FVector(0.f, 0.f, 60.f)); // authored chest-height socket
	}
	SetActorRotation(Direction.Rotation());

	Movement->InitialSpeed = Speed;
	Movement->MaxSpeed = Speed;
	Movement->Velocity = Direction.GetSafeNormal() * Speed;
	Movement->SetActive(true, true);
	Movement->OnProjectileStop.RemoveDynamic(this, &AQinglanEnemyProjectile::HandleImpact);
	Movement->OnProjectileStop.AddDynamic(this, &AQinglanEnemyProjectile::HandleImpact);

	SetActorTickEnabled(true);
}

void AQinglanEnemyProjectile::ResetForPool()
{
	// Pooling contract (docs/05 section 5): timers, victims, owner, tags,
	// collision, and remaining lifetime are all cleared.
	bInFlight = false;
	Victim = nullptr;
	OwningEnemy = nullptr;
	Damage = 0.f;
	RemainingRange = 0.f;
	FlightSeconds = 0.f;
	CastId = FQinglanCastId();
	TechniqueId = NAME_None;
	CueId = NAME_None;
	Tags.Empty();

	if (Movement)
	{
		Movement->StopMovementImmediately();
		Movement->SetActive(false, false);
		Movement->OnProjectileStop.RemoveDynamic(this, &AQinglanEnemyProjectile::HandleImpact);
	}
	if (CollisionSphere)
	{
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CollisionSphere->ClearMoveIgnoreActors();
	}
	SetActorTickEnabled(false);
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void AQinglanEnemyProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bInFlight)
	{
		return;
	}
	// Paused-game-time semantics: accumulate DeltaTime, never wall clock.
	FlightSeconds += DeltaSeconds;
	RemainingRange -= DeltaSeconds * Movement->Velocity.Size(); // UE 5.8: UProjectileMovementComponent has no GetVelocity(); use Velocity
	if (RemainingRange <= 0.f || FlightSeconds >= MaxFlightSeconds)
	{
		Deactivate();
	}
}

void AQinglanEnemyProjectile::HandleBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/,
	AActor* OtherActor, UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/,
	bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	if (!bInFlight || !OtherActor)
	{
		return;
	}
	// Enemy shots only threaten the player; no friendly fire, no double hits.
	AQinglanCharacter* PlayerVictim = Cast<AQinglanCharacter>(OtherActor);
	if (!PlayerVictim || Victim.IsValid() || Victim.Get() == OtherActor)
	{
		return;
	}
	Victim = OtherActor;

	UQinglanCombatComponent* VictimCombat = PlayerVictim->GetCombat();
	AQinglanEnemyCharacter* OwnerRef = OwningEnemy.Get();
	if (!VictimCombat || !OwnerRef || !GetWorld())
	{
		Deactivate();
		return;
	}

	// Element / status from the owner's catalog spec (e.g. ember_wisp Burn).
	EQinglanElement Element = EQinglanElement::None;
	EQinglanStatus StatusOnHit = EQinglanStatus::None;
	if (const FQinglanArchetypeSpec* Spec = QinglanEnemyCatalog::FindSpec(OwnerRef->GetEnemyId()))
	{
		Element = Spec->Element;
		StatusOnHit = Spec->StatusOnHit;
	}

	FQinglanDamageEvent Event;
	Event.Attacker = OwnerRef;
	Event.Target = PlayerVictim;
	Event.CastId = CastId; // release identity, never the impact frame (AUD-33)
	Event.HitOrdinal = 0;
	Event.TechniqueId = TechniqueId;
	Event.Element = Element;
	Event.RawMagnitude = Damage;
	Event.Timestamp = GetWorld()->GetTimeSeconds();
	const bool bAccepted = VictimCombat->ApplyDamage(Event);
	// Status application follows an accepted hit per the central precedence
	// chain (code-contracts: immunity -> barrier/health -> reaction -> status
	// -> cue). Dodged and duplicate-rejected hits never apply or refresh the
	// status (AUD-31).
	if (bAccepted && StatusOnHit != EQinglanStatus::None)
	{
		VictimCombat->ApplyStatus(StatusOnHit);
	}
	Deactivate();
}

void AQinglanEnemyProjectile::HandleImpact(const FHitResult& /*Hit*/)
{
	Deactivate();
}

void AQinglanEnemyProjectile::Deactivate()
{
	bInFlight = false;
	if (Movement)
	{
		Movement->StopMovementImmediately();
		Movement->SetActive(false, false);
	}
	if (CollisionSphere)
	{
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SetActorTickEnabled(false);
	SetActorHiddenInGame(true);
}
