// UQinglanGraphicsSettingsService: capability detection, preset application,
// platform-local persistence, feature gating, and the resolution/fullscreen
// preview-revert window.
// Contracts: docs/06-platform-and-performance.md §2-§5 (GPU use, feature
// policy, first-launch settings, frame budgets), code-contracts.md
// ("Every shipped technique keeps telegraph + impact silhouette at Low
// quality: cue selection goes through quality tiers (GraphicsSettingsService)").
#include "Platform/QinglanGraphicsSettingsService.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "RHI.h"

#include "Platform/QinglanGraphicsPresets.h"

namespace
{
	/**
	 * Ini section inside GGameUserSettingsIni. Graphics settings are
	 * platform-local and never transferred with progression saves (scaffold
	 * header comment; docs/dev/implementation-blueprint.md §1.6).
	 */
	const TCHAR* GraphicsIniSection = TEXT("Qinglan.GraphicsSettings");

	/**
	 * Per-subsystem runtime state. The scaffold header is fixed (no data
	 * members allowed), so state lives beside the instance keyed by weak
	 * pointer — the same pattern as GSaveBookkeeping in
	 * Persistence/QinglanSaveSubsystem.cpp. One game instance exists per
	 * process; weak keys keep parallel automation instances isolated.
	 */
	struct FQinglanGraphicsBookkeeping
	{
		bool bLoaded = false;
		FQinglanGraphicsSettings Current;
		bool bPreviewActive = false;
		float PreviewSecondsRemaining = 0.f;
		FIntPoint PreviewResolution = FIntPoint::NoneValue;
		EWindowMode::Type PreviewFullscreenMode = EWindowMode::Windowed;
		FTSTicker::FDelegateHandle PreviewTickerHandle;
	};

	TMap<TWeakObjectPtr<UQinglanGraphicsSettingsService>, FQinglanGraphicsBookkeeping> GGraphicsBookkeeping;

	FQinglanGraphicsBookkeeping& Book(const UQinglanGraphicsSettingsService* Key)
	{
		return GGraphicsBookkeeping.FindOrAdd(const_cast<UQinglanGraphicsSettingsService*>(Key));
	}

	UGameUserSettings* ResolveGameUserSettings()
	{
		return GEngine ? GEngine->GetGameUserSettings() : nullptr;
	}

	/** Active RHI backend name from the engine (e.g. D3D12, Metal, Vulkan). */
	FString GetRhiNameString()
	{
		return FString(GDynamicRHI ? GDynamicRHI->GetName() : TEXT(""));
	}

	/**
	 * Unified (shared CPU/GPU) memory pool: Apple Silicon Macs. Unified memory
	 * must not be mistaken for dedicated VRAM (docs/06 §4); Intel Mac is
	 * outside the release scope (docs/06 §1).
	 */
	bool IsUnifiedMemoryPlatform()
	{
#if PLATFORM_MAC
		return true;
#else
		return false;
#endif
	}

	/** Total system memory in GiB via engine platform APIs (docs/06 §4). */
	int64 GetTotalPhysicalGB()
	{
		const FPlatformMemoryConstants& Memory = FPlatformMemory::GetConstants();
		return static_cast<int64>(Memory.TotalPhysical / (1024ULL * 1024ULL * 1024ULL));
	}

	/**
	 * Dedicated GPU memory in GiB through the engine RHI globals
	 * (GRHIGlobals.GpuInfo.DedicatedVideoMemory; populated for D3D11/D3D12).
	 * 0 means unknown (or a unified-memory part): never a downgrade signal.
	 */
	int64 GetDedicatedVramGB()
	{
		return static_cast<int64>(GRHIGlobals.GpuInfo.DedicatedVideoMemory / (1024ULL * 1024ULL * 1024ULL));
	}

	/** Dedicated VRAM below this selects the Low preset (docs/06 §1 matrix has no sub-4GB tier). */
	constexpr int64 MinDedicatedVramGB = 4;

