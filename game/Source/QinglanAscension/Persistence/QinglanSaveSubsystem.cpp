// UQinglanSaveSubsystem: six-step durable write protocol
// (docs/05-technical-architecture.md §6 "Durable write protocol"):
//   1. snapshot game-thread state and validate invariant bounds
//   2. serialize to a temporary file in the same save directory
//   3. flush, re-open, parse, verify checksum/sequence
//   4. preserve the last known valid save as a backup (2 per profile, rotation)
//   5. replace the primary atomically; recovery metadata retained until success
//   6. report completion on the game thread
// A failed write never deletes the last valid primary.
#include "Persistence/QinglanSaveSubsystem.h"

#include "Async/TaskGraphInterfaces.h"
#include "GenericPlatform/GenericPlatformFile.h" // IFileHandle
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/GuardValue.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/UObjectGlobals.h"

#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveMigration.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"

namespace
{
	// --- On-disk format constants (Persistence module storage contract) ---
	// Container: [magic][container version][schema][sequence][body length][body bytes][FNV-1a-64 trailer].
	constexpr uint32 SaveContainerMagic = 0x514C5341u; // 'QLSA' signature
	constexpr int32 SaveContainerVersion = 1;
	constexpr int32 SaveContainerHeaderSize = 20; // 5 x int32-sized fields
	constexpr int32 SaveContainerTrailerSize = 8;  // uint64 checksum
	constexpr int32 SaveContainerMinSize = SaveContainerHeaderSize + SaveContainerTrailerSize;

	// --- New-profile template values (data/examples/save.json) ---
	const FName DefaultContentVersion(TEXT("design-1"));        // save.json "content_version"
	const FName DefaultNarrativeRevision(TEXT("narrative-1"));  // save.json narrative "revision"
	const TCHAR* StartingTechniques[] = { TEXT("jade_sword_arc"), TEXT("flowing_ward") }; // save.json unlocked/equipped

	/**
	 * Per-subsystem bookkeeping. The scaffold header is fixed (Phase is the only
	 * private member), so runtime slot/sequence tracking lives beside the
	 * instance, keyed by weak pointer. One game instance exists per process
	 * (offline game); weak keys keep parallel automation instances isolated.
	 */
	struct FQinglanSaveBookkeeping
	{
		int32 ActiveSlot = INDEX_NONE;
		int32 LastAcceptedSequence = 0;
		FName ProfileId = NAME_None;
		bool bCheckpointSuppressed = false;
		/** AUD-01: a profile was loaded/created in this process session; hub re-arrival must not re-import the disk snapshot. */
		bool bProfileActiveThisSession = false;
		/** Reentrancy guard: profile/story/run state is being swapped by LoadProfile/CreateNewProfile; commit-boundary autosaves stay off. */
		bool bApplyingProfileState = false;
		/** Includes completion/failure delegates: callbacks must not recursively write or replace the active profile. */
		bool bSaving = false;
		/**
		 * Story-owned narrative fields (revision, scenes, flags, decisions,
		 * agreements, ending) carried from the last load/save. SaveNow prefers
		 * the live narrative export bridge (QinglanStoryRuntime::
		 * ExportNarrativeState) and uses this carry only to seed fields the
		 * bridge leaves untouched (missing story subsystem, or a session that
		 * has not been created yet, e.g. a brand-new profile).
		 */
		FQinglanNarrativeState CarriedNarrative;
	};
	TMap<TWeakObjectPtr<UQinglanSaveSubsystem>, FQinglanSaveBookkeeping> GSaveBookkeeping;

	FQinglanSaveBookkeeping& Book(UQinglanSaveSubsystem* Key)
	{
		return GSaveBookkeeping.FindOrAdd(Key);
	}

	FString GetSaveDirectory()
	{
		// Platform-mapped engine Saved location (docs/05 §6 "Use engine Saved
		// locations resolved by platform APIs, not the installation directory").
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"));
	}

	bool IsValidSlot(int32 Slot)
	{
		return Slot >= 0 && Slot < UQinglanSaveSubsystem::ProfileSlotCount;
	}

	FString GetPrimaryPath(int32 Slot)
	{
		return FPaths::Combine(GetSaveDirectory(), *FString::Printf(TEXT("QinglanProfile_%d.sav"), Slot));
	}

	/** BackupIndex is 1..BackupsPerProfile (.bak1/.bak2). */
	FString GetBackupPath(int32 Slot, int32 BackupIndex)
	{
		return FPaths::Combine(GetSaveDirectory(), *FString::Printf(TEXT("QinglanProfile_%d.bak%d"), Slot, BackupIndex));
	}

	FString GetTempPath(int32 Slot)
	{
		// Same directory as the primary so the replace is a same-volume rename.
		return FPaths::Combine(GetSaveDirectory(), *FString::Printf(TEXT("QinglanProfile_%d.tmp"), Slot));
	}

	FText IoErrorText(uint8 IoKind)
	{
		switch (IoKind)
		{
		case 1: return QinglanSave::Loc(TEXT("error.disk_full"));      // actionable + retry
		case 2: return QinglanSave::Loc(TEXT("error.write_permission"));
		default: return QinglanSave::Loc(TEXT("error.save_failed"));
		}
	}

	enum class EFileLoadOutcome : uint8 { Ok, Missing, Corrupt, FutureSchema };

	struct FQinglanParsedContainer
	{
		int32 ContainerVersion = 0;
		int32 SchemaVersion = 0;
		int32 SaveSequence = 0;
		TArray<uint8> Body;
		uint64 Checksum = 0;
	};

