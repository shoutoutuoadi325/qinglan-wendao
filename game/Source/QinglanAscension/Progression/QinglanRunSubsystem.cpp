// Run subsystem implementation: one expedition's seed, room pipeline,
// temporary loot, and blessing draft.
//
// Docs: docs/01 section 6 (seeded streams x3, encounter selection, caps),
// docs/03 sections 2/5/6 (payouts, blessing draft after rooms 2 and 4,
// banking rules, transaction ids), docs/dev/implementation-blueprint.md
// sections 1.4 (7-room contract) and 1.5 (banking/first-clear idempotency).
//
// FIXME-CONTRACT: the frozen scaffold header declares no state members, so the
// per-instance expedition state lives beside the subsystem in a TU-local
// weak-keyed map (same house pattern as Persistence/QinglanSaveSubsystem.cpp,
// Core/QinglanGameInstance.cpp, and AI/QinglanEncounterDirector.cpp). Merge it
// into the class when the header unfreeze happens.
//
// The realm template is resolved from a cooked data asset when the data
// pipeline provides one (UQinglanRealmDataAsset instances are populated by the
// editor importer); otherwise it is synthesized from the same constants the
// importer authors (data/examples/realm.json mirrors bamboo_hollow exactly),
// so headless automation drives identical room pipelines.
#include "Progression/QinglanRunSubsystem.h"

#include "Progression/QinglanProgressionTables.h"

