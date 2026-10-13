// Room layout contract for level tooling: the world-space bounds query API
// for authoring the 7-room realm layouts. Pure functions over the documented
// room contract; nothing here touches runtime expedition state.
//
// Sources:
//   - docs/01-game-design.md section 5: "Modular room footprints: 35-55
//     meters across, at least two unobstructed dodge lanes, no damaging
//     bottomless pits."
//   - docs/01-game-design.md section 5 "Seven-room layout" (index/kind table).
//   - docs/dev/implementation-blueprint.md section 1.4: "Rooms 35-55 m
//     across, >=2 unobstructed dodge lanes, no damaging pits" and the
//     entrance->exit path validation requirement.
//
// Level designers (and the editor module) place one room volume per index on
// a straight line along +X; the query returns the center/half-extents each
// room volume should cover so alcoves, door interactables, and navigation
// validation can be authored against identical numbers at every quality tier.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "QinglanRoomLayout.generated.h"

/** Axis-aligned bounds of one room volume (center + half extents, cm). */
USTRUCT(BlueprintType)
struct FQinglanRoomBounds
{
	GENERATED_BODY()

	/** Room index in the realm contract (0 = entrance); INDEX_NONE when invalid. */
	UPROPERTY(BlueprintReadOnly) int32 RoomIndex = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FVector Center = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly) FVector HalfExtents = FVector(2250.0, 2250.0, 400.0);
	UPROPERTY(BlueprintReadOnly) bool bIsValid = false;
};

/**
 * Static room-layout queries for the realm level tooling and automation
 * (deterministic, headless-safe: pure math, no assets, no RNG).
 */
UCLASS()
class UQinglanRoomLayoutLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** docs/01 section 5: rooms are 35 m across at minimum. */
	static constexpr float MinRoomAcrossCm = 3500.f;
	/** docs/01 section 5: rooms are 55 m across at maximum. */
	static constexpr float MaxRoomAcrossCm = 5500.f;
	/** Authored default footprint (midpoint of the documented range). */
	static constexpr float DefaultRoomAcrossCm = 4500.f;
	/** docs/01 section 5: "at least two unobstructed dodge lanes." */
	static constexpr int32 MinDodgeLanes = 2;
	/** docs/01 section 5: "no damaging bottomless pits" (count must not exceed). */
	static constexpr int32 MaxDamagingPitCount = 0;
	/**
	 * Authored gap between adjacent room volumes (corridor/door band). The
	 * docs fix no corridor width; this default keeps every door band inside
	 * the parent rooms' footprint tolerance for the nav-validation gate.
	 */
	static constexpr float RoomGapCm = 1500.f;
	/** Authored room volume height (docs specify only the across measurement). */
	static constexpr float RoomHeightCm = 800.f;

	/** Rooms in the full 7-room contract (blueprint section 1.4). */
	static constexpr int32 FullRealmRoomCount = 7;

	/**
	 * Bounds of one room volume. Rooms are placed on a straight line along +X:
	 * Center = LayoutOrigin + (RoomIndex * Pitch, 0, 0) where
	 * Pitch = clamped room size + RoomGapCm. Invalid indices (or RoomCount
	 * <= 0) return an invalid bounds struct.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static FQinglanRoomBounds GetRoomBounds(int32 RoomIndex, int32 RoomCount, const FVector& LayoutOrigin, float RoomAcrossCm)
	{
		FQinglanRoomBounds Bounds;
		if (RoomCount <= 0 || RoomIndex < 0 || RoomIndex >= RoomCount)
		{
			return Bounds; // bIsValid stays false
		}
		const double Size = static_cast<double>(GetClampedRoomSize(RoomAcrossCm));
		const double Pitch = Size + static_cast<double>(RoomGapCm);
		Bounds.RoomIndex = RoomIndex;
		Bounds.bIsValid = true;
		Bounds.Center = LayoutOrigin + FVector(static_cast<double>(Pitch) * static_cast<double>(RoomIndex), 0.0, 0.0);
		Bounds.HalfExtents = FVector(Size * 0.5, Size * 0.5, static_cast<double>(RoomHeightCm) * 0.5);
		return Bounds;
	}

	/** True when the across measurement is inside the documented 35-55 m range. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static bool IsRoomSizeWithinBounds(float RoomAcrossCm)
	{
		return RoomAcrossCm >= MinRoomAcrossCm && RoomAcrossCm <= MaxRoomAcrossCm;
	}

	/** Clamps an authored room size into the documented range. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static float GetClampedRoomSize(float RoomAcrossCm)
	{
		return FMath::Clamp(RoomAcrossCm, MinRoomAcrossCm, MaxRoomAcrossCm);
	}

	/** The documented (min, max) room-across range in cm. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static FVector2D GetRoomSizeRangeCm() { return FVector2D(MinRoomAcrossCm, MaxRoomAcrossCm); }

	/** Minimum unobstructed dodge lanes every room must ship with (docs/01 section 5). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static int32 GetMinDodgeLaneCount() { return MinDodgeLanes; }

	/** Maximum damaging pits allowed per room; zero by contract (docs/01 section 5). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static int32 GetMaxDamagingPitCount() { return MaxDamagingPitCount; }

	/** Default room size (cm) for tooling that authors from the midpoint. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|LevelTooling")
	static float GetDefaultRoomAcrossCm() { return DefaultRoomAcrossCm; }
};
