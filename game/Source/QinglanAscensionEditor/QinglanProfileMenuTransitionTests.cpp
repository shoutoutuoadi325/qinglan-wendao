// Active menu selection must save the outgoing run and replace its whole world.
#include "Combat/QinglanCharacter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	using EFileOperation = UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting;
	struct FProfileMenuWorld
	{
		FString Name = TEXT("L_ProfileMenu_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name);
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		UQinglanSaveSubsystem* Save = nullptr;
		UQinglanRunSubsystem* Run = nullptr;
		AQinglanCharacter* Player = nullptr;

		FProfileMenuWorld()
		{
			if (!GEngine) { return; }
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			Save = Instance->GetSubsystem<UQinglanSaveSubsystem>();
			Run = Instance->GetSubsystem<UQinglanRunSubsystem>();
			if (!World || !Save || !Run) { return; }
			FText Error;
			if (!Save->TryCreateNewProfile(1, Error) || !Save->TryCreateNewProfile(0, Error)) { return; }
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->SetBegunPlay(true);
			Player = World->SpawnActor<AQinglanCharacter>();
			FQinglanCheckpoint Checkpoint;
			Checkpoint.RealmId = TEXT("bamboo_hollow");
			Checkpoint.RealmSeed = 73;
			Checkpoint.RoomIndex = 2;
			Checkpoint.CommittedRoomIds = { 0, 1 };
			Checkpoint.Loot.SpiritShards = 123;
			Checkpoint.PlayerAttributes.Health = 44.f;
			Run->ImportCheckpoint(Checkpoint);
		}
		~FProfileMenuWorld()
		{
			UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EFileOperation::Flush, -1);
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}
		FString TravelURL() const
		{
			const FWorldContext* Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr;
			return Context ? Context->TravelURL : FString();
		}
		bool ReadSlot(int32 Slot, UQinglanSaveGame*& Out) const
		{
			TArray<uint8> Bytes;
			FText Error;
			return FFileHelper::LoadFileToArray(Bytes, *(Directory / FString::Printf(TEXT("QinglanProfile_%d.sav"), Slot)))
				&& Save->DeserializeVerified(Bytes, Out, Error);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanActiveProfileSelection,
	"Qinglan.Persistence.ActiveMenuProfileSwitchSavesAndRebuildsWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanActiveProfileSelection::RunTest(const FString& Parameters)
{
	FProfileMenuWorld Fixture;
	if (!TestNotNull(TEXT("Real profile-menu character"), Fixture.Player)) { return false; }
	TArray<uint8> TargetBefore, TargetAfter;
	const FString TargetPath = Fixture.Directory / TEXT("QinglanProfile_1.sav");
	if (!TestTrue(TEXT("The selected slot already exists"), FFileHelper::LoadFileToArray(TargetBefore, *TargetPath))) { return false; }
	FText Notice;
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EFileOperation::Flush, 0);
	TestFalse(TEXT("A failed outgoing save blocks selection"), Fixture.Player->SelectProfileFromMenu(1, false, Notice));
	TestFalse(TEXT("Save failure returns a visible retry reason"), Notice.IsEmpty());
	TestTrue(TEXT("The old expedition remains playable after failure"), Fixture.Run->IsInExpedition());
	TestEqual(TEXT("Unbanked loot is not lost on failed selection"), Fixture.Run->GetUnbankedLoot().SpiritShards, int64(123));
	TestTrue(TEXT("Failed save queues no map replacement"), Fixture.TravelURL().IsEmpty());

	AddExpectedError(TEXT("LoadProfile: slot 2 unreadable"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("An unreadable destination keeps the current session"), Fixture.Player->SelectProfileFromMenu(2, false, Notice));
	TestTrue(TEXT("Destination failure retains the outgoing expedition"), Fixture.Run->IsInExpedition());
	TestTrue(TEXT("Destination failure leaves the existing world intact"), Fixture.TravelURL().IsEmpty());

	TestTrue(TEXT("The same menu operation can be retried"), Fixture.Player->SelectProfileFromMenu(1, false, Notice));
	TestTrue(TEXT("Success queues a fresh hub, including from an existing hub or arena"), Fixture.TravelURL().Contains(TEXT("L_Hub")));
	TestFalse(TEXT("Selected sanctuary slot replaces the outgoing run"), Fixture.Run->IsInExpedition());
	TestTrue(TEXT("The old pawn cannot resume input while travel is queued"), Fixture.Player->IsMenuInputActive());
	Fixture.Player->InputPause(FInputActionValue(true));
	TestTrue(TEXT("Escape cannot re-enable the outgoing pawn"), Fixture.Player->IsMenuInputActive());
	UQinglanSaveGame* Outgoing = nullptr;
	if (!TestTrue(TEXT("Outgoing primary validates"), Fixture.ReadSlot(0, Outgoing))) { return false; }
	TestTrue(TEXT("Outgoing expedition is resumable"), Outgoing->bHasCheckpoint);
	TestEqual(TEXT("Selection saved earned loot without retreat deductions"), Outgoing->Checkpoint.Loot.SpiritShards, int64(123));
	TestEqual(TEXT("The unfinished room keeps its entrance state"), Outgoing->Checkpoint.PlayerAttributes.Health, 44.f);
	TestTrue(TEXT("Selected primary remains readable"), FFileHelper::LoadFileToArray(TargetAfter, *TargetPath));
	TestTrue(TEXT("Travel never saves the outgoing pawn into the selected slot"), TargetBefore == TargetAfter);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanActiveNewProfileSelection,
	"Qinglan.Persistence.ActiveMenuProfileCreationFailureIsRetryable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanActiveNewProfileSelection::RunTest(const FString& Parameters)
{
	FProfileMenuWorld Fixture;
	if (!TestNotNull(TEXT("Real profile-menu character"), Fixture.Player)) { return false; }
	FText Notice;
	// The outgoing checkpoint flush succeeds, then the new slot fails.
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EFileOperation::Flush, 1);
	TestFalse(TEXT("Creation reports its own failure after saving the outgoing slot"), Fixture.Player->SelectProfileFromMenu(2, true, Notice));
	TestFalse(TEXT("Creation failure includes its storage reason"), Notice.IsEmpty());
	TestTrue(TEXT("Failed creation retains the outgoing expedition"), Fixture.Run->IsInExpedition());
	TestFalse(TEXT("A failed candidate is not exposed as a profile"), Fixture.Save->HasProfile(2));
	TestTrue(TEXT("Failed creation leaves the old world in place"), Fixture.TravelURL().IsEmpty());
	TestTrue(TEXT("Retry creates the selected new profile"), Fixture.Player->SelectProfileFromMenu(2, true, Notice));
	TestTrue(TEXT("A new profile receives a fresh world too"), Fixture.TravelURL().Contains(TEXT("L_Hub")));
	TestFalse(TEXT("Fresh profile has no previous expedition"), Fixture.Run->IsInExpedition());
	UQinglanSaveGame* Outgoing = nullptr;
	UQinglanSaveGame* Created = nullptr;
	if (!TestTrue(TEXT("Outgoing save validates"), Fixture.ReadSlot(0, Outgoing))
		|| !TestTrue(TEXT("New save validates"), Fixture.ReadSlot(2, Created))) { return false; }
	TestEqual(TEXT("Creating another slot preserves the outgoing loot"), Outgoing->Checkpoint.Loot.SpiritShards, int64(123));
	TestEqual(TEXT("The replacement is the requested slot"), Created->ProfileId, FName(TEXT("profile_2")));
	TestFalse(TEXT("The new slot never inherits the old checkpoint"), Created->bHasCheckpoint);
	return true;
}
#endif
