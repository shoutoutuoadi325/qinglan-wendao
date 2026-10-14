// Trial director: the timed survival trials on the tribulation arena's storm
// platform (CH03_S02 foundation, CH06_S02 golden core).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "QinglanTrialDirector.generated.h"

class AQinglanEncounterDirector;
class UQinglanEnemyDataAsset;

/**
 * Runs ONE trial at a time: a countdown the player must outlive while waves of
 * ordinary catalog enemies spawn through a dedicated AQinglanEncounterDirector
 * (the "wave engine"). Reusing the engine verbatim buys the expedition systems
 * - portal warnings, pooling, spawn caps, melee/ranged slot arbitration - and
 * is reward-safe: the engine's completion path commits room rewards only
 * inside an open expedition (Run->IsInExpedition() guard), and trials never
 * open one. The director itself owns only the clock, the wave schedule, and
 * the outcome contract:
 * - Success: the player is still alive when the clock runs out ->
 *   UQinglanStorySubsystem::NotifyObjectiveCompleted(ObjectiveId), remaining
 *   enemies swept, music back to Explore.
 * - Failure: the player dies first -> the trial ends silently (the character's
 *   defeat flow already returns to the hub; the anchor re-press restarts).
 *
 * Spawned once per arena world by the trial anchor's proof press
 * (find-or-spawn, following how AQinglanRealmGameMode provisions its
 * AQinglanEncounterDirector in Core/QinglanGameModeBase.cpp). The HUD polls
 * IsTrialActive()/GetRemainingSeconds() every frame (no per-tick messages).
 */
UCLASS()
class AQinglanTrialDirector : public AActor
{
	GENERATED_BODY()

public:
	AQinglanTrialDirector();

	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Begins one timed trial. Returns false when a trial already runs, the
	 * world has no wave engine, or the catalog yielded no archetypes; the
	 * caller then keeps its direct-complete fallback.
	 */
	bool BeginTrial(FName SceneId, FName ObjectiveId, float DurationSeconds, int32 Tier);

	UFUNCTION(BlueprintPure) bool IsTrialActive() const { return bActive; }
	/** Seconds left in the running trial (0 when none); the HUD polls this. */
	UFUNCTION(BlueprintPure) float GetRemainingSeconds() const { return FMath::Max(0.f, RemainingSeconds); }
	UFUNCTION(BlueprintPure) FName GetTrialSceneId() const { return TrialSceneId; }

	/** Wave-engine completion (one wave cleared): schedules the next wave. */
	UFUNCTION() void HandleWaveEngineCompleted();

private:
	/** Finds or spawns the dedicated wave engine bound to this director. */
	AQinglanEncounterDirector* EnsureWaveEngine();
	/** Creates the six catalog archetype assets once per world (round-robin pool). */
	void EnsureCatalogAssets();
	/** Begins the next wave now (size derives from the elapsed reinforcements). */
	void BeginNextWave();
	/** Stops the running trial: sweeps engine enemies, resets the engine. */
	void EndTrial(bool bSuccess);
	/** Destroys every enemy the wave engine owns (living, pending, pooled). */
	void SweepEngineEnemies();

	UPROPERTY() TObjectPtr<AQinglanEncounterDirector> WaveEngine = nullptr;
	/** Cached archetype data assets (QinglanEnemyCatalog::GetAllArchetypes). */
	UPROPERTY() TArray<TObjectPtr<UQinglanEnemyDataAsset>> CatalogAssets;

	bool bActive = false;
	FName TrialSceneId = NAME_None;
	FName TrialObjectiveId = NAME_None;
	int32 TrialTier = 0;
	float RemainingSeconds = 0.f;
	float ElapsedSeconds = 0.f;
	/** Waves begun so far; also salts the engine's per-wave spawn stream. */
	int32 WavesBegun = 0;
	/** Round-robin cursor into CatalogAssets; resets per attempt (determinism). */
	int32 CatalogCursor = 0;
	/** True while a wave's enemies may still be alive (engine mid-encounter). */
	bool bWaveOutstanding = false;
	/** Countdown to the next wave (post-clear breather or reinforcement mark). */
	float NextWaveCountdown = 0.f;
};
