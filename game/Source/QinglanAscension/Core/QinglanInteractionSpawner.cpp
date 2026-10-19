// Interaction spawner implementation.
//
// Anchor-name resolution: the bootstrap's SpawnAnchor names both the object
// (MakeUniqueObjectName) and the actor label (SetActorLabel) with the same
// key, so the object name is authoritative at runtime and the label is a
// development-build fallback.
//
// Realm door positions follow the layout contract the bootstrap authors
// (rooms spaced 55 m along +X, corridors centered between rooms); the exit
// room (6) has no door because the exit station banks and travels instead.
#include "Core/QinglanInteractionSpawner.h"
#include "Core/QinglanRoomLayout.h"

#include "Core/QinglanInteractable.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "QinglanAscension.h"

namespace
{
	/** Room layout contract (QinglanContentBootstrap.cpp BootstrapMaps). */
	constexpr int32 QinglanRealmRoomCount = 7;
	constexpr float QinglanDoorForwardOffset = 500.f; // corridor mouth, just inside the current room
	constexpr float QinglanDoorHeight = 150.f;

	/** Synthesized hub gates sit beside the bamboo gate (level-layout.md "expedition departure gate"). */
	constexpr float QinglanGateSpacing = 900.f;

	/** Anchor key -> interact, keyed by prefix/exact id (docs/dev/level-layout.md). */
	bool IsRealmSceneOrExitAnchor(const FName Key)
	{
		const FString Id = Key.ToString();
		// Bamboo chapter scenes + the safe-room exit; ashen exits at the boss room.
		return Id == TEXT("bamboo.room_0")
			|| Id == TEXT("bamboo.room_1_2_guided")
			|| Id == TEXT("bamboo.rooms_1_to_5")
			|| Id == TEXT("bamboo.room_5_safe")
			|| Id == TEXT("ashen.boss_5");
	}

	/**
	 * Bootstrap anchor name: object name first (shipping-safe), actor label as
	 * a development fallback. The engine uniquifies actor names on spawn, so
	 * every authored anchor lands as "<Key>_<n>" (e.g. hub.bamboo_gate_0);
	 * the numeric suffix must be stripped or interaction ids and every
	 * exact-key lookup (authored gates, scene anchors) miss the authored key.
	 */
	FName AnchorKeyOf(const ATargetPoint* Point)
	{
		if (Point == nullptr)
		{
			return NAME_None;
		}
		FString Name = Point->GetName();
		int32 DigitStart = Name.Len();
		while (DigitStart > 0 && FChar::IsDigit(Name[DigitStart - 1]))
		{
			--DigitStart;
		}
		if (DigitStart < Name.Len() && DigitStart > 1 && Name[DigitStart - 1] == TEXT('_'))
		{
			Name = Name.Left(DigitStart - 1);
		}
		return FName(*Name);
	}
} // namespace

bool UQinglanInteractionSpawner::ShouldCreateSubsystem(UObject* Outer) const
{
	// Game worlds only: editor/commandlet worlds never spawn gameplay actors.
	const UWorld* World = Cast<UWorld>(Outer);
	return World != nullptr && World->IsGameWorld();
}

void UQinglanInteractionSpawner::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	TMap<FName, FVector> AnchorPositions;
	int32 Spawned = 0;
	for (TActorIterator<ATargetPoint> It(World); It; ++It)
	{
		const FName Key = AnchorKeyOf(*It);
		if (Key == NAME_None || AnchorPositions.Contains(Key))
		{
			continue;
		}
		AnchorPositions.Add(Key, It->GetActorLocation());
		if (ShouldSpawnForAnchor(Key) && SpawnInteractableAt(World, Key, It->GetActorLocation()) != nullptr)
		{
			++Spawned;
		}
	}

	const FString MapName = World->GetMapName();
	if (MapName.Contains(TEXT("Bamboo")) || MapName.Contains(TEXT("Ashen")))
	{
		SynthesizeRealmDoors(World);
	}
	else if (MapName.Contains(TEXT("Hub")))
	{
		// The hub is the only gate map (docs/dev/level-layout.md "Maps" table);
		// the dev-only benchmark map and others stay untouched.
		SynthesizeHubGateAnchors(World, AnchorPositions);
		SynthesizeHubRestAnchor(World, AnchorPositions);
		SynthesizeHubSceneAnchors(World, AnchorPositions);
	}

	UE_LOG(LogQinglan, Log, TEXT("Interaction spawner: %d anchors found (%d spawned) in %s"),
		AnchorPositions.Num(), Spawned, *MapName);
}

bool UQinglanInteractionSpawner::ShouldSpawnForAnchor(const FName Key)
{
	const FString Id = Key.ToString();
	// Records, side-quest stations, sanctuary scenes/gates/shrine/shop, trial
	// stations, and the realm scene/exit anchors above. Pure geometry markers
	// (corridor/floor/dressing names, untagged PlayerStarts) never match.
	return Id.StartsWith(TEXT("record."))
		|| Id.StartsWith(TEXT("sq."))
		|| Id.StartsWith(TEXT("hub."))
		|| Id.StartsWith(TEXT("trial."))
		|| IsRealmSceneOrExitAnchor(Key);
}