#include "Abilities/QinglanBlessingLibrary.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanRoomLayout.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanBlessingDrafting.h"
#include "Progression/QinglanDeterminism.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "QinglanAscension.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	// --- 7-room contract (blueprint section 1.4; data/examples/realm.json) ---
	constexpr int32 RealmRoomCount = QinglanRoomLayout::RoomCount;

	/** Rooms 2 and 4 (0-based) end with a blessing draft (blueprint 1.4 "blessing #1/#2"). */
	bool IsBlessingDraftRoom(int32 RoomIndex)
	{
		return RoomIndex == 2 || RoomIndex == 4;
	}

	/**
	 * Synthesizes the realm template for the known realm ids. bamboo_hollow
	 * mirrors data/examples/realm.json exactly (0/30/40/0/50/60/0 = 180 base,
	 * blueprint 1.4); the ashen split sums to the authored 300-shard base and
	 * the tutorial split to the 80-shard base (docs/03 section 2) because no
	 * doc authors their per-room numbers (FIXME-CONTRACT(data): ashen/tutorial
	 * room splits are authored; replace with imported realm assets when the
	 * data pipeline ships them).
	 */
	struct FRoomSpec { EQinglanRoomKind Kind; int32 SpiritShards; };

	void FillRoomSpecs(FName RealmId, FRoomSpec (&OutRooms)[RealmRoomCount])
	{
		static const FRoomSpec Bamboo[RealmRoomCount] =
		{
			{ EQinglanRoomKind::Entrance, 0 }, { EQinglanRoomKind::Combat, 30 }, { EQinglanRoomKind::Combat, 40 },
			{ EQinglanRoomKind::Shrine, 0 }, { EQinglanRoomKind::Elite, 50 },
			{ EQinglanRoomKind::Boss, 60 }, { EQinglanRoomKind::Exit, 0 }
		};
		static const FRoomSpec Ashen[RealmRoomCount] =
		{
			{ EQinglanRoomKind::Entrance, 0 }, { EQinglanRoomKind::Combat, 50 }, { EQinglanRoomKind::Combat, 70 },
			{ EQinglanRoomKind::Shrine, 0 }, { EQinglanRoomKind::Elite, 80 },
			{ EQinglanRoomKind::Boss, 100 }, { EQinglanRoomKind::Exit, 0 }
		};
		static const FRoomSpec Tutorial[RealmRoomCount] =
		{
			{ EQinglanRoomKind::Entrance, 0 }, { EQinglanRoomKind::Combat, 20 }, { EQinglanRoomKind::Combat, 20 },
			{ EQinglanRoomKind::Shrine, 0 }, { EQinglanRoomKind::Elite, 20 },
			{ EQinglanRoomKind::Boss, 20 }, { EQinglanRoomKind::Exit, 0 }
		};
		const FRoomSpec* Selected = Bamboo;
		if (RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
		{
			Selected = Ashen;
		}
		else if (RealmId == FName(UQinglanEconomyLibrary::TutorialRealmIdText))
		{
			Selected = Tutorial;
		}
		for (int32 Index = 0; Index < RealmRoomCount; ++Index)
		{
			OutRooms[Index] = Selected[Index];
		}
	}

	/** Per-instance expedition state (see file-header FIXME-CONTRACT). */
	struct FQinglanRunState
	{
		bool bInExpedition = false;
		bool bEndingExpedition = false; // Blocks reentrant banking from commit delegates.
		bool bNeedsCheckpointResume = false;
		FName RealmId = NAME_None;
		int32 RealmSeed = 1;

		/** The three seeded streams (docs/01 section 6; determinism helpers). */
		FRandomStream EncounterStream;
		int32 EntryEncounterStreamState = 0;
		bool bHasRoomEncounterSnapshot = false;
		FRandomStream LootStream;
		FRandomStream BlessingStream;

		/** Synthesized realm template, rooted for the instance lifetime. */
		TStrongObjectPtr<UQinglanRealmDataAsset> SynthesizedRealm;

		int32 CurrentRoomIndex = INDEX_NONE;
		FQinglanInventory UnbankedLoot;

		/** Blessings owned this expedition; expire at expedition end (docs/03 section 5). */
		TArray<FName> AcquiredBlessings;
		/** Cached 1-of-3 draft rolled at draft-room completion (docs/03 section 5). */
		TArray<FName> PendingBlessingOffer;
		/** Room whose completion rolled the pending draft (checkpoint restore, AUD-06). */
		int32 PendingOfferRoom = INDEX_NONE;
		bool bDraftPending = false;

		/** Room ids whose reward already committed; idempotency for crash-resume. */
		TArray<int32> CommittedRoomIds;

		/** Player attributes captured at room entry (pre-room checkpoint snapshot). */
		FQinglanAttributes EntryAttributes;
	};

	TMap<TWeakObjectPtr<UQinglanRunSubsystem>, FQinglanRunState>& RunStates()
	{
		static TMap<TWeakObjectPtr<UQinglanRunSubsystem>, FQinglanRunState> Map;
		return Map;
	}

	FQinglanRunState& StateFor(UQinglanRunSubsystem* Key)
	{
		TMap<TWeakObjectPtr<UQinglanRunSubsystem>, FQinglanRunState>& States = RunStates();
		for (auto It = States.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent(); // subsystem torn down with its game instance
			}
		}
		return States.FindOrAdd(Key);
	}

	const FQinglanRunState* FindStateFor(const UQinglanRunSubsystem* Key)
	{
		return RunStates().Find(Key);
	}

	/** Resolves the realm template: imported asset first, synthesis otherwise. */
	const UQinglanRealmDataAsset* ResolveRealmTemplate(const UQinglanRunSubsystem* Subsystem, FName RealmId)
	{
		// FIXME-CONTRACT(data): the imported realm asset path is a convention
		// (/Game/Qinglan/Data/DA_Realm_<id>) until the editor importer authors
		// the realm data assets; synthesis keeps headless runs identical.
		const FString AssetPath = FString::Printf(TEXT("/Game/Qinglan/Data/DA_Realm_%s"), *RealmId.ToString());
		// TCHAR* constructor: the FString overload is explicit in some engine versions.
		if (UQinglanRealmDataAsset* Imported = Cast<UQinglanRealmDataAsset>(FSoftObjectPath(*AssetPath).TryLoad()))
		{
			return Imported;
		}
		FQinglanRunState& State = StateFor(const_cast<UQinglanRunSubsystem*>(Subsystem));
		if (State.SynthesizedRealm.IsValid() && State.SynthesizedRealm->RealmId == RealmId)
		{
			return State.SynthesizedRealm.Get();
		}
		UQinglanRealmDataAsset* Realm = NewObject<UQinglanRealmDataAsset>(GetTransientPackage());
		if (!Realm)
		{
			return nullptr;
		}
		Realm->RealmId = RealmId;
		Realm->NameTextKey = FName(*FString::Printf(TEXT("realm.%s"), *RealmId.ToString()));
		Realm->DefaultSeed = State.RealmSeed;
		Realm->MaxConcurrentEnemies = 8; // docs/01 section 6 cap
		FRoomSpec Specs[RealmRoomCount];
		FillRoomSpecs(RealmId, Specs);
		Realm->Rooms.SetNum(RealmRoomCount);
		for (int32 Index = 0; Index < RealmRoomCount; ++Index)
		{
			Realm->Rooms[Index].Kind = Specs[Index].Kind;
			Realm->Rooms[Index].SpiritShards = Specs[Index].SpiritShards;
		}
		// The run state's strong pointer owns the lifetime (replacing it on a
		// realm switch releases the previous template for GC; no rooting).
		State.SynthesizedRealm = TStrongObjectPtr<UQinglanRealmDataAsset>(Realm);
		return Realm;
	}

	/** Player pawn's combat attributes for the pre-room snapshot (best effort). */
	FQinglanAttributes SnapshotPlayerAttributes(const UQinglanRunSubsystem* Subsystem)
	{
		const UGameInstance* Instance = Subsystem->GetGameInstance();
		UWorld* World = Instance ? Instance->GetWorld() : nullptr;
		AQinglanCharacter* Player = World ? Cast<AQinglanCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0)) : nullptr;
		const UQinglanCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
		return Combat ? Combat->GetAttributes() : FQinglanAttributes();
	}

	/**
	 * Deterministic wave composition per room kind (blueprint 1.4: room 1 = one
	 * archetype 4-6 enemies; room 2 = two archetypes; room 4 elite = mixed wave;
	 * room 5 boss = boss + <=2 adds). Draws exclusively from the encounter
	 * stream (docs/01 section 6; no FMath::RandRange anywhere).
	 */
	void BuildRoomWave(FQinglanRunState& State, const UQinglanRealmDataAsset* Realm, int32 RoomIndex, TArray<UQinglanEnemyDataAsset*>& OutWave)
	{
		OutWave.Reset();
		if (!Realm || !Realm->Rooms.IsValidIndex(RoomIndex))
		{
			return;
		}
		const TArray<FQinglanArchetypeSpec>& Archetypes = QinglanEnemyCatalog::GetAllArchetypes();
		if (Archetypes.IsEmpty())
		{
			return;
		}
		FRandomStream& Stream = State.EncounterStream;
		auto SpecToAsset = [](const FQinglanArchetypeSpec& Spec) -> UQinglanEnemyDataAsset*
		{
			return QinglanEnemyCatalog::CreateDataAssetFromSpec(Spec, GetTransientPackage());
		};

		const EQinglanRoomKind Kind = Realm->GetRoomKind(RoomIndex);
		switch (Kind)
		{
		case EQinglanRoomKind::Combat:
		{
			// Room 1: one archetype; room 2 (draft room): two archetypes.
			const int32 ArchetypeCount = (RoomIndex >= 2) ? 2 : 1;
			const int32 FirstIndex = QinglanDeterminism::RollIndex(Stream, Archetypes.Num());
			const int32 SecondIndex = (Archetypes.Num() > 1)
				? (FirstIndex + 1 + QinglanDeterminism::RollIndex(Stream, Archetypes.Num() - 1)) % Archetypes.Num()
				: FirstIndex;
			const int32 TotalCount = 4 + QinglanDeterminism::RollIndex(Stream, 3); // 4-6
			for (int32 Count = 0; Count < TotalCount; ++Count)
			{
				const int32 Pick = (ArchetypeCount == 2 && (Count & 1)) ? SecondIndex : FirstIndex;
				if (UQinglanEnemyDataAsset* Data = SpecToAsset(Archetypes[Pick]))
				{
					OutWave.Add(Data);
				}
			}
			break;
		}
		case EQinglanRoomKind::Elite:
		{
			// Mixed wave: 3 archetypes, 5-6 enemies (blueprint 1.4 "mixed wave").
			const int32 FirstIndex = QinglanDeterminism::RollIndex(Stream, Archetypes.Num());
			const int32 TotalCount = 5 + QinglanDeterminism::RollIndex(Stream, 2);
			for (int32 Count = 0; Count < TotalCount; ++Count)
			{
				const int32 Pick = (FirstIndex + Count % 3) % Archetypes.Num();
				if (UQinglanEnemyDataAsset* Data = SpecToAsset(Archetypes[Pick]))
				{
					OutWave.Add(Data);
				}
			}
			break;
		}
		case EQinglanRoomKind::Boss:
		{
			const TArray<FQinglanArchetypeSpec>& Bosses = QinglanEnemyCatalog::GetAllBosses();
			const FName RealmBossId = UQinglanEconomyLibrary::GetBossIdForRealm(State.RealmId);
			const FQinglanArchetypeSpec* Chosen = Bosses.Num() > 0 ? &Bosses[0] : nullptr;
			for (const FQinglanArchetypeSpec& Boss : Bosses)
			{
				if (Boss.EnemyId == RealmBossId)
				{
					Chosen = &Boss;
					break;
				}
			}
			if (Chosen)
			{
				if (UQinglanEnemyDataAsset* BossData = SpecToAsset(*Chosen))
				{
					OutWave.Add(BossData); // 1 boss ...
				}
				for (int32 Add = 0; Add < 2; ++Add) // ... with at most two adds (docs/01 section 6)
				{
					const int32 Pick = QinglanDeterminism::RollIndex(Stream, Archetypes.Num());
					if (UQinglanEnemyDataAsset* AddData = SpecToAsset(Archetypes[Pick]))
					{
						OutWave.Add(AddData);
					}
				}
			}
			break;
		}
		case EQinglanRoomKind::Entrance:
		case EQinglanRoomKind::Shrine:
		case EQinglanRoomKind::Exit:
		default:
			break; // no encounter; entrance is loadout-confirm only (blueprint 1.4)
		}
	}

	/** The world's single encounter director (realm game mode owns spawning it). */
	AQinglanEncounterDirector* FindDirector(const UQinglanRunSubsystem* Subsystem)
	{
		const UGameInstance* Instance = Subsystem->GetGameInstance();
		UWorld* World = Instance ? Instance->GetWorld() : nullptr;
		return World
			? Cast<AQinglanEncounterDirector>(UGameplayStatics::GetActorOfClass(World, AQinglanEncounterDirector::StaticClass()))
			: nullptr;
	}

	/**
	 * Unique, journal-stable banking transaction id. Repeat expeditions of the
	 * same realm must never collide (a replayed id would silently skip the
	 * payout), so the counter probes the profile journal upward without an
	 * arbitrary cap (the journal's own 8192-entry invariant bound keeps the
	 * search finite); the journal makes the probe stable across reloads.
	 */
	FName MakeBankingTransactionId(UQinglanProfileSubsystem* Profile, FName RealmId)
	{
		if (!Profile)
		{
			return NAME_None;
		}
		FQinglanProfileSnapshot Snapshot;
		Profile->ExportStateForSave(Snapshot);
		// At most N candidates can be occupied by N journal entries.
		for (int32 Counter = 1; Counter <= Snapshot.CommittedTransactionIds.Num() + 1; ++Counter)
		{
			const FName Candidate(*FString::Printf(TEXT("tx.run.%s.%02d.bank"), *RealmId.ToString(), Counter));
			if (!Profile->WasTransactionCommitted(Candidate))
			{
				return Candidate;
			}
		}
		return NAME_None;
	}

	/**
	 * Durable save at the run's safe boundaries (entrance, completed-room
	 * reward, post-selection draft commit, banking; AUD-02). The save
	 * subsystem broadcasts OnSaveFailed for the UI when a write does not
	 * commit; the boundary caller only keeps a log breadcrumb.
	 */
	void CommitBoundarySave(const UQinglanRunSubsystem* Subsystem)
	{
		const UGameInstance* Instance = Subsystem->GetGameInstance();
		UQinglanSaveSubsystem* Save = Instance ? const_cast<UGameInstance*>(Instance)->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
		if (!Save)
		{
			return;
		}
		FText SaveError;
		if (!Save->SaveNow(SaveError))
		{
			UE_LOG(LogQinglan, Warning, TEXT("Run boundary save did not commit: %s"), *SaveError.ToString());
		}
	}

	/**
	 * Victory material top-up (AUD-08): the economy table's repeat material
	 * total is the guaranteed per-clear material payout; the elite room's
	 * guaranteed material is one unit of it granted early at room completion,
	 * so the banking grant tops the remainder up. Retreat/defeat keep only
	 * the per-stack floored loot (ApplyBankingMultiplier ran before this).
	 */
	bool AppendRepeatMaterialPayout(const FQinglanRunState& State, const UQinglanRealmDataAsset* Realm, FQinglanInventory& InOutGrant)
	{
		FQinglanInventory Repeat;
		if (!UQinglanEconomyLibrary::GetExpeditionPayout(State.RealmId, /*bFirstClear*/ false, Repeat))
		{
			return true; // tutorial: room rewards + elite drop already match the authored payout
		}
		int32 CommittedEliteRooms = 0;
		for (const int32 RoomId : State.CommittedRoomIds)
		{
			if (Realm && Realm->Rooms.IsValidIndex(RoomId) && Realm->GetRoomKind(RoomId) == EQinglanRoomKind::Elite)
			{
				++CommittedEliteRooms;
			}
		}
		FQinglanInventory Candidate = InOutGrant;
		const auto Add = [](int64& Amount, int64 Delta)
		{
			if (Amount < 0 || Delta < 0 || Delta > MAX_int64 - Amount) { return false; }
			Amount += Delta;
			return true;
		};
		if (State.RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
		{
			if (!Add(Candidate.VeinCrystal, FMath::Max<int64>(0, Repeat.VeinCrystal - CommittedEliteRooms))) { return false; }
		}
		else
		{
			if (!Add(Candidate.SpiritHerb, FMath::Max<int64>(0, Repeat.SpiritHerb - CommittedEliteRooms))
				|| !Add(Candidate.VeinCrystal, Repeat.VeinCrystal)) { return false; }
		}
		InOutGrant = Candidate;
		return true;
	}
}