	bool ParseContainerBytes(const TArray<uint8>& Bytes, FQinglanParsedContainer& Out, EFileLoadOutcome& OutOutcome)
	{
		OutOutcome = EFileLoadOutcome::Corrupt;
		if (Bytes.Num() < SaveContainerMinSize)
		{
			return false;
		}
		FMemoryReader Reader(Bytes);
		uint32 Magic = 0;
		int32 BodyLength = 0;
		Reader << Magic;
		Reader << Out.ContainerVersion;
		Reader << Out.SchemaVersion;
		Reader << Out.SaveSequence;
		Reader << BodyLength;
		if (Reader.IsError() || Magic != SaveContainerMagic)
		{
			return false;
		}
		if (Out.ContainerVersion > SaveContainerVersion)
		{
			// A container from a newer build: same explicit rejection as a newer
			// schema ("never overwrite an unsupported newer save", docs/05 §6).
			OutOutcome = EFileLoadOutcome::FutureSchema;
			return false;
		}
		// Bounds-check before the sum so an attacker-ish length cannot overflow.
		if (BodyLength <= 0 ||
			BodyLength > Bytes.Num() - SaveContainerMinSize ||
			SaveContainerHeaderSize + BodyLength + SaveContainerTrailerSize != Bytes.Num())
		{
			return false;
		}
		Out.Body.SetNumUninitialized(BodyLength);
		Reader.Serialize(Out.Body.GetData(), BodyLength);
		uint64 Trailer = 0;
		Reader << Trailer;
		Out.Checksum = Trailer;
		if (Reader.IsError() || !Reader.AtEnd())
		{
			return false;
		}
		// Checksum covers every byte except its own trailer. It detects
		// corruption; it is not an anti-cheat guarantee (docs/05 §6).
		if (QinglanSave::Fnv1a64(Bytes.GetData(), Bytes.Num() - SaveContainerTrailerSize) != Out.Checksum)
		{
			return false;
		}
		OutOutcome = EFileLoadOutcome::Ok;
		return true;
	}

	/** Full byte -> save pipeline: container parse, checksum, deserialize, migration, invariants. */
	EFileLoadOutcome ParseBytesToSave(const TArray<uint8>& Bytes, UQinglanSaveGame*& OutSave, FText& OutError)
	{
		OutSave = nullptr;
		OutError = FText::GetEmpty();
		FQinglanParsedContainer Parsed;
		EFileLoadOutcome Outcome = EFileLoadOutcome::Corrupt;
		if (!ParseContainerBytes(Bytes, Parsed, Outcome))
		{
			OutError = QinglanSave::Loc(Outcome == EFileLoadOutcome::FutureSchema ? TEXT("save.future_schema") : TEXT("save.corrupt"));
			return Outcome;
		}

		UQinglanSaveGame* Loaded = NewObject<UQinglanSaveGame>(GetTransientPackage());
		FMemoryReader BodyReader(Parsed.Body);
		FObjectAndNameAsStringProxyArchive BodyArchive(BodyReader, true);
		Loaded->Serialize(BodyArchive);
		if (BodyReader.IsError())
		{
			OutError = QinglanSave::Loc(TEXT("save.corrupt"));
			return EFileLoadOutcome::Corrupt;
		}
		// The serialized body always carries BodyChecksum == 0 (canonical form);
		// mirror the verified trailer value onto the object.
		Loaded->BodyChecksum = Parsed.Checksum;
		if (Loaded->SchemaVersion != Parsed.SchemaVersion || Loaded->SaveSequence != Parsed.SaveSequence)
		{
			// Container header and object body disagree: treat as corrupt.
			OutError = QinglanSave::Loc(TEXT("save.corrupt"));
			return EFileLoadOutcome::Corrupt;
		}

		const EQinglanMigrationResult Migration = QinglanSave::MigrateToCurrentSchema(Loaded, OutError);
		if (Migration == EQinglanMigrationResult::RejectedFutureSchema)
		{
			return EFileLoadOutcome::FutureSchema;
		}
		if (Migration == EQinglanMigrationResult::Failed || !QinglanSave::ValidateSaveInvariants(Loaded, OutError))
		{
			return EFileLoadOutcome::Corrupt;
		}
		OutSave = Loaded;
		return EFileLoadOutcome::Ok;
	}

