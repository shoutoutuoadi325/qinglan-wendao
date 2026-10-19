// Economy library: pure, stateless payout / banking / catalog lookups for the
// progression economy. All tunables cite docs/03-progression-and-economy.md
// sections 2-5 and docs/dev/implementation-blueprint.md section 1.5. These
// functions are also the automation-test hooks required by code-contracts.md
// ("rank costs, banking multipliers" specs must be deterministic and headless).
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanEconomyLibrary.generated.h"

/** One passive manual from the fixed catalog of eight. docs/03 section 4. */
USTRUCT(BlueprintType)
struct FQinglanPassiveCatalogEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName PassiveId = NAME_None;
	/** String-table key inside /Game/Qinglan/Data/ST_Game; never a display string. */
	UPROPERTY(BlueprintReadOnly) FName NameTextKey = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName DescriptionTextKey = NAME_None;
	/** EnergyRecovery: +1 energy per second. */
	UPROPERTY(BlueprintReadOnly) float EnergyRegenPerSecond = 0.f;
	/** SteadyFoundation: +10 flat health. */
	UPROPERTY(BlueprintReadOnly) float MaxHealthBonus = 0.f;
	/** GroundedSteps: +10 flat stamina. */
	UPROPERTY(BlueprintReadOnly) float MaxStaminaBonus = 0.f;
	/** SwordDiscipline: +10% sword damage (additive within the category). */
	UPROPERTY(BlueprintReadOnly) float SwordDamagePercent = 0.f;
	/** QuietMind: -10% technique energy cost. */
	UPROPERTY(BlueprintReadOnly) float TechniqueCostReductionPercent = 0.f;
	/** HerbalKnowledge: +10% healing. */
	UPROPERTY(BlueprintReadOnly) float HealingPercent = 0.f;
	/** ElementalInsight: +10% reaction damage; affects Conduct only. */
	UPROPERTY(BlueprintReadOnly) float ConductDamagePercent = 0.f;
	/** MeasuredBreath: +10% stamina recovery. */
	UPROPERTY(BlueprintReadOnly) float StaminaRegenPercent = 0.f;
};

/** One talisman from the fixed catalog of six; expedition charges, not inventory. docs/03 section 5. */
USTRUCT(BlueprintType)
struct FQinglanTalismanCatalogEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName TalismanId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName NameTextKey = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName DescriptionTextKey = NAME_None;
	/** Mirrors UQinglanTalismanDataAsset::EffectKind: heal_25 / cleanse / barrier_20 / focus_25 / shock_interrupt / recall. */
	UPROPERTY(BlueprintReadOnly) FName EffectKind = NAME_None;
	/** Expedition charges; refilled for free at the sanctuary. docs/03 section 5. */
	UPROPERTY(BlueprintReadOnly) int32 Charges = 0;
	/** Health healed, barrier points, or energy restored. */
	UPROPERTY(BlueprintReadOnly) float Magnitude = 0.f;
	/** Shock Talisman interrupt radius in cm (300). */
	UPROPERTY(BlueprintReadOnly) float Range = 0.f;
};

/**
 * One permanent relic from the fixed catalog of six. Each grants one modest
 * element-related bonus plus one clear tradeoff (docs/03 section 5: "one
 * modest element-related bonus and a clear tradeoff or cooldown"). Modifiers
 * fold into combat maths through the same surfaces as blessing modifiers.
 */
USTRUCT(BlueprintType)
struct FQinglanRelicDef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName RelicId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName NameTextKey = NAME_None;
	UPROPERTY(BlueprintReadOnly) FName DescriptionTextKey = NAME_None;
	/** Purchase price in Vein Crystal at the sanctuary keeper. */
	UPROPERTY(BlueprintReadOnly) int32 VeinCrystalCost = 0;
	/** Positive effect folded into combat maths while equipped. */
	UPROPERTY(BlueprintReadOnly) float BonusBarrierPoints = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusMaxHealth = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusConductDamagePercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusBurnDuration = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusWetDuration = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusHealingPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float BonusSwordDamagePercent = 0.f;
	/** Tradeoff folded the same way (positive number = the penalty magnitude). */
	UPROPERTY(BlueprintReadOnly) float TradeoffDamageTakenPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TradeoffTechniqueCostPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TradeoffTechniqueCooldownPercent = 0.f;
	UPROPERTY(BlueprintReadOnly) float TradeoffSwordDamagePercent = 0.f;
};