// ---------------------------------------------------------------------------
// Expedition lifecycle
// ---------------------------------------------------------------------------

bool UQinglanRunSubsystem::StartExpedition(FName RealmId, int32 Seed)
{
	FQinglanRunState& State = StateFor(this);
	if (State.bInExpedition || State.bEndingExpedition)
	{
		UE_LOG(LogQinglan, Warning, TEXT("StartExpedition rejected: expedition to %s already open"), *State.RealmId.ToString());
		return false;
	}

	const bool bKnownRealm = RealmId == FName(UQinglanEconomyLibrary::BambooRealmIdText)
		|| RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText)
		|| RealmId == FName(UQinglanEconomyLibrary::TutorialRealmIdText);

	if (!bKnownRealm)
	{
		UE_LOG(LogQinglan, Warning, TEXT("StartExpedition rejected: unknown realm '%s'"), *RealmId.ToString());
		return false;
	}
	UQinglanSaveSubsystem* Save = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	if (!Save || !Save->IsProfileSessionActive())
	{
		UE_LOG(LogQinglan, Warning, TEXT("StartExpedition rejected: no active profile session"));
		return false;
	}
	State.bInExpedition = true;
	State.bNeedsCheckpointResume = false;
	State.RealmId = RealmId;
	State.RealmSeed = QinglanDeterminism::CanonicalSeed(Seed);
	State.EncounterStream = QinglanDeterminism::MakeEncounterStream(State.RealmSeed);
	State.EntryEncounterStreamState = State.EncounterStream.GetCurrentSeed();
	State.bHasRoomEncounterSnapshot = false;
	State.LootStream = QinglanDeterminism::MakeLootStream(State.RealmSeed);
	State.BlessingStream = QinglanDeterminism::MakeBlessingStream(State.RealmSeed);
	// Travel may save before the realm world exists: use a valid entrance
	// checkpoint now; EnterRoom(0) captures the actual realm entrance later.
	State.CurrentRoomIndex = 0;
	State.UnbankedLoot = FQinglanInventory();
	State.AcquiredBlessings.Reset();
	State.PendingBlessingOffer.Reset();
	State.PendingOfferRoom = INDEX_NONE;
	State.bDraftPending = false;
	State.CommittedRoomIds.Reset();
	State.EntryAttributes = SnapshotPlayerAttributes(this);
	Save->BeginExpeditionCheckpoint();

	// Force the template to exist (imported asset or synthesis) so the realm id
	// is available before the first entrance checkpoint.
	const UQinglanRealmDataAsset* Realm = ResolveRealmTemplate(this, RealmId);
	UE_LOG(LogQinglan, Log, TEXT("Expedition begin: realm %s seed %d (%d rooms)"),
		*RealmId.ToString(), State.RealmSeed, Realm ? Realm->GetRoomCount() : 0);
	return true;
}

