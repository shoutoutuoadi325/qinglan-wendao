#include "QinglanLookDevelopment.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"

namespace QinglanLookDevelopment
{
	void AddLighting(UWorld* World, const FLinearColor& SunTint, float SunIntensity,
		float SunPitchDeg, float SunYawDeg)
	{
		if (!World) { return; }
		const FString MapName = World->GetName();
		const bool bBamboo = MapName.Contains(TEXT("Bamboo"));
		const bool bAshen = MapName.Contains(TEXT("Ashen"));
		const bool bArena = MapName.Contains(TEXT("Arena"));
		FActorSpawnParameters Params;

		// Keep the established scene luminance range so existing spell emission
		// remains legible under fixed exposure. A restrained key/fill ratio
		// separates forms without crushing the shaded side of the player.
		if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(), FVector(0, 0, 3000),
			FRotator(SunPitchDeg, SunYawDeg, 0), Params))
		{
			Sun->SetActorLabel(TEXT("Sun"));
			UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Sun->GetLightComponent());
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensity(SunIntensity * 1.5f);
			Light->SetLightColor(SunTint);
			Light->SetAtmosphereSunLight(true);
			Light->LightSourceAngle = bBamboo ? 2.1f : 1.25f;
			Light->ShadowBias = 0.35f;
			Light->VolumetricScatteringIntensity = bBamboo ? 0.55f : 0.25f;
		}
		if (ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(
			ASkyAtmosphere::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params))
		{
			Atmosphere->SetActorLabel(TEXT("SkyAtmosphere"));
		}
		if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(
			ASkyLight::StaticClass(), FVector(0, 0, 500), FRotator::ZeroRotator, Params))
		{
			Sky->SetActorLabel(TEXT("SkyLight"));
			USkyLightComponent* Light = Sky->GetLightComponent();
			Light->SetMobility(EComponentMobility::Movable);
			Light->SetIntensity(bBamboo ? 1.35f : 1.15f);
			Light->SetRealTimeCaptureEnabled(true);
		}
		if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(
			AExponentialHeightFog::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params))
		{
			Fog->SetActorLabel(TEXT("HeightFog"));
			UExponentialHeightFogComponent* Settings = Fog->GetComponent();
			Settings->FogDensity = bBamboo ? 0.011f : bAshen ? 0.008f : 0.0045f;
			Settings->FogHeightFalloff = 0.22f;
			Settings->StartDistance = 1800.f; // preserve nearby enemy silhouettes
			Settings->FogMaxOpacity = 0.72f;
			Settings->FogInscatteringLuminance = bAshen
				? FLinearColor(0.19f, 0.12f, 0.10f)
				: bBamboo ? FLinearColor(0.20f, 0.27f, 0.25f) : FLinearColor(0.24f, 0.30f, 0.38f);
			// Engine Effects scalability disables volumetrics in lower presets.
			// A bounded distance keeps the optional Metal path inexpensive.
			Settings->bEnableVolumetricFog = true;
			Settings->VolumetricFogDistance = 18000.f;
			Settings->VolumetricFogStartDistance = 1800.f;
			Settings->VolumetricFogNearFadeInDistance = 800.f;
			Settings->VolumetricFogScatteringDistribution = 0.35f;
			Settings->VolumetricFogExtinctionScale = 0.6f;
		}
		if (APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params))
		{
			Volume->SetActorLabel(TEXT("Qinglan_FixedLook"));
			Volume->bUnbound = true;
			Volume->Priority = 1.f;
			FPostProcessSettings& Look = Volume->Settings;
			const float ExposureEV100 = bBamboo ? 0.8f : bArena ? 0.6f : 1.2f;
			Look.bOverride_AutoExposureMinBrightness = true;
			Look.bOverride_AutoExposureMaxBrightness = true;
			Look.AutoExposureMinBrightness = ExposureEV100;
			Look.AutoExposureMaxBrightness = ExposureEV100;
			Look.bOverride_AutoExposureBias = true;
			Look.AutoExposureBias = 0.3f;
			Look.bOverride_BloomIntensity = true;
			Look.BloomIntensity = 0.18f;
			Look.bOverride_BloomThreshold = true;
			Look.BloomThreshold = 3.f;
			Look.bOverride_ColorSaturation = true;
			Look.ColorSaturation = FVector4(0.94f, 0.94f, 0.94f, 1.f);
			Look.bOverride_ColorContrast = true;
			Look.ColorContrast = FVector4(1.035f, 1.035f, 1.035f, 1.f);
			Look.bOverride_VignetteIntensity = true;
			Look.VignetteIntensity = 0.12f;
			Look.bOverride_MotionBlurAmount = true;
			Look.MotionBlurAmount = 0.08f;
			Look.bOverride_DepthOfFieldFstop = true;
			Look.DepthOfFieldFstop = 32.f; // retain focus throughout gameplay
		}
	}
}
