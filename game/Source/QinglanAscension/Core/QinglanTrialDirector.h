// Trial director: the phased survival trials on the tribulation arena's storm
// platform (CH03_S02 foundation, CH06_S02 golden core).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Math/RandomStream.h"
#include "QinglanTrialDirector.generated.h"

class AQinglanCharacter;
class AQinglanEncounterDirector;
class AQinglanEnemyCharacter;
class AQinglanTrialDirector;
class UQinglanEnemyDataAsset;

/** One timed trial's macro phases (AUD-20). */
UENUM(BlueprintType)
enum class EQinglanTrialPhase : uint8
{
	/** No attempt is running (never started, or already resolved). */
	Idle,
	/** Timed survival under the storm: countdown, ordinary waves, lightning. */
	Storm,
	/** Clock expired; the required elite must die before any proof commits. */
	RequiredKill,
	/** The required kill landed; the success contract resolves on the next Tick. */
	Victory
};

/** One telegraphed-but-unlanded storm strike (plain struct: no UObject refs). */
struct FQinglanPendingStrike
{
	FVector Center = FVector::ZeroVector;
	float RadiusCm = 0.f;
	/** Telegraph time left before the impact check (>= 0.9 s at enqueue). */
	float WarnRemaining = 0.f;
};

/**
 * Storm-hazard placement contract (AUD-20). Free functions + exported
 * constants so the editor automation specs can verify the safety geometry
 * headlessly (same linkage-only pattern as SetEncounterSpawnRingOverride for
 * the encounter director).
 */
namespace QinglanTrialStorm
{
	/** Never more than this many strike outlines alive on the field at once. */
	inline constexpr int32 MaxSimultaneousStrikes = 4;
	/** Strike impact radius (cm). */
	inline constexpr float StrikeRadiusCm = 300.f;
	/** Telegraph duration before impact (trial contract: >= 0.9 s warning). */
	inline constexpr float StrikeWarnSeconds = 1.0f;
	/** Strike centers sit at most this far from the player's current spot (cm). */
	inline constexpr float StrikeMaxOffsetCm = 1200.f;
	/** Any two simultaneously-alive strike centers keep at least this gap (cm). */
	inline constexpr float StrikeMinSeparationCm = 900.f;
	/** Deterministic candidate draws per strike (best-of-N keeps clearances honest). */
	inline constexpr int32 StrikePlacementAttempts = 8;

	/**
	 * Deterministically places one volley of strike centers around Anchor.
	 * Same stream + same inputs -> same volley (house determinism: draws come
	 * only from Stream.FRand()). Each strike draws StrikePlacementAttempts
	 * candidates and keeps the one farthest from every already-placed /
	 * avoided center; a strike whose best candidate still cannot clear
	 * MinSeparationCm is dropped, never placed on top of a live outline. The
	 * returned count is capped so AvoidCenters + volley <= MaxSimultaneousStrikes.
	 */
	QINGLANASCENSION_API TArray<FVector> PickStrikeVolley(FRandomStream& Stream, const FVector& Anchor,
		int32 Count, float MinSeparationCm, const TArray<FVector>& AvoidCenters);
}

/** Fired when a volley of strike outlines appears (HUD/presentation hook). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FQinglanTrialHazardTelegraph,
	AQinglanTrialDirector*, Director, const TArray<FVector>&, StrikeCenters,
	float, StrikeRadiusCm, float, WarnSeconds);

/**
 * Runs ONE trial at a time as a two-phase survival contract (AUD-20):
 *
 * - Storm phase: the authored countdown (90 s Foundation / 150 s Golden Core)
 *   with ordinary catalog waves trickling through a dedicated
 *   AQinglanEncounterDirector (the "wave engine" — reusing it verbatim buys the
 *   expedition systems: portal warnings, pooling, spawn caps, melee/ranged slot
 *   arbitration; it stays reward-safe because its completion path commits room
 *   rewards only inside an open expedition, and trials never open one).
 *   The director additionally schedules storm lightning: Foundation fields
 *   three warning waves inside authored windows; Golden Core fires continuous
 *   volleys across three 50 s storm phases. Every strike telegraphs a
 *   persistent ground outline (OnTrialHazardTelegraph) for >= 0.9 s before its
 *   single distance-checked impact.
 *
 * - RequiredKill phase: surviving the clock alone grants nothing. The trial
 *   sweeps the ordinary wave and the engine spawns the required elite (the
 *   elite construct for Foundation, the Broken Guardian shell for Golden
 *   Core) in boss-room mode. Only its death — observed alive first — resolves
 *   the attempt into the success contract: scoped scene objective proof +
 *   permanent breakthrough, enemies swept, music back to Explore; Foundation
 *   returns to the hub. A boss that never materialized fails the attempt with
 *   the trial.spawn_failed message instead.
 *
 * - Failure: the player dying still ends the trial silently (the character's
 *   defeat flow already returns to the hub; the anchor re-press restarts the
 *   clock — "failure restarts the encounter timer only").
 *
 * Story beats route through BroadcastTrialEvent (QinglanStoryRuntime::
 * NotifyGameplayEvent): the JSON script's trigger-gated lines name
 * storm_time_0 / first_lightning_telegraph / storm_time_50 / storm_time_100 /
 * storm_survived_150 / shell_combat_started / first_shell_recovery_window /
 * shell_defeated, and Foundation's reserved trial.lightning_wave_N beats.
 * GetTrialEventLog exposes the in-order log for HUD/QA and automation specs.
 *
 * Spawned once per arena world by the trial anchor's proof press
 * (find-or-spawn, following how AQinglanRealmGameMode provisions its
 * AQinglanEncounterDirector in Core/QinglanGameModeBase.cpp). The HUD polls
 * IsTimedSurvivalActive()/GetRemainingSeconds() every frame (no per-tick
 * messages); the countdown intentionally closes when the clock does, so the
 * boss fight reads as its own beat.
 */
