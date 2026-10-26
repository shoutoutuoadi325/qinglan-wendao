// Direct run/checkpoint regressions. Transient instances intentionally omit the
// registered save subsystem and world: no disk writes, travel, or combat actors.
// Full StartExpedition / EndExpedition payout integration still needs an engine fixture.
#include "Misc/AutomationTest.h"

#include "Engine/GameInstance.h"
#include "Persistence/QinglanSaveGame.h"
#include "Progression/QinglanDeterminism.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	constexpr EAutomationTestFlags RunCheckpointTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter;

	FQinglanCheckpoint MakePendingDraftCheckpoint()
	{
		FQinglanCheckpoint Checkpoint;
		Checkpoint.RealmId = FName(TEXT("bamboo_hollow"));
		Checkpoint.RealmSeed = 73;
		Checkpoint.EncounterStreamState = MIN_int32;
		Checkpoint.LootStreamState = 0;
		Checkpoint.BlessingStreamState = -917;
		Checkpoint.RoomIndex = 2;
		Checkpoint.AcquiredBlessings = { FName(TEXT("b_sharp_edge")) };
		Checkpoint.PendingBlessingOffer = { FName(TEXT("b_quiet_current")),
			FName(TEXT("b_jade_shell")), FName(TEXT("b_thunder_mandate")) };
		Checkpoint.PendingBlessingRoom = 2;
		Checkpoint.CommittedRoomIds = { 0, 1, 2 };
		Checkpoint.Loot.SpiritShards = 70;
		Checkpoint.Loot.SpiritHerb = 1;
		Checkpoint.PlayerAttributes.Health = 37.f;
		Checkpoint.PlayerAttributes.MaxHealth = 120.f;
		return Checkpoint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRunPendingDraftCheckpoint,
	"Qinglan.Persistence.RunPendingDraftRoundtrip", RunCheckpointTestFlags)

