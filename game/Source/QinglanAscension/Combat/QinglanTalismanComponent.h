// Talisman component: the two equipment slots behind the Q/R bindings
// (docs/02-combat-and-camera.md section 1) driving the six fixed talismans of
// docs/03-progression-and-economy.md section 5. Talismans carry expedition
// charges in the profile, not inventory: using a slot resolves the effect
// against the owner's UQinglanCombatComponent and consumes exactly one charge
// through UQinglanProfileSubsystem::TryConsumeTalismanCharge on success.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanTalismanComponent.generated.h"

class UQinglanCombatComponent;
class UQinglanProfileSubsystem;

/** Fired after one successful use; charges are read through the profile. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanTalismanUsed, FName, TalismanId, int32, RemainingCharges);

/**
 * Player talisman slots. Owns no game rules beyond effect execution; charges,
 * refills, and persistence live in UQinglanProfileSubsystem (docs/03 section 5:
 * "Refill at the sanctuary for free ... cannot restore their own charges").
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class QINGLANASCENSION_API UQinglanTalismanComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanTalismanComponent();

	/**
	 * Replaces both slots. Every id must be catalog-valid, duplicate-free, and
	 * (when a profile is present) carry charges; at most
	 * QinglanProgression::EquippedTalismanLimit entries (docs/03 section 5).
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Talisman")
	bool SetEquippedTalismans(const TArray<FName>& TalismanIds);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Talisman")
	TArray<FName> GetEquippedTalismans() const { return EquippedTalismans; }

	/**
	 * Uses the talisman bound to one slot (0/1). Consumes one profile charge
	 * only when the effect actually fires; rejected uses waste nothing.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Talisman")
	bool UseSlot(int32 Slot);

	UPROPERTY(BlueprintAssignable) FQinglanTalismanUsed OnTalismanUsed;

	// --- Tunables (docs/03 section 5 authors points/charges only) ---
	/** barrier_20 duration; docs give no lifetime, so it mirrors flowing_ward's 5 s (docs/02 section 6). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Talisman") float BarrierDurationSeconds = 5.f;
	/**
	 * Damage riding shock_interrupt. The stagger gate requires at least
	 * QinglanAITunables::StaggerMinimumDamage to interrupt, so the shock rides
	 * the existing TakeDamage/stagger entry point with this authored amount.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Talisman") float ShockDamage = 10.f;

protected:
	virtual void BeginPlay() override;

private:
	/** Executes one catalog effect against the owner; false rejects the use (no charge consumed). */
	bool ExecuteEffect(const struct FQinglanTalismanCatalogEntry& Entry, AActor* Owner, UQinglanCombatComponent& Combat);
	UQinglanProfileSubsystem* GetProfile() const;

	/** Slot contents in binding order (docs/02 section 1: slots Q/R). */
	TArray<FName> EquippedTalismans;
};