	void SetIntCvar(const TCHAR* CvarName, int32 Value,
		EConsoleVariableFlags Priority = ECVF_SetByGameSetting)
	{
		IConsoleVariable* Cvar = IConsoleManager::Get().FindConsoleVariable(CvarName);
		if (Cvar != nullptr)
		{
			// Device profiles, system configuration, and command-line/debug
			// overrides remain authoritative. Do not escalate to SetByCode just
			// to force a user preset through one of those constraints.
			const uint32 CurrentPriority = Cvar->GetFlags() & ECVF_SetByMask;
			if (CurrentPriority > static_cast<uint32>(Priority))
			{
				UE_LOG(LogQinglanGraphics, Verbose,
					TEXT("Console variable '%s' retains its higher-priority override"), CvarName);
				return;
			}
			Cvar->Set(Value, Priority);
		}
		else
		{
			// Headless/early-boot tolerance: never fail the whole apply for one cvar.
			UE_LOG(LogQinglanGraphics, Warning, TEXT("Console variable '%s' unavailable; value %d not applied"), CvarName, Value);
		}
	}

	void SetFeaturePolicyCvar(const TCHAR* CvarName, int32 Value)
	{
		// RendererSettings supplies the project's starting GI/reflection
		// methods at ProjectSetting priority. These three explicitly managed
		// runtime switches belong to that same project policy; GameSetting is
		// weaker and silently leaves the initial renderer methods unchanged.
		SetIntCvar(CvarName, Value, ECVF_SetByProjectSetting);
	}

	/** Console variables UGameUserSettings does not persist itself. */
	void ApplyResolutionScaleAndDynamicRes(const FQinglanGraphicsSettings& Settings)
	{
		// Internal resolution (docs/06 §4 "resolution scale"); TSR/dynamic
		// resolution adapts within user-selected bounds (docs/06 §4).
		SetIntCvar(TEXT("r.ScreenPercentage"), FMath::RoundToInt(Settings.ResolutionScale * 100.f));
		// Engine contract: r.DynamicRes.OperationMode gates the system
		// (0 disabled, 1 follows the game user settings flag), and the
		// user-side enable state lives in UGameUserSettings. "r.DynamicRes"
		// alone is not a console variable.
		SetIntCvar(TEXT("r.DynamicRes.OperationMode"), Settings.bDynamicResolution ? 1 : 0);
	}

	/**
	 * Preset feature policy made real in the renderer: Low drops Lumen GI and
	 * Nanite and falls back to screen-space reflections (IsFeatureAllowed
	 * mirrors this table; "lumen/nanite off at Low", docs/06 §3).
	 */
	void ApplyFeaturePolicy(const FQinglanGraphicsSettings& Settings)
	{
		const bool bReduced = Settings.Preset == EQinglanQualityPreset::Low;
		// 0 none / 1 Lumen / 2 SSGI for r.DynamicGlobalIlluminationMethod;
		// 0 none / 1 Lumen / 2 SSR for r.ReflectionMethod; r.Nanite 0/1.
		SetFeaturePolicyCvar(TEXT("r.DynamicGlobalIlluminationMethod"), bReduced ? 0 : 1);
		SetFeaturePolicyCvar(TEXT("r.ReflectionMethod"), bReduced ? 2 : 1);
		SetFeaturePolicyCvar(TEXT("r.Nanite"), bReduced ? 0 : 1);
	}

