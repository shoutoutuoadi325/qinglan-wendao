// Economy library implementation: pure stateless lookups, fully deterministic
// (no RNG anywhere; the only randomness in expeditions lives in the
// FRandomStream instances owned by UQinglanRunSubsystem).
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProgressionTables.h"

namespace
{
	/** Catalog order follows docs/03 section 4 exactly (8 passives). */
	const TArray<FQinglanPassiveCatalogEntry>& GetPassiveCatalogImpl()
	{
		static const TArray<FQinglanPassiveCatalogEntry> Catalog =
		{
			// Energy Recovery (+1 energy/s) - docs/03 section 4
			{ FName(TEXT("energy_recovery")), FName(TEXT("passive.energy_recovery.name")), FName(TEXT("passive.energy_recovery.desc")),
			  1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f },
			// Steady Foundation (+10 health)
			{ FName(TEXT("steady_foundation")), FName(TEXT("passive.steady_foundation.name")), FName(TEXT("passive.steady_foundation.desc")),
			  0.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f },
			// Sword Discipline (+10% sword damage)
			{ FName(TEXT("sword_discipline")), FName(TEXT("passive.sword_discipline.name")), FName(TEXT("passive.sword_discipline.desc")),
			  0.f, 0.f, 0.f, 10.f, 0.f, 0.f, 0.f, 0.f },
			// Quiet Mind (-10% technique energy cost)
			{ FName(TEXT("quiet_mind")), FName(TEXT("passive.quiet_mind.name")), FName(TEXT("passive.quiet_mind.desc")),
			  0.f, 0.f, 0.f, 0.f, 10.f, 0.f, 0.f, 0.f },
			// Grounded Steps (+10 stamina)
			{ FName(TEXT("grounded_steps")), FName(TEXT("passive.grounded_steps.name")), FName(TEXT("passive.grounded_steps.desc")),
			  0.f, 0.f, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f },
			// Herbal Knowledge (+10% healing)
			{ FName(TEXT("herbal_knowledge")), FName(TEXT("passive.herbal_knowledge.name")), FName(TEXT("passive.herbal_knowledge.desc")),
			  0.f, 0.f, 0.f, 0.f, 0.f, 10.f, 0.f, 0.f },
			// Elemental Insight (+10% reaction damage; Conduct only)
			{ FName(TEXT("elemental_insight")), FName(TEXT("passive.elemental_insight.name")), FName(TEXT("passive.elemental_insight.desc")),
			  0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 10.f, 0.f },
			// Measured Breath (+10% stamina recovery)
			{ FName(TEXT("measured_breath")), FName(TEXT("passive.measured_breath.name")), FName(TEXT("passive.measured_breath.desc")),
			  0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 10.f },
		};
		return Catalog;
	}

	/** Catalog order follows docs/03 section 5 exactly (6 talismans). */
	const TArray<FQinglanTalismanCatalogEntry>& GetTalismanCatalogImpl()
	{
		static const TArray<FQinglanTalismanCatalogEntry> Catalog =
		{
			// Healing Talisman: 25 health, 2 charges - docs/03 section 5
			{ FName(TEXT("healing_talisman")), FName(TEXT("talisman.healing_talisman.name")), FName(TEXT("talisman.healing_talisman.desc")),
			  FName(TEXT("heal_25")), 2, 25.f, 0.f },
			// Cleansing Talisman: remove Burn/Root, 2 charges
			{ FName(TEXT("cleansing_talisman")), FName(TEXT("talisman.cleansing_talisman.name")), FName(TEXT("talisman.cleansing_talisman.desc")),
			  FName(TEXT("cleanse")), 2, 0.f, 0.f },
			// Protective Talisman: 20 barrier, 2 charges
			{ FName(TEXT("protective_talisman")), FName(TEXT("talisman.protective_talisman.name")), FName(TEXT("talisman.protective_talisman.desc")),
			  FName(TEXT("barrier_20")), 2, 20.f, 0.f },
			// Focus Talisman: restore 25 energy, 2 charges
			{ FName(TEXT("focus_talisman")), FName(TEXT("talisman.focus_talisman.name")), FName(TEXT("talisman.focus_talisman.desc")),
			  FName(TEXT("focus_25")), 2, 25.f, 0.f },
			// Shock Talisman: interrupt ordinary enemies in 300 cm, 1 charge
			{ FName(TEXT("shock_talisman")), FName(TEXT("talisman.shock_talisman.name")), FName(TEXT("talisman.shock_talisman.desc")),
			  FName(TEXT("shock_interrupt")), 1, 0.f, 300.f },
			// Recall Talisman: request retreat from a cleared room, 1 charge
			{ FName(TEXT("recall_talisman")), FName(TEXT("talisman.recall_talisman.name")), FName(TEXT("talisman.recall_talisman.desc")),
			  FName(TEXT("recall")), 1, 0.f, 0.f },
		};
		return Catalog;
	}

