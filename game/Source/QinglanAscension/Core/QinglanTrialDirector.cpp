// Trial director implementation: the two-phase survival contract (AUD-20) -
// Storm (countdown + ordinary waves + lightning schedule) -> RequiredKill
// (the authored elite must die before any proof commits) -> EndTrial.
//
// Docs: docs/story/CH03.md S02 (90-second foundation trial with three warning
// lightning waves and a final elite construct, "failure restarts the encounter
// timer only"), docs/story/CH06.md S02 (150-second trial as three 50-second
// storm phases ending in the Broken Guardian shell; the JSON script triggers
// storm_time_0/first_lightning_telegraph/storm_time_50/storm_time_100/
// storm_survived_150/shell_combat_started/first_shell_recovery_window/
// shell_defeated encode the same beats), docs/dev/level-layout.md
// (L_TribulationArena: storm platform 60 m across, both trial anchors at its
// center).
//
// The wave engine is a dedicated AQinglanEncounterDirector spawned by this
// actor - NOT the realm game mode's room director - so expedition room flow is
// untouched, and the engine's completion path skips room rewards because
// trials never open an expedition (its Run->IsInExpedition() guard).
#include "Core/QinglanTrialDirector.h"

#include "AI/QinglanAIController.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyCharacter.h"
#include "AI/QinglanEnemyTypes.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Components/SceneComponent.h"
#include "Core/QinglanAudioSubsystem.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanTrialAdmission.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UI/QinglanHUDStatics.h"
#include "QinglanAscension.h"

namespace
{
	// --- Wave cadence (authored v1; docs give no trial-wave numbers) ---
	// One wave of 3 at t=0, +1 enemy per 30 s reinforcement mark, never more
	// than 6 alive: a wave only begins after the previous one cleared, so the
	// wave size is also the alive count.
	constexpr int32 TrialBaseWaveSize = 3;
	constexpr int32 TrialMaxAlive = 6;
	constexpr float TrialReinforceIntervalSeconds = 30.f;
	// Minimum breather after a cleared wave before the next one opens
	// (docs/story/CH03.md S02 asks wave beats to sit "in safe gaps between
	// attacks"); the engine also drops music to Explore for the gap.
	constexpr float TrialWaveClearGapSeconds = 2.f;

	// Trial portal ring: authored ~2000 cm band on the 60 m storm platform
	// (half-width 3000 cm, so portals stay on the stone with >= 10 m margin).
	// Expedition rooms keep the engine default 600-1200 cm band.
	constexpr float TrialSpawnRingMinCm = 1600.f;
	constexpr float TrialSpawnRingMaxCm = 2000.f;

	// --- AUD-20 storm schedule (authorized spec; docs give the beat names,
	// the windows are the authored design pass) ---
	// Foundation: three warning waves, each 3 volleys at 2.2 s spacing, inside
	// the windows [4,14] / [34,44] / [64,76] s of the 90 s clock.
	constexpr int32 FoundationWaveCount = 3;
	constexpr int32 FoundationVolleysPerWave = 3;
	constexpr float FoundationVolleyIntervalSeconds = 2.2f;
	constexpr float FoundationWaveStartSeconds[FoundationWaveCount] = { 4.f, 34.f, 64.f };
	constexpr int32 FoundationStrikesPerVolley = 2;
	// Golden Core: continuous volleys every 3.0 s across the whole 150 s clock;
	// volley size grows by storm phase (2 -> 3 -> 4 with the 50 s bands).
	constexpr float GoldenCoreVolleyIntervalSeconds = 3.f;
	constexpr float GoldenCoreStormPhaseSeconds = 50.f;
	// Lightning damage (raw magnitude; the player's defense still mitigates).
	constexpr float TrialLightningDamageTier1 = 14.f;
	constexpr float TrialLightningDamageTier2 = 18.f;
	// Elapsed-time sentinel meaning "no further volley is scheduled".
	constexpr float TrialScheduleDoneSeconds = 1.0e9f;

