// Game instance implementation: bootstrap, early graphics, story registry
// warm load, first-launch locale, and first-map travel.
//
// Docs: docs/05-technical-architecture.md section 1 (UQinglanGameInstance =
// bootstrap services, profile selection, travel coordination),
// docs/06-platform-and-performance.md section 4 (first launch: conservative
// preset by capability class, optional benchmark before gameplay, settings
// applied early enough to prevent an expensive startup scene),
// docs/dev/implementation-blueprint.md section 1.8 (first launch auto-zh-Hans
// on OS match) and section 1.9 (scenes imported into data assets),
// docs/dev/level-layout.md (map files L_Hub / realm maps).
//
// FIXME-CONTRACT: the frozen scaffold header declares no storage members, so
// the cached subsystem pointers and the streamable registry handle live in a
// TU-local weak-keyed map beside each game instance (same pattern as
// Persistence/QinglanSaveSubsystem.cpp and AI/QinglanEncounterDirector.cpp).
// Merge them into the class when headers unfreeze.
#include "Core/QinglanGameInstance.h"

#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "Internationalization/Internationalization.h"
#include "Kismet/GameplayStatics.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Platform/QinglanGraphicsSettingsService.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"

namespace
{
	// Map assets (docs/dev/level-layout.md "Maps" table: L_Hub, L_BambooHollow,
	// L_AshenVerge, L_TribulationArena live under /Game/Qinglan/Maps).
	const TCHAR* GHubMapPath = TEXT("/Game/Qinglan/Maps/L_Hub");

	// First-launch benchmark offer map (blueprint M1 "VFXBenchmark map";
	// docs/06 section 4 "run a short optional hardware benchmark before
	// gameplay or use a tested default"). Development builds only: shipping
	// removes benchmark maps (docs/05 section on shipping builds).
	const TCHAR* GBenchmarkMapPath = TEXT("/Game/Qinglan/Maps/L_Benchmark");

	// Travel options appended for the first-launch flow so the benchmark map's
	// frontend knows it should offer the preset preview + language pick before
	// continuing to the hub (docs/06 section 4).
	const TCHAR* GFirstLaunchOption = TEXT("firstlaunch=1");

	// Story registry data asset: the editor importer writes
	// UQinglanStoryRegistry instances under /Game/Qinglan/Data
	// (blueprint section 1.9; code-contracts.md "runtime code only references
	// keys" - the path is content, not player-facing text).
	// FIXME-CONTRACT(data): the imported asset name is a convention until the
	// importer lands; UQinglanStorySubsystem::GetRegistry() remains the
	// authoritative accessor.
	const TCHAR* GStoryRegistryPath = TEXT("/Game/Qinglan/Data/DA_StoryRegistry");

	/** Subsystem cache + registry handle for one game instance. */
	struct FQinglanInstanceExtra
	{
		TWeakObjectPtr<UQinglanGraphicsSettingsService> Graphics;
		TWeakObjectPtr<UQinglanSaveSubsystem> Save;
		TWeakObjectPtr<UQinglanProfileSubsystem> Profile;
		TWeakObjectPtr<UQinglanRunSubsystem> Run;
		TWeakObjectPtr<UQinglanStorySubsystem> Story;
		/** Keeps the async registry load resident until shutdown. */
		TSharedPtr<FStreamableHandle> RegistryHandle;
		bool bEarlySettingsApplied = false;
	};

	TMap<TWeakObjectPtr<UQinglanGameInstance>, FQinglanInstanceExtra>& InstanceExtras()
	{
		static TMap<TWeakObjectPtr<UQinglanGameInstance>, FQinglanInstanceExtra> Map;
		return Map;
	}

	FQinglanInstanceExtra& ExtraFor(UQinglanGameInstance* Instance)
	{
		TMap<TWeakObjectPtr<UQinglanGameInstance>, FQinglanInstanceExtra>& Extras = InstanceExtras();
		for (auto It = Extras.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent();
			}
		}
		return Extras.FindOrAdd(Instance);
	}

	/** Lazy re-fetch: Init caches these, but accessors stay valid if a subsystem
	 *  was not yet created during the collection's early phase. */
	template <typename TSubsystem>
	TSubsystem* ResolveSubsystem(UQinglanGameInstance* Instance, TWeakObjectPtr<TSubsystem>& Cached)
	{
		if (TSubsystem* Found = Cached.Get())
		{
			return Found;
		}
		if (!Instance)
		{
			return nullptr;
		}
		Cached = Instance->GetSubsystem<TSubsystem>();
		return Cached.Get();
	}

	/**
	 * First launch = no profile exists in any of the 3 slots (SaveSubsystem
	 * slot contract; 3 slots x 2 backups, blueprint section 1.6).
	 * Fail-open when the save subsystem is unavailable: treating a broken save
	 * system as "first launch" keeps the game playable read-only.
	 */
	bool IsFirstLaunch(UQinglanGameInstance* Instance)
	{
		FQinglanInstanceExtra& Extra = ExtraFor(Instance);
		UQinglanSaveSubsystem* Save = ResolveSubsystem(Instance, Extra.Save);
		if (!Save)
		{
			return true;
		}
		for (int32 Slot = 0; Slot < UQinglanSaveSubsystem::ProfileSlotCount; ++Slot)
		{
			if (Save->HasProfile(Slot))
			{
				return false;
			}
		}
		return true;
	}

	/** First launch auto-selects zh-Hans when the OS language matches (blueprint section 1.8). */
	void ApplyFirstLaunchLocale(UQinglanGameInstance* Instance)
	{
		if (!IsFirstLaunch(Instance))
		{
			return; // returning players keep their persisted culture
		}
		FInternationalization& I18N = FInternationalization::Get();
		const FString OSLanguage = I18N.GetCurrentLanguage();
		if (OSLanguage.StartsWith(TEXT("zh")))
		{
			if (I18N.SetCurrentCulture(TEXT("zh-Hans")))
			{
				UE_LOG(LogQinglan, Log, TEXT("First launch: OS language matched, zh-Hans culture applied"));
			}
		}
	}

	void TravelToMap(UQinglanGameInstance* Instance, const TCHAR* MapPath, const TCHAR* Options)
	{
		UWorld* World = Instance ? Instance->GetWorld() : nullptr;
		if (!World)
		{
			UE_LOG(LogQinglan, Warning, TEXT("StartFirstMap: no world to travel from yet"));
			return;
		}
		UGameplayStatics::OpenLevel(World, FName(MapPath), true, Options);
	}
}