bool FQinglanRunPendingDraftCheckpoint::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	const FQinglanCheckpoint In = MakePendingDraftCheckpoint();
	Run->ImportCheckpoint(In);
	TestTrue(TEXT("Import opens the expedition"), Run->IsInExpedition());
	TestTrue(TEXT("Imported run needs world resume"), Run->NeedsCheckpointResume());
	TestEqual(TEXT("Import restores resume realm"), Run->GetPendingCheckpointRealm(), In.RealmId);
	TestEqual(TEXT("Import restores current room"), Run->GetCurrentRoomIndex(), In.RoomIndex);
	TestNotNull(TEXT("Known realm resolves to an actual template"), Run->GetCurrentRealm());
	FQinglanCheckpoint Out;
	Run->ExportCheckpoint(Out);
	TestEqual(TEXT("Master seed survives"), Out.RealmSeed, In.RealmSeed);
	TestEqual(TEXT("Negative encounter state survives"), Out.EncounterStreamState, In.EncounterStreamState);
	TestEqual(TEXT("Zero loot state survives"), Out.LootStreamState, In.LootStreamState);
	TestEqual(TEXT("Negative blessing state survives"), Out.BlessingStreamState, In.BlessingStreamState);
	TestTrue(TEXT("Acquired blessings survive"), Out.AcquiredBlessings == In.AcquiredBlessings);
	TestTrue(TEXT("Pending offer survives in authored order"), Out.PendingBlessingOffer == In.PendingBlessingOffer);
	TestEqual(TEXT("Pending room survives"), Out.PendingBlessingRoom, In.PendingBlessingRoom);
	TestTrue(TEXT("Committed room IDs survive"), Out.CommittedRoomIds == In.CommittedRoomIds);
	TestEqual(TEXT("Unbanked shards survive"), Out.Loot.SpiritShards, In.Loot.SpiritShards);
	TestEqual(TEXT("Unbanked materials survive"), Out.Loot.SpiritHerb, In.Loot.SpiritHerb);
	TestEqual(TEXT("Entrance health survives"), Out.PlayerAttributes.Health, In.PlayerAttributes.Health);
	TestEqual(TEXT("Entrance max health survives"), Out.PlayerAttributes.MaxHealth, In.PlayerAttributes.MaxHealth);
	Run->MarkCheckpointResumed();
	TestFalse(TEXT("World resume consumes the travel request"), Run->NeedsCheckpointResume());
	TestEqual(TEXT("Resumed run has no pending travel realm"), Run->GetPendingCheckpointRealm(), FName());
	TestTrue(TEXT("Consuming travel keeps expedition active"), Run->IsInExpedition());

	for (int32 Read = 0; Read < 2; ++Read)
	{
		TArray<FQinglanBlessing> Offers;
		TestTrue(TEXT("Pending draft is available repeatedly"), Run->GetBlessingOffer(Offers));
		TestEqual(TEXT("Exactly three options remain"), Offers.Num(), 3);
		for (int32 Index = 0; Index < Offers.Num() && Index < In.PendingBlessingOffer.Num(); ++Index)
		{
			TestEqual(TEXT("Reading does not reroll options"), Offers[Index].BlessingId, In.PendingBlessingOffer[Index]);
		}
	}
	TestFalse(TEXT("Unlisted blessing cannot consume draft"), Run->AcceptBlessing(FName(TEXT("b_still_water"))));
	Run->ExportCheckpoint(Out);
	TestEqual(TEXT("Offer reads and invalid choice consume no RNG"), Out.BlessingStreamState, In.BlessingStreamState);
	TestTrue(TEXT("Offered blessing is accepted"), Run->AcceptBlessing(In.PendingBlessingOffer[0]));
	TestFalse(TEXT("Consumed draft cannot grant twice"), Run->AcceptBlessing(In.PendingBlessingOffer[0]));
	Run->ExportCheckpoint(Out);
	TestEqual(TEXT("One blessing was added"), Out.AcquiredBlessings.Num(), 2);
	TestTrue(TEXT("Selected blessing is retained"), Out.AcquiredBlessings.Contains(In.PendingBlessingOffer[0]));
	TestTrue(TEXT("Selected draft is removed"), Out.PendingBlessingOffer.IsEmpty());
	TestEqual(TEXT("Selected draft no longer owns a room"), Out.PendingBlessingRoom, INDEX_NONE);
	TestEqual(TEXT("Selection does not advance RNG"), Out.BlessingStreamState, In.BlessingStreamState);
	Run->ImportCheckpoint(Out);
	TArray<FQinglanBlessing> Offers;
	TestFalse(TEXT("Reloading a selected draft cannot offer again"), Run->GetBlessingOffer(Offers));
	Run->ResetRun();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRunCheckpointReplacement,
	"Qinglan.Persistence.RunCheckpointReplacement", RunCheckpointTestFlags)