	/** Deterministic semantic digest; TMap fields are folded in key order so re-parse comparisons stay stable. */
	uint64 ComputeSaveFingerprint(const UQinglanSaveGame* Save)
	{
		auto AppendNameArray = [](FString& Out, const TArray<FName>& Values)
		{
			for (const FName Value : Values) { Out += Value.ToString(); Out += TCHAR(','); }
		};
		auto AppendNameMap = [](FString& Out, const TMap<FName, FName>& Values)
		{
			TArray<FName> Keys;
			Values.GetKeys(Keys);
			Keys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName Key : Keys)
			{
				const FName* Value = Values.Find(Key);
				Out += Key.ToString();
				Out += TEXT("->");
				Out += Value ? Value->ToString() : FString();
				Out += TCHAR(',');
			}
		};
		auto AppendChargeMap = [](FString& Out, const TMap<FName, int32>& Values)
		{
			TArray<FName> Keys;
			Values.GetKeys(Keys);
			Keys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName Key : Keys)
			{
				const int32* Value = Values.Find(Key);
				Out += Key.ToString();
				Out += TEXT("->");
				Out += FString::Printf(TEXT("%d"), Value ? *Value : 0);
				Out += TCHAR(',');
			}
		};
		FString Dump;
		Dump += FString::Printf(TEXT("v%d|%s|%s|seq%d|st%d|r%d|"), Save->SchemaVersion,
			*Save->ContentVersion.ToString(), *Save->ProfileId.ToString(), Save->SaveSequence,
			static_cast<int32>(Save->CultivationStage), Save->CultivationRank);
		Dump += FString::Printf(TEXT("i%lld,%lld,%lld|"), Save->Inventory.SpiritShards, Save->Inventory.SpiritHerb, Save->Inventory.VeinCrystal);
		AppendNameArray(Dump, Save->UnlockedTechniques);
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->EquippedTechniques);
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->OwnedRelics);
		Dump += TCHAR('|');
		Dump += Save->EquippedRelic.ToString();
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->OwnedPassives);
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->EquippedPassives);
		Dump += TCHAR('|');
		AppendChargeMap(Dump, Save->TalismanCharges);
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->CampaignFlags);
		Dump += TCHAR('|');
		AppendNameArray(Dump, Save->CommittedTransactionIds);
		Dump += TCHAR('|');
		const FQinglanNarrativeState& N = Save->Narrative;
		Dump += N.Revision.ToString() + TCHAR('|');
		AppendNameArray(Dump, N.CompletedScenes);
		AppendNameArray(Dump, N.CompletedSideStages);
		AppendNameArray(Dump, N.Flags);
		AppendNameArray(Dump, N.ReadRecords);
		Dump += TCHAR('|');
		AppendNameMap(Dump, N.ChoiceDecisions);
		AppendNameMap(Dump, N.PendingAgreements);
		AppendNameMap(Dump, N.EffectiveAgreements);
		Dump += FString::Printf(TEXT("|e%d|ck%d"), static_cast<int32>(N.ConfirmedEnding), Save->bHasCheckpoint ? 1 : 0);
		if (Save->bHasCheckpoint)
		{
			const FQinglanCheckpoint& C = Save->Checkpoint;
			Dump += FString::Printf(TEXT("|s%d,%d,%d,%d,%d,%s|"), C.RealmSeed, C.EncounterStreamState, C.LootStreamState,
				C.BlessingStreamState, C.RoomIndex, *C.RealmId.ToString());
			AppendNameArray(Dump, C.AcquiredBlessings);
			Dump += TCHAR('|');
			AppendNameArray(Dump, C.PendingBlessingOffer);
			Dump += FString::Printf(TEXT("|p%d"), C.PendingBlessingRoom);
			Dump += FString::Printf(TEXT("|a%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f|l%lld,%lld,%lld|r"),
				C.PlayerAttributes.Health, C.PlayerAttributes.MaxHealth, C.PlayerAttributes.SpiritualEnergy,
				C.PlayerAttributes.MaxSpiritualEnergy, C.PlayerAttributes.Stamina, C.PlayerAttributes.MaxStamina,
				C.PlayerAttributes.TechniquePower, C.PlayerAttributes.Defense,
				C.Loot.SpiritShards, C.Loot.SpiritHerb, C.Loot.VeinCrystal);
			for (const int32 RoomId : C.CommittedRoomIds) { Dump += FString::Printf(TEXT("%d,"), RoomId); }
		}
		return QinglanSave::Fnv1a64(reinterpret_cast<const uint8*>(*Dump), Dump.Len() * sizeof(TCHAR));
	}

	/**
	 * Canonical container bytes for a save. The body is serialized through a
	 * duplicate whose BodyChecksum is zeroed so the checksummed form is stable;
	 * the verified value rides the trailer and is mirrored onto the duplicate.
	 */
	bool BuildContainerBytes(const UQinglanSaveGame* SourceSave, TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (SourceSave == nullptr)
		{
			return false;
		}
		UQinglanSaveGame* WorkCopy = DuplicateObject<UQinglanSaveGame>(SourceSave, GetTransientPackage());
		if (WorkCopy == nullptr)
		{
			return false;
		}
		WorkCopy->BodyChecksum = 0;

		TArray<uint8> Body;
		FMemoryWriter BodyWriter(Body);
		FObjectAndNameAsStringProxyArchive BodyArchive(BodyWriter, true);
		WorkCopy->Serialize(BodyArchive);
		if (BodyWriter.IsError() || Body.Num() == 0)
		{
			return false;
		}
		// The parser verifies the trailer over EVERY container byte except the
		// trailer itself (header + body), so the writer must hash the same span:
		// write header + body first, checksum those bytes, then append the trailer.
		FMemoryWriter ContainerWriter(OutBytes);
		uint32 Magic = SaveContainerMagic;
		int32 ContainerVersion = SaveContainerVersion;
		int32 SchemaVersion = WorkCopy->SchemaVersion;
		int32 SaveSequence = WorkCopy->SaveSequence;
		int32 BodyLength = Body.Num();
		ContainerWriter << Magic;
		ContainerWriter << ContainerVersion;
		ContainerWriter << SchemaVersion;
		ContainerWriter << SaveSequence;
		ContainerWriter << BodyLength;
		ContainerWriter.Serialize(Body.GetData(), Body.Num());
		if (ContainerWriter.IsError())
		{
			return false;
		}
		const uint64 Checksum = QinglanSave::Fnv1a64(OutBytes.GetData(), OutBytes.Num());
		WorkCopy->BodyChecksum = Checksum; // mirror only; not part of the body bytes
		uint64 Trailer = Checksum;
		ContainerWriter << Trailer;
		return !ContainerWriter.IsError();
	}

	EFileLoadOutcome TryLoadValidSave(const FString& Path, UQinglanSaveGame*& OutSave, FText& OutError)
	{
		OutSave = nullptr;
		OutError = FText::GetEmpty();
		if (!IFileManager::Get().FileExists(*Path))
		{
			return EFileLoadOutcome::Missing;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			return EFileLoadOutcome::Corrupt;
		}
		return ParseBytesToSave(Bytes, OutSave, OutError);
	}

	/** 0 = ok, 1 = disk full, 2 = permission/unwritable, 3 = unknown (IoErrorText maps to localized text). */
	uint8 WriteAllBytesDurably(const FString& Path, const TArray<uint8>& Bytes)
	{
		IFileManager& FileManager = IFileManager::Get();
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Directory = FPaths::GetPath(Path);
		if (!FileManager.DirectoryExists(*Directory) && !FileManager.MakeDirectory(*Directory, true))
		{
			return 2; // cannot obtain the save directory at all
		}
		// IPlatformFile::OpenWrite is the raw IFileHandle factory in UE 5.8;
		// the handle closes in its destructor (no Close() member).
		IFileHandle* Handle = PlatformFile.OpenWrite(*Path);
		if (Handle == nullptr)
		{
			// Distinguish "nothing can be created here" (permission) from quota
			// exhaustion with a tiny probe file in the same directory.
			const FString ProbePath = FPaths::Combine(Directory, TEXT("QinglanWriteProbe.tmp"));
			IFileHandle* Probe = PlatformFile.OpenWrite(*ProbePath);
			if (Probe != nullptr)
			{
				delete Probe;
				FileManager.Delete(*ProbePath);
				return 1; // small file fits, full payload does not: disk full
			}
			return 2;
		}
		const bool bWrote = Handle->Write(Bytes.GetData(), Bytes.Num());
		Handle->Flush(true); // durable-write step 3: flush before verification
		delete Handle;
		return bWrote ? 0 : 1; // failed write of an opened handle: quota
	}

	/**
	 * Post-commit backup repair (AUD-03): seeds a missing/invalid backup slot
	 * with already-validated bytes so a successful write always retains a
	 * validated backup. Called only after the new primary committed, so a
	 * failed repair can never reduce the loadable-file count below one; the
	 * result is verified and a torn write is dropped.
	 */
	void SeedBackupFromValidatedBytes(const FString& BackupPath, const TArray<uint8>& Bytes)
	{
		if (WriteAllBytesDurably(BackupPath, Bytes) != 0)
		{
			return; // the new primary stays committed; the repair is best effort
		}
		UQinglanSaveGame* Check = nullptr;
		FText CheckError;
		if (TryLoadValidSave(BackupPath, Check, CheckError) != EFileLoadOutcome::Ok)
		{
			IFileManager::Get().Delete(*BackupPath);
		}
	}

	FText LocalizedStageName(EQinglanStage Stage)
	{
		switch (Stage)
		{
		case EQinglanStage::FoundationEstablishment: return QinglanSave::Loc(TEXT("cultivation.foundation"));
		case EQinglanStage::GoldenCore:              return QinglanSave::Loc(TEXT("cultivation.golden_core"));
		default:                                     return QinglanSave::Loc(TEXT("cultivation.qi_refining"));
		}
	}

	/** Timestamp + progress preview for backup fallback notices (docs/05 §6). */
	FText MakeProgressPreview(const UQinglanSaveGame* Save)
	{
		return FText::Format(QinglanSave::Loc(TEXT("save.progress_preview")),
			LocalizedStageName(Save->CultivationStage), Save->CultivationRank);
	}

	FQinglanProfileSnapshot SnapshotFromSave(const UQinglanSaveGame* Save)
	{
		FQinglanProfileSnapshot Snapshot;
		Snapshot.ContentVersion = Save->ContentVersion;
		Snapshot.ProfileId = Save->ProfileId;
		Snapshot.CultivationStage = Save->CultivationStage;
		Snapshot.CultivationRank = Save->CultivationRank;
		Snapshot.Inventory = Save->Inventory;
		Snapshot.UnlockedTechniques = Save->UnlockedTechniques;
		Snapshot.EquippedTechniques = Save->EquippedTechniques;
		Snapshot.OwnedRelics = Save->OwnedRelics;
		Snapshot.EquippedRelic = Save->EquippedRelic;
		Snapshot.OwnedPassives = Save->OwnedPassives;
		Snapshot.EquippedPassives = Save->EquippedPassives;
		Snapshot.TalismanCharges = Save->TalismanCharges;
		Snapshot.CampaignFlags = Save->CampaignFlags;
		Snapshot.CommittedTransactionIds = Save->CommittedTransactionIds;
		Snapshot.Narrative = Save->Narrative;
		return Snapshot;
	}

	void ApplySnapshotToSave(UQinglanSaveGame* Save, const FQinglanProfileSnapshot& Snapshot)
	{
		Save->ContentVersion = Snapshot.ContentVersion;
		Save->ProfileId = Snapshot.ProfileId;
		Save->CultivationStage = Snapshot.CultivationStage;
		Save->CultivationRank = Snapshot.CultivationRank;
		Save->Inventory = Snapshot.Inventory;
		Save->UnlockedTechniques = Snapshot.UnlockedTechniques;
		Save->EquippedTechniques = Snapshot.EquippedTechniques;
		Save->OwnedRelics = Snapshot.OwnedRelics;
		Save->EquippedRelic = Snapshot.EquippedRelic;
		Save->OwnedPassives = Snapshot.OwnedPassives;
		Save->EquippedPassives = Snapshot.EquippedPassives;
		Save->TalismanCharges = Snapshot.TalismanCharges;
		Save->CampaignFlags = Snapshot.CampaignFlags;
		Save->CommittedTransactionIds = Snapshot.CommittedTransactionIds;
		Save->Narrative = Snapshot.Narrative;
	}

	FName MakeProfileIdForSlot(int32 Slot)
	{
		return FName(*FString::Printf(TEXT("profile_%d"), Slot));
	}
}

