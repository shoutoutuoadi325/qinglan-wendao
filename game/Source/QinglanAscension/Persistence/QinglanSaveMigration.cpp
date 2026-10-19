// Save schema migration registry + shared invariant validation.
#include "Persistence/QinglanSaveMigration.h"

#include "Narrative/QinglanStoryRuntime.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "QinglanAscension.h"

namespace
{
	// --- Invariant bounds (docs/dev/implementation-blueprint.md §1.6
	// "validate nonnegative bounded currency, unique technique IDs, ... rank
	// limits"; "bounded arrays" per Persistence module contract) ---
	constexpr int32 MaxCultivationRank = 6;          // §1.5: six ranks (80..400 Shards)
	constexpr int32 MaxStageValue = 2;               // EQinglanStage::GoldenCore
	constexpr int32 MaxEndingValue = 3;              // EQinglanEnding::Shared
	constexpr int32 MaxTechniqueEntries = 64;        // bounded array (§1.6)
	constexpr int32 MaxFlagEntries = 1024;           // bounded array (§1.6)
	constexpr int32 MaxTransactionEntries = 8192;    // bounded array (§1.6)
	constexpr int32 MaxNarrativeEntries = 1024;      // bounded array (§1.6)
	constexpr int32 MaxBlessingEntries = 64;         // bounded array (§1.6); 2 drafted per expedition (§1.4)
	constexpr int32 MaxRoomEntries = 32;             // 7-room contract (§1.4), generous bound
	constexpr int32 MaxTalismanChargeEntries = 16;   // bounded map (§1.6); 6 catalog talismans
	constexpr int32 MaxTalismanCharges = 9;          // charges 0..9; catalog ships at most 2 (docs/03 section 5)
	constexpr double MaxStoryClockSeconds = 366.0 * 10.0 * 86400.0; // ten in-fiction years of hub rests
	constexpr double MaxDeliverySpanSeconds = 7.0 * 86400.0;        // round trips are 12-18 h; 7 d is the corruption bound
	// FNV-1a constants for UQinglanSaveGame::BodyChecksum ("detects corruption,
	// not tampering", docs/05-technical-architecture.md §6).
	constexpr uint64 Fnv1a64OffsetBasis = 0xcbf29ce484222325ULL;
	constexpr uint64 Fnv1a64Prime = 0x100000001b3ULL;

	bool HasDuplicates(const TArray<FName>& Values)
	{
		TSet<FName> Seen;
		Seen.Reserve(Values.Num());
		for (const FName Value : Values)
		{
			if (Seen.Contains(Value))
			{
				return true;
			}
			Seen.Add(Value);
		}
		return false;
	}

	bool IsBoundedNameArray(const TArray<FName>& Values, int32 MaxEntries, const char* InvariantName, FText& OutError)
	{
		if (Values.Num() > MaxEntries)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant '%s' failed: array has %d entries (max %d)"),
				UTF8_TO_TCHAR(InvariantName), Values.Num(), MaxEntries);
			OutError = QinglanSave::Loc(TEXT("save.invalid"));
			return false;
		}
		for (const FName Value : Values)
		{
			if (Value.IsNone())
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant '%s' failed: contains an empty id"), UTF8_TO_TCHAR(InvariantName));
				OutError = QinglanSave::Loc(TEXT("save.invalid"));
				return false;
			}
		}
		return true;
	}

	bool IsNonnegativeInventory(const FQinglanInventory& Inventory, FText& OutError)
	{
		if (Inventory.SpiritShards < 0 || Inventory.SpiritHerb < 0 || Inventory.VeinCrystal < 0)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'nonnegative currency' failed"));
			OutError = QinglanSave::Loc(TEXT("save.invalid"));
			return false;
		}
		return true;
	}

	bool IsBoundedFiniteAttributes(const FQinglanAttributes& Attributes, FText& OutError)
	{
		const float Values[] = { Attributes.Health, Attributes.MaxHealth, Attributes.SpiritualEnergy,
			Attributes.MaxSpiritualEnergy, Attributes.Stamina, Attributes.MaxStamina,
			Attributes.TechniquePower, Attributes.Defense };
		for (const float Value : Values)
		{
			if (Value < 0.f || !FMath::IsFinite(Value))
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'checkpoint attributes nonnegative/finite' failed"));
				OutError = QinglanSave::Loc(TEXT("save.invalid"));
				return false;
			}
		}
		return true;
	}

	TMap<int32, FQinglanSaveMigrationStep>& GetMigrationRegistry()
	{
		static TMap<int32, FQinglanSaveMigrationStep> Registry;
		return Registry;
	}

	// Self-registration of the future v1 -> v2 step (docs/05 §6 pattern).
	struct FQinglanSaveMigrationBootstrap
	{
		FQinglanSaveMigrationBootstrap()
		{
			QinglanSave::RegisterMigrationStep(1, &QinglanSave::MigrateV1_ToV2);
		}
	};
	const FQinglanSaveMigrationBootstrap GQinglanSaveMigrationBootstrap;
}

