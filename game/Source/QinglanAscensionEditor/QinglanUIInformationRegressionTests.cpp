// Cached language changes and the information players need before choosing.
#include "Abilities/QinglanBlessingLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Core/QinglanLocalizationService.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Persistence/QinglanSaveGame.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UI/QinglanHUDStatics.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	struct FQinglanInformationWorld
	{
		UGameInstance* Instance = nullptr;
		UWorld* World = nullptr;
		FString OriginalCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
		FQinglanInformationWorld()
		{
			if (!GEngine) { return; }
			const FString Name = TEXT("L_UIInformation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Instance = NewObject<UGameInstance>(GEngine);
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
		}
		~FQinglanInformationWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance) { Instance->Shutdown(); }
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
			FInternationalization::Get().SetCurrentCulture(OriginalCulture);
			FTextLocalizationManager::Get().WaitForAsyncTasks();
		}
	};

	UTextBlock* OfferRow(UQinglanMenuWidget* Menu, int32 Index)
	{
		UVerticalBox* Rows = Menu && Menu->WidgetTree
			? Menu->WidgetTree->FindWidget<UVerticalBox>(TEXT("Box_MenuRows")) : nullptr;
		UButton* Button = Rows ? Cast<UButton>(Rows->GetChildAt(Index)) : nullptr;
		return Button ? Cast<UTextBlock>(Button->GetContent()) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCachedTextLanguageChange,
	"Qinglan.Localization.CachedWidgetTextChangesLanguage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanCachedTextLanguageChange::RunTest(const FString& Parameters)
{
	const FString OriginalCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	ON_SCOPE_EXIT
	{
		FInternationalization::Get().SetCurrentCulture(OriginalCulture);
		FTextLocalizationManager::Get().WaitForAsyncTasks();
	};
	if (!TestTrue(TEXT("Production translations initialize"), FQinglanLocalizationService::InitializeRuntimeResources())
		|| !TestTrue(TEXT("English applies"), FQinglanLocalizationService::ApplyCulture(TEXT("en-US")))) { return false; }
	UTextBlock* Label = NewObject<UTextBlock>();
	const FText Cached = UQinglanHUDStatics::GameText(TEXT("menu.profile"));
	Label->SetText(Cached);
	const FString English = Label->GetText().ToString();
	TestTrue(TEXT("Chinese applies"), FQinglanLocalizationService::ApplyCulture(TEXT("zh-Hans")));
	const FString Chinese = FQinglanLocalizationService::ResolveForCulture(TEXT("menu.profile"), TEXT("zh-Hans")).ToString();
	TestNotEqual(TEXT("Fixture translations differ"), English, Chinese);
	TestEqual(TEXT("A visible cached label updates without another lookup or SetText"), Label->GetText().ToString(), Chinese);
	TestEqual(TEXT("A copied cached text updates too"), Cached.ToString(), Chinese);
	TestTrue(TEXT("English reapplies"), FQinglanLocalizationService::ApplyCulture(TEXT("en-US")));
	TestEqual(TEXT("The same cached label switches back"), Label->GetText().ToString(), English);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanOfferDescriptionsVisible,
	"Qinglan.UI.OfferEffectsAndTradeoffsAreVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanOfferDescriptionsVisible::RunTest(const FString& Parameters)
{
	FQinglanInformationWorld Fixture;
	if (!TestNotNull(TEXT("Owning world"), Fixture.World)
		|| !TestTrue(TEXT("Production translations initialize"), FQinglanLocalizationService::InitializeRuntimeResources())) { return false; }
	UQinglanMenuWidget* Menu = CreateWidget<UQinglanMenuWidget>(Fixture.Instance);
	if (!TestNotNull(TEXT("Native menu"), Menu)) { return false; }
	Menu->TakeWidget();
	for (const TCHAR* Culture : { TEXT("en-US"), TEXT("zh-Hans") })
	{
		TestTrue(TEXT("Culture applies"), FQinglanLocalizationService::ApplyCulture(Culture));
		FQinglanBlessingDef Blessing;
		if (!TestTrue(TEXT("Real blessing definition"), UQinglanBlessingLibrary::FindBlessingDef(TEXT("b_second_wind"), Blessing))) { return false; }
		FQinglanOfferChoice Draft;
		Draft.OfferId = Blessing.BlessingId;
		Draft.Title = UQinglanHUDStatics::GameText(Blessing.NameTextKey);
		Draft.bBlessing = true;
		Menu->ShowOffers({ Draft });
		UTextBlock* DraftText = OfferRow(Menu, 0);
		if (!TestNotNull(TEXT("Visible blessing row"), DraftText)) { return false; }
		TestTrue(TEXT("Mandatory selection describes the blessing effect"),
			DraftText->GetText().ToString().Contains(UQinglanHUDStatics::GameText(Blessing.DescriptionTextKey).ToString()));
		UTextBlock* Hint = Menu->WidgetTree->FindWidget<UTextBlock>(TEXT("Txt_MenuHint"));
		if (!TestNotNull(TEXT("Visible input hint"), Hint)) { return false; }
		TestEqual(TEXT("Mandatory draft does not teach an unavailable dismissal"), Hint->GetText().ToString(),
			UQinglanHUDStatics::GameText(TEXT("menu.offer.blessing_hint")).ToString());
		FQinglanRelicDef Relic;
		if (!TestTrue(TEXT("Real relic definition"), UQinglanEconomyLibrary::FindRelic(TEXT("mountain_token"), Relic))) { return false; }
		FQinglanOfferChoice Shop;
		Shop.OfferId = Relic.RelicId;
		Shop.Title = UQinglanHUDStatics::GameText(Relic.NameTextKey);
		Shop.Cost = FText::AsNumber(Relic.VeinCrystalCost);
		Menu->ShowOffers({ Shop });
		UTextBlock* ShopText = OfferRow(Menu, 0);
		if (!TestNotNull(TEXT("Visible shop row"), ShopText)) { return false; }
		TestTrue(TEXT("Purchase describes both the health bonus and sword penalty"),
			ShopText->GetText().ToString().Contains(UQinglanHUDStatics::GameText(Relic.DescriptionTextKey).ToString()));
		TestTrue(TEXT("Purchase price remains visible"), ShopText->GetText().ToString().Contains(Shop.Cost.ToString()));
		TestEqual(TEXT("Optional shop keeps its dismissal hint"), Hint->GetText().ToString(),
			UQinglanHUDStatics::GameText(TEXT("menu.offer.hint")).ToString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanLockedChoiceRequirements,
	"Qinglan.UI.LockedEndingChoicesExplainMissingAgreements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanLockedChoiceRequirements::RunTest(const FString& Parameters)
{
	FQinglanInformationWorld Fixture;
	if (!TestNotNull(TEXT("Owning world"), Fixture.World)
		|| !TestTrue(TEXT("Production translations initialize"), FQinglanLocalizationService::InitializeRuntimeResources())) { return false; }
	UClass* Class = LoadClass<UQinglanDialogueWidget>(nullptr, TEXT("/Game/Qinglan/UI/WBP_Dialogue.WBP_Dialogue_C"));
	if (!TestNotNull(TEXT("Production dialogue class"), Class)) { return false; }
	UQinglanDialogueWidget* Widget = CreateWidget<UQinglanDialogueWidget>(Fixture.Instance, Class);
	if (!TestNotNull(TEXT("Dialogue widget"), Widget)) { return false; }
	Widget->TakeWidget();
	for (const TCHAR* Culture : { TEXT("en-US"), TEXT("zh-Hans") })
	{
		TestTrue(TEXT("Culture applies"), FQinglanLocalizationService::ApplyCulture(Culture));
		FQinglanSceneChoice Choice;
		Choice.ChoiceId = TEXT("s04.preview_shared");
		Choice.Label = UQinglanHUDStatics::GameText(TEXT("story.ch06.s04.preview_shared"));
		Choice.UnmetRequirements = { TEXT("sq_spillway_verified"), TEXT("sq_allocation_agreed"), TEXT("sq_handover_accepted") };
		Widget->SetChoiceEntries({ Choice });
		UTextBlock* Label = Widget->WidgetTree->FindWidget<UTextBlock>(TEXT("Txt_Choice0"));
		if (!TestNotNull(TEXT("Visible ending choice"), Label)) { return false; }
		UButton* Button = Cast<UButton>(Label->GetParent());
		if (!TestNotNull(TEXT("Choice button"), Button)) { return false; }
		TestFalse(TEXT("Missing agreements still block selection"), Button->GetIsEnabled());
		const FName RequirementKeys[] = { TEXT("story.ch06.s04.spillway_missing"),
			TEXT("story.ch06.s04.allocation_missing"), TEXT("story.ch06.s04.handover_missing") };
		for (int32 Index = 0; Index < Choice.UnmetRequirements.Num(); ++Index)
		{
			TestTrue(TEXT("Disabled choice includes its authored localized requirement"),
				Label->GetText().ToString().Contains(UQinglanHUDStatics::GameText(RequirementKeys[Index]).ToString()));
			TestFalse(TEXT("Implementation flag identifiers are not player instructions"),
				Label->GetText().ToString().Contains(Choice.UnmetRequirements[Index].ToString()));
		}
	}
	Widget->NativeDestruct();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanPauseCheckpointExplanation,
	"Qinglan.UI.PauseExplainsCheckpointSuspension",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanPauseCheckpointExplanation::RunTest(const FString& Parameters)
{
	FQinglanInformationWorld Fixture;
	if (!TestNotNull(TEXT("Owning world"), Fixture.World)
		|| !TestTrue(TEXT("Production translations initialize"), FQinglanLocalizationService::InitializeRuntimeResources())) { return false; }
	UQinglanRunSubsystem* Run = Fixture.Instance->GetSubsystem<UQinglanRunSubsystem>();
	UQinglanMenuWidget* Menu = CreateWidget<UQinglanMenuWidget>(Fixture.Instance);
	if (!TestNotNull(TEXT("Run subsystem"), Run) || !TestNotNull(TEXT("Native menu"), Menu)) { return false; }
	Menu->TakeWidget();
	for (const TCHAR* Culture : { TEXT("en-US"), TEXT("zh-Hans") })
	{
		TestTrue(TEXT("Culture applies"), FQinglanLocalizationService::ApplyCulture(Culture));
		Run->ResetRun();
		Menu->ShowScreen(EQinglanMenuScreen::Pause);
		UTextBlock* QuitText = OfferRow(Menu, 6);
		if (!TestNotNull(TEXT("Pause return option"), QuitText)) { return false; }
		TestEqual(TEXT("Outside expeditions return still targets the sanctuary"), QuitText->GetText().ToString(),
			UQinglanHUDStatics::GameText(TEXT("menu.quit_sanctuary")).ToString());

		FQinglanCheckpoint Checkpoint;
		Checkpoint.RealmId = UQinglanEconomyLibrary::BambooRealmIdText;
		Checkpoint.RealmSeed = 42;
		Checkpoint.RoomIndex = 1;
		Run->ImportCheckpoint(Checkpoint);
		Menu->Refresh();
		QuitText = OfferRow(Menu, 6);
		if (!TestNotNull(TEXT("Expedition suspension option"), QuitText)) { return false; }
		TestTrue(TEXT("The action identifies the profile selection destination"),
			QuitText->GetText().ToString().Contains(UQinglanHUDStatics::GameText(TEXT("menu.suspend_profile")).ToString()));
		TestTrue(TEXT("Before leaving the player sees fight replay and reward retention"),
			QuitText->GetText().ToString().Contains(UQinglanHUDStatics::GameText(TEXT("menu.suspend_profile_hint")).ToString()));
		TestTrue(TEXT("Rendering the suspension explanation does not end the expedition"), Run->IsInExpedition());
	}
	Run->ResetRun();
	return true;
}
#endif