bool UQinglanRunSubsystem::EnterRoom(int32 RoomIndex)
{
	FQinglanRunState& State = StateFor(this);
	if (!State.bInExpedition)
	{
		return false;
	}
	if (State.bDraftPending && RoomIndex != State.CurrentRoomIndex)
	{
		UE_LOG(LogQinglan, Warning, TEXT("EnterRoom rejected: select the pending blessing before leaving"));
		return false;
	}
	const UQinglanRealmDataAsset* Realm = ResolveRealmTemplate(this, State.RealmId);
	if (!Realm || RoomIndex < 0 || RoomIndex >= Realm->GetRoomCount())
	{
		UE_LOG(LogQinglan, Warning, TEXT("EnterRoom rejected: room %d outside the realm contract"), RoomIndex);
		return false;
	}

	// An unfinished room checkpoint stores its pre-wave RNG state, because
	// the new world must reproduce this room's wave rather than draw the next.
	if (!State.CommittedRoomIds.Contains(RoomIndex))
	{
		if (State.CurrentRoomIndex == RoomIndex && State.bHasRoomEncounterSnapshot)
		{
			State.EncounterStream = QinglanDeterminism::RestoreStream(State.EntryEncounterStreamState);
		}
		State.EntryEncounterStreamState = State.EncounterStream.GetCurrentSeed();
		State.bHasRoomEncounterSnapshot = true;
	}
	State.CurrentRoomIndex = RoomIndex;
	// Pre-room snapshot: this exact attributes value rides the checkpoint
	// (docs/05 section 6 crash-resume restores room-entrance vitals).
	State.EntryAttributes = SnapshotPlayerAttributes(this);
	OnRoomEntered.Broadcast(RoomIndex);

	// Room pipeline drives the encounter: the realm game mode owns the
	// director actor; combat/elite/boss rooms begin their wave now, so doors
	// and rewards stay driven by exactly one trigger (blueprint 1.4). An
	// already-committed room never respawns its wave (crash-resume re-entry
	// keeps cleared rooms cleared, blueprint 1.6).
	TArray<UQinglanEnemyDataAsset*> Wave;
	if (State.CommittedRoomIds.Contains(RoomIndex))
	{
		UE_LOG(LogQinglan, Verbose, TEXT("EnterRoom: room %d already committed; no respawn"), RoomIndex);
		return true;
	}
	BuildRoomWave(State, Realm, RoomIndex, Wave);
	if (!Wave.IsEmpty())
	{
		if (AQinglanEncounterDirector* Director = FindDirector(this))
		{
			Director->SetRoomOrigin(QinglanRoomLayout::RoomCenter(RoomIndex));
			Director->BeginEncounter(Wave, RoomIndex, Realm->GetRoomKind(RoomIndex) == EQinglanRoomKind::Boss);
		}
		else
		{
			// No world/director yet (hub-side preview, headless automation):
			// the room still counts as entered; rewards stay committed once.
			UE_LOG(LogQinglan, Verbose, TEXT("EnterRoom: no encounter director in the world; room %d armed without a wave"), RoomIndex);
		}
	}

	// Room entrance is a safe checkpoint boundary (blueprint section 1.6);
	// the durable write persists the pre-room snapshot for crash resume (AUD-02).
	CommitBoundarySave(this);
	return true;
}