namespace QinglanSave
{
	uint64 Fnv1a64(const uint8* Data, int64 Num)
	{
		uint64 Hash = Fnv1a64OffsetBasis;
		for (int64 Index = 0; Index < Num; ++Index)
		{
			Hash ^= static_cast<uint64>(Data[Index]);
			Hash *= Fnv1a64Prime;
		}
		return Hash;
	}

	bool RegisterMigrationStep(int32 FromVersion, FQinglanSaveMigrationStep Step)
	{
		if (FromVersion < 1 || Step == nullptr)
		{
			return false;
		}
		GetMigrationRegistry().Add(FromVersion, Step);
		return true;
	}

	bool MigrateV1_ToV2(UQinglanSaveGame* Save, FText& OutError)
	{
		// v2 adds the narrative courier clock and per-quest delivery entries
		// (AUD-16). v1 persisted pending selections without dispatch
		// timestamps, so every surviving pending version gains a fresh
		// undispatched-in-fiction delivery from clock zero: the reply stays one
		// sanctuary rest away while the effective agreement is retained
		// (side-quests.json SQ02 closure: "restart the timestamp and retain old
		// accepted agreement until a new agreement is confirmed").
		if (Save == nullptr || Save->SchemaVersion != 1)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("MigrateV1_ToV2: save is not a v1 save"));
			OutError = Loc(TEXT("save.migration_missing"));
			return false;
		}
		if (!ValidateSaveInvariants(Save, OutError))
		{
			return false;
		}
		Save->Narrative.StoryClockSeconds = 0.0;
		for (auto It = Save->Narrative.PendingAgreements.CreateIterator(); It; ++It)
		{
			if (It.Key().IsNone() || It.Value().IsNone())
			{
				// An empty quest or version cannot carry a delivery; drop the slot
				// instead of failing the whole profile over it.
				UE_LOG(LogQinglanSave, Warning, TEXT("MigrateV1_ToV2: dropping empty pending agreement slot for %s"),
					*It.Key().ToString());
				It.RemoveCurrent();
				continue;
			}
			FQinglanNarrativeDelivery Delivery;
			Delivery.Version = It.Value();
			Delivery.DispatchedAtSeconds = 0.0;
			Delivery.ReadyAtStorySeconds = QinglanStoryRuntime::DeliveryGameHoursForQuest(It.Key()) * QinglanStoryRuntime::SecondsPerGameHour;
			Delivery.bDelivered = false;
			Save->Narrative.Deliveries.Add(It.Key(), Delivery);
		}
		Save->SchemaVersion = 2;
		return ValidateSaveInvariants(Save, OutError);
	}

	EQinglanMigrationResult MigrateToCurrentSchema(UQinglanSaveGame* Save, FText& OutError)
	{
		if (Save == nullptr)
		{
			OutError = Loc(TEXT("save.corrupt"));
			return EQinglanMigrationResult::Failed;
		}
		const int32 CurrentVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
		if (Save->SchemaVersion > CurrentVersion)
		{
			// "Never overwrite an unsupported newer save" - reject explicitly.
			UE_LOG(LogQinglanSave, Warning, TEXT("Rejecting save with future schema %d (current %d)"),
				Save->SchemaVersion, CurrentVersion);
			OutError = Loc(TEXT("save.future_schema"));
			return EQinglanMigrationResult::RejectedFutureSchema;
		}
		bool bAppliedAny = false;
		while (Save->SchemaVersion < CurrentVersion)
		{
			const int32 FromVersion = Save->SchemaVersion;
			const FQinglanSaveMigrationStep* Step = GetMigrationRegistry().Find(FromVersion);
			if (Step == nullptr || *Step == nullptr)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("No migration step registered from schema %d"), FromVersion);
				OutError = Loc(TEXT("save.migration_missing"));
				return EQinglanMigrationResult::Failed;
			}
			if (!(*Step)(Save, OutError))
			{
				return EQinglanMigrationResult::Failed;
			}
			if (Save->SchemaVersion != FromVersion + 1)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Migration step from schema %d did not advance exactly one version"), FromVersion);
				OutError = Loc(TEXT("save.migration_missing"));
				return EQinglanMigrationResult::Failed;
			}
			bAppliedAny = true;
		}
		return bAppliedAny ? EQinglanMigrationResult::Migrated : EQinglanMigrationResult::NotNeeded;
	}

	bool ValidateSaveInvariants(const UQinglanSaveGame* Save, FText& OutError)
	{
		if (Save == nullptr)
		{
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		OutError = FText::GetEmpty();

		if (Save->SchemaVersion < 1 || Save->SchemaVersion > UQinglanSaveSubsystem::CurrentSchemaVersion)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'schema version' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (Save->SaveSequence < 0)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'nonnegative sequence' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (Save->CultivationRank < 0 || Save->CultivationRank > MaxCultivationRank)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'rank 0..6' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (static_cast<int32>(Save->CultivationStage) < 0 || static_cast<int32>(Save->CultivationStage) > MaxStageValue)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'stage enum' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (!IsNonnegativeInventory(Save->Inventory, OutError))
		{
			return false;
		}

		// Unique technique ids (docs §1.6).
		if (HasDuplicates(Save->UnlockedTechniques) || HasDuplicates(Save->EquippedTechniques))
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'unique technique ids' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (!IsBoundedNameArray(Save->UnlockedTechniques, MaxTechniqueEntries, "unlocked techniques", OutError) ||
			!IsBoundedNameArray(Save->EquippedTechniques, MaxTechniqueEntries, "equipped techniques", OutError))
		{
			return false;
		}
		// Equipped must be a subset of unlocked (loadout comes from unlocks, §1.5).
		TSet<FName> Unlocked(Save->UnlockedTechniques);
		for (const FName Equipped : Save->EquippedTechniques)
		{
			if (!Unlocked.Contains(Equipped))
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'equipped subset of unlocked' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
		}

		if (!IsBoundedNameArray(Save->CampaignFlags, MaxFlagEntries, "campaign flags", OutError) ||
			!IsBoundedNameArray(Save->CommittedTransactionIds, MaxTransactionEntries, "committed transactions", OutError))
		{
			return false;
		}

		// Relics/passives: unique ids, equipped subset of owned, equip limits
		// (one relic, two passives — docs/03 sections 4-5; tables hold limits).
		if (HasDuplicates(Save->OwnedRelics) || HasDuplicates(Save->OwnedPassives) || HasDuplicates(Save->EquippedPassives))
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'unique relic/passive ids' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (!IsBoundedNameArray(Save->OwnedRelics, MaxNarrativeEntries, "owned relics", OutError) ||
			!IsBoundedNameArray(Save->OwnedPassives, MaxNarrativeEntries, "owned passives", OutError) ||
			!IsBoundedNameArray(Save->EquippedPassives, MaxNarrativeEntries, "equipped passives", OutError))
		{
			return false;
		}
		{
			const TSet<FName> OwnedRelics(Save->OwnedRelics);
			if (!Save->EquippedRelic.IsNone() && !OwnedRelics.Contains(Save->EquippedRelic))
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'equipped relic owned' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			const TSet<FName> OwnedPassives(Save->OwnedPassives);
			for (const FName EquippedPassive : Save->EquippedPassives)
			{
				if (!OwnedPassives.Contains(EquippedPassive))
				{
					UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'equipped passive owned' failed"));
					OutError = Loc(TEXT("save.invalid"));
					return false;
				}
			}
		}

		// Talisman charges: bounded map, non-empty ids, values 0..9 (docs/03
		// section 5 ships at most 2 charges per talisman; the bound is the sane
		// corruption guard, not a gameplay limit).
		if (Save->TalismanCharges.Num() > MaxTalismanChargeEntries)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'bounded talisman charges' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		for (const TPair<FName, int32>& Charge : Save->TalismanCharges)
		{
			if (Charge.Key.IsNone() || Charge.Value < 0 || Charge.Value > MaxTalismanCharges)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'talisman charges 0..9' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
		}

		const FQinglanNarrativeState& Narrative = Save->Narrative;
		if (static_cast<int32>(Narrative.ConfirmedEnding) < 0 || static_cast<int32>(Narrative.ConfirmedEnding) > MaxEndingValue)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'ending enum' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		if (!IsBoundedNameArray(Narrative.CompletedScenes, MaxNarrativeEntries, "completed scenes", OutError) ||
			!IsBoundedNameArray(Narrative.CompletedSideStages, MaxNarrativeEntries, "completed side stages", OutError) ||
			!IsBoundedNameArray(Narrative.Flags, MaxFlagEntries, "narrative flags", OutError) ||
			!IsBoundedNameArray(Narrative.ReadRecords, MaxNarrativeEntries, "read records", OutError))
		{
			return false;
		}
		if (Narrative.ChoiceDecisions.Num() > MaxNarrativeEntries ||
			Narrative.PendingAgreements.Num() > MaxNarrativeEntries ||
			Narrative.EffectiveAgreements.Num() > MaxNarrativeEntries ||
			Narrative.Deliveries.Num() > MaxNarrativeEntries)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'bounded narrative maps' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		// Courier clock + deliveries (schema v2): a finite clock within ten
		// in-fiction years, complete entries, and consistency with the pending
		// agreements each delivery must still answer.
		if (!FMath::IsFinite(Narrative.StoryClockSeconds) ||
			Narrative.StoryClockSeconds < 0.0 ||
			Narrative.StoryClockSeconds > MaxStoryClockSeconds)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'story clock range' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
		}
		for (const TPair<FName, FQinglanNarrativeDelivery>& Entry : Narrative.Deliveries)
		{
			const FQinglanNarrativeDelivery& Delivery = Entry.Value;
			const FName* PendingVersion = Narrative.PendingAgreements.Find(Entry.Key);
			if (Entry.Key.IsNone() ||
				Delivery.Version.IsNone() ||
				!FMath::IsFinite(Delivery.DispatchedAtSeconds) || Delivery.DispatchedAtSeconds < 0.0 ||
				!FMath::IsFinite(Delivery.ReadyAtStorySeconds) || Delivery.ReadyAtStorySeconds < 0.0 ||
				Delivery.ReadyAtStorySeconds < Delivery.DispatchedAtSeconds ||
				Delivery.ReadyAtStorySeconds - Delivery.DispatchedAtSeconds > MaxDeliverySpanSeconds ||
				PendingVersion == nullptr || *PendingVersion != Delivery.Version)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'delivery entries' failed for quest %s"),
					*Entry.Key.ToString());
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			if (Delivery.bDelivered && Delivery.ReadyAtStorySeconds > Narrative.StoryClockSeconds + QinglanStoryRuntime::DeliveryArrivalToleranceSeconds)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'delivered reply within clock' failed for quest %s"),
					*Entry.Key.ToString());
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
		}

		if (Save->bHasCheckpoint)
		{
			const FQinglanCheckpoint& Checkpoint = Save->Checkpoint;
			if (!IsBoundedFiniteAttributes(Checkpoint.PlayerAttributes, OutError) ||
				!IsNonnegativeInventory(Checkpoint.Loot, OutError))
			{
				return false;
			}
			if (Checkpoint.RoomIndex < 0)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'nonnegative room index' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			if (HasDuplicates(Checkpoint.AcquiredBlessings) ||
				!IsBoundedNameArray(Checkpoint.AcquiredBlessings, MaxBlessingEntries, "acquired blessings", OutError))
			{
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			// Pending draft offer (docs/03 section 5): bounded, unique ids, and
			// canonically paired with its owning room (an offer always carries a
			// room, a room never lingers without an offer).
			if (HasDuplicates(Checkpoint.PendingBlessingOffer) ||
				!IsBoundedNameArray(Checkpoint.PendingBlessingOffer, MaxBlessingEntries, "pending blessing offer", OutError))
			{
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			if (Checkpoint.PendingBlessingRoom < INDEX_NONE || Checkpoint.PendingBlessingRoom >= MaxRoomEntries ||
				Checkpoint.PendingBlessingOffer.IsEmpty() != (Checkpoint.PendingBlessingRoom == INDEX_NONE))
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'pending offer/room pairing' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			if (Checkpoint.CommittedRoomIds.Num() > MaxRoomEntries)
			{
				UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'bounded committed rooms' failed"));
				OutError = Loc(TEXT("save.invalid"));
				return false;
			}
			// Optional stream states are int32 RNG state; any value is legal
			// (docs/05 §7 "optional stream states"), realm warning only.
			if (Checkpoint.RealmId.IsNone())
			{
				UE_LOG(LogQinglanSave, Warning, TEXT("Checkpoint carries no realm id"));
			}
		}
		return true;
	}
}