/**
 * Static economy helpers. Nothing here mutates profile state; payouts reach the
 * inventory only through UQinglanProfileSubsystem::TryGrant so every award stays
 * atomic and idempotent (docs/03 section 6).
 */
UCLASS()
class QINGLANASCENSION_API UQinglanEconomyLibrary : public UBlueprintFunctionLibrary // QINGLANASCENSION_API: linkage-only export for the editor automation specs (rank costs, banking multipliers); public API unchanged.
{
	GENERATED_BODY()

public:
	// --- Banking multipliers (docs/03 section 6; blueprint section 1.5) ---
	/** victory 1.0 / voluntary retreat 0.7 / defeat 0.5. */
	static constexpr float BankingMultiplierVictory = 1.0f;
	static constexpr float BankingMultiplierVoluntaryRetreat = 0.7f;
	static constexpr float BankingMultiplierDefeat = 0.5f;

	/** Outcome multiplier: victory 100%, voluntary retreat 70%, anything else (defeat) 50%. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static float GetBankingMultiplier(bool bVictory, bool bVoluntaryRetreat);

	/** Applies the banking multiplier, floored per resource stack. Boss proof flags never pass through here and are never lost. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static FQinglanInventory ApplyBankingMultiplier(const FQinglanInventory& UnbankedLoot, bool bVictory, bool bVoluntaryRetreat);

	// --- Expedition payouts (docs/03 section 2 table) ---
	static constexpr int64 TutorialPayoutShards = 80;   // guided tutorial: 80 Shards + 1 Herb
	static constexpr int64 TutorialPayoutHerbs = 1;
	static constexpr int64 BambooBaseShards = 180;      // base is the expedition total incl. room rewards
	static constexpr int64 BambooFirstClearBonusShards = 120;
	static constexpr int64 BambooFirstClearHerbs = 3;
	static constexpr int64 BambooRepeatHerbs = 2;
	static constexpr int64 BambooCrystals = 1;          // 1 Crystal on first and repeat clears
	static constexpr int64 AshenBaseShards = 300;
	static constexpr int64 AshenFirstClearBonusShards = 200;
	static constexpr int64 AshenFirstClearCrystals = 3;
	static constexpr int64 AshenRepeatCrystals = 2;

	/** Stable realm ids, mirror data/examples/realm.json and localization realm.* keys. */
	static constexpr const TCHAR* BambooRealmIdText = TEXT("bamboo_hollow");
	static constexpr const TCHAR* AshenRealmIdText = TEXT("ashen_peak");
	static constexpr const TCHAR* TutorialRealmIdText = TEXT("tutorial_guided");

	/**
	 * Full banked payout for one cleared realm: base payout plus the separate,
	 * idempotent first-clear bonus when bFirstClear is true (the caller checks
	 * profile history via MakeFirstClearTransactionId + WasTransactionCommitted).
	 * Returns false for unknown realms.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static bool GetExpeditionPayout(FName RealmId, bool bFirstClear, FQinglanInventory& OutPayout);

	/** Guided tutorial payout: 80 Shards + 1 Spirit Herb. docs/03 section 2. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void GetTutorialPayout(FQinglanInventory& OutPayout);

	/** Boss id owning the first-clear bonus key for a realm (bamboo_guardian / ashen_guardian). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static FName GetBossIdForRealm(FName RealmId);

	// --- Stable transaction ids (docs/03 section 6: profile + expedition + room + category) ---
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static FName MakeRewardTransactionId(FName ProfileId, FName ExpeditionId, int32 RoomIndex, FName Category);
	/** First-clear bonus idempotency key, keyed by boss + profile. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static FName MakeFirstClearTransactionId(FName ProfileId, FName BossId);

	// --- Rank costs and cultivation math (docs/03 section 1; test hooks) ---
	UFUNCTION(BlueprintPure, Category = "Qinglan|Cultivation")
	static bool GetRankCost(EQinglanStage Stage, int32 RankIndex, int64& OutShards, int64& OutMaterialCount, FName& OutMaterialId);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Cultivation")
	static void GetCultivationBonuses(EQinglanStage Stage, int32 Rank, float& OutMaxHealthBonus, float& OutMaxEnergyBonus, float& OutTechniquePowerBonus);

	/** Tier-2 upgrades exist only from Foundation Establishment onward. docs/03 sections 1 and 3. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static bool IsTier2Available(EQinglanStage Stage);

	// --- Technique tier-2 upgrades (docs/03 section 3) ---
	/** 100 Shards + 1 Vein Crystal; cooldown and cast timing stay unchanged. */
	static constexpr int64 Tier2ShardCost = 100;
	static constexpr int64 Tier2CrystalCost = 1;
	/** +15% damage or healing magnitude (barriers: +15% barrier amount); no tier 3 at launch. */
	static constexpr float Tier2MagnitudeMultiplier = 1.15f;
	static constexpr int32 MaxTechniqueTier = 2;

	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void GetTier2UpgradeCost(FQinglanInventory& OutCost);