	/** Full engine application: scalability + VSync + frame limit, then cvars. */
	void ApplyToEngine(const FQinglanGraphicsSettings& Settings, bool bIncludeGameUserSettings)
	{
		if (bIncludeGameUserSettings)
		{
			if (UGameUserSettings* GameUserSettings = ResolveGameUserSettings())
			{
				GameUserSettings->SetTextureQuality(Settings.TextureQuality);
				GameUserSettings->SetShadowQuality(Settings.ShadowQuality);
				GameUserSettings->SetVisualEffectQuality(Settings.EffectsDetail);
				GameUserSettings->SetViewDistanceQuality(Settings.ViewDistance);
				GameUserSettings->SetVSyncEnabled(Settings.bVSync);
				GameUserSettings->SetFrameRateLimit(static_cast<float>(Settings.FrameRateLimit));
				// User-side enable for r.DynamicRes.OperationMode=1 (engine
				// keeps both sides in sync through its own status query).
				GameUserSettings->SetDynamicResolutionEnabled(Settings.bDynamicResolution);
				// Honor command-line window overrides (-windowed/-Res): the
				// settings bool is bCheckForCommandLineOverrides, not a
				// validation switch. Passing false silently re-applied the
				// saved fullscreen mode over the capture/QA flags and put the
				// window into a fullscreen transition before the first frame.
				GameUserSettings->ApplySettings(/*bCheckForCommandLineOverrides=*/true);
			}
			else
			{
				UE_LOG(LogQinglanGraphics, Warning, TEXT("GameUserSettings unavailable; scalability/VSync/frame limit not applied"));
			}
		}
		ApplyResolutionScaleAndDynamicRes(Settings);
		ApplyFeaturePolicy(Settings);
	}

	void PersistToIni(const FQinglanGraphicsSettings& Settings)
	{
		if (GConfig == nullptr || GGameUserSettingsIni.Len() == 0)
		{
			UE_LOG(LogQinglanGraphics, Warning, TEXT("GameUserSettings ini unavailable; graphics settings not persisted"));
			return;
		}
		GConfig->SetInt(GraphicsIniSection, TEXT("Preset"), static_cast<int32>(Settings.Preset), *GGameUserSettingsIni);
		GConfig->SetInt(GraphicsIniSection, TEXT("TextureQuality"), Settings.TextureQuality, *GGameUserSettingsIni);
		GConfig->SetInt(GraphicsIniSection, TEXT("ShadowQuality"), Settings.ShadowQuality, *GGameUserSettingsIni);
		GConfig->SetInt(GraphicsIniSection, TEXT("EffectsDetail"), Settings.EffectsDetail, *GGameUserSettingsIni);
		GConfig->SetInt(GraphicsIniSection, TEXT("ViewDistance"), Settings.ViewDistance, *GGameUserSettingsIni);
		GConfig->SetFloat(GraphicsIniSection, TEXT("ResolutionScale"), Settings.ResolutionScale, *GGameUserSettingsIni);
		GConfig->SetBool(GraphicsIniSection, TEXT("bVSync"), Settings.bVSync, *GGameUserSettingsIni);
		GConfig->SetInt(GraphicsIniSection, TEXT("FrameRateLimit"), Settings.FrameRateLimit, *GGameUserSettingsIni);
		GConfig->SetBool(GraphicsIniSection, TEXT("bLowFlashMode"), Settings.bLowFlashMode, *GGameUserSettingsIni);
		GConfig->SetBool(GraphicsIniSection, TEXT("bDynamicResolution"), Settings.bDynamicResolution, *GGameUserSettingsIni);
		GConfig->Flush(false, *GGameUserSettingsIni);
	}

