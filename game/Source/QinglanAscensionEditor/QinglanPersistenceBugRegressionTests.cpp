// Player-facing persistence regressions use production saves in disposable directories.
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTalismanComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanProfileSnapshot.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveMigration.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags PersistenceBugFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Write actual tagged properties as the pre-loadout class did: both new fields are absent. */
	class FLegacyTalismanSaveArchive : public FObjectAndNameAsStringProxyArchive
	{
	public:
		explicit FLegacyTalismanSaveArchive(FArchive& Inner) : FObjectAndNameAsStringProxyArchive(Inner, true) {}
		virtual bool ShouldSkipProperty(const FProperty* Property) const override
		{
			const FName Name = Property ? Property->GetFName() : NAME_None;
			return Name == FName(TEXT("EquippedTalismans")) || Name == FName(TEXT("bTalismanLoadoutInitialized"))
				|| FObjectAndNameAsStringProxyArchive::ShouldSkipProperty(Property);
		}
	};

	struct FIsolatedProfile
	{
		FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
			FString::Printf(TEXT("PersistenceBugs_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;

		bool Initialize(bool bWithWorld = false, bool bCreateProfile = true)
		{
			if (!IFileManager::Get().MakeDirectory(*Directory, true)) { return false; }
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
			if (bWithWorld)
			{
				if (!GEngine) { return false; }
				const FString Name = TEXT("PersistenceWorld_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
				Instance = NewObject<UGameInstance>(GEngine);
				Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
				World = Instance->GetWorld();
				if (!World) { return false; }
			}
			else
			{
				Instance = NewObject<UGameInstance>(GetTransientPackage());
				Instance->Init();
			}
			if (!Save() || !Profile() || !Run()) { return false; }
			if (!bCreateProfile) { return true; }
			Save()->CreateNewProfile(0);
			return Save()->IsProfileSessionActive() && Save()->HasProfile(0);
		}

		~FIsolatedProfile()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
			UQinglanSaveSubsystem::SetFileOperationFailureForTesting(
				UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting::Flush, INDEX_NONE);
			UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		}

		UQinglanSaveSubsystem* Save() const { return Instance->GetSubsystem<UQinglanSaveSubsystem>(); }
		UQinglanProfileSubsystem* Profile() const { return Instance->GetSubsystem<UQinglanProfileSubsystem>(); }
		UQinglanRunSubsystem* Run() const { return Instance->GetSubsystem<UQinglanRunSubsystem>(); }
		FString Path(const TCHAR* Suffix) const
		{
			return FPaths::Combine(Directory, FString(TEXT("QinglanProfile_0.")) + Suffix);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCheckpointConsumableRewind,
	"Qinglan.Persistence.CheckpointConsumableRewind", PersistenceBugFlags)
bool FQinglanCheckpointConsumableRewind::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	UQinglanProfileSubsystem* Profile = Fixture.Profile();
	UQinglanRunSubsystem* Run = Fixture.Run();
	const FName Healing(TEXT("healing_talisman"));
	Profile->RefillTalismans();
	if (!TestTrue(TEXT("Expedition starts"), Run->StartExpedition(FName(TEXT("bamboo_hollow")), 73))
		|| !TestTrue(TEXT("Unfinished combat starts"), Run->EnterRoom(1))) { return false; }
	TestTrue(TEXT("First combat charge spent"), Profile->TryConsumeTalismanCharge(Healing));
	FText Error;
	TestTrue(TEXT("Pause/quit boundary saves"), Fixture.Save()->SaveNow(Error));
	TestEqual(TEXT("Saving does not refund live charges"), Profile->GetTalismanChargeCount(Healing), 1);
	TestTrue(TEXT("Second charge remains usable in current combat"), Profile->TryConsumeTalismanCharge(Healing));
	TestTrue(TEXT("Repeated save still succeeds"), Fixture.Save()->SaveNow(Error));
	TestEqual(TEXT("Current combat has actually spent both charges"), Profile->GetTalismanChargeCount(Healing), 0);
	TestTrue(TEXT("The unfinished room reloads"), Fixture.Save()->LoadProfile(0, Error));
	TestEqual(TEXT("Replaying all enemies also restores both entrance charges"), Profile->GetTalismanChargeCount(Healing), 2);
	TestEqual(TEXT("Reload preserves the unfinished room"), Run->GetCurrentRoomIndex(), 1);
	TestTrue(TEXT("Resume boundary preserves the same snapshot"), Fixture.Save()->SaveNow(Error));
	TestTrue(TEXT("Charge spent during the retried room"), Profile->TryConsumeTalismanCharge(Healing));
	Run->CompleteCurrentRoom(FName(TEXT("tx.consumable.room1")));
	TestTrue(TEXT("Completed room reloads"), Fixture.Save()->LoadProfile(0, Error));
	TestEqual(TEXT("Completed combat does not refund spent charges"), Profile->GetTalismanChargeCount(Healing), 1);
	TestEqual(TEXT("Room reward survives the same load"), Run->GetUnbankedLoot().SpiritShards, int64(30));
	TestTrue(TEXT("Next combat takes its own entrance snapshot"), Run->EnterRoom(2));
	TestTrue(TEXT("Final charge spent in second combat"), Profile->TryConsumeTalismanCharge(Healing));
	TestTrue(TEXT("Second unfinished combat saves"), Fixture.Save()->SaveNow(Error));
	TestTrue(TEXT("Second combat reloads"), Fixture.Save()->LoadProfile(0, Error));
	TestEqual(TEXT("Rewind restores one remaining charge, not a sanctuary refill"), Profile->GetTalismanChargeCount(Healing), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanProfileJournalCapacity,
	"Qinglan.Persistence.ProfileJournalCapacity", PersistenceBugFlags)
bool FQinglanProfileJournalCapacity::RunTest(const FString& Parameters)
{
	UGameInstance* Instance = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Instance);
	FQinglanProfileSnapshot Full;
	Full.CultivationStage = EQinglanStage::FoundationEstablishment;
	Full.Inventory.SpiritShards = 10000;
	Full.Inventory.SpiritHerb = 100;
	Full.Inventory.VeinCrystal = 100;
	Full.UnlockedTechniques = { FName(TEXT("jade_sword_arc")), FName(TEXT("flowing_ward")) };
	Full.EquippedTechniques = Full.UnlockedTechniques;
	for (int32 Index = 0; Index < QinglanSave::MaxTransactionEntries; ++Index)
	{
		Full.CommittedTransactionIds.Add(FName(*FString::Printf(TEXT("tx.capacity.%d"), Index)));
	}
	Profile->ImportStateFromSave(Full);
	FQinglanInventory Delta;
	Delta.SpiritShards = 1;
	TestFalse(TEXT("Full journal refuses a new grant"), Profile->TryGrant(FName(TEXT("tx.new.grant")), Delta));
	TestFalse(TEXT("Full journal refuses a spend before deducting"), Profile->TrySpend(FName(TEXT("tx.new.spend")), Delta));
	TestFalse(TEXT("Full journal refuses a cultivation purchase"), Profile->TryAdvanceRank(FName(TEXT("tx.new.rank"))));
	TestFalse(TEXT("Full journal refuses a technique unlock"), Profile->UnlockTechnique(FName(TEXT("tx.new.unlock")), FName(TEXT("ember_lance"))));
	TestFalse(TEXT("Full journal refuses a relic purchase"), Profile->TryPurchaseRelic(FName(TEXT("tx.new.relic")), FName(TEXT("jade_pendant"))));
	TestFalse(TEXT("Full journal refuses a passive purchase"), Profile->TryPurchasePassive(FName(TEXT("tx.new.passive")), FName(TEXT("energy_recovery"))));
	TestFalse(TEXT("Full journal refuses a technique upgrade"), Profile->TryPurchaseTechniqueTier(FName(TEXT("tx.new.tier")), FName(TEXT("jade_sword_arc"))));
	TestTrue(TEXT("Already committed transactions remain successful no-ops"), Profile->TrySpend(Full.CommittedTransactionIds[0], Delta));
	TestEqual(TEXT("Rejected transactions never consume shards"), Profile->GetInventory().SpiritShards, Full.Inventory.SpiritShards);
	TestEqual(TEXT("Rejected transactions never consume herbs"), Profile->GetInventory().SpiritHerb, Full.Inventory.SpiritHerb);
	TestEqual(TEXT("Rejected transactions never consume crystals"), Profile->GetInventory().VeinCrystal, Full.Inventory.VeinCrystal);
	TestEqual(TEXT("Rejected rank stays unchanged"), Profile->GetCultivationRank(), 0);
	TestTrue(TEXT("Rejected relic stays unowned"), Profile->GetOwnedRelics().IsEmpty());
	TestTrue(TEXT("Rejected passive stays unowned"), Profile->GetOwnedPassives().IsEmpty());
	TestFalse(TEXT("Rejected technique stays locked"), Profile->IsTechniqueUnlocked(FName(TEXT("ember_lance"))));
	TestEqual(TEXT("Rejected upgrade stays at tier one"), Profile->GetTechniqueTier(FName(TEXT("jade_sword_arc"))), 1);
	Full.CultivationRank = QinglanProgression::RanksPerStage;
	Full.CampaignFlags = { FName(QinglanProgression::AshenClearedCampaignFlag), FName(QinglanProgression::GoldenCoreTrialWonCampaignFlag) };
	Profile->ImportStateFromSave(Full);
	TestFalse(TEXT("Even an otherwise eligible breakthrough cannot overflow the journal"), Profile->TryBreakthrough(FName(TEXT("tx.new.breakthrough"))));
	TestEqual(TEXT("Rejected breakthrough preserves stage"), Profile->GetCultivationStage(), Full.CultivationStage);
	Full.CommittedTransactionIds.Pop();
	Profile->ImportStateFromSave(Full);
	TestTrue(TEXT("The last available journal entry remains usable"), Profile->TryGrant(FName(TEXT("tx.last")), Delta));
	TestFalse(TEXT("The next entry is rejected atomically"), Profile->TryGrant(FName(TEXT("tx.too_many")), Delta));
	FQinglanProfileSnapshot Exported;
	Profile->ExportStateForSave(Exported);
	TestEqual(TEXT("Export stays within the durable journal bound"), Exported.CommittedTransactionIds.Num(), QinglanSave::MaxTransactionEntries);
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>();
	Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
	Save->Inventory = Exported.Inventory;
	Save->CommittedTransactionIds = Exported.CommittedTransactionIds;
	FText Error;
	TestTrue(TEXT("The profile remains valid for persistence"), QinglanSave::ValidateSaveInvariants(Save, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSaveFlushFailure,
	"Qinglan.Persistence.SaveFlushFailure", PersistenceBugFlags)
bool FQinglanSaveFlushFailure::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	TArray<uint8> Before, After;
	if (!TestTrue(TEXT("Original primary read"), FFileHelper::LoadFileToArray(Before, *Fixture.Path(TEXT("sav"))))) { return false; }
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(
		UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting::Flush, 0);
	FText Error;
	TestFalse(TEXT("An undurable write cannot report success"), Fixture.Save()->SaveNow(Error));
	TestFalse(TEXT("Flush failure reports an actionable save error"), Error.IsEmpty());
	TestTrue(TEXT("Original primary still readable"), FFileHelper::LoadFileToArray(After, *Fixture.Path(TEXT("sav"))));
	TestTrue(TEXT("Flush failure leaves the primary byte-identical"), Before == After);
	TestFalse(TEXT("Undurable temporary file is removed"), IFileManager::Get().FileExists(*Fixture.Path(TEXT("tmp"))));
	TestTrue(TEXT("A subsequent successful flush can commit"), Fixture.Save()->SaveNow(Error));
	TestTrue(TEXT("Committed retry loads normally"), Fixture.Save()->LoadProfile(0, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanFutureBackupProtection,
	"Qinglan.Persistence.FutureBackupProtection", PersistenceBugFlags)
bool FQinglanFutureBackupProtection::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	FQinglanInventory Grant;
	Grant.SpiritShards = 73;
	TestTrue(TEXT("Current session has earned progress"), Fixture.Profile()->TryGrant(FName(TEXT("tx.future.seed")), Grant));
	TArray<uint8> Primary, Future, After;
	if (!TestTrue(TEXT("Current profile bytes read"), FFileHelper::LoadFileToArray(Primary, *Fixture.Path(TEXT("sav"))))) { return false; }
	Future = Primary;
	const int32 UnsupportedContainerVersion = 2;
	// A newer container is opaque to this build, including its checksum layout.
	FMemory::Memcpy(Future.GetData() + sizeof(uint32), &UnsupportedContainerVersion, sizeof(int32));
	TestTrue(TEXT("A newer build's backup exists"), FFileHelper::SaveArrayToFile(Future, *Fixture.Path(TEXT("bak1"))));
	FText Error;
	TestFalse(TEXT("A supported primary cannot silently bypass a newer backup"), Fixture.Save()->LoadProfile(0, Error));
	TestFalse(TEXT("Autosave cannot rotate over the newer backup"), Fixture.Save()->SaveNow(Error));
	Fixture.Save()->CreateNewProfile(0);
	TestEqual(TEXT("Rejected replacement retains the active session"), Fixture.Profile()->GetInventory().SpiritShards, int64(73));
	TestTrue(TEXT("The newer recovery file is still readable"), FFileHelper::LoadFileToArray(After, *Fixture.Path(TEXT("bak1"))));
	TestTrue(TEXT("The newer recovery file remains byte-identical"), After == Future);
	TestTrue(TEXT("The supported primary also remains readable"), FFileHelper::LoadFileToArray(After, *Fixture.Path(TEXT("sav"))));
	TestTrue(TEXT("Rejected writes never touch the primary"), After == Primary);
	TestTrue(TEXT("A supported older recovery copy exists"), FFileHelper::SaveArrayToFile(Primary, *Fixture.Path(TEXT("bak2"))));
	TestTrue(TEXT("Missing primary simulates interrupted replacement"), IFileManager::Get().Delete(*Fixture.Path(TEXT("sav"))));
	TestFalse(TEXT("Fallback cannot choose an old copy past the newer backup"), Fixture.Save()->LoadProfile(0, Error));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCompletedRoomVitals,
	"Qinglan.Persistence.CompletedRoomVitals", PersistenceBugFlags)
bool FQinglanCompletedRoomVitals::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable world and profile created"), Fixture.Initialize(true))) { return false; }
	Fixture.World->InitializeActorsForPlay(FURL());
	Fixture.World->BeginPlay();
	Fixture.World->SetBegunPlay(true);
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AQinglanCharacter* Player = Fixture.World->SpawnActor<AQinglanCharacter>(AQinglanCharacter::StaticClass(),
		FVector(0.f, 0.f, 700.f), FRotator::ZeroRotator, Params);
	APlayerController* Controller = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Actual player pawn"), Player) || !TestNotNull(TEXT("Player controller"), Controller)) { return false; }
	Controller->Possess(Player);
	if (!TestEqual(TEXT("Production snapshot lookup resolves the fixture player"),
		UGameplayStatics::GetPlayerCharacter(Fixture.World, 0), static_cast<ACharacter*>(Player))) { return false; }
	UQinglanCombatComponent* Combat = Player->GetCombat();
	if (!TestNotNull(TEXT("Actual combat attributes"), Combat)) { return false; }
	const auto SetHealth = [Combat](float Health)
	{
		FQinglanAttributes Attributes = Combat->GetAttributes();
		Attributes.Health = Health;
		Combat->SetDerivedAttributes(Attributes);
	};
	UQinglanRunSubsystem* Run = Fixture.Run();
	SetHealth(40.f);
	TestTrue(TEXT("Expedition starts with existing vitals"), Run->StartExpedition(FName(TEXT("bamboo_hollow")), 73));
	TestTrue(TEXT("First fight starts"), Run->EnterRoom(1));
	SetHealth(15.f);
	FQinglanCheckpoint Checkpoint;
	Run->ExportCheckpoint(Checkpoint);
	TestEqual(TEXT("An unfinished fight still rewinds to its entrance health"), Checkpoint.PlayerAttributes.Health, 40.f);
	Run->CompleteCurrentRoom(FName(TEXT("tx.vitals.room1")));
	Run->ExportCheckpoint(Checkpoint);
	TestEqual(TEXT("The cleared fight retains actual post-combat health"), Checkpoint.PlayerAttributes.Health, 15.f);
	Combat->Heal(25.f);
	FText Error;
	TestTrue(TEXT("Healing after the reward saves"), Fixture.Save()->SaveNow(Error));
	TArray<uint8> Bytes;
	UQinglanSaveGame* Saved = nullptr;
	const auto ReadPrimary = [&]()
	{
		return FFileHelper::LoadFileToArray(Bytes, *Fixture.Path(TEXT("sav")))
			&& Fixture.Save()->DeserializeVerified(Bytes, Saved, Error);
	};
	if (!TestTrue(TEXT("Actual durable checkpoint reads"), ReadPrimary())) { return false; }
	TestEqual(TEXT("Post-clear healing persists with the reward"), Saved->Checkpoint.PlayerAttributes.Health, 40.f);
	Run->ImportCheckpoint(Saved->Checkpoint);
	SetHealth(90.f);
	Run->ExportCheckpoint(Checkpoint);
	TestEqual(TEXT("A previous-map pawn cannot overwrite pending resume health"), Checkpoint.PlayerAttributes.Health, 40.f);
	Combat->RestoreCheckpointAttributes(Checkpoint.PlayerAttributes);
	Run->MarkCheckpointResumed();
	SetHealth(55.f);
	TestTrue(TEXT("Re-entering the cleared room remains a save boundary"), Run->EnterRoom(1));
	if (!TestTrue(TEXT("Re-entry checkpoint reads"), ReadPrimary())) { return false; }
	TestEqual(TEXT("Cleared-room re-entry durably updates health"), Saved->Checkpoint.PlayerAttributes.Health, 55.f);
	TestEqual(TEXT("Re-entry cannot duplicate room rewards"), Saved->Checkpoint.Loot.SpiritShards, int64(30));
	TestTrue(TEXT("Safe shrine entered"), Run->EnterRoom(3));
	Combat->Heal(10.f);
	TestTrue(TEXT("Safe-room healing saves"), Fixture.Save()->SaveNow(Error));
	if (!TestTrue(TEXT("Safe-room checkpoint reads"), ReadPrimary())) { return false; }
	TestEqual(TEXT("Safe-room health does not rewind to the entrance"), Saved->Checkpoint.PlayerAttributes.Health, 65.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTalismanLoadoutPersistence,
	"Qinglan.Persistence.TalismanLoadoutSurvivesTravelAndProfileSwitch", PersistenceBugFlags)
bool FQinglanTalismanLoadoutPersistence::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable world and profile created"), Fixture.Initialize(true))) { return false; }
	const auto SpawnSlots = [&]() -> UQinglanTalismanComponent*
	{
		AActor* Owner = Fixture.World->SpawnActor<AActor>();
		if (!Owner) { return nullptr; }
		UQinglanTalismanComponent* Slots = NewObject<UQinglanTalismanComponent>(Owner);
		Owner->AddOwnedComponent(Slots);
		Slots->RegisterComponent();
		return Slots;
	};
	UQinglanTalismanComponent* InitialPawnSlots = SpawnSlots();
	if (!TestNotNull(TEXT("The pawn exists before sanctuary refill"), InitialPawnSlots)) { return false; }
	UQinglanProfileSubsystem* Profile = Fixture.Profile();
	const TArray<FName> Defaults = { FName(TEXT("healing_talisman")), FName(TEXT("cleansing_talisman")) };
	const TArray<FName> Selected = { FName(TEXT("focus_talisman")), FName(TEXT("protective_talisman")) };
	TestEqual(TEXT("A new profile has not yet received sanctuary charges"), Profile->GetTalismanChargeCount(Defaults[0]), 0);
	TestTrue(TEXT("Slot defaults do not depend on pawn/refill ordering"), InitialPawnSlots->GetEquippedTalismans() == Defaults);
	Profile->RefillTalismans();
	TestTrue(TEXT("Equipment UI replaces slots through the component"), InitialPawnSlots->SetEquippedTalismans(Selected));
	TestTrue(TEXT("Profile owns the selected order"), Profile->GetEquippedTalismans() == Selected);
	UQinglanTalismanComponent* ReplacementPawnSlots = SpawnSlots();
	if (!TestNotNull(TEXT("A replacement pawn is created for travel"), ReplacementPawnSlots)) { return false; }
	TestTrue(TEXT("The replacement pawn uses the selected loadout"), ReplacementPawnSlots->GetEquippedTalismans() == Selected);
	while (Profile->GetTalismanChargeCount(Selected[0]) > 0) { Profile->TryConsumeTalismanCharge(Selected[0]); }
	FText Error;
	TestTrue(TEXT("An exhausted equipped slot saves"), Fixture.Save()->SaveNow(Error));
	Fixture.Save()->CreateNewProfile(1);
	TestTrue(TEXT("Profile switching updates an existing pawn's slots"), InitialPawnSlots->GetEquippedTalismans() == Defaults);
	TestTrue(TEXT("The original profile reloads"), Fixture.Save()->LoadProfile(0, Error));
	TestTrue(TEXT("Reload restores both selected slots in order"), InitialPawnSlots->GetEquippedTalismans() == Selected);
	TestTrue(TEXT("All components see the active profile rather than stale pawn state"), ReplacementPawnSlots->GetEquippedTalismans() == Selected);
	TestEqual(TEXT("An exhausted selected talisman remains equipped without being refilled"), Profile->GetTalismanChargeCount(Selected[0]), 0);
	TestTrue(TEXT("Expedition starts"), Fixture.Run()->StartExpedition(FName(TEXT("bamboo_hollow")), 73));
	TestFalse(TEXT("The component cannot bypass the expedition loadout lock"), InitialPawnSlots->SetEquippedTalismans({}));
	Fixture.Run()->ResetRun();
	TestTrue(TEXT("An intentionally empty loadout is accepted at the sanctuary"), InitialPawnSlots->SetEquippedTalismans({}));
	TestTrue(TEXT("The empty loadout saves"), Fixture.Save()->SaveNow(Error));
	TestTrue(TEXT("The empty loadout reloads"), Fixture.Save()->LoadProfile(0, Error));
	Profile->RefillTalismans();
	TestTrue(TEXT("Reload and refill never replace intentionally empty slots"), InitialPawnSlots->GetEquippedTalismans().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTalismanLoadoutCompatibility,
	"Qinglan.Persistence.TalismanLoadoutLegacyDefaultsAndValidation", PersistenceBugFlags)
bool FQinglanTalismanLoadoutCompatibility::RunTest(const FString& Parameters)
{
	UGameInstance* Instance = NewObject<UGameInstance>(GetTransientPackage());
	UQinglanProfileSubsystem* Profile = NewObject<UQinglanProfileSubsystem>(Instance);
	FQinglanProfileSnapshot Legacy;
	Legacy.TalismanCharges.Add(FName(TEXT("healing_talisman")), 0);
	Profile->ImportStateFromSave(Legacy);
	const TArray<FName> Defaults = { FName(TEXT("healing_talisman")), FName(TEXT("cleansing_talisman")) };
	TestTrue(TEXT("A save predating slots receives stable defaults even when depleted"), Profile->GetEquippedTalismans() == Defaults);
	FQinglanProfileSnapshot Exported;
	Profile->ExportStateForSave(Exported);
	TestTrue(TEXT("The next save records that the loadout has been initialized"), Exported.bTalismanLoadoutInitialized);
	TestTrue(TEXT("Legacy import does not refill consumed charges"), Exported.TalismanCharges.OrderIndependentCompareEqual(Legacy.TalismanCharges));
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>();
	Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
	FText Error;
	TestTrue(TEXT("Older saves without slot fields remain valid"), QinglanSave::ValidateSaveInvariants(Save, Error));
	Save->bTalismanLoadoutInitialized = true;
	Save->EquippedTalismans = Defaults;
	TestTrue(TEXT("Known exhausted slots are valid even without charges"), QinglanSave::ValidateSaveInvariants(Save, Error));
	Save->EquippedTalismans = { Defaults[0], Defaults[0] };
	TestFalse(TEXT("Duplicate talisman bindings are rejected"), QinglanSave::ValidateSaveInvariants(Save, Error));
	Save->EquippedTalismans = { FName(TEXT("unknown_talisman")) };
	TestFalse(TEXT("Unknown talisman bindings are rejected"), QinglanSave::ValidateSaveInvariants(Save, Error));
	Save->EquippedTalismans = { Defaults[0], Defaults[1], FName(TEXT("focus_talisman")) };
	AddExpectedError(TEXT("Save invariant 'equipped talismans' failed"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("More than two talisman bindings are rejected"), QinglanSave::ValidateSaveInvariants(Save, Error));
	Save->EquippedTalismans.Reset();
	TestTrue(TEXT("An initialized empty loadout is valid"), QinglanSave::ValidateSaveInvariants(Save, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanProfileCreationFailurePreservesSession,
	"Qinglan.Persistence.ProfileCreationFailurePreservesSession", PersistenceBugFlags)
bool FQinglanProfileCreationFailurePreservesSession::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("An empty save directory is ready"), Fixture.Initialize(false, false))) { return false; }
	FText Error;
	UQinglanSaveSubsystem::SetFileOperationFailureForTesting(
		UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting::Flush, 0);
	TestFalse(TEXT("Cold-start profile creation reports its disk failure"), Fixture.Save()->TryCreateNewProfile(0, Error));
	TestFalse(TEXT("Failed creation never activates an unsaved session"), Fixture.Save()->IsProfileSessionActive());
	TestFalse(TEXT("Failed creation leaves the slot empty"), Fixture.Save()->HasProfile(0));
	if (!TestTrue(TEXT("The same cold-start request can retry"), Fixture.Save()->TryCreateNewProfile(0, Error))) { return false; }
	FQinglanInventory Grant;
	Grant.SpiritShards = 73;
	TestTrue(TEXT("The active profile earns progress"), Fixture.Profile()->TryGrant(FName(TEXT("tx.create.progress")), Grant));
	Fixture.Profile()->RefillTalismans();
	const TArray<FName> Selected = { FName(TEXT("focus_talisman")), FName(TEXT("protective_talisman")) };
	TestTrue(TEXT("The active profile selects equipment"), Fixture.Profile()->TrySetEquippedTalismans(Selected));
	TestTrue(TEXT("An expedition is open"), Fixture.Run()->StartExpedition(FName(TEXT("bamboo_hollow")), 73));
	TestTrue(TEXT("A draft reward room is entered"), Fixture.Run()->EnterRoom(2));
	Fixture.Run()->CompleteCurrentRoom(FName(TEXT("tx.create.room2")));
	FQinglanCheckpoint Before, After;
	Fixture.Run()->ExportCheckpoint(Before);
	if (!TestFalse(TEXT("The live expedition has an unresolved blessing draft"), Before.PendingBlessingOffer.IsEmpty())) { return false; }
	TArray<uint8> OriginalBytes, CurrentBytes;
	TestTrue(TEXT("Current profile primary is readable"), FFileHelper::LoadFileToArray(OriginalBytes, *Fixture.Path(TEXT("sav"))));
	for (const int32 TargetSlot : { 0, 1 })
	{
		UQinglanSaveSubsystem::SetFileOperationFailureForTesting(
			UQinglanSaveSubsystem::EQinglanSaveFileOperationForTesting::Flush, 0);
		TestFalse(TEXT("A failed replacement returns failure even when an older file exists"), Fixture.Save()->TryCreateNewProfile(TargetSlot, Error));
		TestFalse(TEXT("Creation failure supplies its write error"), Error.IsEmpty());
		TestTrue(TEXT("The original active session remains active"), Fixture.Save()->IsProfileSessionActive());
		TestEqual(TEXT("A failed replacement preserves permanent resources"), Fixture.Profile()->GetInventory().SpiritShards, int64(73));
		TestTrue(TEXT("A failed replacement preserves equipment"), Fixture.Profile()->GetEquippedTalismans() == Selected);
		TestTrue(TEXT("A failed replacement preserves the live expedition"), Fixture.Run()->IsInExpedition());
		Fixture.Run()->ExportCheckpoint(After);
		TestEqual(TEXT("The current room is unchanged"), After.RoomIndex, Before.RoomIndex);
		TestEqual(TEXT("Unbanked room rewards are unchanged"), After.Loot.SpiritShards, Before.Loot.SpiritShards);
		TestTrue(TEXT("The unresolved draft is unchanged"), After.PendingBlessingOffer == Before.PendingBlessingOffer);
		TestTrue(TEXT("The prior primary still reads"), FFileHelper::LoadFileToArray(CurrentBytes, *Fixture.Path(TEXT("sav"))));
		TestTrue(TEXT("The failed replacement leaves the old primary byte-identical"), CurrentBytes == OriginalBytes);
	}
	TestTrue(TEXT("A later autosave still writes the original active slot"), Fixture.Save()->SaveNow(Error));
	TestFalse(TEXT("The failed destination did not become the active save target"), Fixture.Save()->HasProfile(1));
	TestTrue(TEXT("Successful retry creates the new destination"), Fixture.Save()->TryCreateNewProfile(1, Error));
	TestFalse(TEXT("Successful replacement clears the old expedition"), Fixture.Run()->IsInExpedition());
	TestEqual(TEXT("The new profile starts with fresh inventory"), Fixture.Profile()->GetInventory().SpiritShards, int64(0));
	TestTrue(TEXT("The old profile remains loadable"), Fixture.Save()->LoadProfile(0, Error));
	TestEqual(TEXT("The original progress survives successful creation in another slot"), Fixture.Profile()->GetInventory().SpiritShards, int64(73));
	TestEqual(TEXT("The original expedition checkpoint survives"), Fixture.Run()->GetCurrentRoomIndex(), Before.RoomIndex);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanExitRequestsReportBankingFailure,
	"Qinglan.Progression.ExitRequestsReportBankingFailure", PersistenceBugFlags)
bool FQinglanExitRequestsReportBankingFailure::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	FQinglanInventory FullInventory;
	FullInventory.SpiritShards = MAX_int64;
	TestTrue(TEXT("Inventory reaches its supported bound"), Fixture.Profile()->TryGrant(FName(TEXT("tx.exit.full")), FullInventory));
	TestTrue(TEXT("Expedition starts"), Fixture.Run()->StartExpedition(FName(TEXT("bamboo_hollow")), 73));
	TestTrue(TEXT("Combat reward room entered"), Fixture.Run()->EnterRoom(1));
	Fixture.Run()->CompleteCurrentRoom(FName(TEXT("tx.exit.room1")));
	const int64 RoomLoot = Fixture.Run()->GetUnbankedLoot().SpiritShards;
	TestFalse(TEXT("Rejected retreat banking reports failure"), Fixture.Run()->RequestRetreat());
	TestTrue(TEXT("Rejected retreat leaves the expedition open"), Fixture.Run()->IsInExpedition());
	TestEqual(TEXT("Rejected retreat retains all retryable loot"), Fixture.Run()->GetUnbankedLoot().SpiritShards, RoomLoot);
	TestTrue(TEXT("The boss room can be exercised"), Fixture.Run()->EnterRoom(Fixture.Run()->GetBossRoomIndex()));
	Fixture.Run()->CompleteCurrentRoom(FName(TEXT("tx.exit.boss")));
	TestTrue(TEXT("The final exit room is entered"), Fixture.Run()->EnterRoom(6));
	TestTrue(TEXT("Victory requirements are satisfied"), Fixture.Run()->CanClaimVictory());
	TestFalse(TEXT("Rejected victory banking reports failure"), Fixture.Run()->RequestExit());
	TestTrue(TEXT("Rejected victory leaves the expedition open"), Fixture.Run()->IsInExpedition());
	TestFalse(TEXT("Rejected victory never awards boss proof"), Fixture.Profile()->HasCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag)));
	FQinglanInventory Cost;
	Cost.SpiritShards = 1000;
	TestTrue(TEXT("Resources can be spent to make space for settlement"), Fixture.Profile()->TrySpend(FName(TEXT("tx.exit.space")), Cost));
	TestTrue(TEXT("A successful victory retry reports success"), Fixture.Run()->RequestExit());
	TestFalse(TEXT("Successful settlement closes the expedition"), Fixture.Run()->IsInExpedition());
	TestTrue(TEXT("Successful settlement awards its boss proof"), Fixture.Profile()->HasCampaignFlag(FName(QinglanProgression::BambooClearedCampaignFlag)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanQuestCompletionSavesItsTechniqueReward,
	"Qinglan.Persistence.QuestCompletionSavesItsTechniqueReward", PersistenceBugFlags)
bool FQinglanQuestCompletionSavesItsTechniqueReward::RunTest(const FString& Parameters)
{
	UQinglanStoryRegistry* Registry = NewObject<UQinglanStoryRegistry>();
	Registry->NarrativeRevision = FName(TEXT("narrative-1"));
	FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
	Quest.QuestId = FName(TEXT("SQ04"));
	Quest.CompletionFlags = { FName(TEXT("sq_warning_record_filed")) };
	const FName Reward(TEXT("mist_step"));
	Quest.RewardTechniqueIds = { Reward };
	FQinglanRegistryScene& Scene = Quest.Stages.AddDefaulted_GetRef();
	Scene.SceneId = FName(TEXT("SQ04_REWARD"));
	Scene.SetsFlags = Quest.CompletionFlags;
	FQinglanRegistryObjective& Objective = Scene.Objectives.AddDefaulted_GetRef();
	Objective.ObjectiveId = FName(TEXT("reward.proof"));
	QinglanStoryRuntime::SetRegistryForTesting(Registry);
	ON_SCOPE_EXIT { QinglanStoryRuntime::SetRegistryForTesting(nullptr); };
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	UQinglanStorySubsystem* Story = Fixture.Instance->GetSubsystem<UQinglanStorySubsystem>();
	if (!TestNotNull(TEXT("Real story subsystem"), Story)) { return false; }
	FText Error;
	TArray<uint8> Bytes;
	UQinglanSaveGame* Saved = nullptr;
	const auto ReadSnapshot = [&](const TCHAR* Suffix)
	{
		return FFileHelper::LoadFileToArray(Bytes, *Fixture.Path(Suffix))
			&& Fixture.Save()->DeserializeVerified(Bytes, Saved, Error);
	};
	if (!TestTrue(TEXT("The starting primary reads"), ReadSnapshot(TEXT("sav")))) { return false; }
	const int32 InitialSequence = Saved->SaveSequence;
	TestTrue(TEXT("The reward scene starts"), Story->BeginScene(Scene.SceneId));
	TestTrue(TEXT("The objective completes through the production story path"), Story->NotifySceneObjectiveCompleted(Scene.SceneId, Objective.ObjectiveId));
	TestTrue(TEXT("The quest stage completes"), Story->IsSceneCompleted(Scene.SceneId));
	TestTrue(TEXT("The quest's technique unlocks"), Fixture.Profile()->IsTechniqueUnlocked(Reward));
	if (!TestTrue(TEXT("The complete transaction is durable"), ReadSnapshot(TEXT("sav")))) { return false; }
	TestEqual(TEXT("Nested profile commits produce one complete story save"), Saved->SaveSequence, InitialSequence + 1);
	for (const TCHAR* Suffix : { TEXT("sav"), TEXT("bak1"), TEXT("bak2") })
	{
		if (!IFileManager::Get().FileExists(*Fixture.Path(Suffix))) { continue; }
		if (!TestTrue(TEXT("Every recovery snapshot verifies"), ReadSnapshot(Suffix))) { return false; }
		TestTrue(TEXT("No recovery snapshot records a completed quest without its technique"),
			!Saved->Narrative.CompletedSideStages.Contains(Scene.SceneId) || Saved->UnlockedTechniques.Contains(Reward));
	}
	if (!TestTrue(TEXT("The committed primary remains readable"), ReadSnapshot(TEXT("sav")))) { return false; }
	// Reproduce a valid pre-fix snapshot: completion autosaved before reward
	// dispatch. Loading must recover that earned unlock, not just prevent new gaps.
	Saved->UnlockedTechniques.Remove(Reward);
	Saved->CommittedTransactionIds.Remove(FName(TEXT("sq.SQ04.reward.mist_step")));
	if (!TestTrue(TEXT("The historical partial transaction remains format-valid"), Fixture.Save()->SerializeVerified(Saved, Bytes))) { return false; }
	TestTrue(TEXT("An older incomplete-reward save is installed"), FFileHelper::SaveArrayToFile(Bytes, *Fixture.Path(TEXT("sav"))));
	TestTrue(TEXT("The older save loads"), Fixture.Save()->LoadProfile(0, Error));
	TestTrue(TEXT("Durable quest completion restores the missing earned technique"), Fixture.Profile()->IsTechniqueUnlocked(Reward));
	TestTrue(TEXT("The recovery preserves quest completion"), Story->IsSceneCompleted(Scene.SceneId));
	TestTrue(TEXT("The recovered reward saves"), Fixture.Save()->SaveNow(Error));
	FQinglanProfileSnapshot BeforeReload, AfterReload;
	Fixture.Profile()->ExportStateForSave(BeforeReload);
	TestTrue(TEXT("The repaired profile reloads"), Fixture.Save()->LoadProfile(0, Error));
	Fixture.Profile()->ExportStateForSave(AfterReload);
	TestTrue(TEXT("Repeated loads do not append duplicate reward transactions"), BeforeReload.CommittedTransactionIds == AfterReload.CommittedTransactionIds);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanLegacyTalismanTaggedSaveRoundTrip,
	"Qinglan.Persistence.TalismanLegacyTaggedSaveRoundTrip", PersistenceBugFlags)
bool FQinglanLegacyTalismanTaggedSaveRoundTrip::RunTest(const FString& Parameters)
{
	FIsolatedProfile Fixture;
	if (!TestTrue(TEXT("Disposable profile created"), Fixture.Initialize())) { return false; }
	TArray<uint8> Bytes;
	UQinglanSaveGame* Legacy = nullptr;
	FText Error;
	if (!TestTrue(TEXT("The current-format fixture reads"), FFileHelper::LoadFileToArray(Bytes, *Fixture.Path(TEXT("sav"))))) { return false; }
	if (!TestTrue(TEXT("The fixture uses the real container parser"), Fixture.Save()->DeserializeVerified(Bytes, Legacy, Error))) { return false; }
	Legacy->BodyChecksum = 0;
	Legacy->bTalismanLoadoutInitialized = true;
	Legacy->EquippedTalismans = { FName(TEXT("focus_talisman")) };
	Legacy->TalismanCharges = { { FName(TEXT("healing_talisman")), 0 }, { FName(TEXT("focus_talisman")), 1 } };
	TArray<uint8> Body;
	FMemoryWriter BodyWriter(Body);
	FLegacyTalismanSaveArchive LegacyArchive(BodyWriter);
	Legacy->Serialize(LegacyArchive);
	if (!TestFalse(TEXT("Legacy tagged-property serialization succeeds"), BodyWriter.IsError())) { return false; }
	Bytes.Reset();
	FMemoryWriter Container(Bytes);
	uint32 Magic = 0x514C5341u; // documented QLSA container envelope
	int32 ContainerVersion = 1;
	int32 SchemaVersion = 2;
	int32 Sequence = Legacy->SaveSequence;
	int32 BodyLength = Body.Num();
	Container << Magic << ContainerVersion << SchemaVersion << Sequence << BodyLength;
	Container.Serialize(Body.GetData(), BodyLength);
	uint64 Checksum = QinglanSave::Fnv1a64(Bytes.GetData(), Bytes.Num());
	Container << Checksum;
	UQinglanSaveGame* Parsed = nullptr;
	if (!TestTrue(TEXT("A v2 body with no talisman slot fields loads"), Fixture.Save()->DeserializeVerified(Bytes, Parsed, Error))) { return false; }
	TestFalse(TEXT("The absent initialization tag defaults to legacy semantics"), Parsed->bTalismanLoadoutInitialized);
	TestTrue(TEXT("The absent equipment tag never inherits unrelated slots"), Parsed->EquippedTalismans.IsEmpty());
	TArray<uint8> Reencoded;
	TestTrue(TEXT("Legacy values pass the new semantic fingerprint round trip"), Fixture.Save()->SerializeVerified(Parsed, Reencoded));
	TestTrue(TEXT("The actual legacy bytes can be installed as the primary"), FFileHelper::SaveArrayToFile(Bytes, *Fixture.Path(TEXT("sav"))));
	TestTrue(TEXT("The complete load path imports the legacy profile"), Fixture.Save()->LoadProfile(0, Error));
	const TArray<FName> Defaults = { FName(TEXT("healing_talisman")), FName(TEXT("cleansing_talisman")) };
	TestTrue(TEXT("Legacy import supplies stable slots exactly once"), Fixture.Profile()->GetEquippedTalismans() == Defaults);
	TestEqual(TEXT("Legacy import preserves exhausted charges"), Fixture.Profile()->GetTalismanChargeCount(Defaults[0]), 0);
	TestEqual(TEXT("Legacy import preserves all other charge entries"), Fixture.Profile()->GetTalismanChargeCount(FName(TEXT("focus_talisman"))), 1);
	TestTrue(TEXT("The migrated loadout writes with its initialized marker"), Fixture.Save()->SaveNow(Error));
	TestTrue(TEXT("The new-format write remains loadable"), Fixture.Save()->LoadProfile(0, Error));
	TestTrue(TEXT("The migrated slot order survives a second load"), Fixture.Profile()->GetEquippedTalismans() == Defaults);
	return true;
}
#endif