	/** Player pawn for the failure poll; null outside a game world.
	 *  Local-player lookup first; worlds without a registered local player
	 *  (headless automation fixtures) may not register controllers either,
	 *  so the first player-character actor in the world answers last —
	 *  production maps hold exactly one AQinglanCharacter. */
	AQinglanCharacter* GetTrialPlayer(AActor* WorldContext)
	{
		if (!WorldContext || !WorldContext->GetWorld())
		{
			return nullptr;
		}
		UWorld* World = WorldContext->GetWorld();
		if (AQinglanCharacter* Player = Cast<AQinglanCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0)))
		{
			return Player;
		}
		for (FConstControllerIterator It = World->GetControllerIterator(); It; ++It)
		{
			if (APlayerController* Controller = Cast<APlayerController>(*It))
			{
				if (AQinglanCharacter* Character = Cast<AQinglanCharacter>(Controller->GetPawn()))
				{
					return Character;
				}
			}
		}
		TActorIterator<AQinglanCharacter> CharacterIt(World);
		return CharacterIt ? *CharacterIt : nullptr;
	}

	bool IsFoundationScene(FName SceneId)
	{
		return SceneId == FName(TEXT("CH03_S02"));
	}

	/** Cached story-event ids (FName construction is not free per tick). */
	FName EvStormTime0()                { static const FName V(TEXT("storm_time_0"));                  return V; }
	FName EvFirstLightningTelegraph()   { static const FName V(TEXT("first_lightning_telegraph"));     return V; }
	FName EvStormTime50()               { static const FName V(TEXT("storm_time_50"));                 return V; }
	FName EvStormTime100()              { static const FName V(TEXT("storm_time_100"));                return V; }
	FName EvStormSurvived150()          { static const FName V(TEXT("storm_survived_150"));            return V; }
	FName EvShellCombatStarted()        { static const FName V(TEXT("shell_combat_started"));          return V; }
	FName EvFirstShellRecoveryWindow()  { static const FName V(TEXT("first_shell_recovery_window"));   return V; }
	FName EvShellDefeated()             { static const FName V(TEXT("shell_defeated"));                return V; }

	/**
	 * True when the enemy was spawned by this trial's wave engine. Possessing
	 * a pawn rewrites its Owner to the possessing AIController (APawn::
	 * PossessedBy -> SetOwner(NewController)), so the engine's spawn-time
	 * Owner tag disappears on materialization; the controller's director
	 * back-reference (set in SpawnThroughPortal) survives possession.
	 */
	bool IsWaveEngineEnemy(const AQinglanEnemyCharacter* Enemy, const AQinglanEncounterDirector* Engine)
	{
		if (Enemy == nullptr || Engine == nullptr)
		{
			return false;
		}
		if (Enemy->GetOwner() == Engine)
		{
			return true; // portal-pending, before possession
		}
		const AQinglanAIController* Controller = Cast<AQinglanAIController>(Enemy->GetController());
		return Controller != nullptr && Controller->GetDirector() == Engine;
	}
}

namespace QinglanTrialStorm
{
	TArray<FVector> PickStrikeVolley(FRandomStream& Stream, const FVector& Anchor,
		int32 Count, float MinSeparationCm, const TArray<FVector>& AvoidCenters)
	{
		TArray<FVector> Out;
		// Live-outline cap first: the avoid set (still-telegraphing strikes)
		// plus this volley never exceeds the safety maximum.
		Count = FMath::Clamp(
			FMath::Min(Count, MaxSimultaneousStrikes - AvoidCenters.Num()),
			0, MaxSimultaneousStrikes);
		if (Count <= 0)
		{
			return Out;
		}

		for (int32 Index = 0; Index < Count; ++Index)
		{
			// Best-of-N deterministic selection (mirrors PickPortalLocation's
			// retry shape): every strike draws exactly StrikePlacementAttempts
			// candidates and keeps the one with the largest clearance to every
			// placed/avoided center, so the same seed replays identically.
			FVector Best = Anchor;
			float BestClearance = -1.f;
			for (int32 Attempt = 0; Attempt < StrikePlacementAttempts; ++Attempt)
			{
				const float AngleDeg = Stream.FRand() * 360.f;
				const float Radius = Stream.FRand() * StrikeMaxOffsetCm;
				const FVector Candidate(Anchor.X + FMath::Cos(FMath::DegreesToRadians(AngleDeg)) * Radius,
					Anchor.Y + FMath::Sin(FMath::DegreesToRadians(AngleDeg)) * Radius, Anchor.Z);

				float Clearance = TNumericLimits<float>::Max();
				for (const FVector& Placed : Out)
				{
					Clearance = FMath::Min(Clearance, static_cast<float>(FVector::Dist2D(Candidate, Placed)));
				}
				for (const FVector& Avoid : AvoidCenters)
				{
					Clearance = FMath::Min(Clearance, static_cast<float>(FVector::Dist2D(Candidate, Avoid)));
				}
				if (Clearance > BestClearance)
				{
					BestClearance = Clearance;
					Best = Candidate;
				}
			}
			// Hard safety contract: a strike that cannot clear the minimum
			// separation is dropped, never placed on top of a live outline.
			if (BestClearance >= MinSeparationCm)
			{
				Out.Add(Best);
			}
		}
		return Out;
	}
}

AQinglanTrialDirector::AQinglanTrialDirector()
{
	// Real root so the actor reports its spawn transform (the trial anchor);
	// the wave engine itself is rootless scaffold and anchors portals at the
	// world origin, which is the storm platform center in L_TribulationArena
	// (QinglanContentBootstrap.cpp authors the platform centered there).
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("TrialRoot"));
	SetRootComponent(Root);

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AQinglanTrialDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (AQinglanEncounterDirector* Engine = WaveEngine.Get())
	{
		Engine->OnEncounterCompleted.RemoveDynamic(this, &AQinglanTrialDirector::HandleWaveEngineCompleted);
	}
	Super::EndPlay(EndPlayReason);
}

