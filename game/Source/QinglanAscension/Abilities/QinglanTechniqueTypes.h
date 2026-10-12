// Technique data types and the technique data asset.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanTechniqueTypes.generated.h"

class UAnimMontage;
class UNiagaraSystem;

/** How a technique finds its target(s) and delivers its effect. */
UENUM(BlueprintType)
enum class EQinglanTargeting : uint8
{
	/** Cone/arc from the cast socket toward the aim point (jade_sword_arc). */
	SelfArc,
	/** Single projectile (ember_lance). */
	Projectile,
	/** Barrier on self (flowing_ward, stone_aegis). */
	SelfBarrier,
	/** Heal over time on self (verdant_pulse). */
	SelfHeal,
	/** Ground strike at aim point, fixed radius (thunder_seal). */
	GroundStrike,
	/** Enhanced dodge (mist_step). */
	SelfDash,
	/** Orbiting pulses around self (orbiting_blades). */
	SelfPulses
};

/** Anticipation/release/recovery phases; energy+cooldown commit at release. */
USTRUCT(BlueprintType)
struct FQinglanCastPhases
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) float Anticipation = 0.2f;
	UPROPERTY(BlueprintReadOnly) float Release = 0.1f;
	UPROPERTY(BlueprintReadOnly) float Recovery = 0.3f;
};

/** Runtime technique definition. Mirrors data/examples/techniques.json plus
 *  cast phases, targeting, delivery magnitude, animation/cue soft references,
 *  and the unlock rule. All references are validated during cook. */
UCLASS(BlueprintType)
class UQinglanTechniqueDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName TechniqueId = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName NameTextKey = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) EQinglanElement Element = EQinglanElement::None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) int32 EnergyCost = 0;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float CooldownSeconds = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float PowerCoefficient = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) int32 FlatDamage = 0;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) EQinglanTargeting Targeting = EQinglanTargeting::Projectile;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FQinglanCastPhases Phases;
	/** Element-specific magnitude: barrier points, heal amount, dash distance cm, pulse count... */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float Magnitude = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float Range = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FName UnlockFlag = NAME_None;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TSoftObjectPtr<UAnimMontage> CastMontage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> AnticipationCue;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> ReleaseCue;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TSoftObjectPtr<UNiagaraSystem> ImpactCue;
};

/** Passive items (equip 2). */
UCLASS(BlueprintType)
class UQinglanPassiveDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly) FName PassiveId = NAME_None;
	UPROPERTY(EditDefaultsOnly) FName NameTextKey = NAME_None;
	UPROPERTY(EditDefaultsOnly) FName DescriptionTextKey = NAME_None;
	/** Flat attribute deltas applied while equipped. */
	UPROPERTY(EditDefaultsOnly) FQinglanAttributes AttributeDeltas;
	/** Multiplicative modifiers in percent (sword damage, technique cost, healing, stamina recovery). */
	UPROPERTY(EditDefaultsOnly) float SwordDamagePercent = 0.f;
	UPROPERTY(EditDefaultsOnly) float TechniqueCostPercent = 0.f;
	UPROPERTY(EditDefaultsOnly) float HealingPercent = 0.f;
	UPROPERTY(EditDefaultsOnly) float ConductDamagePercent = 0.f;
	UPROPERTY(EditDefaultsOnly) float EnergyRegenBonus = 0.f;
	UPROPERTY(EditDefaultsOnly) float StaminaRegenPercent = 0.f;
};

/** Talismans: expedition charges, usable from two slots. */
UCLASS(BlueprintType)
class UQinglanTalismanDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly) FName TalismanId = NAME_None;
	UPROPERTY(EditDefaultsOnly) FName NameTextKey = NAME_None;
	UPROPERTY(EditDefaultsOnly) FName DescriptionTextKey = NAME_None;
	/** kind: heal_25 / cleanse / barrier_20 / focus_25 / shock_interrupt / recall */
	UPROPERTY(EditDefaultsOnly) FName EffectKind = NAME_None;
	UPROPERTY(EditDefaultsOnly) int32 Charges = 2;
	UPROPERTY(EditDefaultsOnly) float Magnitude = 0.f;
	UPROPERTY(EditDefaultsOnly) float Range = 0.f;
};