	bool LoadFromIni(FQinglanGraphicsSettings& OutSettings)
	{
		OutSettings = FQinglanGraphicsSettings();
		if (GConfig == nullptr || GGameUserSettingsIni.Len() == 0)
		{
			return false;
		}
		FString PresetValue;
		if (!GConfig->GetString(GraphicsIniSection, TEXT("Preset"), PresetValue, *GGameUserSettingsIni))
		{
			return false; // first launch on this machine
		}
		int32 PresetInt = static_cast<int32>(EQinglanQualityPreset::Balanced);
		int32 Quality = 2;
		float ResolutionScale = 1.f;
		int32 FrameRateLimit = 0;
		bool bVSync = false;
		bool bLowFlashMode = false;
		bool bDynamicResolution = true;
		GConfig->GetInt(GraphicsIniSection, TEXT("Preset"), PresetInt, *GGameUserSettingsIni);
		GConfig->GetInt(GraphicsIniSection, TEXT("TextureQuality"), Quality, *GGameUserSettingsIni);
		OutSettings.TextureQuality = Quality;
		GConfig->GetInt(GraphicsIniSection, TEXT("ShadowQuality"), Quality, *GGameUserSettingsIni);
		OutSettings.ShadowQuality = Quality;
		GConfig->GetInt(GraphicsIniSection, TEXT("EffectsDetail"), Quality, *GGameUserSettingsIni);
		OutSettings.EffectsDetail = Quality;
		GConfig->GetInt(GraphicsIniSection, TEXT("ViewDistance"), Quality, *GGameUserSettingsIni);
		OutSettings.ViewDistance = Quality;
		GConfig->GetFloat(GraphicsIniSection, TEXT("ResolutionScale"), ResolutionScale, *GGameUserSettingsIni);
		OutSettings.ResolutionScale = ResolutionScale;
		GConfig->GetBool(GraphicsIniSection, TEXT("bVSync"), bVSync, *GGameUserSettingsIni);
		OutSettings.bVSync = bVSync;
		GConfig->GetInt(GraphicsIniSection, TEXT("FrameRateLimit"), FrameRateLimit, *GGameUserSettingsIni);
		OutSettings.FrameRateLimit = FrameRateLimit;
		GConfig->GetBool(GraphicsIniSection, TEXT("bLowFlashMode"), bLowFlashMode, *GGameUserSettingsIni);
		OutSettings.bLowFlashMode = bLowFlashMode;
		GConfig->GetBool(GraphicsIniSection, TEXT("bDynamicResolution"), bDynamicResolution, *GGameUserSettingsIni);
		OutSettings.bDynamicResolution = bDynamicResolution;
		OutSettings.Preset = static_cast<EQinglanQualityPreset>(FMath::Clamp(PresetInt,
			static_cast<int32>(EQinglanQualityPreset::Low), static_cast<int32>(EQinglanQualityPreset::Ultra)));
		OutSettings = QinglanGraphics::Sanitize(OutSettings);
		return true;
	}

	/** Paused-game-time semantics: hold while the game world is paused (code-contracts.md). */
	bool IsGamePausedForService(const UQinglanGraphicsSettingsService* Service)
	{
		const UGameInstance* GameInstancePtr = Service->GetGameInstance();
		const UWorld* World = GameInstancePtr ? GameInstancePtr->GetWorld() : nullptr;
		return World != nullptr && UGameplayStatics::IsGamePaused(World);
	}

	void RevertVideoSettings(const FQinglanGraphicsBookkeeping& Bookkeeping)
	{
		UGameUserSettings* GameUserSettings = ResolveGameUserSettings();
		if (GameUserSettings == nullptr)
		{
			UE_LOG(LogQinglanGraphics, Error, TEXT("Preview revert: GameUserSettings unavailable"));
			return;
		}
		GameUserSettings->SetScreenResolution(Bookkeeping.PreviewResolution);
		GameUserSettings->SetFullscreenMode(Bookkeeping.PreviewFullscreenMode);
		GameUserSettings->ApplyResolutionSettings(/*bCheckForCommandLineOverrides=*/true);
		UE_LOG(LogQinglanGraphics, Log, TEXT("Preview window expired: reverted to %dx%d (window mode %d)"),
			Bookkeeping.PreviewResolution.X, Bookkeeping.PreviewResolution.Y,
			static_cast<int32>(Bookkeeping.PreviewFullscreenMode));
	}

