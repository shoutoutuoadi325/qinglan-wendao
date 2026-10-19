// Game instance implementation: bootstrap, early graphics, story registry
// warm load, startup locale, and first-map travel.
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
// AUD-48: FQinglanLocalizationService loads the packaged bilingual table and
// registers the runtime translation source before any text is resolved.
// AUD-49: first-launch state is captured before profile 0 is created.
//
// FIXME-CONTRACT: the frozen scaffold header declares no storage members, so
// the cached subsystem pointers and the streamable registry handle live in a
// TU-local weak-keyed map beside each game instance (same pattern as
// Persistence/QinglanSaveSubsystem.cpp and AI/QinglanEncounterDirector.cpp).
// Merge them into the class when headers unfreeze.
#include "Core/QinglanGameInstance.h"

#include "Core/QinglanLocalizationService.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
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
		/**
		 * AUD-49: sampled in Init before profile 0 is created. StartFirstMap
		 * must reuse this value; re-querying after creation would always say
		 * "returning player".
		 */
		bool bFirstLaunch = false;
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

	/**
	 * AUD-49: startup culture priority is
	 *   explicit persisted preference > first-launch OS detection > en-US.
	 * bFirstLaunch is passed in because it must be sampled before Init creates
	 * profile 0; querying first-launch state here re-introduced the bug.
	 */
	void ApplyStartupLocale(bool bFirstLaunch)
	{
		FString ExplicitPreference;
		const bool bHasExplicitPreference =
			FQinglanLocalizationService::ReadPreferredCulture(ExplicitPreference);

		bool bShouldPersistDetected = false;
		const FString Culture = FQinglanLocalizationService::ResolveStartupCulture(
			bFirstLaunch,
			bHasExplicitPreference ? ExplicitPreference : FString(),
			FQinglanLocalizationService::GetOSDefaultCultureName(),
			bShouldPersistDetected);

		// First-launch detection becomes the persisted preference so a Chinese
		// OS keeps zh-Hans on subsequent launches (it is still lower priority
		// than any explicit player choice saved through the settings flow).
		if (bShouldPersistDetected)
		{
			FQinglanLocalizationService::WritePreferredCulture(Culture);
		}

		FQinglanLocalizationService::ApplyCulture(Culture);
		UE_LOG(LogQinglan, Log, TEXT("Startup locale: %s (first launch: %s, explicit preference: %s)"),
			*Culture,
			bFirstLaunch ? TEXT("yes") : TEXT("no"),
			bHasExplicitPreference ? TEXT("yes") : TEXT("no"));
	}

	void TravelToMap(UQinglanGameInstance* Instance, const TCHAR* MapPath, const TCHAR* Options)
	{
		UWorld* World = Instance ? Instance->GetWorld() : nullptr;
		if (!World)
		{
			UE_LOG(LogQinglan, Warning, TEXT("StartFirstMap: no world to travel from yet"));
			return;
		}
		if (UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>())
		{
			FText Error;
			if (!Save->SaveActiveSession(Error))
			{
				UE_LOG(LogQinglan, Warning, TEXT("First-map travel blocked by save failure: %s"), *Error.ToString());
				return;
			}
		}
		UGameplayStatics::OpenLevel(World, FName(MapPath), true, Options);
	}
}

void UQinglanGameInstance::Init()
{
	Super::Init();

	// AUD-48: load the packaged en-US + zh-Hans table and register the
	// text-localization source before any FText is resolved below.
	FQinglanLocalizationService::InitializeRuntimeResources();

	FQinglanInstanceExtra& Extra = ExtraFor(this);

	// Cache subsystems at boot (gameplay code reaches them through the game
	// instance everywhere else; blueprint section 1.1 ownership table).
	Extra.Graphics = GetSubsystem<UQinglanGraphicsSettingsService>();
	Extra.Save = GetSubsystem<UQinglanSaveSubsystem>();
	Extra.Profile = GetSubsystem<UQinglanProfileSubsystem>();
	Extra.Run = GetSubsystem<UQinglanRunSubsystem>();
	Extra.Story = GetSubsystem<UQinglanStorySubsystem>();

	// AUD-49: capture first-launch state BEFORE profile creation below. The
	// old order created profile 0 first, so ApplyFirstLaunchLocale always saw
	// an existing profile and never selected zh-Hans.
	Extra.bFirstLaunch = IsFirstLaunch(this);
	ApplyStartupLocale(Extra.bFirstLaunch);

	// AUD-01: first launch no longer creates slot 0 behind the player's back.
	// The hub boots with no profile session and AQinglanCharacter opens the
	// slot selector; the chosen slot's CreateNewProfile/LoadProfile starts the
	// session and then runs the deferred hub arrival work. bFirstLaunch was
	// already sampled above, before any profile could exist, so locale
	// detection keeps its ordering guarantee.
	if (Extra.bFirstLaunch)
	{
		UE_LOG(LogQinglan, Log,
			TEXT("First launch: awaiting slot selection at the profile frontend (AUD-01)"));
	}

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
	// GetCurrentSettings forces the service's lazy-load path: persisted
	// platform-local settings when the ini section exists, or the conservative
	// capability-class detection (applied AND persisted) on first launch
	// (docs/06 section 4). Never call DetectInitialSettings() directly here:
	// it always returns detected defaults and would clobber a returning
	// player's saved preset at every boot.
	const FQinglanGraphicsSettings Initial = Graphics->GetCurrentSettings();
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

	// AUD-49: reuse the flag captured before Init created profile 0; a fresh
	// IsFirstLaunch() here would always report false after that creation.
	const bool bFirstLaunch = ExtraFor(this).bFirstLaunch;

#if UE_BUILD_SHIPPING
	// Shipping uses the tested default flow: benchmark maps are stripped from
	// shipping builds (docs/05 shipping section), so both paths go to the hub.
	TravelToMap(this, GHubMapPath, bFirstLaunch ? GFirstLaunchOption : TEXT(""));
#else
	// The benchmark frontend map is not authored (docs/06 section 4 is a
	// deferred nicety); both launch paths go straight to the hub. First-launch
	// specifics (locale pick, profile creation) already ran above.
	TravelToMap(this, GHubMapPath, bFirstLaunch ? GFirstLaunchOption : TEXT(""));
#endif
}