bool FQinglanRunCheckpointReplacement::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	const FQinglanCheckpoint Old = MakePendingDraftCheckpoint();
	Run->ImportCheckpoint(Old);
	FQinglanCheckpoint Replacement;
	Replacement.RealmId = FName(TEXT("ashen_peak"));
	Replacement.RealmSeed = 19;
	Replacement.RoomIndex = 1;
	Run->ImportCheckpoint(Replacement);
	FQinglanCheckpoint Out;
	Run->ExportCheckpoint(Out);
	TestEqual(TEXT("Profile replacement changes realm"), Out.RealmId, Replacement.RealmId);
	TestEqual(TEXT("Profile replacement changes room"), Run->GetCurrentRoomIndex(), 1);
	TestEqual(TEXT("Old profile loot cannot leak"), Run->GetUnbankedLoot().SpiritShards, int64(0));
	TestTrue(TEXT("Old blessings cannot leak"), Out.AcquiredBlessings.IsEmpty());
	TestTrue(TEXT("Old pending draft cannot leak"), Out.PendingBlessingOffer.IsEmpty());
	TestTrue(TEXT("Old committed rooms cannot leak"), Out.CommittedRoomIds.IsEmpty());
	TestEqual(TEXT("Replacement zero stream is preserved"), Out.BlessingStreamState, 0);

	// Exercise both reset paths against a populated run and dirty output payload.
	for (const bool bImportEmpty : { false, true })
	{
		Run->ImportCheckpoint(Old);
		if (bImportEmpty) { Run->ImportCheckpoint(FQinglanCheckpoint()); }
		else { Run->ResetRun(); }
		TestFalse(TEXT("Cleared run is no longer open"), Run->IsInExpedition());
		TestFalse(TEXT("Cleared run never needs world resume"), Run->NeedsCheckpointResume());
		TestEqual(TEXT("Cleared run has no resume realm"), Run->GetPendingCheckpointRealm(), FName());
		TestEqual(TEXT("Cleared run has no current room"), Run->GetCurrentRoomIndex(), INDEX_NONE);
		TestNull(TEXT("Cleared run has no cached realm"), Run->GetCurrentRealm());
		Out = Old;
		Run->ExportCheckpoint(Out);
		TestEqual(TEXT("Export clears stale output realm"), Out.RealmId, FName());
		TestEqual(TEXT("Export clears stale stream state"), Out.EncounterStreamState, 0);
		TestEqual(TEXT("Export clears stale loot"), Out.Loot.SpiritShards, int64(0));
		TestTrue(TEXT("Export clears stale blessings"), Out.AcquiredBlessings.IsEmpty());
		TestTrue(TEXT("Export clears stale offer"), Out.PendingBlessingOffer.IsEmpty());
		TestTrue(TEXT("Export clears stale room commits"), Out.CommittedRoomIds.IsEmpty());
		TestEqual(TEXT("Export clears stale pending room"), Out.PendingBlessingRoom, INDEX_NONE);
		TArray<FQinglanBlessing> Offers;
		TestFalse(TEXT("Cleared run cannot expose old draft"), Run->GetBlessingOffer(Offers));
		Run->ResetRun(); // GetBlessingOffer may allocate an empty internal state.
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRunSignedStreamResume,
	"Qinglan.Persistence.RunSignedStreamResume", RunCheckpointTestFlags)

bool FQinglanRunSignedStreamResume::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	for (const int32 Seed : { MIN_int32, -917, 0, 1, MAX_int32 })
	{
		FRandomStream Uninterrupted(Seed);
		for (int32 Draw = 0; Draw < 7; ++Draw) { Uninterrupted.GetFraction(); }
		FQinglanCheckpoint In;
		In.RealmId = FName(TEXT("bamboo_hollow"));
		In.RealmSeed = 73;
		In.EncounterStreamState = Seed;
		In.LootStreamState = 0;
		In.BlessingStreamState = Uninterrupted.GetCurrentSeed();
		Run->ImportCheckpoint(In);
		FQinglanCheckpoint Out;
		Run->ExportCheckpoint(Out);
		TestEqual(TEXT("Every signed encounter state roundtrips exactly"), Out.EncounterStreamState, Seed);
		TestEqual(TEXT("Zero generator state never gets reseeded"), Out.LootStreamState, 0);
		FRandomStream Resumed = QinglanDeterminism::RestoreStream(Out.BlessingStreamState);
		for (int32 Draw = 0; Draw < 8; ++Draw)
		{
			TestEqual(TEXT("Resumed draws equal uninterrupted engine draws"), Resumed.GetFraction(), Uninterrupted.GetFraction());
		}
	}
	Run->ResetRun();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRunDraftRoomReplay,
	"Qinglan.Persistence.RunDraftRoomReplay", RunCheckpointTestFlags)