bool AQinglanTrialDirector::BeginTrial(FName SceneId, FName ObjectiveId, float DurationSeconds, int32 Tier)
{
	if (bActive)
	{
		UE_LOG(LogQinglan, Warning, TEXT("BeginTrial refused: trial %s still runs"), *TrialSceneId.ToString());
		return false;
	}
	UGameInstance* Instance = GetGameInstance();
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	const QinglanTrialAdmission::FResult Admission = QinglanTrialAdmission::EvaluateScene(SceneId, ObjectiveId, Profile, Story);
	if (!Admission.IsAllowed())
	{
		if (AQinglanCharacter* Player = GetTrialPlayer(this))
		{
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(Admission.ReasonKey));
		}
		return false;
	}
	QinglanTrialAdmission::ETrial Trial;
	QinglanTrialAdmission::ResolveScene(SceneId, Trial); // EvaluateScene validated the scene.
	const float RequiredDuration = Trial == QinglanTrialAdmission::ETrial::Foundation ? 90.f : 150.f;
	const int32 RequiredTier = Trial == QinglanTrialAdmission::ETrial::Foundation ? 1 : 2;
	const UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	AQinglanCharacter* Player = GetTrialPlayer(this);
	if (!FMath::IsNearlyEqual(DurationSeconds, RequiredDuration) || Tier != RequiredTier)
	{
		UE_LOG(LogQinglan, Warning, TEXT("Trial refused: duration %.1f/tier %d does not match the authored %.1f/%d"),
			DurationSeconds, Tier, RequiredDuration, RequiredTier);
		return false;
	}
	if (Run && Run->IsInExpedition())
	{
		UE_LOG(LogQinglan, Warning, TEXT("Trial refused: an expedition is still active"));
		return false;
	}
	if (!Player)
	{
		int32 ControllerCount = 0;
		int32 PlayerControllerCount = 0;
		int32 PossessedPawns = 0;
		if (UWorld* World = GetWorld())
		{
			for (FConstControllerIterator It = World->GetControllerIterator(); It; ++It)
			{
				++ControllerCount;
				if (APlayerController* Controller = Cast<APlayerController>(*It))
				{
					++PlayerControllerCount;
					if (Controller->GetPawn() != nullptr)
					{
						++PossessedPawns;
					}
				}
			}
		}
		UE_LOG(LogQinglan, Warning, TEXT("Trial refused: no player character in the world (controllers=%d playerControllers=%d possessedPawns=%d)"),
			ControllerCount, PlayerControllerCount, PossessedPawns);
		return false;
	}
	if (!Player->GetCombat() || !Player->GetCombat()->IsAlive())
	{
		UE_LOG(LogQinglan, Warning, TEXT("Trial refused: player combat authority missing or dead"));
		return false;
	}
	if (DurationSeconds <= 0.f || !GetWorld() || !EnsureWaveEngine())
	{
		return false;
	}
	EnsureCatalogAssets();
	if (CatalogAssets.IsEmpty())
	{
		UE_LOG(LogQinglan, Error, TEXT("BeginTrial: enemy catalog yielded no archetypes; trial %s dropped"), *SceneId.ToString());
		return false;
	}

	bActive = true;
	TrialSceneId = SceneId;
	TrialObjectiveId = ObjectiveId;
	TrialTier = Tier;
	RemainingSeconds = DurationSeconds;
	ElapsedSeconds = 0.f;
	WavesBegun = 0;
	CatalogCursor = 0; // same composition per wave index across attempts
	bWaveOutstanding = false;

	// AUD-20 phase machine + hazard state: every attempt starts the storm from
	// its first beat. The hazard stream salts with the per-world attempt
	// counter, so a retry fields a different-but-deterministic volley ladder.
	Phase = EQinglanTrialPhase::Storm;
	StormPhaseIndex = 0;
	VolleySerial = 0;
	NextVolleyAtSeconds = IsFoundationScene(SceneId) ? FoundationWaveStartSeconds[0] : 0.f;
	++AttemptCounter;
	HazardStream.Initialize(TrialTier * 100000 + AttemptCounter);
	LightningHitOrdinal = 0;
	RequiredEnemyId = NAME_None;
	RequiredTarget = nullptr;
	bRequiredTargetSeenAlive = false;
	bRequiredKillFailed = false;
	bFirstLightningTelegraphFired = false;
	bRecoveryWindowFired = false;
	PendingStrikes.Reset();
	TrialEventLog.Reset();
	UE_LOG(LogQinglan, Log, TEXT("Trial %s begins: %.0f s, tier %d (attempt %d)"),
		*SceneId.ToString(), DurationSeconds, Tier, AttemptCounter);

	BeginNextWave(); // the engine's BeginEncounter switches music to Combat
	if (!IsFoundationScene(SceneId))
	{
		BroadcastTrialEvent(EvStormTime0());
	}
	return true;
}