UCLASS()
class QINGLANASCENSION_API AQinglanTrialDirector : public AActor
	// QINGLANASCENSION_API: linkage-only export so the editor automation specs
	// can drive BeginTrial/Tick/phase queries from the modular editor build;
	// public API unchanged.
{
	GENERATED_BODY()

public:
	AQinglanTrialDirector();

	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Begins one timed trial. Returns false when a trial already runs, the
	 * admission/story prerequisites fail, the world has no wave engine, or the
	 * catalog yielded no archetypes. Rejection never grants objective proof.
	 */
	bool BeginTrial(FName SceneId, FName ObjectiveId, float DurationSeconds, int32 Tier);
	/** Ordinary retreat action: discard this attempt, keep permanent progression, save and return. */
	UFUNCTION(BlueprintCallable) bool CancelTrialAndReturnToHub();

	UFUNCTION(BlueprintPure) bool IsTrialActive() const { return bActive; }
	/** True only while the timed survival clock itself runs (Storm phase). */
	UFUNCTION(BlueprintPure) bool IsTimedSurvivalActive() const
	{
		return bActive && Phase == EQinglanTrialPhase::Storm;
	}
	/** Seconds left in the running trial (0 when none); the HUD polls this. */
	UFUNCTION(BlueprintPure) float GetRemainingSeconds() const { return FMath::Max(0.f, RemainingSeconds); }
	UFUNCTION(BlueprintPure) FName GetTrialSceneId() const { return TrialSceneId; }
	/** Current macro phase of the last/running attempt (Idle once resolved). */
	UFUNCTION(BlueprintPure) EQinglanTrialPhase GetTrialPhase() const { return Phase; }
	/** In-order story beats broadcast this attempt (reset by BeginTrial). */
	UFUNCTION(BlueprintPure) void GetTrialEventLog(TArray<FName>& OutEvents) const { OutEvents = TrialEventLog; }
	/** The required elite/boss once sighted alive in the required-kill phase. */
	UFUNCTION(BlueprintPure) AActor* GetRequiredTarget() const;

	/** A volley of strike outlines just appeared (persistent telegraphs). */
	UPROPERTY(BlueprintAssignable) FQinglanTrialHazardTelegraph OnTrialHazardTelegraph;

	/** Wave-engine completion: schedules the next wave or resolves the boss kill. */
	UFUNCTION() void HandleWaveEngineCompleted();

private:
	/** Finds or spawns the dedicated wave engine bound to this director. */
	AQinglanEncounterDirector* EnsureWaveEngine();
	/** Creates the six catalog archetype assets once per world (round-robin pool). */
	void EnsureCatalogAssets();
	/** Begins the next ordinary wave now (size derives from the elapsed reinforcements). */
	void BeginNextWave();
	/** Clock expired: sweep the wave and field the required elite (boss room). */
	void EnterRequiredKillPhase();
	/** Storm cadence: phase-boundary beats + volley enqueueing (reads the clock). */
	void UpdateStormSchedule(float DeltaTime);
	/** Advances/resolves telegraphed strikes into their single impact checks. */
	void UpdatePendingStrikes(float DeltaTime);
	/** Tracks the required boss: first sighting, shell beats, recovery window. */
	void UpdateRequiredTarget();
	/** One strike volley around the player (deterministic placement + cues). */
	void EnqueueStrikeVolley(AQinglanCharacter* Player);
	/** Appends to the event log and reaches the story runtime's trigger gate. */
	void BroadcastTrialEvent(FName Event);
	/** Foundation schedule lookup: the volley time of serial 0..8, else "never". */
	float NextFoundationVolleyAt(int32 VolleySerial) const;
	/**
	 * Stops the running trial: sweeps engine enemies, resets the engine.
	 * ReasonKey (when set) is shown to the player — the death path stays
	 * silent by contract and passes NAME_None.
	 */
	void EndTrial(bool bSuccess, FName ReasonKey = NAME_None);
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

	// --- AUD-20 phase machine + storm hazards ---
	EQinglanTrialPhase Phase = EQinglanTrialPhase::Idle;
	/** Golden Core storm phase (0/1/2 across the three 50 s bands). */
	int32 StormPhaseIndex = 0;
	/** Storm volleys fired so far; indexes the Foundation wave schedule. */
	int32 VolleySerial = 0;
	/** Elapsed time at which the next volley enqueues (big = schedule done). */
	float NextVolleyAtSeconds = 0.f;
	/** Monotonic per-world attempt counter; salts HazardStream per attempt. */
	int32 AttemptCounter = 0;
	/** Deterministic strike placement stream (no FMath::RandRange, per contract). */
	FRandomStream HazardStream;
	/** Per-strike hit ordinal; every lightning impact carries a fresh one. */
	int32 LightningHitOrdinal = 0;
	/** EnemyId of this attempt's required elite (set entering RequiredKill). */
	FName RequiredEnemyId = NAME_None;
	TWeakObjectPtr<AQinglanEnemyCharacter> RequiredTarget;
	/** The boss was observed alive: its later death resolves as a victory. */
	bool bRequiredTargetSeenAlive = false;
	/** Engine completed the boss encounter without a live sighting (spawn failure). */
	bool bRequiredKillFailed = false;
	/** Golden Core one-shot beats. */
	bool bFirstLightningTelegraphFired = false;
	bool bRecoveryWindowFired = false;
	/** Strikes telegraphed but not yet resolved (impacts drain in any phase). */
	TArray<FQinglanPendingStrike> PendingStrikes;
	/** In-order BroadcastTrialEvent history for this attempt. */
	TArray<FName> TrialEventLog;
};