bool UQinglanSaveSubsystem::SerializeVerified(const UQinglanSaveGame* Save, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (Save == nullptr)
	{
		return false;
	}
	// Produce the checksummed container...
	if (!BuildContainerBytes(Save, OutBytes))
	{
		OutBytes.Reset();
		return false;
	}
	// ...then confirm it per the header contract: re-parse the bytes, re-serialize
	// the parsed save, and require a stable semantic round trip. (TMap iteration
	// order may differ between the original object and its re-parsed copy, so the
	// confirmation is semantic, not byte-for-byte.)
	UQinglanSaveGame* Parsed = nullptr;
	FText ParseError;
	if (!DeserializeVerified(OutBytes, Parsed, ParseError))
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("SerializeVerified: first parse failed: %s"), *ParseError.ToString());
		OutBytes.Reset();
		return false;
	}
	TArray<uint8> Reserialized;
	if (!BuildContainerBytes(Parsed, Reserialized))
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("SerializeVerified: reserialize failed"));
		OutBytes.Reset();
		return false;
	}
	UQinglanSaveGame* Reparsed = nullptr;
	if (!DeserializeVerified(Reserialized, Reparsed, ParseError))
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("SerializeVerified: reparse failed: %s"), *ParseError.ToString());
		OutBytes.Reset();
		return false;
	}
	const uint64 Reference = ComputeSaveFingerprint(Save);
	if (ComputeSaveFingerprint(Parsed) != Reference || ComputeSaveFingerprint(Reparsed) != Reference)
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("SerializeVerified: fingerprint drift ref=%llu parsed=%llu reparsed=%llu"),
			Reference, ComputeSaveFingerprint(Parsed), ComputeSaveFingerprint(Reparsed));
		OutBytes.Reset();
		return false;
	}
	return true;
}

