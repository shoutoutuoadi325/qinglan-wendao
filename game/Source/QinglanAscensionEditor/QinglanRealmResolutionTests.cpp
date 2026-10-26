// Realm data is optional for the synthesized catalogs; map names are not IDs.
#include "Core/QinglanInteractable.h"
#include "Engine/GameInstance.h"
#include "Misc/AutomationTest.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Persistence/QinglanSaveGame.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanRunSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanOptionalRealmTemplateResolution,
	"Qinglan.Progression.OptionalRealmTemplateResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanOptionalRealmTemplateResolution::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Existing saves retain the stable Ashen identifier"),
		FString(UQinglanEconomyLibrary::AshenRealmIdText), FString(TEXT("ashen_peak")));
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	FAutomationTestExecutionInfo Before, After;
	GetExecutionInfo(Before);
	for (const TCHAR* RealmId : { UQinglanEconomyLibrary::AshenRealmIdText, UQinglanEconomyLibrary::TutorialRealmIdText })
	{
		FQinglanCheckpoint In;
		In.RealmId = FName(RealmId);
		In.RealmSeed = 73;
		In.EncounterStreamState = -17;
		In.LootStreamState = 29;
		In.BlessingStreamState = 37;
		In.RoomIndex = 2;
		In.CommittedRoomIds = { 0, 1 };
		Run->ImportCheckpoint(In);
		const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
		if (!TestNotNull(TEXT("Supported realm resolves without an imported asset"), Realm)) { continue; }
		TestEqual(TEXT("Logical save identity is preserved"), Realm->RealmId, In.RealmId);
		const bool bAshen = In.RealmId == FName(UQinglanEconomyLibrary::AshenRealmIdText);
		TestEqual(TEXT("Full realm has seven rooms; guided tutorial ends at shrine three"),
			Realm->GetRoomCount(), bAshen ? 7 : 4);
		TestEqual(TEXT("Only the full realm has a guardian room"), Run->GetBossRoomIndex(), bAshen ? 5 : INDEX_NONE);
		TestEqual(TEXT("Resumed room kind remains combat"), Run->GetCurrentRoomKind(), EQinglanRoomKind::Combat);
		for (int32 Query = 0; Query < 24; ++Query)
		{
			TestTrue(TEXT("Repeated queries reuse the resolved template"), Run->GetCurrentRealm() == Realm);
		}
		int64 RoomShards = 0;
		for (const FQinglanRealmRoom& Room : Realm->Rooms) { RoomShards += Room.SpiritShards; }
		TestEqual(TEXT("Room rewards preserve catalog totals"), RoomShards,
			bAshen ? UQinglanEconomyLibrary::AshenBaseShards : UQinglanEconomyLibrary::TutorialPayoutShards);
		TestEqual(TEXT("Checkpoint resumes on the corresponding map"), Run->GetPendingCheckpointMap(),
			FName(bAshen ? QinglanInteraction::AshenMapPath : QinglanInteraction::BambooMapPath));
		FQinglanCheckpoint Out;
		Run->ExportCheckpoint(Out);
		TestEqual(TEXT("Export keeps the persisted realm id"), Out.RealmId, In.RealmId);
		TestEqual(TEXT("Template lookup does not consume encounter randomness"), Out.EncounterStreamState, In.EncounterStreamState);
		TestEqual(TEXT("Template lookup does not consume loot randomness"), Out.LootStreamState, In.LootStreamState);
		TestEqual(TEXT("Template lookup does not consume blessing randomness"), Out.BlessingStreamState, In.BlessingStreamState);
	}
	GetExecutionInfo(After);
	TestEqual(TEXT("Optional asset absence emits no failed-object warnings"), After.GetWarningTotal(), Before.GetWarningTotal());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanImportedRealmTemplateResolution,
	"Qinglan.Progression.ImportedRealmTemplateResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanImportedRealmTemplateResolution::RunTest(const FString& Parameters)
{
	UQinglanRealmDataAsset* Imported = LoadObject<UQinglanRealmDataAsset>(nullptr,
		TEXT("/Game/Qinglan/Data/DA_Realm_bamboo_hollow.DA_Realm_bamboo_hollow"));
	if (!TestNotNull(TEXT("Authored bamboo data asset loads"), Imported)) { return false; }
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanRunSubsystem* Run = NewObject<UQinglanRunSubsystem>(Owner);
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = FName(UQinglanEconomyLibrary::BambooRealmIdText);
	Checkpoint.RealmSeed = 73;
	Run->ImportCheckpoint(Checkpoint);
	TestTrue(TEXT("Available imported data retains precedence over synthesis"), Run->GetCurrentRealm() == Imported);
	return true;
}
#endif