	/** Magnitude multiplier per tier: tier 1 = 1.0, tier 2 = 1.15, anything else = 1.0 (no tier 3). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static float GetTechniqueTierMultiplier(int32 Tier);

	// --- Bonus stacking rule (docs/03 section 4) ---
	/**
	 * Energy cost after summed cost-reduction percents: add within the category,
	 * apply once, round up, minimum 1 for an energy-spending skill.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static int32 ComputeTechniqueEnergyCost(int32 BaseEnergyCost, float TotalCostReductionPercent);

	// --- Fixed catalogs (docs/03 sections 4 and 5; blueprint section 2.6) ---
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static TArray<FQinglanPassiveCatalogEntry> GetPassiveCatalog();

	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static bool FindPassive(FName PassiveId, FQinglanPassiveCatalogEntry& OutEntry);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static TArray<FQinglanTalismanCatalogEntry> GetTalismanCatalog();

	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static bool FindTalisman(FName TalismanId, FQinglanTalismanCatalogEntry& OutEntry);

	/** Six permanent relics, names only (Jade Pendant, Ember Bead, River Pearl, Thunder Charm, Mountain Token, Verdant Seed). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static TArray<FName> GetRelicCatalog();

	// --- Permanent relics (docs/03 section 5; blueprint section 2.6) ---
	/** Full definitions with bonuses/tradeoffs; ids mirror GetRelicCatalog. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static TArray<FQinglanRelicDef> GetRelicDefs();

	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static bool FindRelic(FName RelicId, FQinglanRelicDef& OutEntry);

	/**
	 * Modifiers granted by one equipped relic, expressed on the blessing
	 * modifier surface so call sites fold them with the same maths. Tradeoffs
	 * apply as positive penalty percents/durations. No relic (NAME_None): zero.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void ComputeRelicModifiers(FName EquippedRelicId, struct FQinglanBlessingModifiers& OutModifiers);

	// --- Permanent-equipment folding (docs/03 sections 4-5) ---
	/**
	 * Adds the equipped relic (bonuses + tradeoffs) and the equipped passives
	 * onto an existing modifier bundle in place. Callers pass the bundle that
	 * already carries the expedition blessings, then apply the combined totals
	 * exactly once ("add within category, apply once", docs/03 section 4).
	 * No profile subsystem (headless tests): no-op.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void AppendPermanentModifiers(UObject* WorldContext, struct FQinglanBlessingModifiers& InOutModifiers);

	/**
	 * Folds permanent-equipment regen/healing modifiers onto base rates in
	 * place: EnergyRegen += Energy Recovery's flat per-second bonus;
	 * StaminaRegen *= (1 + Measured Breath's recovery percent / 100);
	 * HealingPercent += the equipped relic's healing percent (Verdant Seed).
	 * Pass the authored base constants in; no profile subsystem: no-op.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void AppendPermanentRegen(UObject* WorldContext, float& EnergyRegen, float& StaminaRegen, float& HealingPercent);

	/**
	 * AUD-37: permanent-equipment contributions to the derived maxima, for the
	 * combat component's attribute recomputation. Grounded Steps adds flat
	 * MaxStamina; Steady Foundation and Mountain Token add flat MaxHealth.
	 * No profile subsystem (headless tests/enemies): outputs stay 0.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Economy")
	static void GetPermanentAttributeBonuses(UObject* WorldContext, float& OutMaxHealthBonus, float& OutMaxStaminaBonus);
};