bool UQinglanSaveSubsystem::DeserializeVerified(const TArray<uint8>& Bytes, UQinglanSaveGame*& OutSave, FText& OutError) const
{
	const EFileLoadOutcome Outcome = ParseBytesToSave(Bytes, OutSave, OutError);
	if (Outcome == EFileLoadOutcome::Ok)
	{
		return true;
	}
	if (Outcome == EFileLoadOutcome::Missing)
	{
		OutError = QinglanSave::Loc(TEXT("save.corrupt"));
	}
	OutSave = nullptr;
	return false;
}

bool UQinglanSaveSubsystem::LoadProfile(int32 Slot, FText& OutNotice)
{
	OutNotice = FText::GetEmpty();
	if (!IsInGameThread())
	{
		OutNotice = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}
	FQinglanSaveBookkeeping& Bookkeeping = Book(this);
	if (Bookkeeping.bSaving || Bookkeeping.bApplyingProfileState)
	{
		OutNotice = QinglanSave::Loc(TEXT("error.save_failed"));
		UE_LOG(LogQinglanSave, Warning, TEXT("LoadProfile rejected during a save/profile replacement"));
		return false;
	}
	if (!IsValidSlot(Slot))
	{
		UE_LOG(LogQinglanSave, Error, TEXT("LoadProfile: slot %d out of range"), Slot);
		return false;
	}

	const FString PrimaryPath = GetPrimaryPath(Slot);
	UQinglanSaveGame* LoadedSave = nullptr;
	FText LoadError;
	const EFileLoadOutcome PrimaryOutcome = TryLoadValidSave(PrimaryPath, LoadedSave, LoadError);

	const UQinglanSaveGame* ChosenSave = nullptr;
	FDateTime ChosenStamp = FDateTime::MinValue();
	if (PrimaryOutcome == EFileLoadOutcome::Ok)
	{
		ChosenSave = LoadedSave;
		ChosenStamp = IFileManager::Get().GetTimeStamp(*PrimaryPath);
	}
	else if (PrimaryOutcome == EFileLoadOutcome::FutureSchema)
	{
		// Reject a newer schema explicitly; never silently fall back past it
		// (docs/05 §6 "never overwrite an unsupported newer save").
		OutNotice = LoadError;
		return false;
	}
	else
	{
		// Primary missing or corrupt: fall back to the latest valid backup.
		bool bSawFutureSchema = false;
		int32 BestSequence = -1;
		FDateTime BestStamp = FDateTime::MinValue();
		for (int32 BackupIndex = 1; BackupIndex <= BackupsPerProfile; ++BackupIndex)
		{
			const FString BackupPath = GetBackupPath(Slot, BackupIndex);
			UQinglanSaveGame* BackupSave = nullptr;
			const EFileLoadOutcome Outcome = TryLoadValidSave(BackupPath, BackupSave, LoadError);
			if (Outcome == EFileLoadOutcome::FutureSchema)
			{
				bSawFutureSchema = true;
				continue;
			}
			if (Outcome != EFileLoadOutcome::Ok)
			{
				continue;
			}
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*BackupPath);
			const bool bNewer = BackupSave->SaveSequence > BestSequence ||
				(BackupSave->SaveSequence == BestSequence && Stamp > BestStamp);
			if (bNewer)
			{
				BestSequence = BackupSave->SaveSequence;
				BestStamp = Stamp;
				ChosenSave = BackupSave;
				ChosenStamp = Stamp;
			}
		}
		if (ChosenSave == nullptr)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("LoadProfile: slot %d unreadable (primary %d)"), Slot,
				static_cast<int32>(PrimaryOutcome));
			OutNotice = QinglanSave::Loc(bSawFutureSchema ? TEXT("save.future_schema") : TEXT("save.corrupt"));
			return false;
		}
		// Timestamp + progress preview notice (docs/05 §6).
		OutNotice = FText::Format(QinglanSave::Loc(TEXT("save.backup_restored")),
			FText::AsDate(ChosenStamp, EDateTimeStyle::Short), MakeProgressPreview(ChosenSave));
		UE_LOG(LogQinglanSave, Warning, TEXT("LoadProfile: slot %d fell back to a valid backup"), Slot);
	}

	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (ProfileSubsystem == nullptr)
	{
		// State could not be applied; treat the load as failed rather than
		// silently succeeding with an empty profile.
		UE_LOG(LogQinglanSave, Error, TEXT("LoadProfile: profile subsystem missing; state not applied"));
		OutNotice = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}
	// Publish the selected slot only after the required state owner is available;
	// a failed load must not redirect later saves away from the active profile.
	Bookkeeping.ActiveSlot = Slot;
	Bookkeeping.LastAcceptedSequence = ChosenSave->SaveSequence;
	Bookkeeping.ProfileId = ChosenSave->ProfileId;
	Bookkeeping.bCheckpointSuppressed = false;
	Bookkeeping.CarriedNarrative = ChosenSave->Narrative;
	// AUD-04: a profile switch replaces ALL session state atomically. The
	// applying guard keeps the commit-boundary autosave off while the
	// subsystems are mid-swap; OnProfileChanged fires once everything landed.
	Bookkeeping.bApplyingProfileState = true;
	ProfileSubsystem->ImportStateFromSave(SnapshotFromSave(ChosenSave));
	// Narrative save bridge: story-owned fields (revision, scenes, flags,
	// choice decisions, agreements, ending) import live into the story
	// subsystem so in-session progress reaches the file on the next SaveNow
	// (FQinglanSaveBookkeeping::CarriedNarrative remains the fallback for a
	// missing story subsystem, e.g. headless automation).
	if (UQinglanStorySubsystem* StorySubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		QinglanStoryRuntime::ImportNarrativeState(StorySubsystem, ChosenSave->Narrative);
	}
	UQinglanRunSubsystem* RunSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (RunSubsystem != nullptr)
	{
		if (ChosenSave->bHasCheckpoint)
		{
			// Resume from the pre-room snapshot; committed rewards are never
			// re-granted (idempotent transaction journal, docs/05 §6).
			RunSubsystem->ImportCheckpoint(ChosenSave->Checkpoint);
		}
		else
		{
			// The chosen profile is "at the sanctuary": any expedition left
			// over from the previously active profile must not survive the
			// switch (its loot/blessings would bank into the new profile).
			RunSubsystem->ResetRun();
		}
	}
	Bookkeeping.bApplyingProfileState = false;
	Bookkeeping.bProfileActiveThisSession = true;
	OnProfileChanged.Broadcast(ChosenSave->ProfileId);
	return true;
}