bool FQinglanRunDraftRoomReplay::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	FQinglanCheckpoint Entrance;
	Entrance.RealmId = FName(TEXT("bamboo_hollow"));
	Entrance.RealmSeed = 73;
	Entrance.RoomIndex = 2;
	Entrance.BlessingStreamState = -917;
	Run->ImportCheckpoint(Entrance);
	Run->CompleteCurrentRoom(FName(TEXT("tx.room.first")));
	FQinglanCheckpoint Completed;
	Run->ExportCheckpoint(Completed);
	TestEqual(TEXT("Actual room completion grants its authored shards"), Completed.Loot.SpiritShards, int64(40));
	TestTrue(TEXT("Room completion journals the room"), Completed.CommittedRoomIds.Contains(2));
	TestEqual(TEXT("Actual room completion rolls three options"), Completed.PendingBlessingOffer.Num(), 3);
	TestEqual(TEXT("Actual pending draft belongs to the completed room"), Completed.PendingBlessingRoom, 2);
	TestTrue(TEXT("Actual draft advances its RNG"), Completed.BlessingStreamState != Entrance.BlessingStreamState);
	Run->ImportCheckpoint(Completed);
	Run->CompleteCurrentRoom(FName(TEXT("tx.room.replayed.with.new.id")));
	FQinglanCheckpoint Replayed;
	Run->ExportCheckpoint(Replayed);
	TestEqual(TEXT("Committed room cannot grant again after reload"), Replayed.Loot.SpiritShards, Completed.Loot.SpiritShards);
	TestEqual(TEXT("Committed room cannot draw RNG again"), Replayed.BlessingStreamState, Completed.BlessingStreamState);
	TestTrue(TEXT("Committed room preserves exact offer"), Replayed.PendingBlessingOffer == Completed.PendingBlessingOffer);
	TestTrue(TEXT("Committed room cannot duplicate its journal entry"), Replayed.CommittedRoomIds == Completed.CommittedRoomIds);
	Run->ResetRun();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRunUnfinishedRoomResume,
	"Qinglan.Persistence.RunUnfinishedRoomResume", RunCheckpointTestFlags)

bool FQinglanRunUnfinishedRoomResume::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Uninterrupted = NewObject<UQinglanRunSubsystem>(Owner);
	UQinglanRunSubsystem* Resumed = NewObject<UQinglanRunSubsystem>(Owner);
	FQinglanCheckpoint Entry;
	Entry.RealmId = FName(TEXT("bamboo_hollow"));
	Entry.RealmSeed = 73;
	Entry.RoomIndex = 1;
	Entry.EncounterStreamState = MIN_int32;
	Entry.CommittedRoomIds = { 0 };
	Uninterrupted->ImportCheckpoint(Entry);
	TestTrue(TEXT("Actual combat room entry succeeds without a world"), Uninterrupted->EnterRoom(1));
	FQinglanCheckpoint SavedMidRoom;
	Uninterrupted->ExportCheckpoint(SavedMidRoom);
	TestEqual(TEXT("Unfinished room saves its pre-wave generator state"),
		SavedMidRoom.EncounterStreamState, Entry.EncounterStreamState);
	Uninterrupted->CompleteCurrentRoom(FName(TEXT("tx.room.uninterrupted")));
	FQinglanCheckpoint Expected;
	Uninterrupted->ExportCheckpoint(Expected);
	TestTrue(TEXT("Wave actually consumed encounter randomness"), Expected.EncounterStreamState != Entry.EncounterStreamState);
	TestEqual(TEXT("Actual combat room grants its shards"), Expected.Loot.SpiritShards, int64(30));

	Resumed->ImportCheckpoint(SavedMidRoom);
	TestTrue(TEXT("Reload re-arms the unfinished combat room"), Resumed->EnterRoom(1));
	TestTrue(TEXT("Retry of same unfinished room can re-arm"), Resumed->EnterRoom(1));
	Resumed->CompleteCurrentRoom(FName(TEXT("tx.room.resumed")));
	FQinglanCheckpoint Actual;
	Resumed->ExportCheckpoint(Actual);
	TestEqual(TEXT("Reload and retry consume exactly the original wave draws"),
		Actual.EncounterStreamState, Expected.EncounterStreamState);
	TestEqual(TEXT("Reload and retry preserve original reward"), Actual.Loot.SpiritShards, Expected.Loot.SpiritShards);
	TestTrue(TEXT("Reload and retry preserve room journal"), Actual.CommittedRoomIds == Expected.CommittedRoomIds);
	Uninterrupted->ResetRun();
	Resumed->ResetRun();
	return true;
}