	/** Six permanent relics, docs/03 section 5 (names only; bonuses authored later). */
	const TArray<FName>& GetRelicCatalogImpl()
	{
		static const TArray<FName> Catalog =
		{
			FName(TEXT("jade_pendant")),
			FName(TEXT("ember_bead")),
			FName(TEXT("river_pearl")),
			FName(TEXT("thunder_charm")),
			FName(TEXT("mountain_token")),
			FName(TEXT("verdant_seed")),
		};
		return Catalog;
	}
} // namespace

float UQinglanEconomyLibrary::GetBankingMultiplier(bool bVictory, bool bVoluntaryRetreat)
{
	// docs/03 section 6 + blueprint section 1.5: victory 100%, voluntary retreat
	// 70% (floored per stack), defeat 50%.
	if (bVictory)
	{
		return BankingMultiplierVictory;
	}
	return bVoluntaryRetreat ? BankingMultiplierVoluntaryRetreat : BankingMultiplierDefeat;
}

FQinglanInventory UQinglanEconomyLibrary::ApplyBankingMultiplier(const FQinglanInventory& UnbankedLoot, bool bVictory, bool bVoluntaryRetreat)
{
	const double Multiplier = static_cast<double>(GetBankingMultiplier(bVictory, bVoluntaryRetreat));
	FQinglanInventory Result;
	// Floored per stack so a multiplier never mints fractional currency; boss
	// proof flags are separate campaign flags and never pass through here.
	Result.SpiritShards = static_cast<int64>(FMath::FloorToDouble(static_cast<double>(UnbankedLoot.SpiritShards) * Multiplier));
	Result.SpiritHerb = static_cast<int64>(FMath::FloorToDouble(static_cast<double>(UnbankedLoot.SpiritHerb) * Multiplier));
	Result.VeinCrystal = static_cast<int64>(FMath::FloorToDouble(static_cast<double>(UnbankedLoot.VeinCrystal) * Multiplier));
	return Result;
}

bool UQinglanEconomyLibrary::GetExpeditionPayout(FName RealmId, bool bFirstClear, FQinglanInventory& OutPayout)
{
	OutPayout = FQinglanInventory();
	if (RealmId == FName(BambooRealmIdText))
	{
		// docs/03 section 2: first 180 base + 120 first-clear bonus; 3 Herbs; 1 Crystal.
		// Repeat: 180 base; 2 Herbs; 1 Crystal. Base already includes room rewards.
		OutPayout.SpiritShards = BambooBaseShards + (bFirstClear ? BambooFirstClearBonusShards : 0);
		OutPayout.SpiritHerb = bFirstClear ? BambooFirstClearHerbs : BambooRepeatHerbs;
		OutPayout.VeinCrystal = BambooCrystals;
		return true;
	}
	if (RealmId == FName(AshenRealmIdText))
	{
		// docs/03 section 2: first 300 base + 200 first-clear bonus; 3 Crystals.
		// Repeat: 300 base; 2 Crystals.
		OutPayout.SpiritShards = AshenBaseShards + (bFirstClear ? AshenFirstClearBonusShards : 0);
		OutPayout.VeinCrystal = bFirstClear ? AshenFirstClearCrystals : AshenRepeatCrystals;
		return true;
	}
	return false;
}

void UQinglanEconomyLibrary::GetTutorialPayout(FQinglanInventory& OutPayout)
{
	// docs/03 section 2: guided tutorial pays 80 Shards + 1 Herb (enough for the
	// first cultivation rank at 80 Shards + 1 Herb).
	OutPayout = FQinglanInventory();
	OutPayout.SpiritShards = TutorialPayoutShards;
	OutPayout.SpiritHerb = TutorialPayoutHerbs;
}

FName UQinglanEconomyLibrary::GetBossIdForRealm(FName RealmId)
{
	if (RealmId == FName(BambooRealmIdText)) { return FName(TEXT("bamboo_guardian")); }
	if (RealmId == FName(AshenRealmIdText)) { return FName(TEXT("ashen_guardian")); }
	return NAME_None;
}