void UQinglanGameInstance::Init()
{
	Super::Init();

	FQinglanInstanceExtra& Extra = ExtraFor(this);

	// Cache subsystems at boot (gameplay code reaches them through the game
	// instance everywhere else; blueprint section 1.1 ownership table).
	Extra.Graphics = GetSubsystem<UQinglanGraphicsSettingsService>();
	Extra.Save = GetSubsystem<UQinglanSaveSubsystem>();
	Extra.Profile = GetSubsystem<UQinglanProfileSubsystem>();
	Extra.Run = GetSubsystem<UQinglanRunSubsystem>();
	Extra.Story = GetSubsystem<UQinglanStorySubsystem>();

	// Early graphics: conservative detected preset (or the persisted preset the
	// service returns once settings exist) applied before any heavy scene load
	// (docs/06 section 4; task contract: DetectInitialSettings then ApplySettings).
	ApplyEarlyPlatformSettings();

	// Warm the story registry so the first scene never stalls on a synchronous
	// load after travel (blueprint section 1.9: registries are data assets).
	if (UAssetManager* Manager = UAssetManager::GetIfInitialized())
	{
		Extra.RegistryHandle = Manager->GetStreamableManager().RequestAsyncLoad(
			FSoftObjectPath(GStoryRegistryPath),
			FStreamableDelegate(),
			FStreamableManager::DefaultAsyncLoadPriority);
		UE_LOG(LogQinglan, Log, TEXT("Story registry warm load requested"));
	}

	ApplyFirstLaunchLocale(this);

	UE_LOG(LogQinglan, Log, TEXT("Game instance initialized"));
}

void UQinglanGameInstance::Shutdown()
{
	FQinglanInstanceExtra& Extra = ExtraFor(this);
	if (Extra.RegistryHandle.IsValid())
	{
		Extra.RegistryHandle->CancelHandle();
		Extra.RegistryHandle.Reset();
	}
	InstanceExtras().Remove(this);

	UE_LOG(LogQinglan, Log, TEXT("Game instance shut down"));
	Super::Shutdown();
}

void UQinglanGameInstance::ApplyEarlyPlatformSettings()
{
	FQinglanInstanceExtra& Extra = ExtraFor(this);
	if (Extra.bEarlySettingsApplied)
	{
		// Later calls belong to the settings UI; never clobber user changes
		// with detected defaults a second time.
		return;
	}
	UQinglanGraphicsSettingsService* Graphics = ResolveSubsystem(this, Extra.Graphics);
	if (!Graphics)
	{
		UE_LOG(LogQinglan, Warning, TEXT("ApplyEarlyPlatformSettings: graphics service unavailable"));
		return;
	}
	// DetectInitialSettings returns the persisted preset when the platform
	// settings exist and a conservative capability-class preset otherwise
	// (docs/06 section 4); ApplySettings pushes it into GameUserSettings.
	const FQinglanGraphicsSettings Initial = Graphics->DetectInitialSettings();
	Graphics->ApplySettings(Initial);
	Extra.bEarlySettingsApplied = true;
	UE_LOG(LogQinglan, Log, TEXT("Early platform settings applied (preset %d)"),
		static_cast<int32>(Initial.Preset));
}

void UQinglanGameInstance::StartFirstMap()
{
	// Retry the early-graphics pass: during Init the subsystem collection may
	// not have created the service yet on some boot paths (guarded once-only,
	// and it must land before the first heavy scene per docs/06 section 4).
	ApplyEarlyPlatformSettings();

	const bool bFirstLaunch = IsFirstLaunch(this);

#if UE_BUILD_SHIPPING
	// Shipping uses the tested default flow: benchmark maps are stripped from
	// shipping builds (docs/05 shipping section), so both paths go to the hub.
	TravelToMap(this, GHubMapPath, bFirstLaunch ? GFirstLaunchOption : TEXT(""));
#else
	if (bFirstLaunch)
	{
		// First launch: offer the short hardware benchmark + preset preview
		// before gameplay (docs/06 section 4); that frontend then travels on
		// to the hub.
		TravelToMap(this, GBenchmarkMapPath, GFirstLaunchOption);
	}
	else
	{
		TravelToMap(this, GHubMapPath, TEXT(""));
	}
#endif
}