bool AQinglanTrialDirector::CancelTrialAndReturnToHub()
{
	EndTrial(/*bSuccess=*/false);
	return QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
}

void AQinglanTrialDirector::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bActive)
	{
		return;
	}
	if (!IsValid(WaveEngine.Get()) || CatalogAssets.IsEmpty())
	{
		// A lost combat driver cannot leave an unattended clock that awards proof.
		EndTrial(/*bSuccess=*/false);
		if (AQinglanCharacter* Player = GetTrialPlayer(this))
		{
			Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.unavailable"))));
		}
		return;
	}

	// Failure poll: death or a lost pawn ends the trial silently - the
	// character's defeat flow already banks and travels to the hub, and the
	// anchor re-press back at the arena restarts from scratch.
	AQinglanCharacter* Player = GetTrialPlayer(this);
	const UQinglanCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
	if (!Player || !Combat || !Combat->IsAlive())
	{
		EndTrial(/*bSuccess=*/false);
		return;
	}

	switch (Phase)
	{
	case EQinglanTrialPhase::Storm:
	{
		ElapsedSeconds += DeltaTime;
		RemainingSeconds -= DeltaTime;
		if (RemainingSeconds <= 0.f)
		{
			// AUD-20: outliving the clock alone grants nothing - the required
			// elite takes over before any proof can commit.
			EnterRequiredKillPhase();
			return;
		}
		UpdateStormSchedule(DeltaTime);
		UpdatePendingStrikes(DeltaTime);

		if (!bWaveOutstanding)
		{
			NextWaveCountdown -= DeltaTime;
			if (NextWaveCountdown <= 0.f)
			{
				BeginNextWave();
			}
		}
		break;
	}

	case EQinglanTrialPhase::RequiredKill:
	case EQinglanTrialPhase::Victory:
	{
		// Residual telegraphed strikes still land during the kill phase.
		UpdatePendingStrikes(DeltaTime);
		UpdateRequiredTarget();
		if (Phase == EQinglanTrialPhase::Victory)
		{
			// Queued by HandleWaveEngineCompleted (which fires from inside the
			// engine's Tick); resolving here keeps EndTrial out of that frame.
			EndTrial(/*bSuccess=*/true);
			return;
		}
		if (bRequiredKillFailed)
		{
			EndTrial(/*bSuccess=*/false, FName(TEXT("trial.spawn_failed")));
			return;
		}
		// Poll the engine as the authority on the required encounter: the
		// completion delegate can be missed (e.g. an engine restart between
		// binding and broadcast), so the phase advances on observed state.
		const AQinglanEncounterDirector* Engine = WaveEngine.Get();
		if (Engine != nullptr && !Engine->IsEncounterActive())
		{
			if (bRequiredTargetSeenAlive)
			{
				Phase = EQinglanTrialPhase::Victory;
			}
			else
			{
				bRequiredKillFailed = true;
			}
			break; // resolve on the next Tick, outside this poll
		}
		break;
	}

	default:
		break;
	}
}

void AQinglanTrialDirector::HandleWaveEngineCompleted()
{
	UE_LOG(LogQinglan, Log, TEXT("Trial %s engine-completed callback: active=%d phase=%d seenAlive=%d"),
		*TrialSceneId.ToString(), bActive ? 1 : 0, static_cast<int32>(Phase), bRequiredTargetSeenAlive ? 1 : 0);
	if (!bActive)
	{
		return; // engine reset echo (trial already ended) or teardown noise
	}
	if (Phase == EQinglanTrialPhase::RequiredKill || Phase == EQinglanTrialPhase::Victory)
	{
		// The boss encounter resolved. Completion broadcasts from inside the
		// engine's own Tick, so the outcome is queued and EndTrial runs on
		// this director's next Tick (never reentering BeginEncounter mid-tick).
		// A boss observed alive that then died is the win; a completion with
		// no live sighting means the spawn path failed - retry costs nothing.
		if (bRequiredTargetSeenAlive)
		{
			Phase = EQinglanTrialPhase::Victory;
		}
		else
		{
			bRequiredKillFailed = true;
		}
		return;
	}
	if (!bWaveOutstanding)
	{
		return;
	}
	bWaveOutstanding = false;

	// Next wave at the next 30 s reinforcement mark, with the authored
	// breather as the floor (a slow clear pushes the wave past its mark; the
	// timer keeps running either way - it is a survival clock, not a wave clock).
	const float MarkRemaining = TrialReinforceIntervalSeconds
		- FMath::Fmod(ElapsedSeconds, TrialReinforceIntervalSeconds);
	NextWaveCountdown = FMath::Max(TrialWaveClearGapSeconds, MarkRemaining);
}

