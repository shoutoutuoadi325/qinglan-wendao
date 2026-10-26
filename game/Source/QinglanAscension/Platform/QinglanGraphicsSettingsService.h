// Graphics settings service: capability gating, presets, persistence.
// Graphics settings are platform-local and never transferred with progression.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "QinglanGraphicsSettingsService.generated.h"

UENUM(BlueprintType)
enum class EQinglanQualityPreset : uint8
{
	Low,
	Balanced,
	High,
	Ultra
};

/** Struct-of-settings persisted to platform-local GameUserSettings. */
USTRUCT(BlueprintType)
struct FQinglanGraphicsSettings
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadWrite) EQinglanQualityPreset Preset = EQinglanQualityPreset::Balanced;
	UPROPERTY(BlueprintReadWrite) int32 TextureQuality = 2;
	UPROPERTY(BlueprintReadWrite) int32 ShadowQuality = 2;
	UPROPERTY(BlueprintReadWrite) int32 EffectsDetail = 2;
	UPROPERTY(BlueprintReadWrite) int32 ViewDistance = 2;
	UPROPERTY(BlueprintReadWrite) float ResolutionScale = 1.f;
	UPROPERTY(BlueprintReadWrite) bool bVSync = false;
	UPROPERTY(BlueprintReadWrite) int32 FrameRateLimit = 0;
	UPROPERTY(BlueprintReadWrite) bool bLowFlashMode = false;
	UPROPERTY(BlueprintReadWrite) bool bDynamicResolution = true;
};

/**
 * Reads adapter/backend/memory via engine platform APIs on first launch,
 * selects a conservative preset, applies it before the first heavy scene,
 * and offers preview+revert for resolution/fullscreen changes.
 */
UCLASS()
class UQinglanGraphicsSettingsService : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable) void ApplySettings(const FQinglanGraphicsSettings& NewSettings);
	UFUNCTION(BlueprintPure) FQinglanGraphicsSettings GetCurrentSettings() const;
	UFUNCTION(BlueprintPure) FText GetActiveAdapterName() const;
	UFUNCTION(BlueprintPure) FName GetActiveRHIName() const;
	/** Conservative initial preset chosen from detected capability class. */
	UFUNCTION(BlueprintCallable) FQinglanGraphicsSettings DetectInitialSettings() const;
	/** 15-second preview window with automatic revert. */
	UFUNCTION(BlueprintCallable) void BeginPreviewRevertWindow(float Seconds);
	UFUNCTION(BlueprintCallable) void CancelPreviewRevert();

	/** Low quality must retain spell telegraphs and impact silhouettes. */
	UFUNCTION(BlueprintPure) bool IsFeatureAllowed(FName FeatureId) const;
};
