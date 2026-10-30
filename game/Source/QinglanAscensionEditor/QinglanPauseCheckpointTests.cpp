// Suspending from the pause menu must preserve a resumable slot and never bank loot.
#include "Combat/QinglanCharacter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanPausePreservesCheckpoint,
	"Qinglan.Persistence.PauseSuspendsWithoutStrandingRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanPausePreservesCheckpoint::RunTest(const FString& Parameters)
{
	using EOperation = UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting;
	const FString Name = TEXT("L_PauseCheckpoint_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
	UWorld* World = nullptr;
	UGameInstance* Instance = nullptr;
	ON_SCOPE_EXIT
	{
		UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EOperation::Flush, -1);
		if (World) { World->EndPlay(EEndPlayReason::Quit); }
		if (Instance) { Instance->Shutdown(); }
		if (World)
		{
			if (GEngine) { GEngine->DestroyWorldContext(World); }
			World->DestroyWorld(false);
		}
		UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	if (!TestNotNull(TEXT("Engine"), GEngine)) { return false; }
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
	Instance = NewObject<UGameInstance>(GEngine);
	Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
	World = Instance->GetWorld();
	if (!TestNotNull(TEXT("World"), World)) { return false; }
	UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
	UQinglanRunSubsystem* Run = Instance->GetSubsystem<UQinglanRunSubsystem>();
	UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>();
	if (!TestNotNull(TEXT("Save"), Save) || !TestNotNull(TEXT("Run"), Run)
		|| !TestNotNull(TEXT("Profile"), Profile)) { return false; }
	Save->CreateNewProfile(0);
	Profile->RefillTalismans();
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->SetBegunPlay(true);
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>();
	if (!TestNotNull(TEXT("Real player pause handler"), Player)) { return false; }
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = TEXT("bamboo_hollow");
	Checkpoint.RealmSeed = 73;
	Checkpoint.RoomIndex = 2;
	Checkpoint.CommittedRoomIds = { 0, 1 };
	Checkpoint.Loot.SpiritShards = 100;
	Checkpoint.PlayerAttributes.Health = 43.f;
	Run->ImportCheckpoint(Checkpoint);
	Run->MarkCheckpointResumed();
	Profile->TryConsumeTalismanCharge(TEXT("healing_talisman"));
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EOperation::Flush, 0);
	Player->QuitToSanctuary();
	TestTrue(TEXT("Failed save retains the active session"), Save->IsProfileSessionActive());
	TestTrue(TEXT("Failed save retains the live expedition"), Run->IsInExpedition());
	TestEqual(TEXT("Failed suspension retains earned loot"), Run->GetUnbankedLoot().SpiritShards, int64(100));
	Player->QuitToSanctuary();
	TestFalse(TEXT("Successful suspension opens profile selection on hub arrival"), Save->IsProfileSessionActive());
	TestFalse(TEXT("No live expedition can lock equipment or realm entry in the hub"), Run->IsInExpedition());
	TestEqual(TEXT("Suspension does not bank or penalize earned loot"), Profile->GetInventory().SpiritShards, int64(0));
	const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	TestTrue(TEXT("Real pause action queues the frontend map"), Context && Context->TravelURL.Contains(TEXT("L_Hub")));
	const FString Path = FPaths::Combine(Directory, TEXT("QinglanProfile_0.sav"));
	TArray<uint8> Before, After;
	if (!TestTrue(TEXT("Suspended primary exists"), FFileHelper::LoadFileToArray(Before, *Path))) { return false; }
	// Hub refills and profile-commit observers must not autosave over a
	// suspended checkpoint before the player selects a slot.
	Profile->RefillTalismans();
	FText Error;
	TestTrue(TEXT("Frontend travel has no active save to overwrite"), Save->SaveActiveSession(Error));
	if (!TestTrue(TEXT("Suspended primary remains readable"), FFileHelper::LoadFileToArray(After, *Path))) { return false; }
	TestTrue(TEXT("Frontend cannot overwrite the checkpoint"), Before == After);
	TestTrue(TEXT("Selecting the same profile resumes it"), Save->LoadProfile(0, Error));
	TestTrue(TEXT("Selected profile is active again"), Save->IsProfileSessionActive());
	TestTrue(TEXT("Realm resume is pending before hub refills"), Run->NeedsCheckpointResume());
	FQinglanCheckpoint Restored;
	Run->ExportCheckpoint(Restored);
	TestEqual(TEXT("Checkpoint returns to the same unfinished room"), Restored.RoomIndex, 2);
	TestEqual(TEXT("Completed-room rewards are retained in full"), Restored.Loot.SpiritShards, int64(100));
	TestEqual(TEXT("Unfinished combat resumes entrance vitals"), Restored.PlayerAttributes.Health, 43.f);
	TestEqual(TEXT("Unfinished combat resumes entrance talisman charges"), Profile->GetTalismanChargeCount(TEXT("healing_talisman")), 2);
	return true;
}
#endif
