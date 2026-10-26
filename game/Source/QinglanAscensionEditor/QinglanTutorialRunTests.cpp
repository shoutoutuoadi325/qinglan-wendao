// Guided CH01 settlement uses production room/draft/checkpoint/banking APIs.
// These headless fixtures simulate director completion callbacks; they do not
// establish rendered combat or interaction acceptance.
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGuidedTutorialReturnAndPayout,
	"Qinglan.Progression.GuidedTutorialReturnAndPayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanGuidedTutorialReturnAndPayout::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
		FString::Printf(TEXT("GuidedReturn_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UGameInstance* Instance = nullptr;
	ON_SCOPE_EXIT
	{
		if (Instance) { Instance->Shutdown(); }
		UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestTrue(TEXT("Isolated save directory created"), IFileManager::Get().MakeDirectory(*Directory, true))) { return false; }
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
	Instance = NewObject<UGameInstance>(GetTransientPackage());
	Instance->Init();
	UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
	UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>();
	UQinglanRunSubsystem* Run = Instance->GetSubsystem<UQinglanRunSubsystem>();
	if (!TestNotNull(TEXT("Save subsystem"), Save) || !TestNotNull(TEXT("Profile subsystem"), Profile)
		|| !TestNotNull(TEXT("Run subsystem"), Run)) { return false; }
	Save->CreateNewProfile(0);
	if (!TestTrue(TEXT("Fresh profile is active"), Save->IsProfileSessionActive()) ||
		!TestTrue(TEXT("Guided expedition starts"), Run->StartExpedition(FName(UQinglanEconomyLibrary::TutorialRealmIdText), 73)))
	{
		return false;
	}
	const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
	if (!TestNotNull(TEXT("Guided realm resolves"), Realm)) { return false; }
	TestEqual(TEXT("Guided route has only entrance, two fights and shrine"), Realm->GetRoomCount(), 4);
	TestEqual(TEXT("Return room remains an actual safe shrine"), Realm->GetRoomKind(3), EQinglanRoomKind::Shrine);
	TestEqual(TEXT("Guided route contains no artificial guardian"), Run->GetBossRoomIndex(), INDEX_NONE);
	TestTrue(TEXT("Shrine can be queried before combat in the isolated fixture"), Run->EnterRoom(3));
	TestFalse(TEXT("Merely standing at the shrine cannot claim victory"), Run->CanClaimVictory());
	for (const int32 Room : { 1, 2 })
	{
		TestTrue(TEXT("Guided combat room entered"), Run->EnterRoom(Room));
		Run->CompleteCurrentRoom(FName(*FString::Printf(TEXT("tx.guided.room%d"), Room)));
	}
	TestEqual(TEXT("Both combat rooms earn the entire authored shard payout"), Run->GetUnbankedLoot().SpiritShards,
		UQinglanEconomyLibrary::TutorialPayoutShards);
	TestEqual(TEXT("Second combat earns the tutorial's guaranteed herb"), Run->GetUnbankedLoot().SpiritHerb,
		UQinglanEconomyLibrary::TutorialPayoutHerbs);
	Run->CompleteCurrentRoom(FName(TEXT("tx.guided.room2.replay")));
	TestEqual(TEXT("Replaying the room completion cannot duplicate the herb"), Run->GetUnbankedLoot().SpiritHerb, int64(1));
	AddExpectedMessage(TEXT("EnterRoom rejected: select the pending blessing before leaving"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("The player must choose the actual room-two blessing before returning"), Run->EnterRoom(3));
	FQinglanCheckpoint Pending;
	Run->ExportCheckpoint(Pending);
	TestEqual(TEXT("Pending draft is checkpointed at room two"), Pending.PendingBlessingRoom, 2);
	TestEqual(TEXT("Pending draft contains three real options"), Pending.PendingBlessingOffer.Num(), 3);
	// Resume the old 20+20 room-two checkpoint through the real draft and
	// shrine-return flow. Only the eventual authoritative victory may top up.
	Pending.Loot.SpiritShards = 40;
	Pending.Loot.SpiritHerb = 0;
	Run->ImportCheckpoint(Pending);
	TestEqual(TEXT("Legacy pending-draft temporary shards are not topped up on load"), Run->GetUnbankedLoot().SpiritShards, int64(40));
	TestEqual(TEXT("Pending-draft resume retains the short route"), Run->GetCurrentRealm()->GetRoomCount(), 4);
	TArray<FQinglanBlessing> Offer;
	if (!TestTrue(TEXT("Resumed production draft is available"), Run->GetBlessingOffer(Offer)) || Offer.IsEmpty()) { return false; }
	TestEqual(TEXT("Checkpoint resume preserves the first offered option"), Offer[0].BlessingId, Pending.PendingBlessingOffer[0]);
	TestTrue(TEXT("A real offered blessing is selected"), Run->AcceptBlessing(Offer[0].BlessingId));
	TestFalse(TEXT("Completed fights alone are insufficient before reaching shrine three"), Run->CanClaimVictory());
	TestTrue(TEXT("Shrine becomes reachable after the blessing"), Run->EnterRoom(3));
	FQinglanCheckpoint AtShrine;
	Run->ExportCheckpoint(AtShrine);
	Run->ImportCheckpoint(AtShrine);
	TestEqual(TEXT("Shrine checkpoint resumes in room three"), Run->GetCurrentRoomIndex(), 3);
	TestEqual(TEXT("Shrine checkpoint retains the four-room template"), Run->GetCurrentRealm()->GetRoomCount(), 4);
	TestTrue(TEXT("Full guided requirements authorize return"), Run->CanClaimVictory());
	TestFalse(TEXT("Tutorial return never counts as a defeated guardian"), Run->IsBossRoomCompleted());
	TestFalse(TEXT("Tutorial victory never writes boss proof"), Run->ShouldWriteBossProof(true));
	// Older guided checkpoints earned 20+20 before the short-route fix. The
	// same completed fights/blessing still authorize only the canonical total,
	// rather than an extra 80 on top of the saved temporary shards.
	AtShrine.Loot.SpiritShards = 40;
	AtShrine.Loot.SpiritHerb = 0;
	Run->ImportCheckpoint(AtShrine);
	TestTrue(TEXT("Previously earned tutorial proof remains valid"), Run->CanClaimVictory());
	Run->MarkCheckpointResumed();
	TestTrue(TEXT("Production exit settlement is accepted"), Run->RequestExit());
	TestFalse(TEXT("Successful banking closes the expedition"), Run->IsInExpedition());
	TestEqual(TEXT("Banked tutorial total is exactly eighty shards"), Profile->GetInventory().SpiritShards, int64(80));
	TestEqual(TEXT("Banked tutorial total is exactly one herb"), Profile->GetInventory().SpiritHerb, int64(1));
	TestEqual(TEXT("Tutorial cannot mint a guardian crystal"), Profile->GetInventory().VeinCrystal, int64(0));
	TestFalse(TEXT("Tutorial cannot set Bamboo's first-clear proof"), Profile->HasCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag)));
	FQinglanProfileSnapshot Snapshot;
	Profile->ExportStateForSave(Snapshot);
	for (const FName Transaction : Snapshot.CommittedTransactionIds)
	{
		TestFalse(TEXT("Guided banking creates no boss first-clear bonus transaction"), Transaction.ToString().Contains(TEXT(".first_clear.")));
	}
	TestFalse(TEXT("A second shrine return cannot settle twice"), Run->RequestExit());
	TestEqual(TEXT("Duplicate return leaves the payout unchanged"), Profile->GetInventory().SpiritShards, int64(80));
	FText Notice;
	TestTrue(TEXT("The production banked profile reloads from disk"), Save->LoadProfile(0, Notice));
	TestEqual(TEXT("Saved banked shards survive reload"), Profile->GetInventory().SpiritShards, int64(80));
	TestEqual(TEXT("Saved banked herb survives reload"), Profile->GetInventory().SpiritHerb, int64(1));
	for (const TCHAR* FullRealm : { UQinglanEconomyLibrary::BambooRealmIdText, UQinglanEconomyLibrary::AshenRealmIdText })
	{
		TestTrue(TEXT("Normal realm can start after guided banking"), Run->StartExpedition(FName(FullRealm), 73));
		TestEqual(TEXT("Normal realm keeps all seven rooms"), Run->GetCurrentRealm()->GetRoomCount(), 7);
		TestEqual(TEXT("Normal realm retains its real guardian in room five"), Run->GetBossRoomIndex(), 5);
		TestTrue(TEXT("Normal shrine can be queried"), Run->EnterRoom(3));
		TestFalse(TEXT("The tutorial exception cannot give a normal shrine a victory"), Run->CanClaimVictory());
		Run->ResetRun();
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGuidedLegacyCheckpointPosition,
	"Qinglan.Persistence.GuidedLegacyCheckpointPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanGuidedLegacyCheckpointPosition::RunTest(const FString& Parameters)
{
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	UQinglanSaveSubsystem* Serializer = NewObject<UQinglanSaveSubsystem>(Owner);
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>();
	Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
	Save->ContentVersion = FName(TEXT("design-1"));
	Save->ProfileId = FName(TEXT("legacy-guided"));
	Save->SaveSequence = 1;
	Save->Narrative.Revision = FName(TEXT("narrative-1"));
	Save->bHasCheckpoint = true;
	FQinglanCheckpoint& Legacy = Save->Checkpoint;
	Legacy.RealmId = FName(UQinglanEconomyLibrary::TutorialRealmIdText);
	Legacy.RealmSeed = 73;
	Legacy.EncounterStreamState = -17;
	Legacy.LootStreamState = 0;
	Legacy.BlessingStreamState = -917;
	Legacy.AcquiredBlessings = { FName(TEXT("b_sharp_edge")) };
	Legacy.PendingBlessingOffer = { FName(TEXT("b_quiet_current")),
		FName(TEXT("b_jade_shell")), FName(TEXT("b_thunder_mandate")) };
	Legacy.PendingBlessingRoom = 4;
	Legacy.CommittedRoomIds = { 0, 1, 2, 3, 4, 5 };
	Legacy.Loot.SpiritShards = 60;
	Legacy.Loot.SpiritHerb = 1;
	for (const int32 Room : { 3, 4, 5, 6 })
	{
		Legacy.RoomIndex = Room;
		TArray<uint8> Bytes;
		if (!TestTrue(TEXT("Legacy checkpoint passes the production serializer and invariant path"), Serializer->SerializeVerified(Save, Bytes)))
		{
			return false;
		}
		UQinglanSaveGame* Parsed = nullptr;
		FText Error;
		if (!TestTrue(TEXT("Old tutorial room is accepted before run import"), Serializer->DeserializeVerified(Bytes, Parsed, Error))
			|| !TestNotNull(TEXT("Parsed legacy save"), Parsed)) { return false; }
		TestEqual(TEXT("Deserialization retains the original room for position migration"), Parsed->Checkpoint.RoomIndex, Room);
		Run->ImportCheckpoint(Parsed->Checkpoint);
		FQinglanCheckpoint Out;
		Run->ExportCheckpoint(Out);
		TestEqual(TEXT("Earned old guided route resumes at shrine three"), Out.RoomIndex, 3);
		TestEqual(TEXT("Migrated tutorial still uses the four-room template"), Run->GetCurrentRealm()->GetRoomCount(), 4);
		TestEqual(TEXT("Encounter RNG is preserved"), Out.EncounterStreamState, Legacy.EncounterStreamState);
		TestEqual(TEXT("Loot RNG is preserved"), Out.LootStreamState, Legacy.LootStreamState);
		TestEqual(TEXT("Blessing RNG is preserved"), Out.BlessingStreamState, Legacy.BlessingStreamState);
		TestTrue(TEXT("Selected blessing is preserved"), Out.AcquiredBlessings == Legacy.AcquiredBlessings);
		TestTrue(TEXT("Existing pending choices are preserved without a reroll"), Out.PendingBlessingOffer == Legacy.PendingBlessingOffer);
		TestEqual(TEXT("Pending choice room is preserved"), Out.PendingBlessingRoom, Legacy.PendingBlessingRoom);
		TestEqual(TEXT("Import does not normalize temporary shards"), Out.Loot.SpiritShards, Legacy.Loot.SpiritShards);
		TestEqual(TEXT("Import does not normalize temporary herbs"), Out.Loot.SpiritHerb, Legacy.Loot.SpiritHerb);
		TestFalse(TEXT("An unselected saved draft still prevents tutorial victory"), Run->CanClaimVictory());
	}
	Legacy.PendingBlessingOffer.Reset();
	Legacy.PendingBlessingRoom = INDEX_NONE;
	Legacy.CommittedRoomIds.Remove(1);
	Run->ImportCheckpoint(Legacy);
	TestEqual(TEXT("An unearned old checkpoint is not promoted to the shrine"), Run->GetCurrentRoomIndex(), 6);
	TestFalse(TEXT("Missing combat proof cannot gain tutorial victory"), Run->CanClaimVictory());
	Legacy.RealmId = FName(UQinglanEconomyLibrary::BambooRealmIdText);
	Legacy.CommittedRoomIds.Add(1);
	Run->ImportCheckpoint(Legacy);
	TestEqual(TEXT("A normal realm keeps its original exit position"), Run->GetCurrentRoomIndex(), 6);
	TestEqual(TEXT("A normal realm retains the guardian room"), Run->GetBossRoomIndex(), 5);
	Run->ResetRun();
	return true;
}

#endif
