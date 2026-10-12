// Progression tables: authoritative rank costs, breakthrough gate flags,
// cultivation attribute bonuses, and material/loadout constants. Every number
// below is fixed by docs/03-progression-and-economy.md section 1 and
// docs/dev/implementation-blueprint.md section 1.5; changing one requires a
// design-doc change first. The profile snapshot exchanged with the save
// subsystem is defined in Persistence/QinglanProfileSnapshot.h (save-side
// exchange types are owned by the Persistence folder, like FQinglanCheckpoint).
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"

/** One purchasable cultivation rank's price. docs/03 section 1 table. */
struct FQinglanRankCost
{
	/** Banked Spirit Shards consumed on purchase (never expedition loot). */
	int64 SpiritShards = 0;
	/** Stable material id ("spirit_herb"/"vein_crystal" style, save.json keys). */
	FName MaterialId = NAME_None;
	int64 MaterialCount = 0;
};

/** Rank costs, gate flags, and attribute math. docs/03 sections 1 and 3. */
namespace QinglanProgression
{
	/** Single source of every player-facing string. code-contracts.md "Localization keys". */
	inline constexpr const TCHAR* GameStringTableId = TEXT("/Game/Qinglan/Data/ST_Game");

	/** Three purchasable ranks per stage; Golden Core is rankless. docs/03 section 1. */
	inline constexpr int32 RanksPerStage = 3;

	/** Qi Refining rank shard costs, rank 1..3. docs/03 section 1 table. */
	inline constexpr int64 QiRefiningRankShardCosts[RanksPerStage] = { 80, 120, 180 };
	/** Foundation Establishment rank shard costs, rank 1..3. docs/03 section 1 table. */
	inline constexpr int64 FoundationRankShardCosts[RanksPerStage] = { 220, 300, 400 };
	/** Exactly one material per rank. docs/03 section 1 "Material per rank". */
	inline constexpr int64 MaterialCountPerRank = 1;

	/** Stable inventory ids, mirror data/examples/save.json inventory keys. */
	inline constexpr const TCHAR* SpiritShardIdText = TEXT("spirit_shards");
	inline constexpr const TCHAR* SpiritHerbIdText = TEXT("spirit_herb");
	inline constexpr const TCHAR* VeinCrystalIdText = TEXT("vein_crystal");

	/** 80+120+180+220+300+400; docs/03 section 1 "Total pre-final rank costs: 1,300". */
	inline constexpr int64 TotalPreFinalRankShardCost = 1300;

	/** Attribute gain per purchased rank. docs/03 section 1 "+5 MaxHealth and +2 TechniquePower each". */
	inline constexpr float MaxHealthPerRank = 5.f;
	inline constexpr float TechniquePowerPerRank = 2.f;
	/** Foundation breakthrough bonus; "MaxEnergy" is FQinglanAttributes::MaxSpiritualEnergy. docs/03 section 1. */
	inline constexpr float FoundationBreakthroughMaxHealth = 15.f;
	inline constexpr float FoundationBreakthroughMaxEnergy = 20.f;
	/** Golden Core breakthrough bonus (+ post-ending replay access). docs/03 section 1. */
	inline constexpr float GoldenCoreMaxHealth = 20.f;

	// --- Breakthrough gate flags -------------------------------------------------
	// Gameplay proof flags are banked at the boss safe-reward screen (docs/08
	// save matrix: "Story proof persists"); the story aliases are the scene
	// completion flags from docs/story/CH02.md, CH05.md, CH06.md. Both are
	// accepted so gates keep working regardless of which system banks first
	// (blueprint section 5 contradiction 3: flag ownership is split).
	inline constexpr const TCHAR* BambooClearedCampaignFlag = TEXT("boss_bamboo_guardian_cleared");
	inline constexpr const TCHAR* BambooClearedStoryFlag = TEXT("mq_ch02_bamboo_defeated"); // CH02_S02 completion
	inline constexpr const TCHAR* AshenClearedCampaignFlag = TEXT("boss_ashen_guardian_cleared");
	inline constexpr const TCHAR* AshenClearedStoryFlag = TEXT("mq_ashen_guard_disabled"); // CH05_S01 completion
	inline constexpr const TCHAR* FoundationTrialPassedCampaignFlag = TEXT("trial_foundation_passed");
	inline constexpr const TCHAR* FoundationTrialPassedStoryFlag = TEXT("mq_ch03_trial_passed"); // CH03 trial
	inline constexpr const TCHAR* GoldenCoreTrialWonCampaignFlag = TEXT("trial_golden_core_won");
	inline constexpr const TCHAR* GoldenCoreTrialWonStoryFlag = TEXT("mq_core_trial_won"); // CH06_S02 completion

	/** Markers set by the profile subsystem itself on a committed breakthrough. */
	inline constexpr const TCHAR* FoundationEstablishedFlag = TEXT("foundation_established");
	inline constexpr const TCHAR* GoldenCoreReachedFlag = TEXT("golden_core_reached");

	// --- Loadout limits (docs/03 section 3) -------------------------------------
	inline constexpr int32 EquippedTechniqueLimit = 4;
	inline constexpr int32 EquippedPassiveLimit = 2;
	inline constexpr int32 EquippedTalismanLimit = 2;
	inline constexpr int32 EquippedRelicLimit = 1;

