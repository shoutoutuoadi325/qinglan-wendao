// Pooled enemy projectile (arrows, fire bolts, spirit bolts).
// Pooling contract (docs/05 section 5): ResetForPool clears timers, victims,
// owner, tags, collision, and remaining lifetime so no stale hit can damage a
// new expedition.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "QinglanEnemyProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class AQinglanEnemyCharacter;
class UNiagaraSystem;

UCLASS(NotBlueprintable)
class AQinglanEnemyProjectile : public AActor
{
	GENERATED_BODY()

public:
	AQinglanEnemyProjectile();

	virtual void Tick(float DeltaSeconds) override;

	/** Fires one pooled shot. Deterministic: no randomness inside. */
	void LaunchFrom(AQinglanEnemyCharacter* InOwner, const FVector& Direction, float Speed,
		float InDamage, float InRange, int32 InCastSerial, FName InTechniqueId, FName InCueId);

	/** Pooling contract: clears timers, victims, owner, tags, collision, lifetime. */
	void ResetForPool();

	bool IsInFlight() const { return bInFlight; }

private:
	UFUNCTION()
	void HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/** UProjectileMovementComponent::OnProjectileStop: (Actor, Location, Hit). */
	UFUNCTION()
	void HandleImpact(AActor* EncroachedActor, FVector HitLocation, const FHitResult& Hit);

	void Deactivate();

	UPROPERTY(VisibleAnywhere) USphereComponent* CollisionSphere = nullptr;
	UPROPERTY(VisibleAnywhere) UProjectileMovementComponent* Movement = nullptr;

	/** Deterministic serial used for the hit-key Cast id (dedupe contract). */
	int32 OwnerSerial = 0;
	float Damage = 0.f;
	/** Traveled distance cap = attack range. */
	float RemainingRange = 0.f;
	/** Hard flight-time cap, accumulated paused game time. */
	float FlightSeconds = 0.f;
	FName TechniqueId = NAME_None;
	FName CueId = NAME_None;
	bool bInFlight = false;

	/** One victim per shot; a spent projectile cannot hit again. */
	TWeakObjectPtr<AActor> Victim;
	TWeakObjectPtr<AQinglanEnemyCharacter> OwningEnemy;
};
