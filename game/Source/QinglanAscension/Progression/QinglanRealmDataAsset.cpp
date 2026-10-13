// Realm data asset accessors. The class mirrors data/examples/realm.json
// (blueprint section 3: rooms[]{index, kind, spirit_shards}); instances are
// populated by the data pipeline, or synthesized by the run subsystem for the
// known realm ids when cooked assets are unavailable (headless automation).
#include "Narrative/QinglanStoryDataTypes.h"

#include "QinglanAscension.h"

EQinglanRoomKind UQinglanRealmDataAsset::GetRoomKind(int32 RoomIndex) const
{
	if (Rooms.IsValidIndex(RoomIndex))
	{
		return Rooms[RoomIndex].Kind;
	}
	// Out-of-range rooms fall back to the safest kind: Entrance has no enemies
	// and no rewards, so a bad index can never spawn combat or mint shards.
	UE_LOG(LogQinglan, Warning,
		TEXT("GetRoomKind: realm '%s' has no room %d (%d rooms); returning Entrance"),
		*RealmId.ToString(), RoomIndex, Rooms.Num());
	return EQinglanRoomKind::Entrance;
}

int32 UQinglanRealmDataAsset::GetRoomShards(int32 RoomIndex) const
{
	if (Rooms.IsValidIndex(RoomIndex))
	{
		return Rooms[RoomIndex].SpiritShards;
	}
	// realm.json gives every room an explicit spirit_shards value; an invalid
	// room has no allocation, so it awards nothing.
	UE_LOG(LogQinglan, Warning,
		TEXT("GetRoomShards: realm '%s' has no room %d (%d rooms); returning 0"),
		*RealmId.ToString(), RoomIndex, Rooms.Num());
	return 0;
}