void UQinglanRunSubsystem::CompleteCurrentRoom(FName TransactionId)
{
	FQinglanRunState& State = StateFor(this);
	if (!State.bInExpedition)
	{
		return;
	}
	// Intentionally unused: room reward idempotency is per-expedition through
	// CommittedRoomIds (the checkpoint field), not the profile journal; the
	// parameterless room-id key survives crash-resume replays (docs/05 6).
	(void)TransactionId;

	const int32 RoomIndex = State.CurrentRoomIndex;
	const UQinglanRealmDataAsset* Realm = ResolveRealmTemplate(this, State.RealmId);
	if (RoomIndex == INDEX_NONE || !Realm || !Realm->Rooms.IsValidIndex(RoomIndex))
	{
		return;
	}
	if (State.CommittedRoomIds.Contains(RoomIndex))
	{
		return; // already awarded this room (idempotent replay, docs/05 section 6)
	}
	State.CommittedRoomIds.Add(RoomIndex);

	const int64 Shards = Realm->GetRoomShards(RoomIndex);
	State.UnbankedLoot.SpiritShards += Shards;
	// Elite rooms drop one guaranteed material (blueprint 1.4); the material
	// follows the realm's progression stage (herb for Qi-stage realms,
	// crystal for the ashen range).
	if (Realm->GetRoomKind(RoomIndex) == EQinglanRoomKind::Elite)
	{
		if (State.RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
		{
			++State.UnbankedLoot.VeinCrystal;
		}
		else
		{
			++State.UnbankedLoot.SpiritHerb;
		}
	}

	// Boss proof is earned by this exact encounter, independent of whether
	// the player later wins, retreats, or dies. Never infer it from realm ID.
	if (Realm->GetRoomKind(RoomIndex) == EQinglanRoomKind::Boss)
	{
		if (UQinglanProfileSubsystem* Profile = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanProfileSubsystem>() : nullptr)
		{
			if (State.RealmId == FName(UQinglanEconomyLibrary::BambooRealmIdText))
			{
				Profile->SetCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag));
			}
			else if (State.RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText))
			{
				Profile->SetCampaignFlag(FName(QinglanProgression::AshenClearedCampaignFlag));
			}
		}
	}
	// Blessing draft after rooms 2 and 4 (blueprint 1.4; docs/03 section 5):
	// the roll consumes the blessings stream exactly once per draft room.
	if (IsBlessingDraftRoom(RoomIndex))
	{
		State.PendingBlessingOffer.Reset();
		QinglanBlessingDrafting::RollOffer(State.BlessingStream, State.AcquiredBlessings, State.PendingBlessingOffer);
		State.bDraftPending = !State.PendingBlessingOffer.IsEmpty();
		State.PendingOfferRoom = State.bDraftPending ? RoomIndex : INDEX_NONE;
	}

	// AUD-02/06: persist both the reward and the exact pending offer. Waiting
	// for a selection would leave the promised completed-room boundary unsaved.
	CommitBoundarySave(this);
	// Reward observers can now query the actual offer and committed room state.
	OnRoomCompleted.Broadcast(RoomIndex, Shards);
}