	/** Core-ticker step; returns false to unregister when the window is done. */
	bool TickPreviewRevert(UQinglanGraphicsSettingsService* Service, float DeltaTime)
	{
		FQinglanGraphicsBookkeeping& Bookkeeping = Book(Service);
		if (!Bookkeeping.bPreviewActive)
		{
			Bookkeeping.PreviewTickerHandle = FTSTicker::FDelegateHandle();
			return false;
		}
		// Paused-game-time semantics: the countdown only advances while world
		// time advances (code-contracts.md "Timers use paused game time").
		if (IsGamePausedForService(Service))
		{
			return true;
		}
		Bookkeeping.PreviewSecondsRemaining -= DeltaTime;
		if (Bookkeeping.PreviewSecondsRemaining > 0.f)
		{
			return true;
		}
		Bookkeeping.bPreviewActive = false;
		RevertVideoSettings(Bookkeeping);
		Bookkeeping.PreviewTickerHandle = FTSTicker::FDelegateHandle();
		return false;
	}

	/**
	 * Lazy load: persisted settings if present, otherwise first-launch
	 * detection persisted and applied so a low-memory Mac never reaches the
	 * first heavy scene on wrong settings (docs/06 §4 "Saved settings are
	 * applied early enough to prevent an expensive startup scene").
	 * The scaffold header has no Initialize override; Core's
	 * UQinglanGameInstance::ApplyEarlyPlatformSettings should touch
	 * GetCurrentSettings() during Init to force this path at boot.
	 */
	void EnsureLoaded(const UQinglanGraphicsSettingsService* Service)
	{
		FQinglanGraphicsBookkeeping& Bookkeeping = Book(Service);
		if (Bookkeeping.bLoaded)
		{
			return;
		}
		Bookkeeping.bLoaded = true;
		FQinglanGraphicsSettings Loaded;
		if (LoadFromIni(Loaded))
		{
			// The engine re-applied the persisted GameUserSettings (scalability,
			// VSync, frame limit) during boot; only refresh the console
			// variables it does not persist (screen percentage, dynamic-res
			// operation mode, GI/reflection/Nanite feature policy).
			Bookkeeping.Current = Loaded;
			ApplyToEngine(Loaded, /*bIncludeGameUserSettings=*/false);
		}
		else
		{
			// First launch: conservative preset from detected capability class,
			// applied immediately and persisted (docs/06 §4 "Select a
			// conservative initial preset by capability class").
			Bookkeeping.Current = Service->DetectInitialSettings();
			ApplyToEngine(Bookkeeping.Current, /*bIncludeGameUserSettings=*/true);
			PersistToIni(Bookkeeping.Current);
			if (UGameUserSettings* GameUserSettings = ResolveGameUserSettings())
			{
				GameUserSettings->SaveSettings();
			}
			UE_LOG(LogQinglanGraphics, Log, TEXT("First launch: detected preset %d, scale %.2f, fps limit %d, unified-memory platform: %s"),
				static_cast<int32>(Bookkeeping.Current.Preset), Bookkeeping.Current.ResolutionScale,
				Bookkeeping.Current.FrameRateLimit, IsUnifiedMemoryPlatform() ? TEXT("yes") : TEXT("no"));
		}
	}
}

void UQinglanGraphicsSettingsService::ApplySettings(const FQinglanGraphicsSettings& NewSettings)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogQinglanGraphics, Error, TEXT("ApplySettings called off the game thread; ignored"));
		return;
	}
	const FQinglanGraphicsSettings Sanitized = QinglanGraphics::Sanitize(NewSettings);
	FQinglanGraphicsBookkeeping& Bookkeeping = Book(this);
	Bookkeeping.Current = Sanitized;
	Bookkeeping.bLoaded = true;
	ApplyToEngine(Sanitized, /*bIncludeGameUserSettings=*/true);
	if (UGameUserSettings* GameUserSettings = ResolveGameUserSettings())
	{
		GameUserSettings->SaveSettings(); // platform-local persistence
	}
	PersistToIni(Sanitized);
	// Preset changes are explicit user menu actions; nothing here re-chooses
	// presets automatically during combat (docs/06 §4 "Avoid changing preset
	// continuously during combat").
	UE_LOG(LogQinglanGraphics, Log, TEXT("Graphics settings applied: preset %d, texture %d, shadow %d, effects %d, view %d, scale %.2f, vsync %d, fps %d, dynres %d, low-flash %d"),
		static_cast<int32>(Sanitized.Preset), Sanitized.TextureQuality, Sanitized.ShadowQuality,
		Sanitized.EffectsDetail, Sanitized.ViewDistance, Sanitized.ResolutionScale,
		Sanitized.bVSync ? 1 : 0, Sanitized.FrameRateLimit, Sanitized.bDynamicResolution ? 1 : 0,
		Sanitized.bLowFlashMode ? 1 : 0);
}