AQinglanInteractable* UQinglanInteractionSpawner::SpawnInteractableAt(UWorld* World, const FName Key, const FVector& Location)
{
	if (World == nullptr || Key == NAME_None)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Name = *FString::Printf(TEXT("QinglanInteractable_%s"), *Key.ToString());
	Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested; // keys are unique per map
	AQinglanInteractable* Interactable = World->SpawnActor<AQinglanInteractable>(
		AQinglanInteractable::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (!Interactable)
	{
		UE_LOG(LogQinglan, Warning, TEXT("Interaction spawner: failed to spawn %s"), *Key.ToString());
		return nullptr;
	}
	Interactable->InteractionId = Key;
	Interactable->DisplayNameKey = *FString::Printf(TEXT("interact.%s"), *Key.ToString());
	return Interactable;
}

void UQinglanInteractionSpawner::SynthesizeHubGateAnchors(UWorld* World, const TMap<FName, FVector>& AnchorPositions)
{
	// The bootstrap authors only hub.bamboo_gate; the ashen/trial gates reuse
	// its position +/- the gate spacing so later realms stay reachable
	// (task contract: "add hub.ashen_gate ... for ashen + arena").
	const FVector* BambooGate = AnchorPositions.Find(FName(TEXT("hub.bamboo_gate")));
	const FVector Base = BambooGate ? *BambooGate : FVector(0.f, -4800.f, 500.f);

	struct FGateSpec { FName Id; float Offset; };
	const FGateSpec Gates[] = {
		{ FName(TEXT("hub.ashen_gate")), QinglanGateSpacing },
		{ FName(TEXT("hub.trial_gate")), -QinglanGateSpacing },
	};
	for (const FGateSpec& Gate : Gates)
	{
		if (AnchorPositions.Contains(Gate.Id))
		{
			continue; // authored anchor wins over synthesis
		}
		SpawnInteractableAt(World, Gate.Id, Base + FVector(Gate.Offset, 0.f, 0.f));
	}
}

void UQinglanInteractionSpawner::SynthesizeHubRestAnchor(UWorld* World, const TMap<FName, FVector>& AnchorPositions)
{
	// The sanctuary rest station (safe hub time advance for courier replies)
	// sits beside the courier board so the round trip is one walk apart
	// (level-layout.md hub intent; task contract "rest at the sanctuary").
	const FName RestStationId(TEXT("hub.rest_station"));
	if (AnchorPositions.Contains(RestStationId))
	{
		return; // authored anchor wins over synthesis
	}
	const FVector* CourierBoard = AnchorPositions.Find(FName(TEXT("hub.courier_board")));
	if (CourierBoard == nullptr)
	{
		UE_LOG(LogQinglan, Verbose, TEXT("Interaction spawner: no hub.courier_board to place the rest station beside"));
		return;
	}
	SpawnInteractableAt(World, RestStationId, *CourierBoard + FVector(600.f, 0.f, 0.f));
}

void UQinglanInteractionSpawner::SynthesizeHubSceneAnchors(UWorld* World, const TMap<FName, FVector>& AnchorPositions)
{
	// AUD-10: CH05_S03 and the CH07 epilogue scenes route through two ledger/
	// correspondence stations added to the bootstrap's HubAnchors. Old maps
	// regenerated before that change still need a usable station, so derive
	// them from nearby authored furniture (the shrine and archive table).
	struct FDerivedSpec { FName Id; FName From; FVector Offset; };
	const FDerivedSpec Derived[] = {
		{ FName(TEXT("hub.ledger_table")), FName(TEXT("hub.shrine")), FVector(-600.f, -1400.f, 0.f) },
		{ FName(TEXT("hub.correspondence_table")), FName(TEXT("hub.archive_table")), FVector(1600.f, 800.f, 0.f) },
	};
	for (const FDerivedSpec& Spec : Derived)
	{
		if (AnchorPositions.Contains(Spec.Id))
		{
			continue; // authored anchor wins over synthesis
		}
		const FVector* From = AnchorPositions.Find(Spec.From);
		if (From == nullptr)
		{
			UE_LOG(LogQinglan, Verbose, TEXT("Interaction spawner: no %s to derive %s from"),
				*Spec.From.ToString(), *Spec.Id.ToString());
			continue;
		}
		SpawnInteractableAt(World, Spec.Id, *From + Spec.Offset);
	}
}

void UQinglanInteractionSpawner::SynthesizeRealmDoors(UWorld* World)
{
	// One door per corridor mouth: room i -> i+1 for i in 0..5; the exit room
	// (6) is reached through room 5 and leaves via the exit station.
	for (int32 FromRoom = 0; FromRoom < QinglanRealmRoomCount - 1; ++FromRoom)
	{
		const FName DoorId(*FString::Printf(TEXT("door.%d_%d"), FromRoom, FromRoom + 1));
		// Corridor center, nudged back into the current room so the door is
		// reachable before the corridor geometry.
		const FVector Location(
			FromRoom * QinglanRoomLayout::RoomSpacingCm + QinglanRoomLayout::RoomSpacingCm * 0.5f - QinglanDoorForwardOffset,
			0.f, QinglanDoorHeight);
		SpawnInteractableAt(World, DoorId, Location);
	}
}