void AQinglanTrialDirector::EnterRequiredKillPhase()
{
	RemainingSeconds = 0.f;
	Phase = EQinglanTrialPhase::RequiredKill;
	bWaveOutstanding = false;
	NextWaveCountdown = 0.f;
	RequiredTarget = nullptr;
	bRequiredTargetSeenAlive = false;
	bRecoveryWindowFired = false;

	const bool bFoundation = IsFoundationScene(TrialSceneId);
	RequiredEnemyId = FName(bFoundation ? TEXT("elite_construct") : TEXT("broken_guardian"));

	// The storm's ordinary waves end here: sweep everything the engine owns
	// (living, portal-pending, pooled) before the boss takes the platform.
	SweepEngineEnemies();

	// Boss-room encounter against the required elite. An empty wave (missing
	// spec/asset) completes on the engine's next Tick and routes through the
	// no-live-sighting failure with the retry message.
	TArray<UQinglanEnemyDataAsset*> BossWave;
	if (AQinglanEncounterDirector* Engine = WaveEngine.Get())
	{
		if (const FQinglanArchetypeSpec* Spec = QinglanEnemyCatalog::FindSpec(RequiredEnemyId))
		{
			if (UQinglanEnemyDataAsset* Asset = QinglanEnemyCatalog::CreateDataAssetFromSpec(*Spec, GetTransientPackage()))
			{
				BossWave.Add(Asset);
			}
		}
		if (BossWave.IsEmpty())
		{
			UE_LOG(LogQinglan, Error, TEXT("Trial %s: required elite '%s' missing from the catalog"),
				*TrialSceneId.ToString(), *RequiredEnemyId.ToString());
		}
		Engine->BeginEncounter(BossWave, TrialTier * 100 + 99, /*bIsBossRoom=*/true);
	}

	if (!bFoundation)
	{
		// Golden Core script beat: the three storm phases were survived.
		BroadcastTrialEvent(EvStormSurvived150());
	}
	UE_LOG(LogQinglan, Log, TEXT("Trial %s clock survived; required kill '%s' begins"),
		*TrialSceneId.ToString(), *RequiredEnemyId.ToString());
}

void AQinglanTrialDirector::UpdateStormSchedule(float DeltaTime)
{
	// DeltaTime is kept for signature parity with the sibling phase updaters;
	// the schedule reads the accumulated paused-game clock exclusively.
	(void)DeltaTime;
	const bool bFoundation = IsFoundationScene(TrialSceneId);

	if (!bFoundation)
	{
		// Three 50 s storm phases: crossing each boundary fires its script beat.
		const int32 PhaseIndex = FMath::Min(
			FMath::FloorToInt(ElapsedSeconds / GoldenCoreStormPhaseSeconds),
			/*MaxIndex*/ 2);
		while (StormPhaseIndex < PhaseIndex)
		{
			BroadcastTrialEvent(StormPhaseIndex == 0 ? EvStormTime50() : EvStormTime100());
			++StormPhaseIndex;
		}
	}

	while (ElapsedSeconds >= NextVolleyAtSeconds)
	{
		EnqueueStrikeVolley(GetTrialPlayer(this));
		++VolleySerial;
		NextVolleyAtSeconds = bFoundation
			? NextFoundationVolleyAt(VolleySerial)
			: ElapsedSeconds + GoldenCoreVolleyIntervalSeconds;
	}
}

void AQinglanTrialDirector::EnqueueStrikeVolley(AQinglanCharacter* Player)
{
	if (!Player)
	{
		return; // no player to threaten; the failure poll ends the attempt next frame
	}
	const bool bFoundation = IsFoundationScene(TrialSceneId);
	const int32 Requested = bFoundation
		? FoundationStrikesPerVolley
		: FMath::Min(2 + StormPhaseIndex, QinglanTrialStorm::MaxSimultaneousStrikes);

	TArray<FVector> Avoid;
	Avoid.Reserve(PendingStrikes.Num());
	for (const FQinglanPendingStrike& Strike : PendingStrikes)
	{
		Avoid.Add(Strike.Center);
	}

	const TArray<FVector> Centers = QinglanTrialStorm::PickStrikeVolley(
		HazardStream, Player->GetActorLocation(), Requested,
		QinglanTrialStorm::StrikeMinSeparationCm, Avoid);
	if (Centers.IsEmpty())
	{
		return; // the live-outline cap left no room this beat
	}

	for (const FVector& Center : Centers)
	{
		FQinglanPendingStrike Strike;
		Strike.Center = Center;
		Strike.RadiusCm = QinglanTrialStorm::StrikeRadiusCm;
		Strike.WarnRemaining = QinglanTrialStorm::StrikeWarnSeconds;
		PendingStrikes.Add(Strike);
	}

	// Persistent outlines: presentation (and the player) get the full volley
	// for the whole warning window, then one distance-checked impact each.
	OnTrialHazardTelegraph.Broadcast(this, Centers,
		QinglanTrialStorm::StrikeRadiusCm, QinglanTrialStorm::StrikeWarnSeconds);
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UQinglanAudioSubsystem* Audio = Instance->GetSubsystem<UQinglanAudioSubsystem>())
		{
			Audio->PlaySfx(FName(TEXT("sfx_telegraph_warn")));
		}
	}

	if (!bFoundation)
	{
		if (!bFirstLightningTelegraphFired)
		{
			bFirstLightningTelegraphFired = true;
			BroadcastTrialEvent(EvFirstLightningTelegraph());
		}
	}
	else if (VolleySerial % FoundationVolleysPerWave == 0)
	{
		// Foundation's reserved wave beats (CH03_S02 trigger-gated lines).
		BroadcastTrialEvent(FName(*FString::Printf(TEXT("trial.lightning_wave_%d"),
			VolleySerial / FoundationVolleysPerWave + 1)));
	}
}