FQinglanGraphicsSettings UQinglanGraphicsSettingsService::GetCurrentSettings() const
{
	EnsureLoaded(this);
	return Book(this).Current;
}

FText UQinglanGraphicsSettingsService::GetActiveAdapterName() const
{
	// Engine-reported adapter string shown in Advanced Graphics (docs/06 §2
	// "Show the selected GPU and active graphics API in Advanced Graphics").
	const FString AdapterName(GRHIAdapterName);
	if (!AdapterName.IsEmpty())
	{
		// Dynamic hardware data, not a translatable literal.
		return FText::FromString(AdapterName);
	}
	return QinglanGraphics::Loc(TEXT("settings.adapter_unknown"));
}

FName UQinglanGraphicsSettingsService::GetActiveRHIName() const
{
	const FString RhiName = GetRhiNameString();
	return RhiName.IsEmpty() ? NAME_None : FName(RhiName);
}

FQinglanGraphicsSettings UQinglanGraphicsSettingsService::DetectInitialSettings() const
{
	// Detection via engine platform APIs only: adapter/backend/memory
	// characteristics (docs/06 §4 "Read GPU adapter, backend, memory
	// characteristics ... through engine/platform APIs").
	const bool bUnified = IsUnifiedMemoryPlatform();
	const int64 TotalPhysicalGB = GetTotalPhysicalGB();
	// GPU capability, not just system memory: the RHI-reported dedicated VRAM
	// pool (0/unknown on unified parts and backends that do not report it).
	const int64 DedicatedVramGB = GetDedicatedVramGB();

	FQinglanGraphicsSettings Out;
	if (bUnified && TotalPhysicalGB <= QinglanGraphics::LowMemoryUnifiedGB)
	{
		// Low-memory unified stress case: Low preset + reduced internal
		// resolution + reduced texture residency (docs/06 §1).
		Out = QinglanGraphics::MakePresetSettings(EQinglanQualityPreset::Low);
	}
	else if (bUnified)
	{
		// Base M2/M3-class tier: Performance/Balanced target (docs/06 §1).
		Out = QinglanGraphics::MakePresetSettings(EQinglanQualityPreset::Balanced);
	}
	else if (TotalPhysicalGB < QinglanGraphics::WindowsBaselineSystemGB
		|| (DedicatedVramGB > 0 && DedicatedVramGB < MinDedicatedVramGB))
	{
		// Below the Windows reference matrix floor (16 GB RAM), or a reported
		// dedicated VRAM pool under the reference matrix tier: conservative Low.
		Out = QinglanGraphics::MakePresetSettings(EQinglanQualityPreset::Low);
	}
	else
	{
		// Windows gaming-laptop baseline: conservative Balanced (docs/06 §1
		// "Balanced/High hybrid" is the ceiling, detection stays conservative).
		Out = QinglanGraphics::MakePresetSettings(EQinglanQualityPreset::Balanced);
	}

#if PLATFORM_WINDOWS
	// D3D12/SM6 is the validated Windows backend (docs/06 §2 "Prefer
	// D3D12/SM6 on target gaming laptops"); an unvalidated fallback backend
	// (e.g. D3D11) keeps the conservative Low preset.
	if (!bUnified && GetRhiNameString() != TEXT("D3D12"))
	{
		Out = QinglanGraphics::MakePresetSettings(EQinglanQualityPreset::Low);
	}
#endif

	if (bUnified)
	{
		// Unified memory is shared, not dedicated VRAM (docs/06 §4): cap
		// effects detail at the 16 GB-unified class. Larger unified pools stay
		// capped until Metal validation proves otherwise.
		Out.EffectsDetail = FMath::Min(Out.EffectsDetail, QinglanGraphics::UnifiedEffectsDetailCap);
		// "On Mac, cap the base preset to 30 FPS by default" (docs/06 §4).
		Out.FrameRateLimit = QinglanGraphics::MacDefaultFrameRateLimit;
	}
	else
	{
		Out.FrameRateLimit = QinglanGraphics::WindowsDefaultFrameRateLimit;
	}
	UE_LOG(LogQinglanGraphics, Log, TEXT("Detection: adapter '%s', RHI '%s', dedicated VRAM %d GiB, system %d GiB, unified=%d -> preset %d"),
		*FString(GRHIAdapterName), *GetRhiNameString(), static_cast<int32>(DedicatedVramGB),
		static_cast<int32>(TotalPhysicalGB), bUnified ? 1 : 0, static_cast<int32>(Out.Preset));
	return Out;
}

