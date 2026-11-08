#pragma once

#include "CoreMinimal.h"

class UWorld;

// Fixed-time look development shared by the generated maps. Gameplay,
// collision and navigation remain authored by the content bootstrap.
namespace QinglanLookDevelopment
{
	void AddLighting(UWorld* World, const FLinearColor& SunTint, float SunIntensity,
		float SunPitchDeg, float SunYawDeg);
}