FName UQinglanEconomyLibrary::MakeRewardTransactionId(FName ProfileId, FName ExpeditionId, int32 RoomIndex, FName Category)
{
	// docs/03 section 6: "A reward transaction has a stable ID derived from
	// profile, expedition, room, and reward category." Deterministic ASCII only.
	const FString Id = FString::Printf(TEXT("tx.%s.%s.room%02d.%s"),
		*ProfileId.ToString(), *ExpeditionId.ToString(), RoomIndex, *Category.ToString());
	return FName(*Id);
}

FName UQinglanEconomyLibrary::MakeFirstClearTransactionId(FName ProfileId, FName BossId)
{
	// First-clear bonus is separate and idempotent, keyed by boss + profile
	// (docs/03 section 2 / task gate contract).
	return FName(*FString::Printf(TEXT("tx.%s.first_clear.%s"), *ProfileId.ToString(), *BossId.ToString()));
}

bool UQinglanEconomyLibrary::GetRankCost(EQinglanStage Stage, int32 RankIndex, int64& OutShards, int64& OutMaterialCount, FName& OutMaterialId)
{
	FQinglanRankCost Cost;
	if (!QinglanProgression::GetRankCostFor(Stage, RankIndex, Cost))
	{
		OutShards = 0;
		OutMaterialCount = 0;
		OutMaterialId = NAME_None;
		return false;
	}
	OutShards = Cost.SpiritShards;
	OutMaterialCount = Cost.MaterialCount;
	OutMaterialId = Cost.MaterialId;
	return true;
}

void UQinglanEconomyLibrary::GetCultivationBonuses(EQinglanStage Stage, int32 Rank, float& OutMaxHealthBonus, float& OutMaxEnergyBonus, float& OutTechniquePowerBonus)
{
	QinglanProgression::GetCultivationBonuses(Stage, Rank, OutMaxHealthBonus, OutMaxEnergyBonus, OutTechniquePowerBonus);
}

bool UQinglanEconomyLibrary::IsTier2Available(EQinglanStage Stage)
{
	return QinglanProgression::IsTier2Available(Stage);
}

void UQinglanEconomyLibrary::GetTier2UpgradeCost(FQinglanInventory& OutCost)
{
	// docs/03 section 3: optional tier 2 costs 100 Shards + 1 Crystal.
	OutCost = FQinglanInventory();
	OutCost.SpiritShards = Tier2ShardCost;
	OutCost.VeinCrystal = Tier2CrystalCost;
}

float UQinglanEconomyLibrary::GetTechniqueTierMultiplier(int32 Tier)
{
	// No tier 3 at launch (docs/03 section 3); unknown tiers fall back to 1.0
	// so a data error can never inflate magnitude.
	if (Tier == 2)
	{
		return Tier2MagnitudeMultiplier;
	}
	return 1.f;
}

int32 UQinglanEconomyLibrary::ComputeTechniqueEnergyCost(int32 BaseEnergyCost, float TotalCostReductionPercent)
{
	// docs/03 section 4: add bonuses within a category, apply the category total
	// once, round up, minimum 1 for an energy-spending skill.
	const float ClampedReduction = FMath::Clamp(TotalCostReductionPercent, 0.f, 100.f);
	const float Reduced = static_cast<float>(BaseEnergyCost) * (1.f - ClampedReduction / 100.f);
	return FMath::Max(1, FMath::CeilToInt(Reduced));
}

TArray<FQinglanPassiveCatalogEntry> UQinglanEconomyLibrary::GetPassiveCatalog()
{
	return GetPassiveCatalogImpl();
}

bool UQinglanEconomyLibrary::FindPassive(FName PassiveId, FQinglanPassiveCatalogEntry& OutEntry)
{
	for (const FQinglanPassiveCatalogEntry& Entry : GetPassiveCatalogImpl())
	{
		if (Entry.PassiveId == PassiveId)
		{
			OutEntry = Entry;
			return true;
		}
	}
	return false;
}

TArray<FQinglanTalismanCatalogEntry> UQinglanEconomyLibrary::GetTalismanCatalog()
{
	return GetTalismanCatalogImpl();
}

bool UQinglanEconomyLibrary::FindTalisman(FName TalismanId, FQinglanTalismanCatalogEntry& OutEntry)
{
	for (const FQinglanTalismanCatalogEntry& Entry : GetTalismanCatalogImpl())
	{
		if (Entry.TalismanId == TalismanId)
		{
			OutEntry = Entry;
			return true;
		}
	}
	return false;
}

TArray<FName> UQinglanEconomyLibrary::GetRelicCatalog()
{
	return GetRelicCatalogImpl();
}
