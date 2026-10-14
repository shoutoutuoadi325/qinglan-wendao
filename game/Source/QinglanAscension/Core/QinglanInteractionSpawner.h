// Interaction spawner world subsystem: turns bootstrap map anchors into
// AQinglanInteractable actors at world BeginPlay.
//
// Content bootstrap (game/Source/QinglanAscensionEditor/QinglanContentBootstrap.cpp
// BootstrapMaps -> SpawnAnchor) places ATargetPoint anchors whose object name
// and actor label both carry the interaction key; this subsystem mirrors the
// gameplay-relevant subset into interactables (docs/dev/level-layout.md):
//   - "record.<map>.<place>" lore records
//   - "sq.<realm>.<place>"   side-quest safe-stage stations
//   - "hub.<place>"          sanctuary scenes, gates, shrine, shop
//   - "bamboo.room_*"        realm chapter scenes + the safe-room exit
//   - "ashen.boss_5"         ashen exit station
//   - "trial.*"              arena trial/decision/preparation stations
// plus two synthesized families the bootstrap does not author:
//   - hub realm gates ("hub.ashen_gate"/"hub.trial_gate") beside hub.bamboo_gate
//   - realm room doors at each corridor mouth (ids "door.<from>_<to>")
//
// UWorldSubsystem::OnWorldBeginPlay runs before AGameModeBase::StartPlay
// (Engine/Private/World.cpp UWorld::BeginPlay), so interactables exist before
// the realm entry scene begins and before the first pawn tick.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "QinglanInteractionSpawner.generated.h"

/**
 * Spawns AQinglanInteractable actors for gameplay-relevant map anchors.
 * Game worlds only; every step degrades to logs (code-contracts.md).
 */
UCLASS()
class UQinglanInteractionSpawner : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	/** True when the anchor key carries a gameplay behavior and should get an interactable. */
	static bool ShouldSpawnForAnchor(const FName Key);
	/** Spawns one interactable at an anchor position; returns it (null on failure). */
	class AQinglanInteractable* SpawnInteractableAt(UWorld* World, const FName Key, const FVector& Location);
	/** Hub-only: synthesizes the ashen/trial gate anchors beside hub.bamboo_gate when absent. */
	void SynthesizeHubGateAnchors(UWorld* World, const TMap<FName, FVector>& AnchorPositions);
	/** Realm-only: one door interactable at each corridor mouth (rooms 0..5 -> 1..6). */
	void SynthesizeRealmDoors(UWorld* World);
};
