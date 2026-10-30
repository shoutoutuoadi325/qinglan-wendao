// Profile subsystem implementation: the single authoritative mutation path for
// permanent state (code-contracts.md). Every mutation is atomic, journaled by
// transaction id, and safe to replay: an already-committed id is a no-op that
// reports success, so an autosave callback can never award a second payout
// (docs/03-progression-and-economy.md section 6). Game-thread only; the game is
// offline and single-profile-active, so no locking is required.
#include "Progression/QinglanProfileSubsystem.h"

#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveMigration.h"
#include "QinglanAscension.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	/** All player-facing strings resolve from the single game string table. */
	FText ProfileTableText(const TCHAR* Key)
	{
		return FText::FromStringTable(FName(QinglanProgression::GameStringTableId), Key);
	}

	/**
	 * Adds a non-negative delta to a bounded 64-bit counter.
	 * Rejects negative deltas, negative current values, and any addition that
	 * would overflow (docs/03 section 6: "reject negative grants and overflow").
	 */
	bool TryAddBounded(int64& Value, int64 Delta)
	{
		if (Delta < 0 || Value < 0)
		{
			return false;
		}
		if (Delta > MAX_int64 - Value)
		{
			return false;
		}
		Value += Delta;
		return true;
	}

	/** True when every cost component is non-negative and affordable. */
	bool IsAffordable(const FQinglanInventory& Inventory, const FQinglanInventory& Cost)
	{
		if (Cost.SpiritShards < 0 || Cost.SpiritHerb < 0 || Cost.VeinCrystal < 0)
		{
			return false;
		}
		return Inventory.SpiritShards >= Cost.SpiritShards
			&& Inventory.SpiritHerb >= Cost.SpiritHerb
			&& Inventory.VeinCrystal >= Cost.VeinCrystal;
	}

	/** Rebuilds a name set from a save array (TSet has no TArray constructor). */
	TSet<FName> ToNameSet(const TArray<FName>& Names)
	{
		TSet<FName> Set;
		Set.Reserve(Names.Num());
		for (const FName Name : Names)
		{
			Set.Add(Name);
		}
		return Set;
	}
} // namespace

void UQinglanProfileSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	ResetToFreshProfile();
}