bool UQinglanSaveSubsystem::SaveNow(FText& OutError)
{
	OutError = FText::GetEmpty();
	if (!IsInGameThread())
	{
		// Protocol step 6 reports on the game thread; refuse anything else.
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow called off the game thread"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}
	FQinglanSaveBookkeeping& Bookkeeping = Book(this);
	if (Bookkeeping.bSaving || Bookkeeping.bApplyingProfileState)
	{
		// Do not broadcast here: an OnSaveFailed handler retrying synchronously
		// would otherwise recurse without ever reaching the writer.
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		UE_LOG(LogQinglanSave, Warning, TEXT("SaveNow rejected during a save/profile replacement"));
		return false;
	}
	TGuardValue<bool> SavingGuard(Bookkeeping.bSaving, true);
	if (!IsValidSlot(Bookkeeping.ActiveSlot))
	{
		OutError = QinglanSave::Loc(TEXT("save.no_profile_loaded"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false;
	}
	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	UQinglanRunSubsystem* RunSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (ProfileSubsystem == nullptr)
	{
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: profile subsystem missing"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false;
	}
	const int32 Slot = Bookkeeping.ActiveSlot;
	const FString PrimaryPath = GetPrimaryPath(Slot);

	// Highest sequence known before this write: in-memory + every still-valid
	// file on disk. Guarantees monotonicity across restarts.
	int32 HighestSequence = Bookkeeping.LastAcceptedSequence;
	for (int32 BackupIndex = 0; BackupIndex <= BackupsPerProfile; ++BackupIndex)
	{
		const FString CandidatePath = BackupIndex == 0 ? GetPrimaryPath(Slot) : GetBackupPath(Slot, BackupIndex);
		UQinglanSaveGame* Candidate = nullptr;
		FText CandidateError;
		if (TryLoadValidSave(CandidatePath, Candidate, CandidateError) == EFileLoadOutcome::Ok)
		{
			HighestSequence = FMath::Max(HighestSequence, Candidate->SaveSequence);
		}
	}
	const int32 NextSequence = HighestSequence + 1;

	// ---- Step 1: snapshot game-thread state and validate invariant bounds ----
	Phase = EQinglanSavePhase::Snapshotting;
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>(GetTransientPackage());
	FQinglanProfileSnapshot Snapshot;
	// Narrative save bridge: story-owned fields export live from the story
	// subsystem; the carried state only seeds fields the bridge leaves
	// untouched (missing subsystem or a not-yet-created session, e.g. a
	// brand-new profile before its first scene). ExportStateForSave then
	// merges the progression-owned fields (plus side stages / read records)
	// on top.
	Snapshot.Narrative = Bookkeeping.CarriedNarrative;
	if (UQinglanStorySubsystem* StorySubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		QinglanStoryRuntime::ExportNarrativeState(StorySubsystem, Snapshot.Narrative);
	}
	ProfileSubsystem->ExportStateForSave(Snapshot);
	ApplySnapshotToSave(Save, Snapshot);
	if (Save->ProfileId.IsNone())
	{
		Save->ProfileId = Bookkeeping.ProfileId != NAME_None ? Bookkeeping.ProfileId : MakeProfileIdForSlot(Slot);
	}
	if (Save->ContentVersion.IsNone())
	{
		Save->ContentVersion = DefaultContentVersion;
	}
	// Optional expedition checkpoint: the run subsystem exports the pre-room
	// snapshot, so even a save requested mid-room persists room-entrance state
	// (docs/05 §6 crash-resume semantics). Suppression flag comes from
	// ClearCheckpoint (expedition end / return to hub).
	if (RunSubsystem != nullptr && RunSubsystem->IsInExpedition() && !Bookkeeping.bCheckpointSuppressed)
	{
		RunSubsystem->ExportCheckpoint(Save->Checkpoint);
		Save->bHasCheckpoint = true;
	}
	else
	{
		Save->bHasCheckpoint = false;
	}
	Save->SchemaVersion = CurrentSchemaVersion;
	Save->SaveSequence = NextSequence;
	if (!QinglanSave::ValidateSaveInvariants(Save, OutError))
	{
		Phase = EQinglanSavePhase::Failed;
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: snapshot failed invariant validation"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false; // nothing touched on disk
	}

	// ---- Step 2: serialize to a temporary file in the same save directory ----
	Phase = EQinglanSavePhase::Serializing;
	TArray<uint8> Bytes;
	if (!SerializeVerified(Save, Bytes))
	{
		Phase = EQinglanSavePhase::Failed;
		OutError = QinglanSave::Loc(TEXT("save.invalid"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false;
	}
	const FString TempPath = GetTempPath(Slot);
	const uint8 WriteResult = WriteAllBytesDurably(TempPath, Bytes);
	if (WriteResult != 0)
	{
		Phase = EQinglanSavePhase::Failed;
		IFileManager::Get().Delete(*TempPath);
		OutError = IoErrorText(WriteResult); // localized disk-full / permission, retry by re-invoking
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false; // last valid primary untouched
	}

	// ---- Step 3: re-open, parse, verify checksum + monotonic sequence ----
	Phase = EQinglanSavePhase::Verifying;
	TArray<uint8> Reopened;
	UQinglanSaveGame* ReopenedSave = nullptr;
	if (!FFileHelper::LoadFileToArray(Reopened, *TempPath) ||
		Reopened != Bytes ||
		!DeserializeVerified(Reopened, ReopenedSave, OutError) ||
		ReopenedSave == nullptr ||
		ReopenedSave->SaveSequence != NextSequence ||
		ReopenedSave->SaveSequence <= HighestSequence ||
		ComputeSaveFingerprint(ReopenedSave) != ComputeSaveFingerprint(Save))
	{
		Phase = EQinglanSavePhase::Failed;
		IFileManager::Get().Delete(*TempPath);
		OutError = QinglanSave::Loc(TEXT("save.corrupt"));
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: temp file failed re-open verification"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false; // last valid primary untouched
	}

	// ---- Step 4: preserve validated backups (rotation, AUD-03) ----
	// Invariants: a backup slot only ever receives a file that validated (or
	// was the previously-valid primary); the last valid recovery copy (bak2)
	// is never overwritten unless bak1 itself validated, and never deleted
	// before a new validated primary is committed; a failed move or a crash
	// at any step leaves at least one loadable file. The temp file carrying
	// this save was fully re-opened and verified in step 3.
	Phase = EQinglanSavePhase::Replacing;
	IFileManager& FileManager = IFileManager::Get();
	const FString Bak1Path = GetBackupPath(Slot, 1);
	const FString Bak2Path = GetBackupPath(Slot, 2);
	UQinglanSaveGame* Bak1Save = nullptr;
	FText Bak1Error;
	const bool bBak1Exists = FileManager.FileExists(*Bak1Path);
	const bool bBak1Valid = bBak1Exists &&
		TryLoadValidSave(Bak1Path, Bak1Save, Bak1Error) == EFileLoadOutcome::Ok;
	if (bBak1Valid)
	{
		// bak2 only ever receives a validated file; its previous bytes are
		// superseded while bak1 and (below) the primary keep valid copies.
		if (!FileManager.Move(*Bak2Path, *Bak1Path))
		{
			Phase = EQinglanSavePhase::Failed;
			FileManager.Delete(*TempPath);
			OutError = IoErrorText(3);
			UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: bak1 -> bak2 rotation failed"));
			OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
			return false; // primary, bak1, and bak2 all keep their bytes
		}
	}
	else if (bBak1Exists)
	{
		// An unloadable bak1 is dead weight; removing it cannot reduce the
		// loadable-file count, and bak2 (the recovery copy) is never touched.
		FileManager.Delete(*Bak1Path);
	}
	UQinglanSaveGame* PrimarySave = nullptr;
	FText PrimaryError;
	const bool bPrimaryValid = FileManager.FileExists(*PrimaryPath) &&
		TryLoadValidSave(PrimaryPath, PrimarySave, PrimaryError) == EFileLoadOutcome::Ok;
	if (bPrimaryValid && !FileManager.Move(*Bak1Path, *PrimaryPath))
	{
		Phase = EQinglanSavePhase::Failed;
		// Best-effort restore of the rotation, then abort with primary intact.
		if (bBak1Valid && FileManager.FileExists(*Bak2Path))
		{
			FileManager.Move(*Bak1Path, *Bak2Path);
		}
		FileManager.Delete(*TempPath);
		OutError = IoErrorText(3);
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: could not stage primary into backup"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false; // the primary never moved; it is still the last valid save
	}

	// ---- Step 5: atomic replace; recovery metadata retained until success ----
	if (!FileManager.Move(*PrimaryPath, *TempPath))
	{
		Phase = EQinglanSavePhase::Failed;
		if (bPrimaryValid && !FileManager.FileExists(*PrimaryPath))
		{
			// Replace failed after the primary was moved aside: restore it.
			FileManager.Move(*PrimaryPath, *Bak1Path);
		}
		FileManager.Delete(*TempPath);
		OutError = IoErrorText(3);
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: atomic replace failed"));
		OnSaveFailed.Broadcast(Bookkeeping.ProfileId, OutError);
		return false; // the last valid primary is never deleted by a failed write
	}

	// Post-commit repair: with the new validated primary in place, seed a
	// missing/invalid bak1 from bak2's validated bytes (or, when bak2 is also
	// unusable, from this write's verified bytes) so a successful save always
	// retains a validated backup (AUD-03).
	{
		UQinglanSaveGame* Bak1After = nullptr;
		FText Bak1AfterError;
		if (TryLoadValidSave(Bak1Path, Bak1After, Bak1AfterError) != EFileLoadOutcome::Ok)
		{
			TArray<uint8> SeedBytes;
			UQinglanSaveGame* Bak2After = nullptr;
			FText Bak2AfterError;
			if (TryLoadValidSave(Bak2Path, Bak2After, Bak2AfterError) == EFileLoadOutcome::Ok &&
				FFileHelper::LoadFileToArray(SeedBytes, *Bak2Path))
			{
				SeedBackupFromValidatedBytes(Bak1Path, SeedBytes);
			}
			else
			{
				SeedBackupFromValidatedBytes(Bak1Path, Bytes);
			}
		}
	}

	// ---- Step 6: report completion on the game thread ----
	Bookkeeping.LastAcceptedSequence = NextSequence;
	Bookkeeping.ProfileId = Save->ProfileId;
	Bookkeeping.CarriedNarrative = Save->Narrative; // carry what this write persisted
	Phase = EQinglanSavePhase::Done;
	UE_LOG(LogQinglanSave, Log, TEXT("SaveNow: slot %d sequence %d committed"), Slot, NextSequence);
	OnSaveCompleted.Broadcast(Save->ProfileId);
	Phase = EQinglanSavePhase::Idle;
	return true;
}

bool UQinglanSaveSubsystem::HasProfile(int32 Slot) const
{
	if (!IsValidSlot(Slot))
	{
		return false;
	}
	IFileManager& FileManager = IFileManager::Get();
	if (FileManager.FileExists(*GetPrimaryPath(Slot)))
	{
		return true;
	}
	for (int32 BackupIndex = 1; BackupIndex <= BackupsPerProfile; ++BackupIndex)
	{
		if (FileManager.FileExists(*GetBackupPath(Slot, BackupIndex)))
		{
			return true;
		}
	}
	return false;
}

void UQinglanSaveSubsystem::CreateNewProfile(int32 Slot)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile called off the game thread"));
		return;
	}
	FQinglanSaveBookkeeping& Bookkeeping = Book(this);
	if (Bookkeeping.bSaving || Bookkeeping.bApplyingProfileState)
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("CreateNewProfile rejected during a save/profile replacement"));
		return;
	}
	if (!IsValidSlot(Slot))
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile: slot %d out of range"), Slot);
		return;
	}
	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (ProfileSubsystem == nullptr)
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile: profile subsystem missing"));
		return;
	}

	Bookkeeping.ActiveSlot = Slot;
	Bookkeeping.LastAcceptedSequence = 0;
	Bookkeeping.ProfileId = MakeProfileIdForSlot(Slot);
	Bookkeeping.bCheckpointSuppressed = false;

	// AUD-04: creating a profile replaces the whole session; a run left open
	// by the previously active profile must not survive into the new one.
	Bookkeeping.bApplyingProfileState = true;
	if (UQinglanRunSubsystem* RunSubsystem = GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>())
	{
		RunSubsystem->ResetRun();
	}

	// New-profile template mirrors data/examples/save.json.
	FQinglanProfileSnapshot Fresh;
	Fresh.ProfileId = Bookkeeping.ProfileId;
	Fresh.ContentVersion = DefaultContentVersion;
	Fresh.CultivationStage = EQinglanStage::QiRefining;
	Fresh.CultivationRank = 0;
	Fresh.Narrative.Revision = DefaultNarrativeRevision;
	for (const TCHAR* TechniqueId : StartingTechniques)
	{
		Fresh.UnlockedTechniques.Add(FName(TechniqueId));
		Fresh.EquippedTechniques.Add(FName(TechniqueId));
	}
	ProfileSubsystem->ImportStateFromSave(Fresh);
	// Reset the story subsystem to the same fresh narrative state so flags
	// from a previously loaded profile cannot leak into the new profile's
	// saves through the narrative export bridge.
	if (UGameInstance* StoryGameInstance = GetGameInstance())
	{
		if (UQinglanStorySubsystem* StorySubsystem = StoryGameInstance->GetSubsystem<UQinglanStorySubsystem>())
		{
			QinglanStoryRuntime::ImportNarrativeState(StorySubsystem, Fresh.Narrative);
		}
	}
	Bookkeeping.CarriedNarrative = Fresh.Narrative;
	Bookkeeping.bApplyingProfileState = false;
	Bookkeeping.bProfileActiveThisSession = true;
	OnProfileChanged.Broadcast(Bookkeeping.ProfileId);

	// Persist the fresh profile through the full durable protocol so the slot
	// exists on disk immediately (first accepted save has sequence 1).
	FText CreateError;
	if (!SaveNow(CreateError))
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile: initial durable write failed for slot %d"), Slot);
	}
}

