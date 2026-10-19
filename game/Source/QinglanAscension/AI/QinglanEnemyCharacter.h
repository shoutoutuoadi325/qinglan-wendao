// Enemy character: archetype-driven stats, telegraphs, pooling reset contract.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AI/QinglanEnemyTypes.h"
#include "Combat/QinglanCombatComponent.h"
#include "QinglanEnemyCharacter.generated.h"

class UQinglanStatusComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanEnemyDied, AQinglanEnemyCharacter*, Enemy, FName, EnemyId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanEnemyTelegraph, AQinglanEnemyCharacter*, Enemy, float, Duration);
/** AUD-41: phase-two ember warning. SafeLaneDegrees is the deterministic gap
 *  presentation must keep visible; the mechanic is quality-independent. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FQinglanEmberVolleyTelegraph, AQinglanEnemyCharacter*, Enemy, float, SafeLaneDegrees, float, WarningSeconds);

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
	/**
	 * AUD-36: single authoritative boss identity. InitializeFromData tags
	 * UQinglanBossDataAsset actors with Qinglan.Boss; this query also checks
	 * the data asset directly so callers never depend on tag ordering.
	 */
	UFUNCTION(BlueprintPure) bool IsBoss() const;

	/** Pooling contract: clears timers, victims, owner, tags, collision, lifetime. */
	void ResetForPool();
	void InitializeFromData(UQinglanEnemyDataAsset* InData);

	UPROPERTY() UQinglanCombatComponent* EnemyCombat = nullptr;
	UPROPERTY() UQinglanStatusComponent* EnemyStatus = nullptr;

	UPROPERTY(BlueprintAssignable) FQinglanEnemyDied OnDied;
	UPROPERTY(BlueprintAssignable) FQinglanEnemyTelegraph OnTelegraph;
	/** Phase-two ember warning with the safe-lane bearing (world-space yaw,
	 *  degrees). Presentation uses it for the visible lane marker at every
	 *  quality tier; gameplay never depends on a presenter existing. */
	UPROPERTY(BlueprintAssignable) FQinglanEmberVolleyTelegraph OnEmberVolleyTelegraph;

	/** AUD-41 telemetry/presentation queries. */
	UFUNCTION(BlueprintPure) bool IsPhaseTwoEmbersArmed() const { return bPhaseTwoEmbersArmed; }
	UFUNCTION(BlueprintPure) bool IsEmberTelegraphActive() const { return bEmberTelegraphActive; }
	UFUNCTION(BlueprintPure) float GetEmberSafeLaneDegrees() const { return EmberSafeLaneDeg; }

protected:
	/** Decision tick at 5-10 Hz: Acquire/Approach/Windup transitions. */
	void UpdateDecisions(float DeltaSeconds);
	/** Executes windup->active->recover attack cycle with authored telegraph. */
	void UpdateAttack(float DeltaSeconds);
	/** Ashen Guardian phase-two ember volley producer (AUD-41). */
	void UpdatePhaseTwoEmbers(float DeltaSeconds);

	/**
	 * AUD-28: every accepted hit resolved by the combat authority arrives here
	 * (direct, projectile, area, and Burn-tick sources); this member alone
	 * drives stagger and death, so TakeDamage only forwards engine damage.
	 */
	UFUNCTION()
	void HandleDamageResolved(const FQinglanResolvedDamage& Result);
	/** Stagger reaction on a nonlethal accepted hit (stagger-immunity gated). */
	void EnterStagger();
	/** Exactly-once death: stops AI/actions, drops collision, releases slots. */
	void Die();

	UPROPERTY() TObjectPtr<UQinglanEnemyDataAsset> EnemyData = nullptr;
	EQinglanAIState AIState = EQinglanAIState::Idle;
	float DecisionAccumulator = 0.f;
	float StateTimer = 0.f;
	float TelegraphTimer = 0.f;
	bool bAttackCommitted = false;
	/** True while the committed shape resolves by proximity (lunge/charge); only those use the active-window range hit (AUD-29). */
	bool bProximityActivePhase = false;
	/** Death guard: OnDied/cleanup fire exactly once per life (AUD-28). */
	bool bDead = false;

	// AUD-41: ember volley state, armed exactly once at the 50% HP crossing.
	bool bPhaseTwoEmbersArmed = false;
	bool bEmberTelegraphActive = false;
	float EmberVolleyTimer = 0.f;
	float EmberTelegraphRemaining = 0.f;
	float EmberSafeLaneDeg = 0.f;
};