	/** The eight shipped techniques, blueprint section 2.5 / docs/03 section 3. */
	inline constexpr const TCHAR* TechniqueIdTexts[8] =
	{
		TEXT("jade_sword_arc"),
		TEXT("ember_lance"),
		TEXT("flowing_ward"),
		TEXT("thunder_seal"),
		TEXT("stone_aegis"),
		TEXT("verdant_pulse"),
		TEXT("mist_step"),
		TEXT("orbiting_blades")
	};
	/** Techniques granted to a fresh profile during the tutorial. docs/03 section 3. */
	inline constexpr const TCHAR* StartingTechniqueIdTexts[2] = { TEXT("jade_sword_arc"), TEXT("flowing_ward") };

	/** Cost of purchasing RankIndex (0-based) inside Stage; false when the stage has no such rank. */
	inline bool GetRankCostFor(EQinglanStage Stage, int32 RankIndex, FQinglanRankCost& OutCost)
	{
		const int64* ShardTable = nullptr;
		const TCHAR* MaterialIdText = nullptr;
		switch (Stage)
		{
		case EQinglanStage::QiRefining:
			ShardTable = QiRefiningRankShardCosts;
			MaterialIdText = SpiritHerbIdText; // 1 Spirit Herb per Qi rank
			break;
		case EQinglanStage::FoundationEstablishment:
			ShardTable = FoundationRankShardCosts;
			MaterialIdText = VeinCrystalIdText; // 1 Vein Crystal per Foundation rank
			break;
		case EQinglanStage::GoldenCore:
		default:
			return false; // Golden Core is the campaign culmination, not a rank grind (docs/03 section 1)
		}
		if (ShardTable == nullptr || RankIndex < 0 || RankIndex >= RanksPerStage)
		{
			return false;
		}
		OutCost.SpiritShards = ShardTable[RankIndex];
		OutCost.MaterialId = FName(MaterialIdText);
		OutCost.MaterialCount = MaterialCountPerRank;
		return true;
	}

	/** Total ranks purchased across all stages (used for cumulative attributes). */
	inline int32 TotalPurchasedRanks(EQinglanStage Stage, int32 Rank)
	{
		switch (Stage)
		{
		case EQinglanStage::QiRefining:
			return Rank;
		case EQinglanStage::FoundationEstablishment:
			return RanksPerStage + Rank;
		case EQinglanStage::GoldenCore:
		default:
			return RanksPerStage * 2; // 3 Qi + 3 Foundation; Golden Core adds no ranks
		}
	}

	/**
	 * Cumulative permanent attribute bonuses from cultivation.
	 * docs/03 section 1: +5 MaxHealth/+2 TechniquePower per rank (6 ranks =
	 * +30/+12), Foundation breakthrough +15 MaxHealth/+20 MaxEnergy,
	 * Golden Core +20 MaxHealth.
	 */
	inline void GetCultivationBonuses(EQinglanStage Stage, int32 Rank,
		float& OutMaxHealthBonus, float& OutMaxEnergyBonus, float& OutTechniquePowerBonus)
	{
		const int32 TotalRanks = TotalPurchasedRanks(Stage, Rank);
		const bool bFoundationOrBeyond = (Stage == EQinglanStage::FoundationEstablishment || Stage == EQinglanStage::GoldenCore);
		const bool bGoldenCore = (Stage == EQinglanStage::GoldenCore);
		OutMaxHealthBonus = MaxHealthPerRank * static_cast<float>(TotalRanks)
			+ (bFoundationOrBeyond ? FoundationBreakthroughMaxHealth : 0.f)
			+ (bGoldenCore ? GoldenCoreMaxHealth : 0.f);
		OutMaxEnergyBonus = bFoundationOrBeyond ? FoundationBreakthroughMaxEnergy : 0.f;
		OutTechniquePowerBonus = TechniquePowerPerRank * static_cast<float>(TotalRanks);
	}

	/** Tier-2 technique upgrades unlock at Foundation Establishment. docs/03 sections 1 and 3. */
	inline bool IsTier2Available(EQinglanStage Stage)
	{
		return Stage == EQinglanStage::FoundationEstablishment || Stage == EQinglanStage::GoldenCore;
	}

	/** True when TechniqueId is one of the eight shipped techniques. */
	inline bool IsKnownTechniqueId(FName TechniqueId)
	{
		for (const TCHAR* Text : TechniqueIdTexts)
		{
			if (TechniqueId == FName(Text))
			{
				return true;
			}
		}
		return false;
	}

	/** Maps the stable material ids onto inventory fields; NAME_None yields 0. */
	inline int64 GetMaterialCount(const FQinglanInventory& Inventory, FName MaterialId)
	{
		if (MaterialId == FName(SpiritHerbIdText)) { return Inventory.SpiritHerb; }
		if (MaterialId == FName(VeinCrystalIdText)) { return Inventory.VeinCrystal; }
		if (MaterialId == FName(SpiritShardIdText)) { return Inventory.SpiritShards; }
		return 0;
	}

	/** Writes one material field; unknown ids are ignored (returns false). */
	inline bool SetMaterialCount(FQinglanInventory& Inventory, FName MaterialId, int64 Count)
	{
		if (MaterialId == FName(SpiritHerbIdText)) { Inventory.SpiritHerb = Count; return true; }
		if (MaterialId == FName(VeinCrystalIdText)) { Inventory.VeinCrystal = Count; return true; }
		if (MaterialId == FName(SpiritShardIdText)) { Inventory.SpiritShards = Count; return true; }
		return false;
	}
}
