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
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProgressionTables.h"
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

#if WITH_DEV_AUTOMATION_TESTS
	FString GSaveDirectoryOverrideForTesting;

	/** One-shot fault injection state; RemainingSkips == INDEX_NONE is disarmed. */
	struct FFailureInjectionForTesting
	{
		int32 RemainingSkips = INDEX_NONE;
	};

	FFailureInjectionForTesting GMoveFailureForTesting;
	FFailureInjectionForTesting GDeleteFailureForTesting;
	FFailureInjectionForTesting GWriteFailureForTesting;
	FFailureInjectionForTesting GFlushFailureForTesting;

	bool ConsumeInjectedFailure(FFailureInjectionForTesting& Injection)
	{
		if (Injection.RemainingSkips == INDEX_NONE)
		{
			return false;
		}
		if (Injection.RemainingSkips > 0)
		{
			--Injection.RemainingSkips;
			return false;
		}
		// One-shot: recovery/cleanup calls after the injected failure run normally.
		Injection.RemainingSkips = INDEX_NONE;
		return true;
	}
#endif

	FString GetSaveDirectory()
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (!GSaveDirectoryOverrideForTesting.IsEmpty())
		{
			return GSaveDirectoryOverrideForTesting;
		}
#endif
		// Platform-mapped engine Saved location (docs/05 §6 "Use engine Saved
		// locations resolved by platform APIs, not the installation directory").
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"));
	}

	/** Rotation file operations go through these wrappers so tests can fail one step. */
	bool MoveSaveFile(IFileManager& FileManager, const FString& Dest, const FString& Src)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (ConsumeInjectedFailure(GMoveFailureForTesting))
		{
			return false;
		}
#endif
		return FileManager.Move(*Dest, *Src);
	}

	bool DeleteSaveFile(IFileManager& FileManager, const FString& Path)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (ConsumeInjectedFailure(GDeleteFailureForTesting))
		{
			return false;
		}
#endif
		return FileManager.Delete(*Path);
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
		if (Parsed.SchemaVersion > UQinglanSaveSubsystem::CurrentSchemaVersion)
		{
			OutError = QinglanSave::Loc(TEXT("save.future_schema"));
			return EFileLoadOutcome::FutureSchema; // newer bodies need not deserialize in this build
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
		{
			// AUD-37: fold permanent technique tiers by key order so the
			// checksum is independent of TMap iteration order.
			TArray<FName> TierKeys;
			Save->TechniqueTiers.GetKeys(TierKeys);
			TierKeys.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName& Key : TierKeys)
			{
				const int32* Tier = Save->TechniqueTiers.Find(Key);
				Dump += FString::Printf(TEXT("%s=%d;"), *Key.ToString(), Tier ? *Tier : 0);
			}
			Dump += TCHAR('|');
		}
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
		Dump += Save->bTalismanLoadoutInitialized ? TEXT("talismans1|") : TEXT("talismans0|");
		AppendNameArray(Dump, Save->EquippedTalismans);
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
		Dump += FString::Printf(TEXT("|t%.2f"), N.StoryClockSeconds);
		{
			// Courier deliveries fold in quest order so re-parse comparisons
			// stay stable regardless of TMap iteration order.
			TArray<FName> DeliveryQuests;
			N.Deliveries.GetKeys(DeliveryQuests);
			DeliveryQuests.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName Quest : DeliveryQuests)
			{
				const FQinglanNarrativeDelivery* Delivery = N.Deliveries.Find(Quest);
				Dump += FString::Printf(TEXT("d%s=%s,%.0f,%.0f,%d,"),
					*Quest.ToString(),
					Delivery ? *Delivery->Version.ToString() : TEXT(""),
					Delivery ? Delivery->DispatchedAtSeconds : 0.0,
					Delivery ? Delivery->ReadyAtStorySeconds : 0.0,
					Delivery && Delivery->bDelivered ? 1 : 0);
			}
		}
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
#if WITH_DEV_AUTOMATION_TESTS
		if (ConsumeInjectedFailure(GWriteFailureForTesting))
		{
			// Torn-write injection: persist only a prefix and report success so
			// SaveNow's re-open/verify step is what rejects the temp file.
			const int32 TruncatedBytes = Bytes.Num() / 2;
			const bool bWrotePrefix = TruncatedBytes > 0 && Handle->Write(Bytes.GetData(), TruncatedBytes);
			Handle->Flush(true);
			delete Handle;
			return bWrotePrefix ? 0 : 1;
		}
