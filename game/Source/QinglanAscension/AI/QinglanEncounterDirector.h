// Encounter director: wave activation, completion, rewards, pooling, portals.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "QinglanEncounterDirector.generated.h"

class AQinglanEnemyCharacter;
class UQinglanEnemyDataAsset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanEncounterCompleted);

/**
 * One room's encounter. Spawns waves through portals (0.8 s warning, never
 * within 4 m of the player), enforces <=8 ordinary enemies or 1 boss + <=2 adds,
 * limits simultaneous melee windups to 3, and opens the door exactly once when
 * all living encounter enemies die.
 */
UCLASS()
class AQinglanEncounterDirector : public AActor
{
	GENERATED_BODY()

public:
	AQinglanEncounterDirector();

	virtual void Tick(float DeltaTime) override;

	/** Begins the encounter for the current room (wave list from the realm template). */
	void BeginEncounter(const TArray<UQinglanEnemyDataAsset*>& Wave, int32 RoomIndex, bool bIsBossRoom);
	/** Realm floor center; confines portal placements to this room. Set before BeginEncounter. */
	void SetRoomOrigin(const FVector& Origin);
	/** Simultaneous last-kill opens the door + reward exactly once. */
	UFUNCTION(BlueprintCallable) void NotifyEnemyDied(AQinglanEnemyCharacter* Enemy);
	UFUNCTION(BlueprintPure) bool IsEncounterActive() const { return bActive; }
	UFUNCTION(BlueprintPure) int32 GetLivingEnemyCount() const;

	/** Ranged attacks and melee windups share separate budgets. */
	bool RequestMeleeWindupSlot();
	bool RequestRangedAttackSlot();
	void ReleaseAttackSlots();

	UPROPERTY(BlueprintAssignable) FQinglanEncounterCompleted OnEncounterCompleted;

private:
	void SpawnWave();
	/** Portal VFX + 0.8 s warning before the enemy materializes. */
	void SpawnThroughPortal(UQinglanEnemyDataAsset* Data, const FVector& Location);

	UPROPERTY() TArray<TWeakObjectPtr<AQinglanEnemyCharacter>> LivingEnemies;
	UPROPERTY() TArray<TWeakObjectPtr<AQinglanEnemyCharacter>> PendingSpawns;
	int32 CurrentRoomIndex = -1;
	bool bBossRoom = false;
	bool bActive = false;
	bool bCompleted = false;
	int32 MeleeWindupCount = 0;
	int32 RangedActiveCount = 0;
	float PortalWarningDuration = 0.8f;
	float SpawnTimer = 0.f;
};

/**
 * Overrides one director's portal spawn ring around its anchor (cm). The
 * expedition default stays the authored 600-1200 cm band (docs/01 section 6);
 * the trial director widens its dedicated wave engine to the storm platform's
 * outer band. Free function instead of a member because the scaffold header
 * is frozen (see the FIXME-CONTRACT in the .cpp); same-module callers only.
 */
void SetEncounterSpawnRingOverride(AQinglanEncounterDirector* Director, float MinCm, float MaxCm);