void AQinglanTrialDirector::UpdatePendingStrikes(float DeltaTime)
{
	if (PendingStrikes.IsEmpty())
	{
		return;
	}
	AQinglanCharacter* Player = GetTrialPlayer(this);
	UQinglanCombatComponent* Combat = (Player && Player->GetCombat() && Player->GetCombat()->IsAlive())
		? Player->GetCombat()
		: nullptr;

	for (int32 Index = PendingStrikes.Num() - 1; Index >= 0; --Index)
	{
		FQinglanPendingStrike& Strike = PendingStrikes[Index];
		Strike.WarnRemaining -= DeltaTime;
		if (Strike.WarnRemaining > 0.f)
		{
			continue;
		}
		// Impact: one 2D distance check against the warned outline.
		if (Combat && Player
			&& FVector::Dist2D(Player->GetActorLocation(), Strike.Center) <= Strike.RadiusCm)
		{
			FQinglanDamageEvent Event;
			Event.Attacker = this;
			Event.Target = Player;
			Event.CastId = UQinglanCombatComponent::NextCastId();
			Event.HitOrdinal = LightningHitOrdinal++;
			Event.TechniqueId = FName(TEXT("trial.lightning_strike"));
			Event.Element = EQinglanElement::None;
			Event.RawMagnitude = TrialTier == 1 ? TrialLightningDamageTier1 : TrialLightningDamageTier2;
			Combat->ApplyDamage(Event);
		}
		PendingStrikes.RemoveAtSwap(Index);
	}
}

void AQinglanTrialDirector::UpdateRequiredTarget()
{
	UWorld* World = GetWorld();
	AQinglanEncounterDirector* Engine = WaveEngine.Get();
	if (!World || !Engine)
	{
		return;
	}

	if (!RequiredTarget.IsValid())
	{
		// Ownership tags exactly the engine's spawn (SweepEngineEnemies' filter);
		// after the sweep only the required elite answers to it.
		for (TActorIterator<AQinglanEnemyCharacter> It(World); It; ++It)
		{
			AQinglanEnemyCharacter* Enemy = *It;
			if (IsWaveEngineEnemy(Enemy, Engine) && Enemy->IsAlive()
				&& Enemy->GetEnemyId() == RequiredEnemyId)
			{
				RequiredTarget = Enemy;
				if (!bRequiredTargetSeenAlive)
				{
					bRequiredTargetSeenAlive = true;
					if (!IsFoundationScene(TrialSceneId))
					{
						BroadcastTrialEvent(EvShellCombatStarted());
					}
					UE_LOG(LogQinglan, Log, TEXT("Trial %s: required target %s sighted"),
						*TrialSceneId.ToString(), *RequiredEnemyId.ToString());
				}
				break;
			}
		}
	}

	if (AQinglanEnemyCharacter* Target = RequiredTarget.Get())
	{
		if (!Target->IsAlive())
		{
			RequiredTarget = nullptr; // engine completion resolves the outcome
			return;
		}
		if (!IsFoundationScene(TrialSceneId) && !bRecoveryWindowFired
			&& Target->GetAIState() == EQinglanAIState::Recover)
		{
			// First observable punish window on the shell (the docs' "recovery
			// window" beat): one shot per attempt.
			bRecoveryWindowFired = true;
			BroadcastTrialEvent(EvFirstShellRecoveryWindow());
		}
	}
}

void AQinglanTrialDirector::BroadcastTrialEvent(FName Event)
{
	if (Event == NAME_None)
	{
		return;
	}
	TrialEventLog.Add(Event);
	// Event-line delivery (trigger-gated script lines + mq_* flag routing)
	// lives in the story runtime; the director only names the beat.
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
		{
			QinglanStoryRuntime::NotifyGameplayEvent(Story, Event, {});
		}
	}
}