#endif
		const bool bWrote = Handle->Write(Bytes.GetData(), Bytes.Num());
		bool bFlushed = Handle->Flush(true); // re-opening alone may only read the OS cache
#if WITH_DEV_AUTOMATION_TESTS
		if (ConsumeInjectedFailure(GFlushFailureForTesting)) { bFlushed = false; }
#endif
		delete Handle;
		return !bWrote ? 1 : (bFlushed ? 0 : 3);
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
			DeleteSaveFile(IFileManager::Get(), BackupPath);
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
		FFormatNamedArguments Args;
		Args.Add(TEXT("stage"), LocalizedStageName(Save->CultivationStage));
		Args.Add(TEXT("rank"), Save->CultivationRank);
		return FText::Format(QinglanSave::Loc(TEXT("save.progress_preview")), Args);
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
		Snapshot.TechniqueTiers = Save->TechniqueTiers;
		Snapshot.OwnedRelics = Save->OwnedRelics;
		Snapshot.EquippedRelic = Save->EquippedRelic;
		Snapshot.OwnedPassives = Save->OwnedPassives;
		Snapshot.EquippedPassives = Save->EquippedPassives;
		Snapshot.TalismanCharges = Save->TalismanCharges;
		Snapshot.EquippedTalismans = Save->EquippedTalismans;
		Snapshot.bTalismanLoadoutInitialized = Save->bTalismanLoadoutInitialized;
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
		Save->TechniqueTiers = Snapshot.TechniqueTiers;
		Save->OwnedRelics = Snapshot.OwnedRelics;
		Save->EquippedRelic = Snapshot.EquippedRelic;
		Save->OwnedPassives = Snapshot.OwnedPassives;
		Save->EquippedPassives = Snapshot.EquippedPassives;
		Save->TalismanCharges = Snapshot.TalismanCharges;
		Save->EquippedTalismans = Snapshot.EquippedTalismans;
		Save->bTalismanLoadoutInitialized = Snapshot.bTalismanLoadoutInitialized;
		Save->CampaignFlags = Snapshot.CampaignFlags;
		Save->CommittedTransactionIds = Snapshot.CommittedTransactionIds;
		Save->Narrative = Snapshot.Narrative;
	}

	FName MakeProfileIdForSlot(int32 Slot)
	{
		return FName(*FString::Printf(TEXT("profile_%d"), Slot));
	}
}

#if WITH_DEV_AUTOMATION_TESTS
void UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(const FString& Directory)
{
	GSaveDirectoryOverrideForTesting = Directory;
}

void UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EQinglanSaveFileOperationForTesting Operation, int32 SkipCount)
{
	const int32 RemainingSkips = SkipCount < 0 ? INDEX_NONE : SkipCount;
	switch (Operation)
	{
	case EQinglanSaveFileOperationForTesting::Move:
		GMoveFailureForTesting.RemainingSkips = RemainingSkips;
		break;
	case EQinglanSaveFileOperationForTesting::Delete:
		GDeleteFailureForTesting.RemainingSkips = RemainingSkips;
		break;
	case EQinglanSaveFileOperationForTesting::Write:
		GWriteFailureForTesting.RemainingSkips = RemainingSkips;
		break;
	case EQinglanSaveFileOperationForTesting::Flush:
		GFlushFailureForTesting.RemainingSkips = RemainingSkips;
		break;
	}
}
#endif

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
		for (int32 BackupIndex = 1; BackupIndex <= BackupsPerProfile; ++BackupIndex)
		{
			UQinglanSaveGame* BackupSave = nullptr;
			if (TryLoadValidSave(GetBackupPath(Slot, BackupIndex), BackupSave, LoadError) == EFileLoadOutcome::FutureSchema)
			{
				OutNotice = LoadError;
				return false; // a later build's recovery copy must survive a downgrade
			}
		}
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
		if (bSawFutureSchema)
		{
			OutNotice = QinglanSave::Loc(TEXT("save.future_schema"));
			return false;
		}
		if (ChosenSave == nullptr)
		{
			UE_LOG(LogQinglanSave, Error, TEXT("LoadProfile: slot %d unreadable (primary %d)"), Slot,
				static_cast<int32>(PrimaryOutcome));
			OutNotice = QinglanSave::Loc(TEXT("save.corrupt"));
			return false;
		}
		// Timestamp + progress preview notice (docs/05 §6).
		FFormatNamedArguments Args;
		Args.Add(TEXT("date"), FText::AsDate(ChosenStamp, EDateTimeStyle::Short));
		Args.Add(TEXT("progress"), MakeProgressPreview(ChosenSave));
		OutNotice = FText::Format(QinglanSave::Loc(TEXT("save.backup_restored")), Args);
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
	return WriteProfileSave(OutError, nullptr, INDEX_NONE);
}

