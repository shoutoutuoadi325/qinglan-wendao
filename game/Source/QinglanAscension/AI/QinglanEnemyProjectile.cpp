// Pooled enemy projectile implementation. Damage is routed through the
// victim's UQinglanCombatComponent (single audited path, code-contracts
// "Combat semantics"); the (CastId, victim, ordinal) key dedupes repeats.
#include "AI/QinglanEnemyProjectile.h"

#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInterface.h"

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
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionSphere->SetGenerateOverlapEvents(true);
	CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &AQinglanEnemyProjectile::HandleBeginOverlap);
	RootComponent = CollisionSphere;

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->bAutoActivate = false;
	// The actor supplies one range/lifetime-bounded movement step. An independent
	// component tick could otherwise move before expiry or after our range check.
	Movement->PrimaryComponentTick.bCanEverTick = false;
	Movement->ProjectileGravityScale = 0.f; // aimed shots fly straight
	Movement->bRotationFollowsVelocity = true;
	Movement->bInitialVelocityInLocalSpace = false;
	Movement->bShouldBounce = false;

	// AUD-42: previously the shot was only a collision sphere with no visible
	// body. This engine-shape placeholder travels with the projectile; it has no
	// collision and never participates in the overlap damage path.
	Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visual"));
	Visual->SetupAttachment(CollisionSphere);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Visual->SetGenerateOverlapEvents(false);
	Visual->SetRelativeScale3D(FVector(0.3f, 0.3f, 0.6f));
	// Engine cone points along local +Z; bRotationFollowsVelocity aligns the
	// actor's +X with the velocity, so pitch it to point the tip forward.
	Visual->SetRelativeRotation(FRotator(-90.f, 0.f, 0.f));

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void AQinglanEnemyProjectile::BeginPlay()
{
	Super::BeginPlay();
	// Assets are resolved here rather than in the constructor so CDO creation
	// never loads content. Missing meshes only log; flight/damage are unchanged.
	if (Visual && Visual->GetStaticMesh() == nullptr)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone"));
		if (!Mesh)
		{
			Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
		}
		Visual->SetStaticMesh(Mesh);
		if (!Mesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("Enemy projectile placeholder mesh missing; flying without a visible body"));
		}
	}
	if (Visual)
	{
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial")))
		{
			Visual->SetMaterial(0, Material);
		}
	}
}

void AQinglanEnemyProjectile::LaunchFrom(AQinglanEnemyCharacter* InOwner, const FVector& Direction,
	float Speed, float InDamage, float InRange, const FQinglanCastId& InCastId, FName InTechniqueId, FName InCueId)
{
	ResetForPool();
	OwningEnemy = InOwner;
	SetOwner(InOwner);
	Damage = InDamage;
	RemainingRange = InRange;
	CastId = InCastId;
	TechniqueId = InTechniqueId;
	CueId = InCueId;
	Victim = nullptr;
	bInFlight = true;
	FlightSeconds = 0.f;

	SetActorHiddenInGame(false);
	if (Visual)
	{
		Visual->SetVisibility(true, true);
	}
	if (InOwner)
	{
		CollisionSphere->IgnoreActorWhenMoving(InOwner, true);
		SetActorLocation(InOwner->GetActorLocation() + FVector(0.f, 0.f, 60.f)); // authored chest-height socket
	}
	// Simultaneous boss bolts share a muzzle. They block world geometry but
	// must not consume one another before leaving that shared spawn point.
	for (TActorIterator<AQinglanEnemyProjectile> It(GetWorld()); It; ++It)
	{
		if (*It != this)
		{
			CollisionSphere->IgnoreActorWhenMoving(*It, true);
			It->CollisionSphere->IgnoreActorWhenMoving(this, true);
		}
	}
	SetActorRotation(Direction.Rotation());

	// StopSimulating clears UpdatedComponent after a blocking impact. A
	// pooled projectile must bind its sphere again before the next launch.
	Movement->SetUpdatedComponent(CollisionSphere);
	Movement->InitialSpeed = Speed;
	Movement->MaxSpeed = Speed;
	Movement->Velocity = Direction.GetSafeNormal() * Speed;
	Movement->SetActive(true, true);
	Movement->OnProjectileStop.RemoveDynamic(this, &AQinglanEnemyProjectile::HandleImpact);
	Movement->OnProjectileStop.AddDynamic(this, &AQinglanEnemyProjectile::HandleImpact);

	SetActorTickEnabled(true);
	SetActorEnableCollision(true);
	// Enable overlaps only after placement and flight state are complete.
	// A point-blank contact may deactivate the projectile synchronously.
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}

void AQinglanEnemyProjectile::ResetForPool()
{
	// Pooling contract (docs/05 section 5): timers, victims, owner, tags,
	// collision, and remaining lifetime are all cleared.
	bInFlight = false;
	Victim = nullptr;
	OwningEnemy = nullptr;
	SetOwner(nullptr);
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
	const float TimeLeft = FMath::Max(0.f, MaxFlightSeconds - FlightSeconds);
	const float Speed = Movement->Velocity.Size();
	const float RangeTime = Speed > KINDA_SMALL_NUMBER ? FMath::Max(0.f, RemainingRange) / Speed : TimeLeft;
	const float StepSeconds = FMath::Min3(FMath::Max(0.f, DeltaSeconds), TimeLeft, RangeTime);
	const FVector Start = GetActorLocation();
	if (StepSeconds > 0.f)
	{
		Movement->TickComponent(StepSeconds, LEVELTICK_All, nullptr);
	}
	if (!bInFlight)
	{
		return; // collision may already have spent the shot during the swept step
	}
	// Resolve the final swept segment before retiring, including on a slow frame.
	FlightSeconds += StepSeconds;
	RemainingRange = FMath::Max(0.f, RemainingRange - FVector::Dist(Start, GetActorLocation()));
	if (StepSeconds >= RangeTime || StepSeconds >= TimeLeft
		|| RemainingRange <= KINDA_SMALL_NUMBER || FlightSeconds >= MaxFlightSeconds)
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
	if (bAccepted && VictimCombat->IsAlive() && StatusOnHit != EQinglanStatus::None)
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
