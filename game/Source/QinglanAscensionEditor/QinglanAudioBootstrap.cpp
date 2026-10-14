#include "QinglanAudioBootstrap.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Factories/SoundFactory.h" // AudioEditor module (Build.cs)
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const FString AudioRoot = TEXT("/inspire/hdd/project/qianghuaxuexi/s26044/assets-raw/audio");
	const FString ContentRoot = TEXT("/Game/Qinglan/Audio");

	struct FAudioEntry { const TCHAR* File; const TCHAR* Group; bool bLooping; };
	const FAudioEntry Entries[] = {
		{ TEXT("music/music_boss_epic_boss_battle_loop.wav"),        TEXT("Music"), true },
		{ TEXT("music/music_combat_alt_prepare_your_swords.wav"),    TEXT("Music"), true },
		{ TEXT("music/music_combat_fast_fight_loop.wav"),            TEXT("Music"), true },
		{ TEXT("music/music_hub_calm_orient_peace_valley.wav"),      TEXT("Music"), true },
		{ TEXT("sfx/sfx_sword_swing.wav"),                           TEXT("SFX"),   false },
		{ TEXT("sfx/sfx_hit_impact.wav"),                            TEXT("SFX"),   false },
		{ TEXT("sfx/sfx_technique_release.wav"),                     TEXT("SFX"),   false },
		{ TEXT("sfx/sfx_telegraph_warn.wav"),                        TEXT("SFX"),   false },
	};
}

UQinglanAudioBootstrapCommandlet::UQinglanAudioBootstrapCommandlet()
{
	IsClient = false;
}

USoundWave* UQinglanAudioBootstrapCommandlet::ImportSoundWave(const FString& SourceFile, const TCHAR* Group, bool bLooping)
{
	const FString Base = TEXT("SW_") + FPaths::GetBaseFilename(SourceFile);
	const FString DestPath = FString::Printf(TEXT("%s/%s/%s"), *ContentRoot, Group, *Base);
	if (USoundWave* Existing = LoadObject<USoundWave>(nullptr, *DestPath))
	{
		return Existing; // idempotent re-runs reuse the imported asset
	}
	const FString FullSource = AudioRoot / SourceFile;
	if (!IFileManager::Get().FileExists(*FullSource))
	{
		UE_LOG(LogTemp, Warning, TEXT("audio source missing: %s"), *FullSource);
		return nullptr;
	}

	USoundFactory* Factory = NewObject<USoundFactory>();
	Factory->bAutoCreateCue = false;
	Factory->SuppressImportDialogs(); // headless: no import-option dialogs

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = FullSource;
	Task->DestinationPath = FString::Printf(TEXT("%s/%s"), *ContentRoot, Group);
	Task->DestinationName = Base;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = true;
	Task->Options = Factory;

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	USoundWave* Wave = nullptr;
	for (UObject* Imported : Task->GetObjects())
	{
		if (USoundWave* AsWave = Cast<USoundWave>(Imported))
		{
			Wave = AsWave;
			break;
		}
	}
	if (Wave == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("audio import produced no sound wave: %s"), *FullSource);
		return nullptr;
	}
	// Music tracks loop (docs/04 section 5); SFX stay one-shot. Streaming
	// keeps the long loops out of resident memory.
	Wave->bLooping = bLooping;
	Wave->bStreaming = bLooping;
	Wave->MarkPackageDirty();

	UPackage* Package = Wave->GetOutermost();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Wave, *FileName, Args))
	{
		UE_LOG(LogTemp, Error, TEXT("audio save failed: %s"), *DestPath);
		return nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("audio imported %s -> %s"), *FullSource, *DestPath);
	return Wave;
}

int32 UQinglanAudioBootstrapCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("QinglanAudioBootstrap starting; root=%s"), *AudioRoot);
	bool bOk = true;
	int32 Count = 0;
	for (const FAudioEntry& Entry : Entries)
	{
		if (ImportSoundWave(Entry.File, Entry.Group, Entry.bLooping))
		{
			++Count;
		}
		else
		{
			bOk = false;
		}
	}
	UE_LOG(LogTemp, Log, TEXT("QinglanAudioBootstrap finished: %d/%d imported, ok=%d"),
		Count, UE_ARRAY_COUNT(Entries), bOk);
	return bOk ? 0 : 1;
}