bool UQinglanSaveSubsystem::WriteProfileSave(FText& OutError,
	const FQinglanProfileSnapshot* ReplacementSnapshot, int32 ReplacementSlot, int32* OutSavedSequence)
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
	const int32 Slot = ReplacementSnapshot ? ReplacementSlot : Bookkeeping.ActiveSlot;
	const FName SavingProfileId = ReplacementSnapshot ? ReplacementSnapshot->ProfileId : Bookkeeping.ProfileId;
	if (!IsValidSlot(Slot))
	{
		OutError = QinglanSave::Loc(TEXT("save.no_profile_loaded"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
		return false;
	}
	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	UQinglanRunSubsystem* RunSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (ProfileSubsystem == nullptr)
	{
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: profile subsystem missing"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
		return false;
	}
	const FString PrimaryPath = GetPrimaryPath(Slot);

	// Highest sequence known before this write: in-memory + every still-valid
	// file on disk. Guarantees monotonicity across restarts.
	int32 HighestSequence = ReplacementSnapshot ? 0 : Bookkeeping.LastAcceptedSequence;
	for (int32 BackupIndex = 0; BackupIndex <= BackupsPerProfile; ++BackupIndex)
	{
		const FString CandidatePath = BackupIndex == 0 ? GetPrimaryPath(Slot) : GetBackupPath(Slot, BackupIndex);
		UQinglanSaveGame* Candidate = nullptr;
		FText CandidateError;
		const EFileLoadOutcome Outcome = TryLoadValidSave(CandidatePath, Candidate, CandidateError);
		if (Outcome == EFileLoadOutcome::FutureSchema)
		{
			OutError = CandidateError;
			OnSaveFailed.Broadcast(SavingProfileId, OutError);
			return false; // never rotate over files from an unsupported newer build
		}
		if (Outcome == EFileLoadOutcome::Ok)
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
	if (ReplacementSnapshot)
	{
		Snapshot = *ReplacementSnapshot;
	}
	else
	{
		Snapshot.Narrative = Bookkeeping.CarriedNarrative;
		if (UQinglanStorySubsystem* StorySubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
		{
			QinglanStoryRuntime::ExportNarrativeState(StorySubsystem, Snapshot.Narrative);
		}
		ProfileSubsystem->ExportStateForSave(Snapshot);
	}
	ApplySnapshotToSave(Save, Snapshot);
	if (Save->ProfileId.IsNone())
	{
		Save->ProfileId = SavingProfileId != NAME_None ? SavingProfileId : MakeProfileIdForSlot(Slot);
	}
	if (Save->ContentVersion.IsNone())
	{
		Save->ContentVersion = DefaultContentVersion;
	}
	// Optional expedition checkpoint: the run subsystem exports the pre-room
	// snapshot, so even a save requested mid-room persists room-entrance state
	// (docs/05 §6 crash-resume semantics). Suppression flag comes from
	// ClearCheckpoint (expedition end / return to hub).
	if (!ReplacementSnapshot && RunSubsystem != nullptr && RunSubsystem->IsInExpedition() && !Bookkeeping.bCheckpointSuppressed)
	{
		RunSubsystem->ExportCheckpoint(Save->Checkpoint);
		// An unfinished room replays its enemies and entrance vitals on load.
		// Its talismans must rewind with them; spent charges stay live in memory
		// and become durable when the room reward commits.
		RunSubsystem->ExportCheckpointTalismanCharges(Save->TalismanCharges);
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
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
		return false; // nothing touched on disk
	}

	// ---- Step 2: serialize to a temporary file in the same save directory ----
	Phase = EQinglanSavePhase::Serializing;
	TArray<uint8> Bytes;
	if (!SerializeVerified(Save, Bytes))
	{
		Phase = EQinglanSavePhase::Failed;
		OutError = QinglanSave::Loc(TEXT("save.invalid"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
		return false;
	}
	const FString TempPath = GetTempPath(Slot);
	const uint8 WriteResult = WriteAllBytesDurably(TempPath, Bytes);
	if (WriteResult != 0)
	{
		Phase = EQinglanSavePhase::Failed;
		DeleteSaveFile(IFileManager::Get(), TempPath);
		OutError = IoErrorText(WriteResult); // localized disk-full / permission, retry by re-invoking
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
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
		DeleteSaveFile(IFileManager::Get(), TempPath);
		OutError = QinglanSave::Loc(TEXT("save.corrupt"));
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: temp file failed re-open verification"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
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
		if (!MoveSaveFile(FileManager, Bak2Path, Bak1Path))
		{
			Phase = EQinglanSavePhase::Failed;
			DeleteSaveFile(FileManager, TempPath);
			OutError = IoErrorText(3);
			UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: bak1 -> bak2 rotation failed"));
			OnSaveFailed.Broadcast(SavingProfileId, OutError);
			return false; // primary, bak1, and bak2 all keep their bytes
		}
	}
	else if (bBak1Exists)
	{
		// An unloadable bak1 is dead weight; removing it cannot reduce the
		// loadable-file count, and bak2 (the recovery copy) is never touched.
		// A failure here still aborts the write: the storage is unhealthy, so
		// fail safe with the primary and bak2 still holding the last commits.
		if (!DeleteSaveFile(FileManager, Bak1Path))
		{
			Phase = EQinglanSavePhase::Failed;
			DeleteSaveFile(FileManager, TempPath);
			OutError = IoErrorText(3);
			UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: could not remove the invalid bak1 before rotation"));
			OnSaveFailed.Broadcast(SavingProfileId, OutError);
			return false; // primary/bak2 keep the last valid saves
		}
	}
	UQinglanSaveGame* PrimarySave = nullptr;
	FText PrimaryError;
	const bool bPrimaryValid = FileManager.FileExists(*PrimaryPath) &&
		TryLoadValidSave(PrimaryPath, PrimarySave, PrimaryError) == EFileLoadOutcome::Ok;
	if (bPrimaryValid && !MoveSaveFile(FileManager, Bak1Path, PrimaryPath))
	{
		Phase = EQinglanSavePhase::Failed;
		// Best-effort restore of the rotation, then abort with primary intact.
		if (bBak1Valid && FileManager.FileExists(*Bak2Path))
		{
			MoveSaveFile(FileManager, Bak1Path, Bak2Path);
		}
		DeleteSaveFile(FileManager, TempPath);
		OutError = IoErrorText(3);
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: could not stage primary into backup"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
		return false; // the primary never moved; it is still the last valid save
	}

	// ---- Step 5: atomic replace; recovery metadata retained until success ----
	if (!MoveSaveFile(FileManager, PrimaryPath, TempPath))
	{
		Phase = EQinglanSavePhase::Failed;
		if (bPrimaryValid && !FileManager.FileExists(*PrimaryPath))
		{
			// Replace failed after the primary was moved aside: restore it.
			MoveSaveFile(FileManager, PrimaryPath, Bak1Path);
		}
		DeleteSaveFile(FileManager, TempPath);
		OutError = IoErrorText(3);
		UE_LOG(LogQinglanSave, Error, TEXT("SaveNow: atomic replace failed"));
		OnSaveFailed.Broadcast(SavingProfileId, OutError);
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
	Phase = EQinglanSavePhase::Done;
	if (OutSavedSequence) { *OutSavedSequence = NextSequence; }
	UE_LOG(LogQinglanSave, Log, TEXT("SaveNow: slot %d sequence %d committed"), Slot, NextSequence);
	if (!ReplacementSnapshot)
	{
		Bookkeeping.LastAcceptedSequence = NextSequence;
		Bookkeeping.ProfileId = Save->ProfileId;
		Bookkeeping.CarriedNarrative = Save->Narrative; // carry what this write persisted
		OnSaveCompleted.Broadcast(Save->ProfileId);
	}
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
	FText Error;
	TryCreateNewProfile(Slot, Error);
}

bool UQinglanSaveSubsystem::TryCreateNewProfile(int32 Slot, FText& OutError)
{
	OutError = FText::GetEmpty();
	if (!IsInGameThread())
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile called off the game thread"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}
	FQinglanSaveBookkeeping& Bookkeeping = Book(this);
	if (Bookkeeping.bSaving || Bookkeeping.bApplyingProfileState)
	{
		UE_LOG(LogQinglanSave, Warning, TEXT("CreateNewProfile rejected during a save/profile replacement"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}
	if (!IsValidSlot(Slot))
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile: slot %d out of range"), Slot);
		OutError = QinglanSave::Loc(TEXT("save.invalid"));
		return false;
	}
	UGameInstance* GameInstancePtr = GetGameInstance();
	UQinglanProfileSubsystem* ProfileSubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (!ProfileSubsystem)
	{
		UE_LOG(LogQinglanSave, Error, TEXT("CreateNewProfile: profile subsystem missing"));
		OutError = QinglanSave::Loc(TEXT("error.save_failed"));
		return false;
	}

	// Build and durably write the candidate without importing it. Failure must
	// not replace the active profile, live combat, pending dialogue, or slot.
	FQinglanProfileSnapshot Fresh;
	Fresh.ProfileId = MakeProfileIdForSlot(Slot);
	Fresh.ContentVersion = DefaultContentVersion;
	Fresh.CultivationStage = EQinglanStage::QiRefining;
	Fresh.CultivationRank = 0;
	Fresh.Narrative.Revision = DefaultNarrativeRevision;
	for (const TCHAR* TechniqueId : StartingTechniques)
	{
		Fresh.UnlockedTechniques.Add(FName(TechniqueId));
		Fresh.EquippedTechniques.Add(FName(TechniqueId));
	}
	for (const FQinglanTalismanCatalogEntry& Entry : UQinglanEconomyLibrary::GetTalismanCatalog())
	{
		if (Fresh.EquippedTalismans.Num() >= QinglanProgression::EquippedTalismanLimit) { break; }
		Fresh.EquippedTalismans.Add(Entry.TalismanId);
	}
	Fresh.bTalismanLoadoutInitialized = true;
	int32 SavedSequence = 0;
	if (!WriteProfileSave(OutError, &Fresh, Slot, &SavedSequence))
	{
		return false;
	}

	TGuardValue<bool> ApplyingGuard(Bookkeeping.bApplyingProfileState, true);
	Bookkeeping.ActiveSlot = Slot;
	Bookkeeping.LastAcceptedSequence = SavedSequence;
	Bookkeeping.ProfileId = Fresh.ProfileId;
	Bookkeeping.bCheckpointSuppressed = false;
	if (UQinglanRunSubsystem* RunSubsystem = GameInstancePtr->GetSubsystem<UQinglanRunSubsystem>())
	{
		RunSubsystem->ResetRun();
	}
	ProfileSubsystem->ImportStateFromSave(Fresh);
	if (UQinglanStorySubsystem* StorySubsystem = GameInstancePtr->GetSubsystem<UQinglanStorySubsystem>())
	{
		QinglanStoryRuntime::ImportNarrativeState(StorySubsystem, Fresh.Narrative);
	}
	Bookkeeping.CarriedNarrative = Fresh.Narrative;
	Bookkeeping.bProfileActiveThisSession = true;
	OnProfileChanged.Broadcast(Fresh.ProfileId);
	OnSaveCompleted.Broadcast(Fresh.ProfileId);
	return true;
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
	UQinglanStorySubsystem* StorySubsystem = GameInstancePtr ? GameInstancePtr->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (QinglanStoryRuntime::IsStorySaveDeferred(StorySubsystem))
	{
		return; // the owning scene writes completion and all rewards together
	}
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

bool UQinglanSaveSubsystem::SuspendActiveSession(FText& OutError)
{
	if (!SaveActiveSession(OutError))
	{
		return false; // storage failure leaves the playable session intact
	}
	// The durable checkpoint remains in its slot. The hub must display profile
	// selection before refilling charges or autosaving; otherwise it overwrites
	// the suspended realm with a fresh hub pawn. Selecting the slot resumes it.
	Book(this).bProfileActiveThisSession = false;
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UQinglanRunSubsystem* Run = Instance->GetSubsystem<UQinglanRunSubsystem>())
		{
			Run->ResetRun();
		}
	}
	return true;
}
