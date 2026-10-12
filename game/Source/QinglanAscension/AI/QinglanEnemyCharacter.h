// Enemy character: archetype-driven stats, telegraphs, pooling reset contract.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AI/QinglanEnemyTypes.h"
#include "QinglanEnemyCharacter.generated.h"

class UQinglanCombatComponent;
class UQinglanStatusComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanEnemyDied, AQinglanEnemyCharacter*, Enemy, FName, EnemyId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanEnemyTelegraph, AQinglanEnemyCharacter*, Enemy, float, Duration);

/**
 * Base enemy. One shared humanoid rig; archetypes differ by data asset.
 * Decisions tick at 5-10 Hz; stagger immunity prevents stun locks; the
 * death event drives the encounter director's completion check.
 */
UCLASS()
class AQinglanEnemyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AQinglanEnemyCharacter();

	virtual void Tick(float DeltaSeconds) override;
	virtual float TakeDamage(float Damage, const struct FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	UFUNCTION(BlueprintPure) EQinglanAIState GetAIState() const { return AIState; }
	UFUNCTION(BlueprintPure) FName GetEnemyId() const;
	UFUNCTION(BlueprintPure) bool IsAlive() const;

	/** Pooling contract: clears timers, victims, owner, tags, collision, lifetime. */
	void ResetForPool();
	void InitializeFromData(UQinglanEnemyDataAsset* InData);

	UPROPERTY() UQinglanCombatComponent* EnemyCombat = nullptr;
	UPROPERTY() UQinglanStatusComponent* EnemyStatus = nullptr;

	UPROPERTY(BlueprintAssignable) FQinglanEnemyDied OnDied;
	UPROPERTY(BlueprintAssignable) FQinglanEnemyTelegraph OnTelegraph;

protected:
	/** Decision tick at 5-10 Hz: Acquire/Approach/Windup transitions. */
	void UpdateDecisions(float DeltaSeconds);
	/** Executes windup->active->recover attack cycle with authored telegraph. */
	void UpdateAttack(float DeltaSeconds);

	UPROPERTY() TObjectPtr<UQinglanEnemyDataAsset> EnemyData = nullptr;
	EQinglanAIState AIState = EQinglanAIState::Idle;
	float DecisionAccumulator = 0.f;
	float StateTimer = 0.f;
	float TelegraphTimer = 0.f;
	bool bAttackCommitted = false;
};
