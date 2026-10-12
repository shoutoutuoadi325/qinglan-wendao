// Save schema migration registry + shared invariant validation.
#include "Persistence/QinglanSaveMigration.h"

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
			if (!Seen.Add(Value).IsSet())
			{
				return true;
			}
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
		// Pattern step for the future v2 schema ("migration functions v1_to_v2,
		// etc.", docs/05 §6). Dormant while CurrentSchemaVersion == 1: the chain
		// in MigrateToCurrentSchema never applies it until the constant is
		// bumped, and applying it manually today would fail the final invariant
		// check because v2 > CurrentSchemaVersion.
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
		Save->SchemaVersion = 2;
		// FIXME-CONTRACT(v2): map v1 fields onto the authored v2 layout here
		// when schema v2 exists. The placeholder preserves every v1 field.
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
			Narrative.EffectiveAgreements.Num() > MaxNarrativeEntries)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("Save invariant 'bounded narrative maps' failed"));
			OutError = Loc(TEXT("save.invalid"));
			return false;
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
