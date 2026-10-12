// Preset scalability mixes and key helpers for the graphics settings service.
// Preset policy: docs/06-platform-and-performance.md §1 (reference matrix,
// "Balanced/High hybrid" Windows, "Performance/Balanced" base Mac) and §4
// (conservative initial preset; every setting exposed is user-overridable).
#include "Platform/QinglanGraphicsPresets.h"

DEFINE_LOG_CATEGORY(LogQinglanGraphics);

namespace QinglanGraphics
{
	FQinglanGraphicsSettings MakePresetSettings(EQinglanQualityPreset Preset)
	{
		// Starts from the scaffold defaults; only preset-authored fields are
		// assigned. Low quality never removes warning telegraphs or impact
		// silhouettes — that is a cue-selection rule handled through feature
		// gates, not a scalability number (code-contracts.md).
		FQinglanGraphicsSettings Settings;
		Settings.Preset = Preset;
		switch (Preset)
		{
		case EQinglanQualityPreset::Low:
			Settings.TextureQuality = 1;      // reduced texture residency (docs/06 §1 8 GB stress case)
			Settings.ShadowQuality = 0;
			Settings.EffectsDetail = 0;
			Settings.ViewDistance = 0;
			Settings.ResolutionScale = 0.8f;  // reduced internal resolution (docs/06 §1)
			Settings.bVSync = false;
			Settings.FrameRateLimit = 0;      // platform default comes from detection, never the preset
			Settings.bLowFlashMode = false;   // accessibility choice, never preset-forced (docs/11 §accessibility)
			Settings.bDynamicResolution = true; // TSR/dynamic resolution evaluated + exposed (docs/06 §3)
			break;
		case EQinglanQualityPreset::Balanced:
			Settings.TextureQuality = 2;      // hybrid mix: textures ride high, costlier systems stay mid
			Settings.ShadowQuality = 1;
			Settings.EffectsDetail = 1;
			Settings.ViewDistance = 1;
			Settings.ResolutionScale = 1.f;
			Settings.bVSync = false;
			Settings.FrameRateLimit = 0;
			Settings.bLowFlashMode = false;
			Settings.bDynamicResolution = true;
			break;
		case EQinglanQualityPreset::High:
			Settings.TextureQuality = 2;      // hero textures 2K max (docs/04 §2)
			Settings.ShadowQuality = 2;
			Settings.EffectsDetail = 2;
			Settings.ViewDistance = 2;
			Settings.ResolutionScale = 1.f;
			Settings.bVSync = false;
			Settings.FrameRateLimit = 0;
			Settings.bLowFlashMode = false;
			Settings.bDynamicResolution = true;
			break;
		case EQinglanQualityPreset::Ultra:
			Settings.TextureQuality = 3;      // engine "Epic" scalability band
			Settings.ShadowQuality = 3;
			Settings.EffectsDetail = 3;
			Settings.ViewDistance = 3;
			Settings.ResolutionScale = 1.f;
			Settings.bVSync = false;
			Settings.FrameRateLimit = 0;
			Settings.bLowFlashMode = false;
			Settings.bDynamicResolution = true;
			break;
		}
		return Settings;
	}

	FQinglanGraphicsSettings Sanitize(const FQinglanGraphicsSettings& Settings)
	{
		FQinglanGraphicsSettings Out = Settings;
		Out.Preset = static_cast<EQinglanQualityPreset>(FMath::Clamp(
			static_cast<int32>(Out.Preset),
			static_cast<int32>(EQinglanQualityPreset::Low),
			static_cast<int32>(EQinglanQualityPreset::Ultra)));
		Out.TextureQuality = FMath::Clamp(Out.TextureQuality, MinQualityLevel, MaxQualityLevel);
		Out.ShadowQuality = FMath::Clamp(Out.ShadowQuality, MinQualityLevel, MaxQualityLevel);
		Out.EffectsDetail = FMath::Clamp(Out.EffectsDetail, MinQualityLevel, MaxQualityLevel);
		Out.ViewDistance = FMath::Clamp(Out.ViewDistance, MinQualityLevel, MaxQualityLevel);
		Out.ResolutionScale = FMath::Clamp(Out.ResolutionScale, MinResolutionScale, MaxResolutionScale);
		Out.FrameRateLimit = FMath::Clamp(Out.FrameRateLimit, MinFrameRateLimit, MaxFrameRateLimit);
		return Out;
	}

	const TCHAR* PresetDisplayKey(EQinglanQualityPreset Preset)
	{
		switch (Preset)
		{
		case EQinglanQualityPreset::Low:
			return TEXT("preset.performance"); // the docs name the low tier "Performance" (docs/06 §1)
		case EQinglanQualityPreset::Balanced:
			return TEXT("preset.balanced");
		case EQinglanQualityPreset::High:
			return TEXT("preset.high");
		case EQinglanQualityPreset::Ultra:
			// FIXME-CONTRACT: no preset.ultra key exists in localization/*.json;
			// Ultra currently displays through the High key until string tables
			// gain a fourth tier.
			return TEXT("preset.high");
		}
		return TEXT("preset.balanced");
	}

	const TCHAR* FeatureGateReasonKey(FName FeatureId)
	{
		if (FeatureId == FName(FeatureHardwareRayTracing))
		{
			return TEXT("settings.feature.hardware_ray_tracing.reason"); // capable Windows hardware only (docs/06 §3)
		}
		if (FeatureId == FName(FeatureLumen))
		{
			return TEXT("settings.feature.lumen.reason"); // off at Low (docs/06 §3)
		}
		if (FeatureId == FName(FeatureNanite))
		{
			return TEXT("settings.feature.nanite.reason"); // off at Low, ordinary LOD fallback (docs/06 §3)
		}
		if (FeatureId == FName(FeatureGpuParticles))
		{
			return TEXT("settings.feature.gpu_particles.reason"); // validated paths only (docs/06 §2-§3)
		}
		if (FeatureId == FName(FeatureLowFlashMode))
		{
			return TEXT("settings.feature.low_flash_mode.reason"); // silhouette + caption substitution (docs/11)
		}
		return TEXT("settings.feature.unavailable.reason");
	}
}