void UQinglanSaveSubsystem::ClearCheckpoint()
{
	// Suppresses the checkpoint section of subsequent writes (expedition end /
	// return to hub). The next SaveNow persists checkpoint == null; committed
	// rewards stay journaled through CommittedTransactionIds. Suppression stays
	// on through retries and is released only when a new expedition starts.
	Book(this).bCheckpointSuppressed = true;
}

void UQinglanSaveSubsystem::BeginExpeditionCheckpoint()
{
	// AUD-02: ClearCheckpoint belongs to the previous expedition. Without this
	// lifecycle reset, every later expedition in the same profile session would
	// save successfully but silently omit its checkpoint.
	Book(this).bCheckpointSuppressed = false;
}

bool UQinglanSaveSubsystem::ShowSaveFolder()
{
	// Only through the supported desktop platform API (docs/05 §6).
	const FString Directory = GetSaveDirectory();
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*Directory) && !FileManager.MakeDirectory(*Directory, true))
	{
		return false;
	}
	FPlatformProcess::ExploreFolder(*Directory);
	return true;
}

void UQinglanSaveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// Commit-boundary autosave (AUD-02): hub purchases and rank unlocks all
	// funnel through the profile subsystem's journaled mutations, so binding
	// its commit notification wires every hub-side purchase to a durable save
	// without touching each caller.
	Collection.InitializeDependency(UQinglanProfileSubsystem::StaticClass());
	if (UGameInstance* GameInstancePtr = GetGameInstance())
	{
		if (UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>())
		{
			ProfileSubsystem->OnProfileCommitted.AddDynamic(this, &UQinglanSaveSubsystem::HandleProfileCommitted);
		}
	}
}

