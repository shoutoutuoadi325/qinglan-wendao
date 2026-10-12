// Blessing library: the 12 expedition blessings (4 groups x 3), their stable
// IDs, group membership, modifiers, and one-stack eligibility. Pure data +
// pure functions; the seeded draft roll itself lives in UQinglanRunSubsystem
// (its own FRandomStream), never here. docs/01 §"core loop" (blessing draft),
// docs/03 §5, implementation-blueprint §2.6.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "QinglanBlessingLibrary.generated.h"

class UQinglanRunSubsystem;

/** Blessing group ids (stable ASCII). docs/03 §5: sword offense, casting
 *  economy, protection, elemental interactions. */
namespace QinglanBlessingGroups
{
	extern const FName SwordOffense;      // "sword_offense"
	extern const FName CastingEconomy;    // "casting_economy"
	extern const FName Protection;        // "protection"
	extern const FName ElementalInteractions; // "elemental_interactions"
}

/**
 * One blessing definition. Every blessing has a one-stack cap: once owned it
 * is never offered again this expedition (docs/03 §5: "A blessing cannot be
 * offered twice if it has a one-stack cap"; implementation-blueprint §2.6:
 * "one-stack-cap never re-offered"). All blessings expire at expedition end.
 */
USTRUCT(BlueprintType)
struct FQinglanBlessingDef
{
	GENERATED_BODY()

	/** Stable ASCII id, e.g. "b_sharp_edge". */
	UPROPERTY(BlueprintReadOnly) FName BlessingId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName GroupId = NAME_None;
	/** String-table keys: blessing.<id>.name / blessing.<id>.desc in ST_Game. */
	UPROPERTY(BlueprintReadOnly) FName NameTextKey = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName DescriptionTextKey = NAME_None;
	/** True for every shipped blessing; kept explicit for data parity. */
	UPROPERTY(BlueprintReadOnly) bool bOneStackCap = true;

	// --- Modifiers (percent values in [-100, 100] as fractions x100, e.g. -15 = -15%) ---
	/** Sword offense. */
	UPROPERTY(BlueprintReadOnly) float SwordDamagePercent = 0.f;   // docs/02 §4 (sword damage pool)
	UPROPERTY(BlueprintReadOnly) float SwordEnergyPerHitBonus = 0.f; // docs/02 §4: base 4/target, cap 8/swing
	UPROPERTY(BlueprintReadOnly) float SwordEnergyCapBonus = 0.f;
	/** Casting economy. */
	UPROPERTY(BlueprintReadOnly) float TechniqueCostPercent = 0.f; // docs/03 §4: min cost 1, round up
	UPROPERTY(BlueprintReadOnly) float TechniqueCooldownPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TechniqueRawDamagePercent = 0.f; // pre-mitigation raw, docs/03 §3
	/** Protection. */
	UPROPERTY(BlueprintReadOnly) float BarrierPointsBonus = 0.f;  // docs/02 §6 barriers
	UPROPERTY(BlueprintReadOnly) float DamageTakenPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float MaxHealthBonus = 0.f;      // expedition-local, docs/03 §5 expiry
	/** Elemental interactions. */
	UPROPERTY(BlueprintReadOnly) float ConductDamagePercent = 0.f; // docs/03 §4: reaction mods affect Conduct only
	UPROPERTY(BlueprintReadOnly) float BurnDurationBonus = 0.f;   // docs/02 §7 Burn (3 s base)
	UPROPERTY(BlueprintReadOnly) float WetDurationBonus = 0.f;    // docs/02 §7 Wet (5 s base)
};

/**
 * Aggregated modifiers. Categories are summed within themselves, then applied
 * exactly once by the consumer ("add within category, apply once",
 * docs/03 §4; no hidden stacking with blessings).
 */
USTRUCT(BlueprintType)
struct FQinglanBlessingModifiers
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) float SwordDamagePercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float SwordEnergyPerHitBonus = 0.f;
	UPROPERTY(BlueprintReadOnly) float SwordEnergyCapBonus = 0.f;
	UPROPERTY(BlueprintReadOnly) float TechniqueCostPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TechniqueCooldownPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TechniqueRawDamagePercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float BarrierPointsBonus = 0.f;
	UPROPERTY(BlueprintReadOnly) float DamageTakenPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float MaxHealthBonus = 0.f;
	UPROPERTY(BlueprintReadOnly) float ConductDamagePercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float BurnDurationBonus = 0.f;
	UPROPERTY(BlueprintReadOnly) float WetDurationBonus = 0.f;
};

/**
 * Static registry + maths for blessings. Stateless and deterministic: no
 * random rolls here (the blessings FRandomStream is owned by the run
 * subsystem, blueprint §1.3). Player-facing text resolves from
 * /Game/Qinglan/Data/ST_Game via FText::FromStringTable only.
 */
UCLASS()
class UQinglanBlessingLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** All 12 blessing ids in canonical (group) order. */
	UFUNCTION(BlueprintPure) static void GetAllBlessingIds(TArray<FName>& OutIds);
	/** The 3 blessing ids of one group. */
	UFUNCTION(BlueprintPure) static void GetBlessingsInGroup(FName GroupId, TArray<FName>& OutIds);
	/** Definition lookup; false when the id is not one of the 12. */
	UFUNCTION(BlueprintPure) static bool FindBlessingDef(FName BlessingId, FQinglanBlessingDef& OutDef);
	/** Displays name+description through the string table (never hardcoded). */
	UFUNCTION(BlueprintPure) static void GetBlessingText(const FQinglanBlessingDef& Def, FText& OutName, FText& OutDescription);

	/**
	 * Blessings owned by the active expedition. Read through the run
	 * subsystem's checkpoint export (the checkpoint payload owns "acquired
	 * blessings", blueprint §3 save.json contract); empty outside expeditions.
	 */
	UFUNCTION(BlueprintPure) static void GetOwnedBlessings(UObject* WorldContext, TArray<FName>& OutOwned);
	/** False when already owned (one-stack cap) or unknown. docs/03 §5. */
	UFUNCTION(BlueprintPure) static bool IsBlessingOfferable(FName BlessingId, const TArray<FName>& OwnedBlessingIds);
	/** Sums all owned blessings into one modifier bundle. */
	UFUNCTION(BlueprintPure) static void ComputeModifiers(const TArray<FName>& OwnedBlessingIds, FQinglanBlessingModifiers& OutModifiers);

	/** Resolves id -> definition + localized display texts for the run
	 *  subsystem's offer UI struct; false for unknown ids. */
	static bool MakeBlessingEntry(FName BlessingId, struct FQinglanBlessing& OutEntry);
};