void UQinglanRunSubsystem::EndExpedition(bool bVictory, bool bVoluntaryRetreat)
{
	FQinglanRunState& State = StateFor(this);
	if (!State.bInExpedition || State.bEndingExpedition)
	{
		return;
	}
	TGuardValue<bool> EndingGuard(State.bEndingExpedition, true);
	UGameInstance* Instance = GetGameInstance();
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	const UQinglanRealmDataAsset* Realm = ResolveRealmTemplate(this, State.RealmId);
	bool bBossRoomCompleted = false;
	for (const int32 RoomId : State.CommittedRoomIds)
	{
		if (Realm && Realm->Rooms.IsValidIndex(RoomId) && Realm->GetRoomKind(RoomId) == EQinglanRoomKind::Boss)
		{
			bBossRoomCompleted = true;
		}
	}
	if (!Profile || !Save || !Save->IsProfileSessionActive() || (bVictory && !bBossRoomCompleted))
	{
		UE_LOG(LogQinglan, Warning, TEXT("Banking rejected: missing profile or unearned victory"));
		OnBankingFailed.Broadcast();
		return;
	}

	FQinglanInventory Banked = UQinglanEconomyLibrary::ApplyBankingMultiplier(State.UnbankedLoot, bVictory, bVoluntaryRetreat);
	FQinglanInventory FirstClearBonus;
	FName FirstClearId = NAME_None;
	if (bVictory)
	{
		if (!AppendRepeatMaterialPayout(State, Realm, Banked))
		{
			UE_LOG(LogQinglan, Warning, TEXT("Banking rejected: material top-up overflow"));
			OnBankingFailed.Broadcast();
			return;
		}
		FQinglanInventory First, Repeat;
		if (UQinglanEconomyLibrary::GetExpeditionPayout(State.RealmId, true, First)
			&& UQinglanEconomyLibrary::GetExpeditionPayout(State.RealmId, false, Repeat))
		{
			// Full first-clear totals include base rewards, not another material
			// stack. Only their difference is the one-time bonus (AUD-08).
			FirstClearBonus.SpiritShards = First.SpiritShards - Repeat.SpiritShards;
			FirstClearBonus.SpiritHerb = First.SpiritHerb - Repeat.SpiritHerb;
			FirstClearBonus.VeinCrystal = First.VeinCrystal - Repeat.VeinCrystal;
			FirstClearId = UQinglanEconomyLibrary::MakeFirstClearTransactionId(NAME_None,
				UQinglanEconomyLibrary::GetBossIdForRealm(State.RealmId));
		}
	}
	const FName BankTx = MakeBankingTransactionId(Profile, State.RealmId);
	if (BankTx.IsNone() || !Profile->TryBankExpedition(BankTx, Banked, FirstClearId, FirstClearBonus))
	{
		// No partial grant/bonus/journal mutation: leave all run loot retryable.
		UE_LOG(LogQinglan, Warning, TEXT("Banking rejected: allocation, capacity or inventory overflow"));
		OnBankingFailed.Broadcast();
		return;
	}

	// Profile commit callbacks above saw an open expedition and skipped the hub
	// autosave. Close it now, then save profile + absence of checkpoint together.
	State.bInExpedition = false;
	State.bNeedsCheckpointResume = false;
	State.AcquiredBlessings.Reset();
	State.EntryEncounterStreamState = 0;
	State.bHasRoomEncounterSnapshot = false;
	State.PendingBlessingOffer.Reset();
	State.PendingOfferRoom = INDEX_NONE;
	State.bDraftPending = false;
	State.UnbankedLoot = FQinglanInventory();
	State.CommittedRoomIds.Reset();
	State.CurrentRoomIndex = INDEX_NONE;
	State.EntryAttributes = FQinglanAttributes();
	CommitBoundarySave(this);
	// A disk failure retains the banked in-memory profile and last valid disk
	// snapshot; guarded travel retries SaveActiveSession before leaving. The
	// player must still get a retry/error UI (AUD-02/51), not a success claim.
	OnExpeditionEnded.Broadcast();
	UE_LOG(LogQinglan, Log, TEXT("Expedition ended (victory=%d voluntary=%d)"), bVictory ? 1 : 0, bVoluntaryRetreat ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool UQinglanRunSubsystem::IsInExpedition() const
{
	const FQinglanRunState* State = FindStateFor(this);
	return State && State->bInExpedition;
}

int32 UQinglanRunSubsystem::GetCurrentRoomIndex() const
{
	const FQinglanRunState* State = FindStateFor(this);
	return (State && State->bInExpedition) ? State->CurrentRoomIndex : INDEX_NONE;
}

EQinglanRoomKind UQinglanRunSubsystem::GetCurrentRoomKind() const
{
	const FQinglanRunState* State = FindStateFor(this);
	if (!State || !State->bInExpedition)
	{
		return EQinglanRoomKind::Entrance;
	}
	const UQinglanRealmDataAsset* Realm = ResolveRealmTemplate(this, State->RealmId);
	return (Realm && Realm->Rooms.IsValidIndex(State->CurrentRoomIndex))
		? Realm->GetRoomKind(State->CurrentRoomIndex)
		: EQinglanRoomKind::Entrance;
}

const UQinglanRealmDataAsset* UQinglanRunSubsystem::GetCurrentRealm() const
{
	const FQinglanRunState* State = FindStateFor(this);
	if (!State || !State->bInExpedition)
	{
		return nullptr;
	}
	return ResolveRealmTemplate(this, State->RealmId);
}

// ---------------------------------------------------------------------------
// Blessing draft (docs/03 section 5: 3 distinct options, one-stack cap)
// ---------------------------------------------------------------------------

bool UQinglanRunSubsystem::GetBlessingOffer(TArray<FQinglanBlessing>& OutOffers)
{
	OutOffers.Reset();
	FQinglanRunState& State = StateFor(this);
	if (!State.bInExpedition || !State.bDraftPending)
	{
		return false;
	}
	for (const FName BlessingId : State.PendingBlessingOffer)
	{
		FQinglanBlessing Entry;
		if (UQinglanBlessingLibrary::MakeBlessingEntry(BlessingId, Entry))
		{
			OutOffers.Add(MoveTemp(Entry));
		}
	}
	return !OutOffers.IsEmpty();
}

bool UQinglanRunSubsystem::AcceptBlessing(FName BlessingId)
{
	FQinglanRunState& State = StateFor(this);
	if (!State.bInExpedition || !State.bDraftPending)
	{
		return false;
	}
	if (!State.PendingBlessingOffer.Contains(BlessingId))
	{
		return false; // only the offered options are selectable (mutually exclusive)
	}
	if (!UQinglanBlessingLibrary::IsBlessingOfferable(BlessingId, State.AcquiredBlessings))
	{
		return false; // one-stack cap: never owned twice (docs/03 section 5)
	}
	State.AcquiredBlessings.Add(BlessingId);
	// One pick consumes the draft; remaining options vanish (docs/03 section 5).
	State.PendingBlessingOffer.Reset();
	State.PendingOfferRoom = INDEX_NONE;
	State.bDraftPending = false;
	// Replace the pending-offer checkpoint with the selected blessing: the
	// chosen blessing is granted exactly once and must survive a quit now
	// (AUD-06 acceptance: "a chosen blessing is granted once").
	CommitBoundarySave(this);
	return true;
}

FQinglanInventory UQinglanRunSubsystem::GetUnbankedLoot() const
{
	const FQinglanRunState* State = FindStateFor(this);
	return State ? State->UnbankedLoot : FQinglanInventory();
}

void UQinglanRunSubsystem::ResetRun()
{
	// AUD-04: discard the entire profile-owned session, including cached realm,
	// RNGs, draft, loot and room snapshot. This is not a completed expedition:
	// do not broadcast OnExpeditionEnded (subscribers may trigger banking/travel).
	RunStates().Remove(this);
	// TODO(AUD-04/AUD-51): profile selection must tear down the old world/pawn
	// before resuming another profile; this reset only owns subsystem state.
}

FName UQinglanRunSubsystem::GetPendingCheckpointRealm() const
{
	const FQinglanRunState* State = FindStateFor(this);
	return State && State->bInExpedition && State->bNeedsCheckpointResume ? State->RealmId : NAME_None;
}

bool UQinglanRunSubsystem::NeedsCheckpointResume() const
{
	return !GetPendingCheckpointRealm().IsNone();
}

FName UQinglanRunSubsystem::GetPendingCheckpointMap() const
{
	const FName Realm = GetPendingCheckpointRealm();
	if (Realm == FName(UQinglanEconomyLibrary::BambooRealmIdText)
		|| Realm == FName(UQinglanEconomyLibrary::TutorialRealmIdText))
	{
		return FName(QinglanInteraction::BambooMapPath);
	}
	if (Realm == FName(UQinglanEconomyLibrary::AshenRealmIdText))
	{
		return FName(QinglanInteraction::AshenMapPath);
	}
	return NAME_None;
}

void UQinglanRunSubsystem::MarkCheckpointResumed()
{
	if (FQinglanRunState* State = RunStates().Find(this))
	{
		State->bNeedsCheckpointResume = false;
	}
}

// ---------------------------------------------------------------------------
// Checkpoint bridge (pre-room snapshot; docs/05 section 6 crash-resume)
// ---------------------------------------------------------------------------

void UQinglanRunSubsystem::ExportCheckpoint(FQinglanCheckpoint& Out) const
{
	const FQinglanRunState* State = FindStateFor(this);
	if (!State || !State->bInExpedition)
	{
		Out = FQinglanCheckpoint();
		return;
	}
	Out = FQinglanCheckpoint();
	Out.RealmId = State->RealmId;
	Out.RealmSeed = State->RealmSeed;
	// Generator states serialize so the next draw after resume equals the next
	// draw at save time (QinglanDeterminism::RestoreStream contract).
	Out.EncounterStreamState = State->bHasRoomEncounterSnapshot && !State->CommittedRoomIds.Contains(State->CurrentRoomIndex)
		? State->EntryEncounterStreamState : State->EncounterStream.GetCurrentSeed();
	Out.LootStreamState = State->LootStream.GetCurrentSeed();
	Out.BlessingStreamState = State->BlessingStream.GetCurrentSeed();
	Out.RoomIndex = State->CurrentRoomIndex;
	Out.AcquiredBlessings = State->AcquiredBlessings;
	Out.PendingBlessingOffer = State->PendingBlessingOffer;
	Out.PendingBlessingRoom = State->PendingOfferRoom;
	Out.PlayerAttributes = State->EntryAttributes;
	Out.Loot = State->UnbankedLoot;
	Out.CommittedRoomIds = State->CommittedRoomIds;
}

void UQinglanRunSubsystem::ImportCheckpoint(const FQinglanCheckpoint& In)
{
	// Replace, rather than merge with, the previous profile's run state.
	ResetRun();
	if (In.RealmId == NAME_None)
	{
		return; // Explicit empty checkpoint also clears a previous expedition.
	}
	FQinglanRunState& State = StateFor(this);
	State.bInExpedition = true;
	State.bNeedsCheckpointResume = true;
	State.RealmId = In.RealmId;
	State.RealmSeed = In.RealmSeed > 0 ? In.RealmSeed : 1;
	State.EncounterStream = QinglanDeterminism::RestoreStream(In.EncounterStreamState);
	State.LootStream = QinglanDeterminism::RestoreStream(In.LootStreamState);
	State.BlessingStream = QinglanDeterminism::RestoreStream(In.BlessingStreamState);
	State.CurrentRoomIndex = In.RoomIndex;
	State.AcquiredBlessings = In.AcquiredBlessings;
	State.EntryAttributes = In.PlayerAttributes;
	State.UnbankedLoot = In.Loot;
	State.CommittedRoomIds = In.CommittedRoomIds;
	// AUD-06: importing consumes no random draw and keeps the same choices.
	// The save subsystem validates offer/room pairing before calling this bridge.
	State.PendingBlessingOffer = In.PendingBlessingOffer;
	State.PendingOfferRoom = In.PendingBlessingRoom;
	State.bDraftPending = !State.PendingBlessingOffer.IsEmpty();
	// TODO(AUD-06): migrate legacy saves that already committed a draft room
	// without an offer. Its original roll cannot be recovered from current data;
	// choose/document an explicit compatibility policy rather than reroll silently.

	ResolveRealmTemplate(this, State.RealmId); // ensure the template exists for queries
	UE_LOG(LogQinglan, Log, TEXT("Checkpoint resumed: realm %s room %d"), *State.RealmId.ToString(), State.CurrentRoomIndex);
}
