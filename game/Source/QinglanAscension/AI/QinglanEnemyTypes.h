// Enemy archetype data asset and encounter director declarations.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanEnemyTypes.generated.h"

class UAnimMontage;
class UNiagaraSystem;

/** FSM states; decisions at 5-10 Hz, execution every tick. */
UENUM(BlueprintType)
enum class EQinglanAIState : uint8
{
	Idle,
	Acquire,
	Approach,
	Windup,
	Attack,
	Recover,
	Stagger,
	Dead
};

USTRUCT(BlueprintType)
struct FQinglanTelegraph
{
	GENERATED_BODY()
	/** Visible+audible warning duration before the damage event. */
	UPROPERTY(BlueprintReadOnly) float Duration = 0.65f;
	UPROPERTY(BlueprintReadOnly) FName CueId = NAME_None;
};

/** One archetype: HP/Defense, behavior weights, telegraph spec. */
UCLASS(BlueprintType)
class UQinglanEnemyDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly) FName EnemyId = NAME_None;
	UPROPERTY(EditDefaultsOnly) float Health = 45.f;
	UPROPERTY(EditDefaultsOnly) float Defense = 5.f;
	UPROPERTY(EditDefaultsOnly) float MoveSpeed = 300.f;
	UPROPERTY(EditDefaultsOnly) float AttackRange = 150.f;
	UPROPERTY(EditDefaultsOnly) float AttackDamage = 10.f;
	UPROPERTY(EditDefaultsOnly) float AttackCoefficient = 0.5f;
	/** Attack cycle: windup -> active -> recover seconds. */
	UPROPERTY(EditDefaultsOnly) float AttackWindup = 0.65f;
	UPROPERTY(EditDefaultsOnly) float AttackActive = 0.2f;
	UPROPERTY(EditDefaultsOnly) float AttackRecover = 0.5f;
	UPROPERTY(EditDefaultsOnly) FQinglanTelegraph Telegraph;
	UPROPERTY(EditDefaultsOnly) bool bRanged = false;
	UPROPERTY(EditDefaultsOnly) bool bRootImmune = false;
	UPROPERTY(EditDefaultsOnly) float AggroRadius = 1500.f;
	UPROPERTY(EditDefaultsOnly) FName SwordSocketId = NAME_None;
	UPROPERTY(EditDefaultsOnly) TSoftObjectPtr<UAnimMontage> AttackMontage;
	UPROPERTY(EditDefaultsOnly) TSoftObjectPtr<UAnimMontage> HitReactMontage;
	UPROPERTY(EditDefaultsOnly) TSoftObjectPtr<UNiagaraSystem> DeathCue;
};

/** Boss pattern step for scripted boss encounters. */
USTRUCT(BlueprintType)
struct FQinglanBossPattern
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) FName PatternId = NAME_None;
	UPROPERTY(BlueprintReadOnly) float Weight = 1.f;
	UPROPERTY(BlueprintReadOnly) float TelegraphDuration = 1.0f;
	UPROPERTY(BlueprintReadOnly) float Damage = 30.f;
	UPROPERTY(BlueprintReadOnly) float Range = 300.f;
	UPROPERTY(BlueprintReadOnly) bool bGroundMarker = false;
	UPROPERTY(BlueprintReadOnly) TArray<FName> CueIds;
};

UCLASS(BlueprintType)
class UQinglanBossDataAsset : public UQinglanEnemyDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly) TArray<FQinglanBossPattern> Patterns;
	/** HP fraction where enrage-lite behavior begins (shorter idles, same telegraphs). */
	UPROPERTY(EditDefaultsOnly) float PhaseTwoAtHealthFraction = 0.5f;
	UPROPERTY(EditDefaultsOnly) float ArenaDiameter = 4500.f;
};
