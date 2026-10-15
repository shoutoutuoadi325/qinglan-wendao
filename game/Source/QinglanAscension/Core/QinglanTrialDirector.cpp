// Trial director implementation: countdown, reinforcement schedule, wave
// engine lifecycle, success/failure contract.
//
// Docs: docs/story/CH03.md S02 (90-second foundation trial, "failure restarts
// the encounter timer only"), docs/story/CH06.md S02 (150-second trial as
// three 50-second storm phases; the JSON script triggers storm_time_0/50/100
// and storm_survived_150 encode the same clock), docs/dev/level-layout.md
// (L_TribulationArena: storm platform 60 m across, both trial anchors at its
// center).
//
// The wave engine is a dedicated AQinglanEncounterDirector spawned by this
// actor - NOT the realm game mode's room director - so expedition room flow is
// untouched, and the engine's completion path skips room rewards because
// trials never open an expedition (its Run->IsInExpedition() guard).
#include "Core/QinglanTrialDirector.h"

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

	/** Player pawn for the failure poll; null outside a game world. */
	AQinglanCharacter* GetTrialPlayer(AActor* WorldContext)
	{
		return (WorldContext && WorldContext->GetWorld())
			? Cast<AQinglanCharacter>(UGameplayStatics::GetPlayerCharacter(WorldContext, 0))
			: nullptr;
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
	if (!FMath::IsNearlyEqual(DurationSeconds, RequiredDuration) || Tier != RequiredTier
		|| (Run && Run->IsInExpedition()) || !Player || !Player->GetCombat() || !Player->GetCombat()->IsAlive())
	{
		UE_LOG(LogQinglan, Warning, TEXT("Trial refused: invalid clock/tier, expedition active, or player unavailable"));
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
	UE_LOG(LogQinglan, Log, TEXT("Trial %s begins: %.0f s, tier %d"), *SceneId.ToString(), DurationSeconds, Tier);

	BeginNextWave(); // the engine's BeginEncounter switches music to Combat
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

	ElapsedSeconds += DeltaTime;
	RemainingSeconds -= DeltaTime;
	if (RemainingSeconds <= 0.f)
	{
		EndTrial(/*bSuccess=*/true);
		return;
	}

	if (!bWaveOutstanding)
	{
		NextWaveCountdown -= DeltaTime;
		if (NextWaveCountdown <= 0.f)
		{
			BeginNextWave();
		}
	}
}

void AQinglanTrialDirector::HandleWaveEngineCompleted()
{
	if (!bActive || !bWaveOutstanding)
	{
		return; // engine reset echo (trial already ended) or teardown noise
	}
	bWaveOutstanding = false;

	// Next wave at the next 30 s reinforcement mark, with the authored
	// breather as the floor (a slow clear pushes the wave past its mark; the
	// timer keeps running either way - it is a survival clock, not a wave clock).
	const float MarkRemaining = TrialReinforceIntervalSeconds
		- FMath::Fmod(ElapsedSeconds, TrialReinforceIntervalSeconds);
	NextWaveCountdown = FMath::Max(TrialWaveClearGapSeconds, MarkRemaining);
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

void AQinglanTrialDirector::EndTrial(bool bSuccess)
{
	if (!bActive)
	{
		return;
	}
	bActive = false;
	bWaveOutstanding = false;
	RemainingSeconds = 0.f;

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
		// Silent end: no message, no objective commit - "failure restarts the
		// encounter timer only" (docs/story/CH03.md S02).
		UE_LOG(LogQinglan, Log, TEXT("Trial %s failed at %.0f s"), *TrialSceneId.ToString(), ElapsedSeconds);
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
		const bool bFoundation = TrialSceneId == FName(TEXT("CH03_S02"));
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
	// SpawnThroughPortal sets Params.Owner = the engine, so ownership tags
	// exactly the enemies it spawned (the room director's enemies, if any ever
	// share a world, stay untouched).
	for (TActorIterator<AQinglanEnemyCharacter> It(World); It; ++It)
	{
		AQinglanEnemyCharacter* Enemy = *It;
		if (Enemy && Enemy->GetOwner() == Engine)
		{
			Enemy->Destroy();
		}
	}
}
