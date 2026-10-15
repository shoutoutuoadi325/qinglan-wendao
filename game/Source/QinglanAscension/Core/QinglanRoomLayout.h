// Shared greybox expedition geometry used by generated maps and runtime resume.
#pragma once

#include "CoreMinimal.h"

namespace QinglanRoomLayout
{
	inline constexpr int32 RoomCount = 7;
	inline constexpr float RoomSpacingCm = 5500.f;
	inline constexpr float RoomHalfExtentCm = 2250.f;
	inline constexpr float EntranceHeightCm = 150.f;
	inline constexpr float PortalHeightCm = 120.f;
	/** Leave space for enemy capsules inside the authored 45 m square floors. */
	inline constexpr float PortalHalfExtentCm = RoomHalfExtentCm - 125.f;

	inline FVector RoomCenter(int32 RoomIndex)
	{
		return FVector(RoomIndex * RoomSpacingCm, 0.f, 0.f);
	}

	inline FVector RoomEntrance(int32 RoomIndex)
	{
		return RoomCenter(RoomIndex) + FVector(0.f, 0.f, EntranceHeightCm);
	}

	inline FName EntranceTag(int32 RoomIndex)
	{
		return FName(*FString::Printf(TEXT("Qinglan.RoomEntrance.%d"), RoomIndex));
	}
}