void UQinglanGraphicsSettingsService::BeginPreviewRevertWindow(float Seconds)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogQinglanGraphics, Error, TEXT("BeginPreviewRevertWindow called off the game thread; ignored"));
		return;
	}
	UGameUserSettings* GameUserSettings = ResolveGameUserSettings();
	if (GameUserSettings == nullptr)
	{
		UE_LOG(LogQinglanGraphics, Error, TEXT("BeginPreviewRevertWindow: GameUserSettings unavailable"));
		return;
	}

	FQinglanGraphicsBookkeeping& Bookkeeping = Book(this);
	// Snapshot the last *confirmed* video mode: the state in effect before the
	// pending resolution/fullscreen change the caller just applied (docs/06 §4
	// "15-second preview with Revert if the user changes resolution/fullscreen").
	FIntPoint RevertResolution = FIntPoint::NoneValue;
	EWindowMode::Type RevertMode = EWindowMode::Windowed;
	RevertResolution = GameUserSettings->GetLastConfirmedScreenResolution();
	RevertMode = GameUserSettings->GetLastConfirmedFullscreenMode();
	if (RevertResolution.X <= 0 || RevertResolution.Y <= 0)
	{
		// No confirmed mode recorded yet: snapshot the current mode so an
		// expiry revert is a safe no-op instead of a bogus resolution.
		RevertResolution = GameUserSettings->GetScreenResolution();
		RevertMode = GameUserSettings->GetFullscreenMode();
	}
	Bookkeeping.PreviewResolution = RevertResolution;
	Bookkeeping.PreviewFullscreenMode = RevertMode;
	Bookkeeping.PreviewSecondsRemaining = (Seconds > 0.f) ? Seconds : QinglanGraphics::PreviewRevertSecondsDefault;
	Bookkeeping.bPreviewActive = true;

	if (!Bookkeeping.PreviewTickerHandle.IsValid())
	{
		TWeakObjectPtr<UQinglanGraphicsSettingsService> WeakThis(this);
		Bookkeeping.PreviewTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([WeakThis](float DeltaTime)
			{
				UQinglanGraphicsSettingsService* Service = WeakThis.Get();
				if (Service == nullptr)
				{
					return false; // subsystem gone (shutdown): unregister
				}
				return TickPreviewRevert(Service, DeltaTime);
			}));
	}
	UE_LOG(LogQinglanGraphics, Log, TEXT("Preview revert window armed for %.1f s (revert target %dx%d, window mode %d)"),
		Bookkeeping.PreviewSecondsRemaining, Bookkeeping.PreviewResolution.X, Bookkeeping.PreviewResolution.Y,
		static_cast<int32>(Bookkeeping.PreviewFullscreenMode));
}