void UQinglanSaveSubsystem::HandleProfileCommitted()
{
	const FQinglanSaveBookkeeping& Bookkeeping = Book(this);
	if (!Bookkeeping.bProfileActiveThisSession || Bookkeeping.bApplyingProfileState)
	{
		return; // no session yet, or LoadProfile/CreateNewProfile is mid-swap
	}
	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanRunSubsystem* RunSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (RunSubsystem && RunSubsystem->IsInExpedition())
	{
		return; // in-run rewards persist at the room/banking boundaries instead
	}
	FText SaveError;
	if (!SaveNow(SaveError))
	{
		// SaveNow already broadcast OnSaveFailed; keep a breadcrumb here too.
		UE_LOG(LogQinglanSave, Warning, TEXT("Commit-boundary autosave did not commit: %s"), *SaveError.ToString());
	}
}

bool UQinglanSaveSubsystem::SaveActiveSession(FText& OutError)
{
	OutError = FText::GetEmpty();
	if (!Book(this).bProfileActiveThisSession)
	{
		return true; // nothing earned this session; map travel may proceed
	}
	return SaveNow(OutError);
}

bool UQinglanSaveSubsystem::IsProfileSessionActive() const
{
	return Book(const_cast<UQinglanSaveSubsystem*>(this)).bProfileActiveThisSession;
}