void UQinglanProfileSubsystem::ResetToFreshProfile()
{
	// Fresh profile: Qi Refining rank 0, tutorial techniques granted and equipped
	// (docs/03 section 3 "Jade Sword Arc and Flowing Ward unlock during the
	// tutorial"; mirrors data/examples/save.json).
	Inventory = FQinglanInventory();
	Stage = EQinglanStage::QiRefining;
	Rank = 0;
	CampaignFlags.Reset();
	UnlockedTechniques.Reset();
	EquippedTechniques.Reset();
	TechniqueTiers.Reset();
	OwnedRelics.Reset();
	EquippedRelic = NAME_None;
	OwnedPassives.Reset();
	EquippedPassives.Reset();
	TalismanCharges.Reset();
	ResetTalismanLoadout();
	CommittedTransactionIds.Reset();
	ReadRecords.Reset();
	CompletedSideStages.Reset();
	for (const TCHAR* TechniqueText : QinglanProgression::StartingTechniqueIdTexts)
	{
		const FName TechniqueId(TechniqueText);
		UnlockedTechniques.Add(TechniqueId);
		EquippedTechniques.Add(TechniqueId);
	}
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const FQinglanInventory& UQinglanProfileSubsystem::GetInventory() const
{
	return Inventory;
}

EQinglanStage UQinglanProfileSubsystem::GetCultivationStage() const
{
	return Stage;
}

int32 UQinglanProfileSubsystem::GetCultivationRank() const
{
	return Rank;
}

bool UQinglanProfileSubsystem::HasCampaignFlag(FName Flag) const
{
	return CampaignFlags.Contains(Flag);
}

bool UQinglanProfileSubsystem::IsTechniqueUnlocked(FName TechniqueId) const
{
	return UnlockedTechniques.Contains(TechniqueId);
}

TArray<FName> UQinglanProfileSubsystem::GetEquippedTechniques() const
{
	return EquippedTechniques;
}

int32 UQinglanProfileSubsystem::GetTechniqueTier(FName TechniqueId) const
{
	const int32* Tier = TechniqueTiers.Find(TechniqueId);
	return Tier ? FMath::Clamp(*Tier, 1, UQinglanEconomyLibrary::MaxTechniqueTier) : 1;
}

float UQinglanProfileSubsystem::GetTechniqueTierMultiplier(FName TechniqueId) const
{
	return UQinglanEconomyLibrary::GetTechniqueTierMultiplier(GetTechniqueTier(TechniqueId));
}

TArray<FName> UQinglanProfileSubsystem::GetOwnedRelics() const
{
	return OwnedRelics.Array();
}

FName UQinglanProfileSubsystem::GetEquippedRelic() const
{
	return EquippedRelic;
}

TArray<FName> UQinglanProfileSubsystem::GetOwnedPassives() const
{
	return OwnedPassives.Array();
}

TArray<FName> UQinglanProfileSubsystem::GetEquippedPassives() const
{
	return EquippedPassives;
}

bool UQinglanProfileSubsystem::IsSideQuestStageComplete(FName StageId) const
{
	return CompletedSideStages.Contains(StageId);
}

bool UQinglanProfileSubsystem::HasReadRecord(FName RecordId) const
{
	return ReadRecords.Contains(RecordId);
}

bool UQinglanProfileSubsystem::GetNextRankCost(int64& OutShards, int64& OutExtraMaterialCount, FName& OutExtraMaterialId) const
{
	FQinglanRankCost Cost;
	if (!QinglanProgression::GetRankCostFor(Stage, Rank, Cost))
	{
		// Stage complete (breakthrough pending) or Golden Core (rankless).
		OutShards = 0;
		OutExtraMaterialCount = 0;
		OutExtraMaterialId = NAME_None;
		return false;
	}
	OutShards = Cost.SpiritShards;
	OutExtraMaterialCount = Cost.MaterialCount;
	OutExtraMaterialId = Cost.MaterialId;
	return true;
}

bool UQinglanProfileSubsystem::CanAttemptBreakthrough(FText& OutBlockingReason) const
{
	OutBlockingReason = FText::GetEmpty();
	return CheckBreakthroughGate(OutBlockingReason);
}

bool UQinglanProfileSubsystem::WasTransactionCommitted(FName TransactionId) const
{
	return CommittedTransactionIds.Contains(TransactionId);
}

// ---------------------------------------------------------------------------
// Authoritative mutations
// ---------------------------------------------------------------------------

bool UQinglanProfileSubsystem::TryGrant(FName TransactionId, const FQinglanInventory& Grant)
{
	if (TransactionId == NAME_None)
	{
		// docs/03 section 6: reward transactions carry stable ids; refuse keyless grants.
		UE_LOG(LogQinglan, Warning, TEXT("TryGrant rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		// Idempotent replay: committed once, never re-awarded.
		return true;
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}

	FQinglanInventory Candidate = Inventory;
	if (!TryAddBounded(Candidate.SpiritShards, Grant.SpiritShards)
		|| !TryAddBounded(Candidate.SpiritHerb, Grant.SpiritHerb)
		|| !TryAddBounded(Candidate.VeinCrystal, Grant.VeinCrystal))
	{
		// Negative component or 64-bit overflow: change nothing.
		return false;
	}

	Inventory = Candidate;
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryBankExpedition(FName BankingId, const FQinglanInventory& Grant,
	FName FirstClearId, const FQinglanInventory& FirstClearBonus)
{
	if (BankingId.IsNone() || BankingId == FirstClearId)
	{
		return false;
	}
	if (WasTransactionCommitted(BankingId))
	{
		return true;
	}
	const bool bFirstClear = !FirstClearId.IsNone() && !WasTransactionCommitted(FirstClearId);
	// Match every profile mutation and the save validator before awarding anything.
	if (CommittedTransactionIds.Num() > QinglanSave::MaxTransactionEntries - (bFirstClear ? 2 : 1))
	{
		return false;
	}
	FQinglanInventory Candidate = Inventory;
	const auto AddGrant = [&Candidate](const FQinglanInventory& Delta)
	{
		return TryAddBounded(Candidate.SpiritShards, Delta.SpiritShards)
			&& TryAddBounded(Candidate.SpiritHerb, Delta.SpiritHerb)
			&& TryAddBounded(Candidate.VeinCrystal, Delta.VeinCrystal);
	};
	if (!AddGrant(Grant) || (bFirstClear && !AddGrant(FirstClearBonus)))
	{
		return false;
	}
	Inventory = Candidate;
	CommittedTransactionIds.Add(BankingId);
	if (bFirstClear)
	{
		CommittedTransactionIds.Add(FirstClearId);
	}
	// Observers see both grants and both IDs, never a half-committed first clear.
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TrySpend(FName TransactionId, const FQinglanInventory& Cost)
{
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TrySpend rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		// Idempotent replay of an already-committed spend: no second deduction.
		return true;
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}
	if (!IsAffordable(Inventory, Cost))
	{
		// Insufficient resources (or a negative component): return false, change nothing.
		return false;
	}

	Inventory.SpiritShards -= Cost.SpiritShards;
	Inventory.SpiritHerb -= Cost.SpiritHerb;
	Inventory.VeinCrystal -= Cost.VeinCrystal;
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryAdvanceRank(FName TransactionId)
{
	// FIXME-CONTRACT: the scaffold header comment says TryAdvanceRank "validates
	// boss gates and materials", but docs/03 section 1 scopes boss gates to
	// *breakthroughs* only (the Foundation trial unlocks when Qi ranks are
	// already complete, so Qi ranks 2-3 must be purchasable before any boss
	// clear). Gates are therefore enforced in TryBreakthrough; rank purchases
	// validate materials only, exactly as docs/03 section 1 requires.
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryAdvanceRank rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // this rank was already purchased by the same transaction
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}

	FQinglanRankCost Cost;
	if (!QinglanProgression::GetRankCostFor(Stage, Rank, Cost))
	{
		return false; // all ranks of this stage purchased, or Golden Core (rankless)
	}

	FQinglanInventory Spend;
	Spend.SpiritShards = Cost.SpiritShards;
	QinglanProgression::SetMaterialCount(Spend, Cost.MaterialId, Cost.MaterialCount);
	if (!IsAffordable(Inventory, Spend))
	{
		// Insufficient banked resources: atomic rejection, no partial state
		// (docs/03 section 1 "insufficient resources change nothing"). Note
		// cultivation can never consume temporary expedition loot: unbanked loot
		// lives in UQinglanRunSubsystem and is not part of this inventory.
		return false;
	}

	Inventory.SpiritShards -= Spend.SpiritShards;
	Inventory.SpiritHerb -= Spend.SpiritHerb;
	Inventory.VeinCrystal -= Spend.VeinCrystal;
	++Rank; // +5 MaxHealth / +2 TechniquePower are derived, see QinglanProgression::GetCultivationBonuses
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnCultivationChanged.Broadcast(Stage, Rank);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryBreakthrough(FName TransactionId)
{
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryBreakthrough rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // this breakthrough already committed
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}

	FText BlockingReason;
	if (!CheckBreakthroughGate(BlockingReason))
	{
		// Gates (docs/dev/implementation-blueprint.md section 1.5):
		// Qi -> Foundation: Bamboo Guardian cleared + 3 Qi ranks + trial passed.
		// Foundation -> Golden Core: Ashen Guardian cleared + 3 Foundation ranks
		// + Golden Core trial passed. Trial entry itself is free (docs/03 section 1).
		return false;
	}

	// Mutate everything first; broadcasts fire only after the whole
	// transaction (state + journal) is complete, so a listener saving on
	// OnProfileCommitted can never capture a half-committed breakthrough.
	FName NewCampaignFlag = NAME_None;
	switch (Stage)
	{
	case EQinglanStage::QiRefining:
		Stage = EQinglanStage::FoundationEstablishment;
		Rank = 0;
		// docs/03 section 1: Foundation breakthrough gives +15 MaxHealth, +20
		// MaxEnergy, and unlocks tier-2 technique upgrades (tier-2 availability
		// is derived from stage). docs/03 section 3: Stone Aegis and Verdant
		// Pulse unlock at Foundation.
		NewCampaignFlag = FName(QinglanProgression::FoundationEstablishedFlag);
		UnlockedTechniques.Add(FName(TEXT("stone_aegis")));
		UnlockedTechniques.Add(FName(TEXT("verdant_pulse")));
		break;

	case EQinglanStage::FoundationEstablishment:
		Stage = EQinglanStage::GoldenCore;
		Rank = 0; // Golden Core is the campaign culmination, rankless (docs/03 section 1)
		// docs/03 section 1: Golden Core gives +20 MaxHealth and access to normal
		// post-ending replay (the flag carries replay access).
		NewCampaignFlag = FName(QinglanProgression::GoldenCoreReachedFlag);
		break;

	case EQinglanStage::GoldenCore:
	default:
		return false; // unreachable: the gate check above already rejected this
	}

	SetCampaignFlagInternal(NewCampaignFlag);
	CommittedTransactionIds.Add(TransactionId);
	OnCultivationChanged.Broadcast(Stage, Rank);
	if (NewCampaignFlag != NAME_None)
	{
		OnCampaignFlagSet.Broadcast(NewCampaignFlag);
	}
	OnProfileCommitted.Broadcast();
	return true;
}

void UQinglanProfileSubsystem::SetCampaignFlag(FName Flag)
{
	if (Flag == NAME_None)
	{
		return;
	}
	if (SetCampaignFlagInternal(Flag))
	{
		OnCampaignFlagSet.Broadcast(Flag);
		OnProfileCommitted.Broadcast();
	}
}

bool UQinglanProfileSubsystem::SetCampaignFlagInternal(FName Flag)
{
	if (Flag == NAME_None || CampaignFlags.Contains(Flag))
	{
		return false; // idempotent: already-banked proof
	}
	CampaignFlags.Add(Flag);
	return true;
}

bool UQinglanProfileSubsystem::UnlockTechnique(FName TransactionId, FName TechniqueId)
{
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("UnlockTechnique rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // idempotent replay of this grant
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}
	if (!QinglanProgression::IsKnownTechniqueId(TechniqueId))
	{
		UE_LOG(LogQinglan, Warning, TEXT("UnlockTechnique rejected: unknown technique id"));
		return false;
	}
	if (UnlockedTechniques.Contains(TechniqueId))
	{
		// Already owned (e.g. granted by a different transaction): commit the
		// transaction id so replays stay no-ops, but do not double-grant.
		CommittedTransactionIds.Add(TransactionId);
		OnProfileCommitted.Broadcast();
		return true;
	}

	UnlockedTechniques.Add(TechniqueId);
	CommittedTransactionIds.Add(TransactionId);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TrySetEquippedTechniques(const TArray<FName>& TechniqueIds)
{
	// docs/03 section 3: swapping is free but only at a sanctuary/entrance
	// shrine; never mid-expedition. (Mid-combat/cooldown-preserving rules are
	// enforced by the technique component at the shrine interaction itself.)
	const UGameInstance* GameInstance = GetGameInstance();
	UQinglanRunSubsystem* RunSubsystem = GameInstance ? GameInstance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (RunSubsystem && RunSubsystem->IsInExpedition())
	{
		return false;
	}

	if (TechniqueIds.Num() > QinglanProgression::EquippedTechniqueLimit)
	{
		return false; // at most four equipped techniques (docs/03 section 3)
	}

	TSet<FName> Seen;
	Seen.Reserve(TechniqueIds.Num());
	for (const FName TechniqueId : TechniqueIds)
	{
		if (!UnlockedTechniques.Contains(TechniqueId))
		{
			return false; // cannot equip a locked technique
		}
		if (Seen.Contains(TechniqueId))
		{
			return false; // no duplicate slots
		}
		Seen.Add(TechniqueId);
	}

	EquippedTechniques = TechniqueIds;
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::IsLoadoutLockedByExpedition() const
{
	// Shared gate for every loadout swap (techniques, relic, passives, talismans):
	// swapping is free but only at a sanctuary/entrance shrine, never
	// mid-expedition (docs/03 sections 3-5).
	const UGameInstance* GameInstance = GetGameInstance();
	const UQinglanRunSubsystem* RunSubsystem = GameInstance ? GameInstance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	return RunSubsystem != nullptr && RunSubsystem->IsInExpedition();
}

// ---------------------------------------------------------------------------
// Permanent equipment mutations (docs/03 sections 4-5)
// ---------------------------------------------------------------------------

bool UQinglanProfileSubsystem::TryPurchaseRelic(FName TransactionId, FName RelicId)
{
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchaseRelic rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // idempotent replay of this purchase
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}
	FQinglanRelicDef Def;
	if (!UQinglanEconomyLibrary::FindRelic(RelicId, Def))
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchaseRelic rejected: unknown relic id"));
		return false;
	}
	if (OwnedRelics.Contains(RelicId))
	{
		// Already owned (e.g. granted by a different transaction): commit the
		// transaction id so replays stay no-ops, but do not double-charge.
		CommittedTransactionIds.Add(TransactionId);
		OnProfileCommitted.Broadcast();
		return true;
	}

	FQinglanInventory Cost;
	Cost.VeinCrystal = Def.VeinCrystalCost; // sanctuary keeper prices in Vein Crystal (docs/03 section 5)
	if (!IsAffordable(Inventory, Cost))
	{
		// Insufficient banked crystals: atomic rejection, no partial state
		// (docs/03 section 6 "insufficient resources change nothing").
		return false;
	}

	Inventory.VeinCrystal -= Cost.VeinCrystal;
	OwnedRelics.Add(RelicId);
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryEquipRelic(FName RelicId)
{
	// docs/03 section 5: at most one equipped relic (EquippedRelicLimit);
	// equip changes happen at the sanctuary like every other loadout swap.
	if (IsLoadoutLockedByExpedition())
	{
		return false;
	}
	if (RelicId == NAME_None)
	{
		// Unequip is always legal (an empty loadout is a valid loadout).
		if (EquippedRelic == NAME_None)
		{
			return true; // already empty: no broadcast, no journal entry needed
		}
		EquippedRelic = NAME_None;
		OnProfileCommitted.Broadcast();
		return true;
	}
	if (!OwnedRelics.Contains(RelicId))
	{
		return false; // cannot equip an unbought relic
	}

	EquippedRelic = RelicId;
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TrySetEquippedPassives(const TArray<FName>& PassiveIds)
{
	// docs/03 section 4: "Eight passives with two equipped"; swap rules match
	// the technique loadout (sanctuary only, never mid-expedition).
	if (IsLoadoutLockedByExpedition())
	{
		return false;
	}
	if (PassiveIds.Num() > QinglanProgression::EquippedPassiveLimit)
	{
		return false; // at most two equipped passives (docs/03 section 4)
	}

	TSet<FName> Seen;
	Seen.Reserve(PassiveIds.Num());
	for (const FName PassiveId : PassiveIds)
	{
		if (!OwnedPassives.Contains(PassiveId))
		{
			return false; // cannot equip an unstudied manual
		}
		if (Seen.Contains(PassiveId))
		{
			return false; // no duplicate slots
		}
		Seen.Add(PassiveId);
	}

	EquippedPassives = PassiveIds;
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryPurchasePassive(FName TransactionId, FName PassiveId)
{
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchasePassive rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // idempotent replay of this purchase
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}
	FQinglanPassiveCatalogEntry Entry;
	if (!UQinglanEconomyLibrary::FindPassive(PassiveId, Entry))
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchasePassive rejected: unknown passive id"));
		return false;
	}
	if (OwnedPassives.Contains(PassiveId))
	{
		// Already studied (e.g. granted by a different transaction): commit the
		// transaction id so replays stay no-ops, but do not double-charge.
		CommittedTransactionIds.Add(TransactionId);
		OnProfileCommitted.Broadcast();
		return true;
	}

	// Authored study cost (docs/03 section 4 lists no prices): see the
	// PassiveStudyCost* constants beside the API declaration.
	FQinglanInventory Cost;
	Cost.SpiritShards = PassiveStudyCostShards;
	Cost.SpiritHerb = PassiveStudyCostHerbs;
	if (!IsAffordable(Inventory, Cost))
	{
		return false; // atomic rejection, no partial state
	}

	Inventory.SpiritShards -= Cost.SpiritShards;
	Inventory.SpiritHerb -= Cost.SpiritHerb;
	OwnedPassives.Add(PassiveId);
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

bool UQinglanProfileSubsystem::TryPurchaseTechniqueTier(FName TransactionId, FName TechniqueId)
{
	// AUD-37: tier-2 upgrades are optional purchases from Foundation
	// Establishment onward (docs/03 sections 1 and 3). Costs come from the
	// single economy table so UI previews and the charge agree.
	if (TransactionId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchaseTechniqueTier rejected: empty transaction id"));
		return false;
	}
	if (WasTransactionCommitted(TransactionId))
	{
		return true; // idempotent replay of this purchase
	}
	if (CommittedTransactionIds.Num() >= QinglanSave::MaxTransactionEntries)
	{
		return false; // never accept a mutation that would make the profile unsavable
	}
	if (!QinglanProgression::IsKnownTechniqueId(TechniqueId)
		|| !UnlockedTechniques.Contains(TechniqueId))
	{
		UE_LOG(LogQinglan, Warning, TEXT("TryPurchaseTechniqueTier rejected: unknown or locked technique"));
		return false;
	}
	if (!UQinglanEconomyLibrary::IsTier2Available(Stage))
	{
		return false; // Qi Refining has no tier-2 upgrades
	}

	if (GetTechniqueTier(TechniqueId) >= UQinglanEconomyLibrary::MaxTechniqueTier)
	{
		// Already upgraded (e.g. by an earlier transaction): commit the id so
		// replays stay no-ops, but never charge twice.
		CommittedTransactionIds.Add(TransactionId);
		OnProfileCommitted.Broadcast();
		return true;
	}

	FQinglanInventory Cost;
	UQinglanEconomyLibrary::GetTier2UpgradeCost(Cost);
	if (!IsAffordable(Inventory, Cost))
	{
		return false; // atomic rejection, no partial state
	}

	Inventory.SpiritShards -= Cost.SpiritShards;
	Inventory.SpiritHerb -= Cost.SpiritHerb;
	Inventory.VeinCrystal -= Cost.VeinCrystal;
	TechniqueTiers.Add(TechniqueId, UQinglanEconomyLibrary::MaxTechniqueTier);
	CommittedTransactionIds.Add(TransactionId);
	OnInventoryChanged.Broadcast(Inventory);
	OnProfileCommitted.Broadcast();
	return true;
}

// ---------------------------------------------------------------------------
// Talisman charges (docs/03 section 5)
// ---------------------------------------------------------------------------

void UQinglanProfileSubsystem::GetTalismanCharges(TMap<FName, int32>& OutCharges) const
{
	OutCharges = TalismanCharges;
}

int32 UQinglanProfileSubsystem::GetTalismanChargeCount(FName TalismanId) const
{
	const int32* Charges = TalismanCharges.Find(TalismanId);
	return Charges ? *Charges : 0;
}

void UQinglanProfileSubsystem::ResetTalismanLoadout()
{
	EquippedTalismans.Reset();
	// Defaults are independent of refill timing and remaining charges. A pawn
	// can already exist when the player selects a profile at the sanctuary.
	for (const FQinglanTalismanCatalogEntry& Entry : UQinglanEconomyLibrary::GetTalismanCatalog())
	{
		if (EquippedTalismans.Num() >= QinglanProgression::EquippedTalismanLimit) { break; }
		EquippedTalismans.Add(Entry.TalismanId);
	}
}

TArray<FName> UQinglanProfileSubsystem::GetEquippedTalismans() const
{
	return EquippedTalismans;
}

bool UQinglanProfileSubsystem::TrySetEquippedTalismans(const TArray<FName>& TalismanIds)
{
	if (IsLoadoutLockedByExpedition() || TalismanIds.Num() > QinglanProgression::EquippedTalismanLimit)
	{
		return false;
	}
	TSet<FName> Seen;
	for (const FName TalismanId : TalismanIds)
	{
		FQinglanTalismanCatalogEntry Entry;
		if (!UQinglanEconomyLibrary::FindTalisman(TalismanId, Entry) || Seen.Contains(TalismanId)
			|| (GetTalismanChargeCount(TalismanId) <= 0 && !EquippedTalismans.Contains(TalismanId)))
		{
			return false;
		}
		Seen.Add(TalismanId);
	}
	if (EquippedTalismans != TalismanIds)
	{
		EquippedTalismans = TalismanIds;
		OnProfileCommitted.Broadcast();
	}
	return true;
}

bool UQinglanProfileSubsystem::TryConsumeTalismanCharge(FName TalismanId)
{
	// Talismans carry expedition charges, not inventory (docs/03 section 5):
	// no transaction journal, but the mutation still routes through this
	// subsystem and broadcasts so a listener can persist it.
	int32* Charges = TalismanCharges.Find(TalismanId);
	if (Charges == nullptr || *Charges <= 0)
	{
		return false; // empty or unknown: nothing consumed
	}
	--*Charges;
	OnProfileCommitted.Broadcast();
	return true;
}

void UQinglanProfileSubsystem::RefillTalismans()
{
	// Sanctuary arrival refills every catalog talisman for free (docs/03
	// section 5); talismans cannot restore their own charges elsewhere.
	bool bChanged = false;
	for (const FQinglanTalismanCatalogEntry& Entry : UQinglanEconomyLibrary::GetTalismanCatalog())
	{
		int32* Charges = TalismanCharges.Find(Entry.TalismanId);
		if (Charges == nullptr)
		{
			TalismanCharges.Add(Entry.TalismanId, Entry.Charges);
			bChanged = true;
		}
		else if (*Charges != Entry.Charges)
		{
			*Charges = Entry.Charges;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		OnProfileCommitted.Broadcast();
	}
}

void UQinglanProfileSubsystem::MarkRecordRead(FName RecordId)
{
	if (RecordId == NAME_None || ReadRecords.Contains(RecordId))
	{
		return;
	}
	ReadRecords.Add(RecordId);
	OnProfileCommitted.Broadcast();
}

void UQinglanProfileSubsystem::MarkSideQuestStageComplete(FName StageId)
{
	if (StageId == NAME_None || CompletedSideStages.Contains(StageId))
	{
		return;
	}
	CompletedSideStages.Add(StageId);
	OnProfileCommitted.Broadcast();
}

// ---------------------------------------------------------------------------
// Narrative bridge
// ---------------------------------------------------------------------------

bool UQinglanProfileSubsystem::SetNarrativeFlag(FName Flag)
{
	// FIXME-CONTRACT: the scaffold UQinglanStorySubsystem exposes no validated
	// flag-setter (flags commit only through scene/choice/objective completion
	// inside that subsystem). This subsystem must not own narrative flags
	// itself -- that would create the campaign/narrative split-brain flagged in
	// implementation-blueprint.md section 5 (contradiction 3). Until the story
	// subsystem grows an explicit validated entry point (e.g.
	// `bool SetFlagValidated(FName Flag)`), this returns false without mutating
	// anything; callers drive flags through BeginScene/SelectChoice/etc.
	UQinglanStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (Story == nullptr)
	{
		return false;
	}
	UE_LOG(LogQinglan, Verbose, TEXT("SetNarrativeFlag(%s) requires a validated story-subsystem entry point; route through scene/choice completion"), *Flag.ToString());
	return false;
}

bool UQinglanProfileSubsystem::HasNarrativeFlag(FName Flag) const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UQinglanStorySubsystem* Story = GameInstance ? GameInstance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	return Story ? Story->HasFlag(Flag) : false;
}

// ---------------------------------------------------------------------------
// Breakthrough gates
// ---------------------------------------------------------------------------

bool UQinglanProfileSubsystem::IsBambooCleared() const
{
	// Gameplay proof banked at the boss safe-reward screen, or the story scene
	// completion flag (docs/story/CH02.md: mq_ch02_bamboo_defeated).
	return CampaignFlags.Contains(FName(QinglanProgression::BambooClearedCampaignFlag))
		|| HasNarrativeFlag(FName(QinglanProgression::BambooClearedStoryFlag));
}

bool UQinglanProfileSubsystem::IsAshenCleared() const
{
	// docs/story/CH05.md: the Ashen Guardian defeat completes CH05_S01 with
	// mq_ashen_guard_disabled.
	return CampaignFlags.Contains(FName(QinglanProgression::AshenClearedCampaignFlag))
		|| HasNarrativeFlag(FName(QinglanProgression::AshenClearedStoryFlag));
}

bool UQinglanProfileSubsystem::IsGoldenCoreTrialWon() const
{
	// docs/story/CH06.md: the 150 s Golden Core trial completes with mq_core_trial_won.
	return CampaignFlags.Contains(FName(QinglanProgression::GoldenCoreTrialWonCampaignFlag))
		|| HasNarrativeFlag(FName(QinglanProgression::GoldenCoreTrialWonStoryFlag));
}

bool UQinglanProfileSubsystem::CheckBreakthroughGate(FText& OutBlockingReason) const
{
	switch (Stage)
	{
	case EQinglanStage::QiRefining:
		if (Rank < QinglanProgression::RanksPerStage)
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.ranks_incomplete"));
			return false;
		}
		if (!IsBambooCleared())
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.boss_required"));
			return false;
		}
		if (!HasCampaignFlag(FName(QinglanProgression::FoundationTrialPassedCampaignFlag))
			&& !HasNarrativeFlag(FName(QinglanProgression::FoundationTrialPassedStoryFlag)))
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.trial_required"));
			return false;
		}
		return true;

	case EQinglanStage::FoundationEstablishment:
		if (Rank < QinglanProgression::RanksPerStage)
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.ranks_incomplete"));
			return false;
		}
		if (!IsAshenCleared())
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.boss_required"));
			return false;
		}
		if (!IsGoldenCoreTrialWon())
		{
			OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.trial_required"));
			return false;
		}
		return true;

	case EQinglanStage::GoldenCore:
	default:
		OutBlockingReason = ProfileTableText(TEXT("cultivation.blocked.stage_max"));
		return false;
	}
}

// ---------------------------------------------------------------------------
// Persistence bridge
// ---------------------------------------------------------------------------

void UQinglanProfileSubsystem::ExportStateForSave(FQinglanProfileSnapshot& OutSnapshot) const
{
	// Merge semantics: the save subsystem builds the snapshot (and the story
	// subsystem may pre-fill narrative content), so only the fields this
	// subsystem owns are overwritten. ProfileId/ContentVersion and the
	// story-owned Narrative members (revision, scenes, flags, choices,
	// agreements, ending) pass through untouched.
	OutSnapshot.CultivationStage = Stage;
	OutSnapshot.CultivationRank = Rank;
	OutSnapshot.Inventory = Inventory;
	// Sorted copies keep the serialized body byte-stable for the checksum step
	// of the durable write protocol (TSet iteration order is not stable).
	// FName has no operator< in UE 5.8; LexicalLess gives the deterministic
	// cross-platform order the checksum requires.
	// EquippedTechniques keeps loadout order, which the UI depends on.
	OutSnapshot.UnlockedTechniques = UnlockedTechniques.Array();
	OutSnapshot.UnlockedTechniques.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutSnapshot.EquippedTechniques = EquippedTechniques;
	// AUD-37: permanent upgrade tiers. The save-side fingerprint folds maps in
	// key order, so iteration order does not affect the checksum.
	OutSnapshot.TechniqueTiers = TechniqueTiers;
	OutSnapshot.OwnedRelics = OwnedRelics.Array();
	OutSnapshot.OwnedRelics.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutSnapshot.EquippedRelic = EquippedRelic;
	OutSnapshot.OwnedPassives = OwnedPassives.Array();
	OutSnapshot.OwnedPassives.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	// EquippedPassives keeps slot order, which the UI depends on.
	OutSnapshot.EquippedPassives = EquippedPassives;
	// TMap iteration order is not stable, but the save-side fingerprint folds
	// the charge map in key order, so the checksum step stays deterministic.
	OutSnapshot.TalismanCharges = TalismanCharges;
	OutSnapshot.EquippedTalismans = EquippedTalismans;
	OutSnapshot.bTalismanLoadoutInitialized = true;
	OutSnapshot.CampaignFlags = CampaignFlags.Array();
	OutSnapshot.CampaignFlags.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutSnapshot.CommittedTransactionIds = CommittedTransactionIds.Array();
	OutSnapshot.CommittedTransactionIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutSnapshot.Narrative.CompletedSideStages = CompletedSideStages.Array();
	OutSnapshot.Narrative.CompletedSideStages.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	OutSnapshot.Narrative.ReadRecords = ReadRecords.Array();
	OutSnapshot.Narrative.ReadRecords.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

void UQinglanProfileSubsystem::ImportStateFromSave(const FQinglanProfileSnapshot& Snapshot)
{
	Stage = Snapshot.CultivationStage;
	Rank = Snapshot.CultivationRank;
	Inventory = Snapshot.Inventory;
	UnlockedTechniques = ToNameSet(Snapshot.UnlockedTechniques);
	EquippedTechniques = Snapshot.EquippedTechniques;
	// AUD-37: restore permanent upgrade tiers; clamp to the authored range so a
	// malformed save cannot exceed tier 2.
	TechniqueTiers.Reset();
	for (const TPair<FName, int32>& Pair : Snapshot.TechniqueTiers)
	{
		if (!Pair.Key.IsNone() && Pair.Value >= 1)
		{
			TechniqueTiers.Add(Pair.Key, FMath::Clamp(Pair.Value, 1, UQinglanEconomyLibrary::MaxTechniqueTier));
		}
	}
	OwnedRelics = ToNameSet(Snapshot.OwnedRelics);
	EquippedRelic = Snapshot.EquippedRelic;
	OwnedPassives = ToNameSet(Snapshot.OwnedPassives);
	EquippedPassives = Snapshot.EquippedPassives;
	TalismanCharges = Snapshot.TalismanCharges;
	if (Snapshot.bTalismanLoadoutInitialized)
	{
		EquippedTalismans = Snapshot.EquippedTalismans;
	}
	else
	{
		ResetTalismanLoadout();
	}
	CampaignFlags = ToNameSet(Snapshot.CampaignFlags);
	CommittedTransactionIds = ToNameSet(Snapshot.CommittedTransactionIds);
	CompletedSideStages = ToNameSet(Snapshot.Narrative.CompletedSideStages);
	ReadRecords = ToNameSet(Snapshot.Narrative.ReadRecords);

	// Let UI refresh after a profile load; import itself is not a transaction.
	OnInventoryChanged.Broadcast(Inventory);
	OnCultivationChanged.Broadcast(Stage, Rank);
	OnProfileCommitted.Broadcast();
}
