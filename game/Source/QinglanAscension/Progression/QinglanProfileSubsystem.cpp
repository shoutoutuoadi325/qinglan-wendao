// Profile subsystem implementation: the single authoritative mutation path for
// permanent state (code-contracts.md). Every mutation is atomic, journaled by
// transaction id, and safe to replay: an already-committed id is a no-op that
// reports success, so an autosave callback can never award a second payout
// (docs/03-progression-and-economy.md section 6). Game-thread only; the game is
// offline and single-profile-active, so no locking is required.
#include "Progression/QinglanProfileSubsystem.h"

#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "QinglanAscension.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	/** All player-facing strings resolve from the single game string table. */
	FText TableText(const TCHAR* Key)
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

	FText BlockingReason;
	if (!CheckBreakthroughGate(BlockingReason))
	{
		// Gates (docs/dev/implementation-blueprint.md section 1.5):
		// Qi -> Foundation: Bamboo Guardian cleared + 3 Qi ranks.
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
			OutBlockingReason = TableText(TEXT("cultivation.blocked.ranks_incomplete"));
			return false;
		}
		if (!IsBambooCleared())
		{
			OutBlockingReason = TableText(TEXT("cultivation.blocked.boss_required"));
			return false;
		}
		return true;

	case EQinglanStage::FoundationEstablishment:
		if (Rank < QinglanProgression::RanksPerStage)
		{
			OutBlockingReason = TableText(TEXT("cultivation.blocked.ranks_incomplete"));
			return false;
		}
		if (!IsAshenCleared())
		{
			OutBlockingReason = TableText(TEXT("cultivation.blocked.boss_required"));
			return false;
		}
		if (!IsGoldenCoreTrialWon())
		{
			OutBlockingReason = TableText(TEXT("cultivation.blocked.trial_required"));
			return false;
		}
		return true;

	case EQinglanStage::GoldenCore:
	default:
		OutBlockingReason = TableText(TEXT("cultivation.blocked.stage_max"));
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
	// EquippedTechniques keeps loadout order, which the UI depends on.
	OutSnapshot.UnlockedTechniques = UnlockedTechniques.Array();
	OutSnapshot.UnlockedTechniques.Sort();
	OutSnapshot.EquippedTechniques = EquippedTechniques;
	OutSnapshot.CampaignFlags = CampaignFlags.Array();
	OutSnapshot.CampaignFlags.Sort();
	OutSnapshot.CommittedTransactionIds = CommittedTransactionIds.Array();
	OutSnapshot.CommittedTransactionIds.Sort();
	OutSnapshot.Narrative.CompletedSideStages = CompletedSideStages.Array();
	OutSnapshot.Narrative.CompletedSideStages.Sort();
	OutSnapshot.Narrative.ReadRecords = ReadRecords.Array();
	OutSnapshot.Narrative.ReadRecords.Sort();
}

void UQinglanProfileSubsystem::ImportStateFromSave(const FQinglanProfileSnapshot& Snapshot)
{
	Stage = Snapshot.CultivationStage;
	Rank = Snapshot.CultivationRank;
	Inventory = Snapshot.Inventory;
	UnlockedTechniques = ToNameSet(Snapshot.UnlockedTechniques);
	EquippedTechniques = Snapshot.EquippedTechniques;
	CampaignFlags = ToNameSet(Snapshot.CampaignFlags);
	CommittedTransactionIds = ToNameSet(Snapshot.CommittedTransactionIds);
	CompletedSideStages = ToNameSet(Snapshot.Narrative.CompletedSideStages);
	ReadRecords = ToNameSet(Snapshot.Narrative.ReadRecords);

	// Let UI refresh after a profile load; import itself is not a transaction.
	OnInventoryChanged.Broadcast(Inventory);
	OnCultivationChanged.Broadcast(Stage, Rank);
	OnProfileCommitted.Broadcast();
}
