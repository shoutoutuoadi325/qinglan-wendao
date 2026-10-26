// Production catalog coverage and localized backup notices. No game travel.
#include "Abilities/QinglanBlessingLibrary.h"
#include "Core/QinglanLocalizationService.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Internationalization/TextLocalizationResource.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanBlessingCatalogLocalization,
	"Qinglan.Localization.BlessingCatalogCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanBlessingCatalogLocalization::RunTest(const FString& Parameters)
{
	UStringTable* Table = LoadObject<UStringTable>(nullptr, TEXT("/Game/Qinglan/Data/ST_Game"));
	UDataTable* Bilingual = LoadObject<UDataTable>(nullptr, TEXT("/Game/Qinglan/Data/DT_Localization"));
	if (!TestNotNull(TEXT("String table loads"), Table) ||
		!TestNotNull(TEXT("Bilingual table loads"), Bilingual))
	{
		return false;
	}
	TArray<FName> Ids;
	UQinglanBlessingLibrary::GetAllBlessingIds(Ids);
	TestEqual(TEXT("All twelve production blessings are checked"), Ids.Num(), 12);
	for (const FName Id : Ids)
	{
		FQinglanBlessingDef Def;
		if (!TestTrue(TEXT("Catalog definition resolves"), UQinglanBlessingLibrary::FindBlessingDef(Id, Def)))
		{
			continue;
		}
		for (const FName Key : { Def.NameTextKey, Def.DescriptionTextKey })
		{
			TestTrue(*FString::Printf(TEXT("String-table key exists: %s"), *Key.ToString()),
				Table->GetMutableStringTable()->FindEntry(FTextKey(Key.ToString())).IsValid());
			const FQinglanLocalizedTextRow* Row = Bilingual->FindRow<FQinglanLocalizedTextRow>(
				Key, TEXT("BlessingCatalogCoverage"), false);
			if (TestNotNull(*Key.ToString(), Row))
			{
				TestFalse(TEXT("English catalog text is populated"), Row->TextEn.TrimStartAndEnd().IsEmpty());
				TestFalse(TEXT("Chinese catalog text is populated"), Row->TextZh.TrimStartAndEnd().IsEmpty());
			}
		}
		FText Name, Description;
		UQinglanBlessingLibrary::GetBlessingText(Def, Name, Description);
		TestFalse(TEXT("Production name lookup resolves"), Name.ToString().Contains(TEXT("MISSING STRING TABLE ENTRY")));
		TestFalse(TEXT("Production description lookup resolves"), Description.ToString().Contains(TEXT("MISSING STRING TABLE ENTRY")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanBackupNoticeNamedFormatting,
	"Qinglan.Localization.BackupNoticeNamedFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanBackupNoticeNamedFormatting::RunTest(const FString& Parameters)
{
	const FString OriginalCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
		FString::Printf(TEXT("BackupNotice_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UGameInstance* Instance = nullptr;
	ON_SCOPE_EXIT
	{
		if (Instance) { Instance->Shutdown(); }
		UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		FInternationalization::Get().SetCurrentCulture(OriginalCulture);
		FTextLocalizationManager::Get().WaitForAsyncTasks();
	};
	if (!TestTrue(TEXT("Isolated save directory created"), IFileManager::Get().MakeDirectory(*Directory, true)) ||
		!TestTrue(TEXT("Real bilingual resources initialize"), FQinglanLocalizationService::InitializeRuntimeResources()))
	{
		return false;
	}
	// RegisterTextSource refreshes asynchronously in UE 5.8. Finish that real
	// refresh before inspecting its resource identity or changing culture.
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	UQinglanSaveSubsystem::SetSaveDirectoryOverrideForTesting(Directory);
	Instance = NewObject<UGameInstance>(GetTransientPackage());
	Instance->Init();
	UQinglanSaveSubsystem* Reader = Instance->GetSubsystem<UQinglanSaveSubsystem>();
	if (!TestNotNull(TEXT("Production save subsystem initializes"), Reader)) { return false; }
	UQinglanSaveGame* Save = NewObject<UQinglanSaveGame>();
	Save->SchemaVersion = UQinglanSaveSubsystem::CurrentSchemaVersion;
	Save->ContentVersion = FName(TEXT("design-1"));
	Save->ProfileId = FName(TEXT("localized-backup"));
	Save->SaveSequence = 7;
	Save->CultivationStage = EQinglanStage::FoundationEstablishment;
	Save->CultivationRank = 2;
	Save->Narrative.Revision = FName(TEXT("narrative-1"));
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("Production serializer accepts backup"), Reader->SerializeVerified(Save, Bytes))) { return false; }
	const FString Backup = Directory / TEXT("QinglanProfile_0.bak1");
	if (!TestTrue(TEXT("Backup fixture written"), FFileHelper::SaveArrayToFile(Bytes, *Backup))) { return false; }
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Backup);
	UDataTable* Bilingual = LoadObject<UDataTable>(nullptr, TEXT("/Game/Qinglan/Data/DT_Localization"));
	if (!TestNotNull(TEXT("Bilingual table loads"), Bilingual)) { return false; }
	const FQinglanLocalizedTextRow* Preview = Bilingual->FindRow<FQinglanLocalizedTextRow>(FName(TEXT("save.progress_preview")), TEXT("BackupNotice"));
	const FQinglanLocalizedTextRow* Restored = Bilingual->FindRow<FQinglanLocalizedTextRow>(FName(TEXT("save.backup_restored")), TEXT("BackupNotice"));
	const FQinglanLocalizedTextRow* Stage = Bilingual->FindRow<FQinglanLocalizedTextRow>(FName(TEXT("cultivation.foundation")), TEXT("BackupNotice"));
	if (!TestNotNull(TEXT("Preview pattern exists"), Preview) || !TestNotNull(TEXT("Backup pattern exists"), Restored) ||
		!TestNotNull(TEXT("Stage text exists"), Stage)) { return false; }

	// Backup fallback itself is intentional; named-format parsing warnings are not.
	AddExpectedMessage(TEXT("LoadProfile: slot 0 fell back to a valid backup"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);
	for (const TCHAR* Culture : { TEXT("en-US"), TEXT("zh-Hans") })
	{
		if (!TestTrue(TEXT("Culture applied"), FQinglanLocalizationService::ApplyCulture(Culture))) { continue; }
		FTextLocalizationManager::Get().WaitForAsyncTasks();
		FString LocResId;
		if (TestTrue(TEXT("Runtime resource has a registered identity"), FTextLocalizationManager::Get().GetLocResID(
			FTextKey(TEXT("ST_Game")), FTextKey(TEXT("save.progress_preview")), LocResId)))
		{
			TestFalse(TEXT("Runtime resource identity is a full path"), FPaths::IsRelative(LocResId));
			TestEqual(TEXT("Runtime identity has the locres extension"), FPaths::GetExtension(LocResId), FString(TEXT("locres")));
			const FString Target = FPaths::GetPath(FPaths::GetPath(LocResId));
			TestFalse(TEXT("Removing filename and culture leaves a target"), Target.IsEmpty());
			TestTrue(TEXT("Virtual target stays inside project localization content"), FPaths::IsSamePath(Target,
				FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Localization/QinglanRuntime")))));
			TestEqual(TEXT("Resource identity includes the selected culture"),
				FPaths::GetCleanFilename(FPaths::GetPath(LocResId)), FString(Culture));
			if (!Target.IsEmpty())
			{
				TestTrue(TEXT("UE accepts the virtual localization target"),
					TextLocalizationResourceUtil::GetLocalizationTargetPathIdFromLocResId(FTextKey(LocResId)) != INDEX_NONE);
			}
		}
		const bool bChinese = FCString::Strcmp(Culture, TEXT("zh-Hans")) == 0;
		const FString Progress = (bChinese ? Preview->TextZh : Preview->TextEn)
			.Replace(TEXT("{stage}"), *(bChinese ? Stage->TextZh : Stage->TextEn))
			.Replace(TEXT("{rank}"), *FText::AsNumber(2).ToString());
		const FString Expected = (bChinese ? Restored->TextZh : Restored->TextEn)
			.Replace(TEXT("{date}"), *FText::AsDate(Stamp, EDateTimeStyle::Short).ToString())
			.Replace(TEXT("{progress}"), *Progress);
		FAutomationTestExecutionInfo Before, After;
		GetExecutionInfo(Before);
		FText Notice;
		TestTrue(TEXT("Production backup recovery succeeds"), Reader->LoadProfile(0, Notice));
		GetExecutionInfo(After);
		TestEqual(TEXT("No formatter warnings during recovery"), After.GetWarningTotal(), Before.GetWarningTotal());
		TestEqual(*FString::Printf(TEXT("Named values resolve in %s"), Culture), Notice.ToString(), Expected);
	}
	return true;
}
#endif
