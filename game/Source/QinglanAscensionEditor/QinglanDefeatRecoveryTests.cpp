// Failed disk writes after defeat remain retryable through real player input.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanDefeatSaveRetry,
	"Qinglan.Persistence.DefeatedPlayerCanRetryFailedReturn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanDefeatSaveRetry::RunTest(const FString& Parameters)
{
	using EOperation = UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting;
	const FString Name = TEXT("L_DefeatRecovery_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
	if (!TestTrue(TEXT("Private profile active"), Save->IsProfileSessionActive())) { return false; }
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(),
		FVector(0.f, 0.f, 150.f), FRotator::ZeroRotator, Params);
	APlayerController* Controller = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Player"), Player) || !TestNotNull(TEXT("Controller"), Controller)) { return false; }
	Controller->SetPlayerState(World->SpawnActor<APlayerState>());
	Controller->Possess(Player);
	World->SetBegunPlay(true);
	Controller->DispatchBeginPlay();
	Player->DispatchBeginPlay();
	FQinglanCheckpoint Checkpoint;
	Checkpoint.RealmId = TEXT("bamboo_hollow");
	Checkpoint.RealmSeed = 73;
	Checkpoint.RoomIndex = 1;
	Checkpoint.CommittedRoomIds = { 0 };
	Checkpoint.Loot.SpiritShards = 100;
	Run->ImportCheckpoint(Checkpoint);
	Run->MarkCheckpointResumed();
	FQinglanDamageEvent Lethal;
	Lethal.CastId = UQinglanCombatComponent::NextCastId();
	Lethal.RawMagnitude = 99999.f;
	if (!TestNotNull(TEXT("Combat"), Player->GetCombat())
		|| !TestTrue(TEXT("Player defeated"), Player->GetCombat()->ApplyDamage(Lethal))) { return false; }
	TestFalse(TEXT("Defeat closes the settled run"), Run->IsInExpedition());
	TestEqual(TEXT("Defeat banks half the earned shards"), Profile->GetInventory().SpiritShards, int64(50));
	AddExpectedMessage(TEXT("Travel blocked by save failure"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EOperation::Flush, 0);
	// Invoke the real timer callback without relying on engine frame counters.
	Player->ProcessEvent(Player->FindFunctionChecked(TEXT("HandleDefeatTravel")), nullptr);
	TestTrue(TEXT("Failed delayed return exposes a retry while dead"), Player->IsDefeatReturnPending());
	TestFalse(TEXT("Dead player cannot resume combat during retry"), Player->CanStartAction(EQinglanActionKind::Sword));
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(EOperation::Flush, 0);
	Player->InputInteract(FInputActionValue(true));
	TestTrue(TEXT("A failed input retry remains available"), Player->IsDefeatReturnPending());
	Player->InputInteract(FInputActionValue(true));
	TestFalse(TEXT("Input retry succeeds once storage recovers"), Player->IsDefeatReturnPending());
	const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	TestTrue(TEXT("Successful retry queues the actual sanctuary travel"), Context && Context->TravelURL.Contains(TEXT("L_Hub")));
	TestEqual(TEXT("Retries do not bank rewards more than once"), Profile->GetInventory().SpiritShards, int64(50));
	return true;
}
#endif
