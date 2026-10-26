#include "QinglanSaveInspect.h"

#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSaveInspectionEvidence,
	"Qinglan.Persistence.ReadOnlySaveInspection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanSaveInspectionEvidence::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
		FString::Printf(TEXT("SaveInspect_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
	if (!TestTrue(TEXT("Isolated evidence directory created"), IFileManager::Get().MakeDirectory(*Directory, true)))
	{
		return false;
	}
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanSaveSubsystem* Serializer = NewObject<UQinglanSaveSubsystem>(Owner);
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>();
	Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
	Save->ContentVersion = FName(TEXT("design-1"));
	Save->ProfileId = FName(TEXT("inspection-fixture"));
	Save->SaveSequence = 11;
	Save->Inventory.SpiritShards = 9007199254740993LL; // one above exact JSON-double integer range
	Save->Narrative.Revision = FName(TEXT("narrative-1"));
	Save->Narrative.CompletedScenes.Add(FName(TEXT("CH00_S01")));
	Save->bHasCheckpoint = true;
	Save->Checkpoint.RealmId = FName(TEXT("bamboo_hollow"));
	Save->Checkpoint.RealmSeed = 123;
	Save->Checkpoint.EncounterStreamState = -123456789;
	Save->Checkpoint.LootStreamState = 456;
	Save->Checkpoint.BlessingStreamState = 789;
	Save->Checkpoint.RoomIndex = 2;
	Save->Checkpoint.CommittedRoomIds = { 0, 1 };
	Save->Checkpoint.Loot.SpiritShards = 17;
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Production serializer creates a valid checkpoint"), Serializer->SerializeVerified(Save, Bytes)))
	{
		return false;
	}
	const FString Input = Directory / TEXT("checkpoint.sav");
	const FString Output = Directory / TEXT("checkpoint.json");
	if (!TestTrue(TEXT("Fixture written"), FFileHelper::SaveArrayToFile(Bytes, *Input)))
	{
		return false;
	}
	UQinglanSaveInspectCommandlet* Commandlet = NewObject<UQinglanSaveInspectCommandlet>();
	const FString Args = FString::Printf(TEXT("-input=\"%s\" -output=\"%s\""), *Input, *Output);
	if (!TestEqual(TEXT("Read-only inspection succeeds"), Commandlet->Main(Args), 0))
	{
		return false;
	}
	TArray<uint8> SourceAfter;
	FFileHelper::LoadFileToArray(SourceAfter, *Input);
	TestTrue(TEXT("Source save is byte-identical after inspection"), SourceAfter == Bytes);
	FString Json;
	TSharedPtr<FJsonObject> Report;
	if (!TestTrue(TEXT("Report readable"), FFileHelper::LoadFileToString(Json, *Output)) ||
		!TestTrue(TEXT("Report parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> State = Report->GetObjectField(TEXT("state"));
	TestEqual(TEXT("Sequence exported"), State->GetNumberField(TEXT("saveSequence")), 11.0);
	TestEqual(TEXT("64-bit inventory remains exact"),
		State->GetObjectField(TEXT("inventory"))->GetStringField(TEXT("spiritShards")), FString(TEXT("9007199254740993")));
	const TSharedPtr<FJsonObject> Checkpoint = State->GetObjectField(TEXT("checkpoint"));
	TestEqual(TEXT("Checkpoint room exported"), Checkpoint->GetNumberField(TEXT("roomIndex")), 2.0);
	TestEqual(TEXT("Signed RNG state exported"), Checkpoint->GetNumberField(TEXT("encounterStreamState")), -123456789.0);
	TestEqual(TEXT("Reward ledger exported"), Checkpoint->GetArrayField(TEXT("committedRoomIds")).Num(), 2);
	TestEqual(TEXT("Earned run loot exported"), Checkpoint->GetObjectField(TEXT("loot"))->GetStringField(TEXT("spiritShards")), FString(TEXT("17")));

	AddExpectedError(TEXT("Save inspection refuses an existing output"), EAutomationExpectedErrorFlags::Contains, 2);
	TestEqual(TEXT("Existing evidence cannot be overwritten"), Commandlet->Main(Args), 1);
	FString JsonAfter;
	FFileHelper::LoadFileToString(JsonAfter, *Output);
	TestEqual(TEXT("Existing evidence is unchanged"), JsonAfter, Json);
	TestEqual(TEXT("Original cannot be chosen as output"), Commandlet->Main(
		FString::Printf(TEXT("-input=\"%s\" -output=\"%s\""), *Input, *Input)), 1);

	TArray<uint8> Corrupt = Bytes;
	Corrupt.Last() ^= 1;
	const FString CorruptInput = Directory / TEXT("corrupt.sav");
	const FString RejectedOutput = Directory / TEXT("rejected.json");
	FFileHelper::SaveArrayToFile(Corrupt, *CorruptInput);
	AddExpectedError(TEXT("Save inspection rejected"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("Corrupt checkpoint is rejected"), Commandlet->Main(
		FString::Printf(TEXT("-input=\"%s\" -output=\"%s\""), *CorruptInput, *RejectedOutput)), 1);
	TestFalse(TEXT("Invalid evidence is never written"), IFileManager::Get().FileExists(*RejectedOutput));
	return true;
}
#endif