void UQinglanGraphicsSettingsService::CancelPreviewRevert()
{
	FQinglanGraphicsBookkeeping& Bookkeeping = Book(this);
	if (!Bookkeeping.bPreviewActive)
	{
		return;
	}
	Bookkeeping.bPreviewActive = false; // ticker unregisters itself on its next pulse
	// The user accepted the new video mode: promote it to the engine's
	// confirmed state so a later preview window snapshots the right revert
	// target.
	if (UGameUserSettings* GameUserSettings = ResolveGameUserSettings())
	{
		GameUserSettings->ConfirmVideoMode();
	}
	UE_LOG(LogQinglanGraphics, Log, TEXT("Preview revert window cancelled; new video mode kept"));
}

bool UQinglanGraphicsSettingsService::IsFeatureAllowed(FName FeatureId) const
{
	static const FName FeatureHardwareRayTracingName(QinglanGraphics::FeatureHardwareRayTracing);
	static const FName FeatureLumenName(QinglanGraphics::FeatureLumen);
	static const FName FeatureNaniteName(QinglanGraphics::FeatureNanite);
	static const FName FeatureGpuParticlesName(QinglanGraphics::FeatureGpuParticles);
	static const FName FeatureLowFlashModeName(QinglanGraphics::FeatureLowFlashMode);

	const FQinglanGraphicsSettings Settings = GetCurrentSettings();

	if (FeatureId == FeatureLowFlashModeName)
	{
		// Low-flash mode swaps flashing cues for silhouette + caption; enabled
		// purely by the user accessibility setting (docs/11 accessibility set,
		// docs/08 §52 "Performance/Low-flash settings preserve dangerous attack
		// markers").
		return Settings.bLowFlashMode;
	}
	if (FeatureId == FeatureLumenName)
	{
		// Lumen / scalable GI is off at Low (docs/06 §3 "evaluated scalable
		// GI"; module contract "lumen off at Low"). ApplyFeaturePolicy enforces
		// this in the renderer whenever settings are applied; this query only
		// reports it.
		return Settings.Preset != EQinglanQualityPreset::Low;
	}
	if (FeatureId == FeatureNaniteName)
	{
		// Nanite off at Low; ordinary LOD/shadow fallback below (docs/06 §3).
		return Settings.Preset != EQinglanQualityPreset::Low;
	}
	if (FeatureId == FeatureGpuParticlesName)
	{
		// GPU particle simulation runs only on validated paths: D3D12 on
		// Windows, validated Metal paths on Mac, never at Low (docs/06 §2-§3).
		if (Settings.Preset == EQinglanQualityPreset::Low)
		{
			return false;
		}
#if PLATFORM_WINDOWS
		return GetRhiNameString() == TEXT("D3D12");
#elif PLATFORM_MAC
		return GetRhiNameString() == TEXT("Metal");
#else
		return false;
#endif
	}
	if (FeatureId == FeatureHardwareRayTracingName)
	{
		// Hardware ray tracing: optional, capable GPUs only, Windows-only;
		// disabled for the Mac shipping baseline (docs/06 §3) and off at Low.
		if (Settings.Preset == EQinglanQualityPreset::Low)
		{
			return false;
		}
#if PLATFORM_WINDOWS
		return GRHISupportsRayTracing && GRHISupportsRayTracingShaders && GetRhiNameString() == TEXT("D3D12");
#else
		return false;
#endif
	}
	// Unknown ids fail closed; cue selection must never silently upgrade.
	UE_LOG(LogQinglanGraphics, Verbose, TEXT("IsFeatureAllowed: unknown feature id '%s' denied"), *FeatureId.ToString());
	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
// Same translation unit so the isolated CVar tests exercise the production
// setter without exposing a runtime API or changing real renderer state.
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGraphicsRuntimeFeaturePriority,
	"Qinglan.Graphics.RuntimeFeaturePriority",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGraphicsRespectsPlatformConstraints,
	"Qinglan.Graphics.RespectsPlatformConstraints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

#include "Platform/QinglanGraphicsSettingsTests.inl"
#endif