float AQinglanTrialDirector::NextFoundationVolleyAt(int32 Serial) const
{
	const int32 Wave = Serial / FoundationVolleysPerWave;
	if (Wave >= FoundationWaveCount)
	{
		return TrialScheduleDoneSeconds;
	}
	return FoundationWaveStartSeconds[Wave]
		+ (Serial % FoundationVolleysPerWave) * FoundationVolleyIntervalSeconds;
}

AQinglanEncounterDirector* AQinglanTrialDirector::EnsureWaveEngine()
{
	if (AQinglanEncounterDirector* Existing = WaveEngine.Get())
	{
		return Existing;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	// A dedicated engine, not the world's room director: GetActorOfClass could
	// return the realm game mode's instance anchored to a PlayerStart, and the
	// trial needs its own bookkeeping (pool, wave queue, spawn ring).
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.Name = TEXT("QinglanTrialWaveEngine");
	AQinglanEncounterDirector* Engine = World->SpawnActor<AQinglanEncounterDirector>(
		AQinglanEncounterDirector::StaticClass(), GetActorLocation(), FRotator::ZeroRotator, Params);
	if (!Engine)
	{
		UE_LOG(LogQinglan, Error, TEXT("Trial director could not spawn a wave engine"));
		return nullptr;
	}
	WaveEngine = Engine;
	Engine->OnEncounterCompleted.AddDynamic(this, &AQinglanTrialDirector::HandleWaveEngineCompleted);
	// Widen the portal ring to the storm platform's outer band; expedition
	// rooms keep the authored 600-1200 default.
	SetEncounterSpawnRingOverride(Engine, TrialSpawnRingMinCm, TrialSpawnRingMaxCm);
	UE_LOG(LogQinglan, Log, TEXT("Trial director spawned wave engine"));
	return Engine;
}

void AQinglanTrialDirector::EnsureCatalogAssets()
{
	if (!CatalogAssets.IsEmpty())
	{
		return;
	}
	// Rooted transient assets (house pattern: Progression/QinglanRunSubsystem.cpp
	// BuildRoomWave); the UPROPERTY array keeps them GC-visible afterwards.
	for (const FQinglanArchetypeSpec& Spec : QinglanEnemyCatalog::GetAllArchetypes())
	{
		if (UQinglanEnemyDataAsset* Data = QinglanEnemyCatalog::CreateDataAssetFromSpec(Spec, GetTransientPackage()))
		{
			CatalogAssets.Add(Data);
		}
	}
}

void AQinglanTrialDirector::BeginNextWave()
{
	AQinglanEncounterDirector* Engine = WaveEngine.Get();
	if (!Engine || CatalogAssets.IsEmpty())
	{
		return;
	}

	// Wave size from the elapsed reinforcement marks (3 at t=0, +1 per 30 s,
	// capped at 6 alive); drawn round-robin from the six catalog archetypes
	// with a per-trial-reset cursor, so every attempt fields the same
	// deterministic ladder (house determinism: no FMath::RandRange).
	const int32 WaveSize = FMath::Min(
		TrialBaseWaveSize + FMath::FloorToInt(ElapsedSeconds / TrialReinforceIntervalSeconds),
		TrialMaxAlive);
	TArray<UQinglanEnemyDataAsset*> Wave;
	Wave.Reserve(WaveSize);
	for (int32 Count = 0; Count < WaveSize; ++Count)
	{
		if (UQinglanEnemyDataAsset* Data = CatalogAssets[CatalogCursor % CatalogAssets.Num()])
		{
			Wave.Add(Data);
		}
		++CatalogCursor;
	}

	// The "RoomIndex" argument only salts the engine's spawn stream; room
	// rewards never commit because trials open no expedition.
	Engine->BeginEncounter(Wave, TrialTier * 100 + WavesBegun, /*bIsBossRoom=*/false);
	++WavesBegun;
	bWaveOutstanding = true;
	UE_LOG(LogQinglan, Log, TEXT("Trial wave %d: %d enemies (%.0f s elapsed)"), WavesBegun, Wave.Num(), ElapsedSeconds);
}

void AQinglanTrialDirector::EndTrial(bool bSuccess, FName ReasonKey)
{
	UE_LOG(LogQinglan, Log, TEXT("Trial %s EndTrial(success=%d, reason=%s, active=%d, phase=%d)"),
		*TrialSceneId.ToString(), bSuccess ? 1 : 0, *ReasonKey.ToString(), bActive ? 1 : 0,
		static_cast<int32>(Phase));
	if (!bActive)
	{
		return;
	}
	const bool bFoundation = IsFoundationScene(TrialSceneId);
	bActive = false;
	bWaveOutstanding = false;
	RemainingSeconds = 0.f;
	Phase = EQinglanTrialPhase::Idle;
	PendingStrikes.Reset();

	// Sweep every engine-owned enemy (living, portal-pending, pooled alike)
	// and reset the engine with an empty wave: its restart path releases the
	// arbitration slots and clears its bookkeeping, and its next Tick resolves
	// the empty encounter exactly once (Explore music included; the guarded
	// reward path stays a no-op). The "restarted an active encounter" warning
	// the engine logs for a mid-fight reset is this intentional abort.
	SweepEngineEnemies();
	if (AQinglanEncounterDirector* Engine = WaveEngine.Get())
	{
		TArray<UQinglanEnemyDataAsset*> NoWave;
		Engine->BeginEncounter(NoWave, TrialTier * 100 + WavesBegun, /*bIsBossRoom=*/false);
	}

	if (!bSuccess)
	{
		// Silent end by contract ("failure restarts the encounter timer only")
		// - except when the caller names a reason worth a player message (the
		// spawn-failure path); the death path passes NAME_None and stays quiet.
		UE_LOG(LogQinglan, Log, TEXT("Trial %s failed at %.0f s"), *TrialSceneId.ToString(), ElapsedSeconds);
		if (ReasonKey != NAME_None)
		{
			if (AQinglanCharacter* Player = GetTrialPlayer(this))
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(ReasonKey));
			}
		}
		return;
	}

	UE_LOG(LogQinglan, Log, TEXT("Trial %s survived; committing scoped objective %s"),
		*TrialSceneId.ToString(), *TrialObjectiveId.ToString());
	if (UGameInstance* Instance = GetGameInstance())
	{
		UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>();
		UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>();
		// Recheck after combat. A lost registry or changed prerequisite cannot
		// turn a stale attempt into a reward for a different story objective.
		const QinglanTrialAdmission::FResult Admission = QinglanTrialAdmission::EvaluateScene(
			TrialSceneId, TrialObjectiveId, Profile, Story);
		if (!Admission.IsAllowed() || !Story->NotifySceneObjectiveCompleted(TrialSceneId, TrialObjectiveId)
			|| !Story->IsSceneCompleted(TrialSceneId))
		{
			UE_LOG(LogQinglan, Warning, TEXT("Trial completion rejected; proof remains pending"));
			if (AQinglanCharacter* Player = GetTrialPlayer(this))
			{
				Player->ShowGameMessage(UQinglanHUDStatics::GameText(FName(TEXT("trial.unavailable"))));
			}
			return;
		}
		Profile->SetCampaignFlag(FName(bFoundation ? QinglanProgression::FoundationTrialPassedCampaignFlag
			: QinglanProgression::GoldenCoreTrialWonCampaignFlag));
		// The profile owns permanent attribute/unlock changes and transaction idempotency.
		if (!Profile->TryBreakthrough(FName(bFoundation ? TEXT("tx.trial.foundation.breakthrough")
			: TEXT("tx.trial.golden_core.breakthrough"))))
		{
			// Proof is retained for the cultivation station to retry; never erase a won trial.
			UE_LOG(LogQinglan, Warning, TEXT("Trial proof committed but breakthrough rejected; cultivation retry required"));
		}
		if (UQinglanAudioSubsystem* Audio = Instance->GetSubsystem<UQinglanAudioSubsystem>())
		{
			Audio->SetMusicState(EQinglanMusicState::Explore); // completion beat
		}
		// AUD-11: authored trial beats are emitted after the settlement so the
		// guard-driven objective completion cannot complete the scene before
		// the admission recheck above (which would skip the campaign flag and
		// the deterministic breakthrough).
		if (!bFoundation)
		{
			BroadcastTrialEvent(EvShellDefeated());
		}
		BroadcastTrialEvent(FName(bFoundation ? TEXT("trial.survived.foundation") : TEXT("trial.survived.golden_core")));

		if (bFoundation)
		{
			// TravelToMap saves first. A failed save keeps the player in the cleared
			// arena, where the ordinary retreat action can retry the same return.
			QinglanInteraction::TravelToMap(this, QinglanInteraction::HubMapPath);
		}
	}
}

void AQinglanTrialDirector::SweepEngineEnemies()
{
	UWorld* World = GetWorld();
	AQinglanEncounterDirector* Engine = WaveEngine.Get();
	if (!World || !Engine)
	{
		return;
	}
	// SpawnThroughPortal tags the engine's spawns (spawn-time Owner, then the
	// controller's director back-reference after possession), so the sweep
	// leaves the room director's enemies untouched if any share a world.
	for (TActorIterator<AQinglanEnemyCharacter> It(World); It; ++It)
	{
		AQinglanEnemyCharacter* Enemy = *It;
		if (IsWaveEngineEnemy(Enemy, Engine))
		{
			Enemy->Destroy();
		}
	}
}

AActor* AQinglanTrialDirector::GetRequiredTarget() const
{
	return RequiredTarget.Get();
}
