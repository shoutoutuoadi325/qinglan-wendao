// Graphics preset tables, capability thresholds, and frame-budget constants
// for Qinglan: Path of Ascension.
// Sources: docs/06-platform-and-performance.md sections 1-5 (targets, feature
// policy, first-launch settings, frame budgets) and
// docs/dev/implementation-blueprint.md section 1.7 (performance gates).
// Plain header: no reflected types, therefore no UHT macros (code-contracts.md).
#pragma once

#include "CoreMinimal.h"
#include "Platform/QinglanGraphicsSettingsService.h"

DECLARE_LOG_CATEGORY_EXTERN(LogQinglanGraphics, Log, All);

namespace QinglanGraphics
{
	/** All player-facing text resolves from ST_Game only (code-contracts.md). */
	inline constexpr const TCHAR* GameStringTableId = TEXT("/Game/Qinglan/Data/ST_Game.ST_Game");

	inline FText Loc(const TCHAR* Key)
	{
		return FText::FromStringTable(FName(GameStringTableId), Key);
	}

	// ---------------------------------------------------------------------
	// Feature gate ids accepted by UQinglanGraphicsSettingsService::IsFeatureAllowed.
	// Stable ASCII ids so callers and tests share exact spellings.
	// ---------------------------------------------------------------------
	inline constexpr const TCHAR* FeatureHardwareRayTracing = TEXT("hardware_ray_tracing");
	inline constexpr const TCHAR* FeatureLumen = TEXT("lumen");
	inline constexpr const TCHAR* FeatureNanite = TEXT("nanite");
	inline constexpr const TCHAR* FeatureGpuParticles = TEXT("gpu_particles");
	inline constexpr const TCHAR* FeatureLowFlashMode = TEXT("low_flash_mode");

	// ---------------------------------------------------------------------
	// Preview revert window (docs/06 §4: "a 15-second preview with Revert if
	// the user changes resolution/fullscreen")
	// ---------------------------------------------------------------------
	inline constexpr float PreviewRevertSecondsDefault = 15.f; // docs/06 §4

	// ---------------------------------------------------------------------
	// Quality bounds. UGameUserSettings scalability levels run 0..3
	// (Low/Medium/High/Epic); presets map onto that scale.
	// ---------------------------------------------------------------------
	inline constexpr int32 MinQualityLevel = 0;
	inline constexpr int32 MaxQualityLevel = 3;
	/** User-selectable internal-resolution bounds (docs/06 §4 "resolution scale"). */
	inline constexpr float MinResolutionScale = 0.5f;
	inline constexpr float MaxResolutionScale = 1.f;
	/** Frame rate limit 0 = unlimited (docs/06 §4 "FPS limit"). */
	inline constexpr int32 MinFrameRateLimit = 0;
	inline constexpr int32 MaxFrameRateLimit = 240;

	// ---------------------------------------------------------------------
	// Capability classes (docs/06 §1 reference matrix, §4 first-launch policy)
	// ---------------------------------------------------------------------
	/** Unified memory <= 8 GB is the low-memory stress case: Low preset (docs/06 §1). */
	inline constexpr int64 LowMemoryUnifiedGB = 8;
	/** Base unified tier at 16 GB (M2/M3-class base chip, docs/06 §1). */
	inline constexpr int64 BaseUnifiedGB = 16;
	/**
	 * Unified memory is shared system memory, never dedicated VRAM
	 * (docs/06 §4): effects detail is capped at the 16 GB-unified class.
	 * Larger unified pools stay capped until validated (conservative policy).
	 */
	inline constexpr int32 UnifiedEffectsDetailCap = 2;
	/** Windows baseline system memory (docs/06 §1: gaming laptops, 16 GB RAM). */
	inline constexpr int64 WindowsBaselineSystemGB = 16;

	// ---------------------------------------------------------------------
	// Default frame caps (docs/06 §4: "On Mac, cap the base preset to 30 FPS
	// by default; expose 60 where tested")
	// ---------------------------------------------------------------------
	inline constexpr int32 MacDefaultFrameRateLimit = 30;
	/** Windows ships uncapped; smoothness is enforced by the 60 FPS budget below. */
	inline constexpr int32 WindowsDefaultFrameRateLimit = 0;

	// ---------------------------------------------------------------------
	// Frame budgets, exported for the performance test suite.
	// 60 FPS tier = Windows baseline, 1080p, 16.67 ms frame (docs/06 §5);
	// 30 FPS tier = base Mac, 900p-1080p, 33.33 ms frame (docs/06 §5).
	// Threads overlap; the numbers are parallel ceilings, not a sum.
	// ---------------------------------------------------------------------
	inline constexpr float FrameBudget60Tier_GameThreadMs = 5.f;   // game thread <= 5 ms
	inline constexpr float FrameBudget60Tier_RenderThreadMs = 5.f; // render thread <= 5 ms
	inline constexpr float FrameBudget60Tier_GpuMs = 14.f;         // GPU <= 14 ms
	inline constexpr float FrameBudget60Tier_P95FrameMs = 18.5f;   // benchmark p95 after warmup
	inline constexpr float FrameBudget60Tier_P99FrameMs = 25.f;    // benchmark p99 after warmup
	inline constexpr float FrameBudget30Tier_GameThreadMs = 8.f;   // base Mac game thread <= 8 ms
	inline constexpr float FrameBudget30Tier_GpuMs = 28.f;         // base Mac GPU <= 28 ms
	inline constexpr float FrameBudget30Tier_P95FrameMs = 35.f;
	inline constexpr float FrameBudget30Tier_P99FrameMs = 45.f;
	/** No recurring combat frame-time spike above this in either tier (docs/06 §5). */
	inline constexpr float CombatSpikeCeilingMs = 50.f;
	/** VFX GPU sub-budget inside the totals above (docs/06 §5, docs/04 §4). */
	inline constexpr float VfxGpuBudget60Ms = 2.f;   // <= 2.0 ms within a 16.67 ms frame
	inline constexpr float VfxGpuBudget30Ms = 3.5f;  // <= 3.5 ms within a 33.33 ms frame
	/** Memory ceilings for sustained-load validation (docs/06 §5). */
	inline constexpr int64 ResidentMemoryCeilingGB = 6;     // process resident <= 6 GB on 16 GB base Mac
	inline constexpr float TextureResidencyCeilingGB = 3.5f; // texture residency <= 3.5 GB on 6 GB Windows GPU

	/** Base scalability mix for one preset (docs/06 §1 matrix, §4 conservative defaults). */
	FQinglanGraphicsSettings MakePresetSettings(EQinglanQualityPreset Preset);
	/** Change visual quality without resetting accessibility or display preferences. */
	FQinglanGraphicsSettings MakePresetSettings(EQinglanQualityPreset Preset, const FQinglanGraphicsSettings& Current);

	/** Clamps user input into engine-valid ranges before it reaches UGameUserSettings/cvars. */
	FQinglanGraphicsSettings Sanitize(const FQinglanGraphicsSettings& Settings);

	/**
	 * String-table display key for a preset. The localization set names the
	 * lowest tier "Performance" (docs/06 §1 "Performance/Balanced"), which is
	 * EQinglanQualityPreset::Low here.
	 */
	const TCHAR* PresetDisplayKey(EQinglanQualityPreset Preset);

	/**
	 * String-table key for the short reason shown when an advanced feature is
	 * unavailable (docs/06 §4 "Advanced unavailable features display a short
	 * reason"). Key only; callers resolve via Loc().
	 */
	const TCHAR* FeatureGateReasonKey(FName FeatureId);
}
