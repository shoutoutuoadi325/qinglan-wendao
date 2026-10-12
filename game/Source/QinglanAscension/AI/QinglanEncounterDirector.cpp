// Encounter director implementation: portal spawns with warnings, enemy caps,
// melee/ranged arbitration budgets, exactly-once completion, enemy pooling.
//
// Docs: docs/01 section 6 (encounter selection, caps, portals),
// docs/05 section 5 (AI arbitration, pooled projectiles/portals),
// docs/dev/implementation-blueprint.md section 1.3.
//
// FIXME-CONTRACT: the frozen scaffold header has no members for the pending
// wave queue, the enemy pool, or the seeded encounter stream, and exposes no
// per-enemy release for ReleaseAttackSlots(). That state therefore lives in
// a TU-local registry keyed by weak director pointers (pruned every tick);
// merge it into the class when the headers unfreeze. The scaffold also
// declares private helpers SpawnWave/SpawnThroughPortal only, so shared logic
// (spawn trickle, completion) lives inside member bodies with free-function
// helpers that receive the arrays as parameters.
#include "AI/QinglanEncounterDirector.h"

#include "AI/QinglanAIController.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Crc.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Sound/SoundBase.h"

namespace
{
	// --- Encounter caps (docs/01 section 6; blueprint section 1.3) ---
	constexpr int32 MaxOrdinaryEnemies = 8;          // "At most eight active ordinary enemies"
	constexpr int32 MaxBossCount = 1;                // "...or one boss"
	constexpr int32 MaxBossAdds = 2;                 // "...with at most two adds"
	constexpr int32 MaxSimultaneousMeleeWindups = 3; // "<= 3 simultaneous melee windup" (docs/05 section 5)
	// "Ranged attacks have their own budget" (docs/05 section 5) - number
	// authored; the doc deliberately leaves it unnumbered.
	constexpr int32 MaxSimultaneousRangedAttackers = 4;

	// --- Portal spawn pacing (authored) ---
	constexpr float PortalStaggerIntervalSeconds = 0.5f; // one portal per beat
	constexpr float PortalSpawnRingMinCm = 600.f;
	constexpr float PortalSpawnRingMaxCm = 1200.f;
	constexpr int32 PortalPlacementAttempts = 8;         // deterministic retries

	// Authored: dead bodies return to the pool after this pause-time delay.
	constexpr float PoolReturnDelaySeconds = 2.5f;

	// Deterministic domain salt for the room encounter stream: same room ->
	// same spawn layout across runs (docs/01 section 6 same-seed rule).
	// FIXME-CONTRACT: the seed should come from the FRandomStream owned by
	// UQinglanRunSubsystem ("encounters" stream), but its scaffold header
	// exposes no stream getter and BeginEncounter() carries no seed; swap this
	// derivation for the run-owned stream once that API exists.
	constexpr uint32 EncounterDomainSalt = 0x514C414Eu; // "QLAN"

	// Placeholder portal warning sound (docs/05 section 3); null-safe.
	const TCHAR* GPortalWarningCuePath = TEXT("/Game/Qinglan/Audio/S_PortalWarning");

	/** Per-director state that cannot live in the frozen scaffold header. */
	struct FQinglanDirectorExtra
	{
		TArray<TObjectPtr<UQinglanEnemyDataAsset>> WaveQueue;
		TArray<TWeakObjectPtr<AQinglanEnemyCharacter>> EnemyPool;
		FRandomStream EncounterStream;
	};

	TMap<TWeakObjectPtr<AQinglanEncounterDirector>, FQinglanDirectorExtra> GDirectorExtras;

	FQinglanDirectorExtra& ExtraFor(AQinglanEncounterDirector* Director)
	{
		// Prune stale entries (level transitions, torn-down worlds).
		for (auto It = GDirectorExtras.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent();
			}
		}
		return GDirectorExtras.FindOrAdd(Director);
	}

	ACharacter* GetPlayer(AActor* WorldContext)
	{
		return (WorldContext && WorldContext->GetWorld())
			? Cast<ACharacter>(UGameplayStatics::GetPlayerCharacter(WorldContext, 0))
			: nullptr;
	}

	AQinglanAIController* ControllerOf(AQinglanEnemyCharacter* Enemy)
	{
		return Enemy ? Cast<AQinglanAIController>(Enemy->GetController()) : nullptr;
	}

	/** Count living ordinary enemies currently holding a slot of either budget. */
	int32 CountHolders(const TArray<TWeakObjectPtr<AQinglanEnemyCharacter>>& Enemies, bool bMelee)
	{
		int32 Count = 0;
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : Enemies)
		{
			AQinglanEnemyCharacter* Enemy = Ref.Get();
			AQinglanAIController* Controller = ControllerOf(Enemy);
			if (!Enemy || !Controller || !Enemy->IsAlive() || Controller->IsBoss())
			{
				continue; // ordinary enemies only
			}
			Count += bMelee ? (Controller->HoldsMeleeWindupSlot() ? 1 : 0)
				: (Controller->HoldsRangedAttackSlot() ? 1 : 0);
		}
		return Count;
	}

	/** Cap rule: <= 8 ordinary, or 1 boss + <= 2 adds (docs/01 section 6). */
	bool CanSpawnWith(const UQinglanEnemyDataAsset* Data,
		const TArray<TWeakObjectPtr<AQinglanEnemyCharacter>>& Living,
		const TArray<TWeakObjectPtr<AQinglanEnemyCharacter>>& Pending, bool bBossRoom)
	{
		int32 OrdinaryCount = 0;
		int32 BossCount = 0;
		auto Account = [&OrdinaryCount, &BossCount](AQinglanEnemyCharacter* Enemy)
		{
			if (!Enemy || !Enemy->IsAlive())
			{
				return;
			}
			if (ControllerOf(Enemy) && ControllerOf(Enemy)->IsBoss())
			{
				++BossCount;
			}
			else
			{
				++OrdinaryCount;
			}
		};
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : Living)
		{
			Account(Ref.Get());
		}
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : Pending)
		{
			Account(Ref.Get());
		}

		if (Cast<UQinglanBossDataAsset>(Data))
		{
			++BossCount;
		}
		else
		{
			++OrdinaryCount;
		}

		if (bBossRoom)
		{
			return BossCount <= MaxBossCount && OrdinaryCount <= MaxBossAdds;
		}
		return OrdinaryCount <= MaxOrdinaryEnemies && BossCount <= MaxBossCount;
	}

	/** Deterministic ring placement that respects the 4 m player exclusion (docs/01 section 6). */
	FVector PickPortalLocation(FRandomStream& Stream, const FVector& Anchor, const ACharacter* Player)
	{
		for (int32 Attempt = 0; Attempt < PortalPlacementAttempts; ++Attempt)
		{
			// Deterministic ring position from the seeded stream (no RandRange).
			const float AngleDeg = Stream.FRand() * 360.f;
			const float Radius = PortalSpawnRingMinCm +
				Stream.FRand() * (PortalSpawnRingMaxCm - PortalSpawnRingMinCm);
			const FVector Candidate = Anchor + FVector(
				FMath::Cos(FMath::DegreesToRadians(AngleDeg)) * Radius,
				FMath::Sin(FMath::DegreesToRadians(AngleDeg)) * Radius, 0.f);
			if (!Player ||
				FVector::Dist(Candidate, Player->GetActorLocation()) >= QinglanAITunables::PortalMinPlayerDistanceCm)
			{
				return Candidate;
			}
		}
		// Tiny room fallback: push straight out of the 4 m exclusion sphere.
		if (Player)
		{
			const FVector Away = (Anchor - Player->GetActorLocation()).GetSafeNormal2D();
			const FVector Base = Player->GetActorLocation() +
				Away * (QinglanAITunables::PortalMinPlayerDistanceCm + 25.f);
			return FVector(Base.X, Base.Y, Anchor.Z);
		}
		return Anchor;
	}
}

AQinglanEncounterDirector::AQinglanEncounterDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PortalWarningDuration = 0.8f; // "0.8-second warning" (docs/01 section 6)
}

void AQinglanEncounterDirector::BeginEncounter(const TArray<UQinglanEnemyDataAsset*>& Wave,
	int32 RoomIndex, bool bIsBossRoom)
{
	FQinglanDirectorExtra& Extra = ExtraFor(this);

	if (bActive && !bCompleted)
	{
		// Restarting mid-encounter: release arbitration without rewards
		// (defensive; realm flow should complete rooms before re-entering).
		UE_LOG(LogQinglanAI, Warning, TEXT("BeginEncounter restarted an active encounter"));
		ReleaseAttackSlots();
	}

	LivingEnemies.Reset();
	PendingSpawns.Reset();
	MeleeWindupCount = 0;
	RangedActiveCount = 0;
	CurrentRoomIndex = RoomIndex;
	bBossRoom = bIsBossRoom;
	bActive = true;
	bCompleted = false;

	Extra.WaveQueue.Reset();
	for (UQinglanEnemyDataAsset* Data : Wave)
	{
		if (Data)
		{
			Extra.WaveQueue.Add(Data);
		}
	}

	// Deterministic room seed (see FIXME-CONTRACT above).
	const uint32 RoomSeed = FCrc::StrCrc32(
		*FString::Printf(TEXT("qinglan.encounter.room.%d"), RoomIndex), EncounterDomainSalt);
	Extra.EncounterStream.Initialize(static_cast<int32>(RoomSeed));

	UE_LOG(LogQinglanAI, Log, TEXT("Encounter begin: room %d, boss room %d, wave size %d"),
		RoomIndex, static_cast<int32>(bIsBossRoom), Extra.WaveQueue.Num());

	SpawnWave();
}

void AQinglanEncounterDirector::SpawnWave()
{
	// Arms the trickle: the next Tick opens the first portal immediately and
	// continues one portal per PortalStaggerIntervalSeconds while caps allow.
	SpawnTimer = PortalStaggerIntervalSeconds;
}

void AQinglanEncounterDirector::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bActive)
	{
		return;
	}
	FQinglanDirectorExtra& Extra = ExtraFor(this);

	// Paused-game-time semantics: accumulate DeltaTime, never wall clock.
	SpawnTimer += DeltaTime;

	// 1) Trickle the wave through portals while caps allow.
	if (SpawnTimer >= PortalStaggerIntervalSeconds)
	{
		SpawnTimer = 0.f;
		if (!Extra.WaveQueue.IsEmpty())
		{
			UQinglanEnemyDataAsset* Next = Extra.WaveQueue[0].Get();
			if (!Next)
			{
				Extra.WaveQueue.RemoveAt(0);
			}
			else if (CanSpawnWith(Next, LivingEnemies, PendingSpawns, bBossRoom))
			{
				Extra.WaveQueue.RemoveAt(0);
				SpawnThroughPortal(Next, PickPortalLocation(Extra.EncounterStream,
					GetActorLocation(), GetPlayer(this)));
			}
		}
	}

	// 2) Promote materialized portal spawns to living.
	for (int32 Index = PendingSpawns.Num() - 1; Index >= 0; --Index)
	{
		AQinglanEnemyCharacter* Enemy = PendingSpawns[Index].Get();
		AQinglanAIController* Controller = ControllerOf(Enemy);
		if (!Enemy || !Controller)
		{
			PendingSpawns.RemoveAtSwap(Index);
			continue;
		}
		if (!Controller->IsPortalPending())
		{
			LivingEnemies.Add(Enemy);
			PendingSpawns.RemoveAtSwap(Index);
		}
	}

	// 3) Return dead enemies to the pool after the body delay.
	for (int32 Index = LivingEnemies.Num() - 1; Index >= 0; --Index)
	{
		AQinglanEnemyCharacter* Enemy = LivingEnemies[Index].Get();
		AQinglanAIController* Controller = ControllerOf(Enemy);
		if (!Enemy || !Controller)
		{
			LivingEnemies.RemoveAtSwap(Index);
			continue;
		}
		if (!Enemy->IsAlive() && Controller->GetDeathAgeSeconds() >= PoolReturnDelaySeconds)
		{
			Enemy->ResetForPool();
			LivingEnemies.RemoveAtSwap(Index);
			Extra.EnemyPool.Add(Enemy);
		}
	}

	// 4) Re-sync arbitration counters (holders are tracked per controller).
	MeleeWindupCount = CountHolders(LivingEnemies, true);
	RangedActiveCount = CountHolders(LivingEnemies, false);

	// 5) Completion: simultaneous last-kill opens door + reward exactly once,
	// and never while portals or queue entries remain (no deadlock)
	// (docs/dev blueprint section 1.3 room contract).
	if (!bCompleted && GetLivingEnemyCount() == 0 && PendingSpawns.Num() == 0 && Extra.WaveQueue.IsEmpty())
	{
		bCompleted = true;
		bActive = false;
		ReleaseAttackSlots();
		OnEncounterCompleted.Broadcast();
		UE_LOG(LogQinglanAI, Log, TEXT("Encounter completed in room %d"), CurrentRoomIndex);

		UQinglanRunSubsystem* Run = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UQinglanRunSubsystem>()
			: nullptr;
		if (Run && Run->IsInExpedition())
		{
			// Room rewards (spirit shards per realm template) commit through
			// the run subsystem's single mutation path; the door interactable
			// listens to OnEncounterCompleted.
			Run->CompleteCurrentRoom(FName(*FString::Printf(TEXT("room.%d.clear"), CurrentRoomIndex)));
		}
	}
}

int32 AQinglanEncounterDirector::GetLivingEnemyCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : LivingEnemies)
	{
		if (const AQinglanEnemyCharacter* Enemy = Ref.Get())
		{
			if (Enemy->IsAlive())
			{
				++Count;
			}
		}
	}
	return Count;
}

void AQinglanEncounterDirector::NotifyEnemyDied(AQinglanEnemyCharacter* Enemy)
{
	if (!Enemy)
	{
		return;
	}

	// Release this enemy's slots immediately (budget release on death);
	// completion itself is centralized in Tick so simultaneous last-kills
	// resolve through one exactly-once path.
	if (AQinglanAIController* Controller = ControllerOf(Enemy))
	{
		Controller->ReleaseHeldSlots();
	}

	LivingEnemies.RemoveAll([Enemy](const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref)
	{
		return Ref.Get() == Enemy;
	});
	PendingSpawns.RemoveAll([Enemy](const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref)
	{
		return Ref.Get() == Enemy;
	});

	MeleeWindupCount = CountHolders(LivingEnemies, true);
	RangedActiveCount = CountHolders(LivingEnemies, false);
}

bool AQinglanEncounterDirector::RequestMeleeWindupSlot()
{
	// <= 3 ordinary simultaneous melee windups (docs/05 section 5). The count
	// derives from actual holders among living enemies, so a slot frees the
	// moment its holder's attack ends, aborts, staggers, or dies.
	MeleeWindupCount = CountHolders(LivingEnemies, true);
	return MeleeWindupCount < MaxSimultaneousMeleeWindups;
}

bool AQinglanEncounterDirector::RequestRangedAttackSlot()
{
	RangedActiveCount = CountHolders(LivingEnemies, false);
	return RangedActiveCount < MaxSimultaneousRangedAttackers;
}

void AQinglanEncounterDirector::ReleaseAttackSlots()
{
	// FIXME-CONTRACT: the scaffold's parameterless signature cannot identify a
	// specific holder, so this releases every slot (encounter end / restart).
	// Per-enemy release on attack end/death goes through
	// AQinglanAIController::ReleaseHeldSlots().
	auto ReleaseAll = [this](const TArray<TWeakObjectPtr<AQinglanEnemyCharacter>>& Group)
	{
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : Group)
		{
			if (AQinglanAIController* Controller = ControllerOf(Ref.Get()))
			{
				Controller->ReleaseHeldSlots();
			}
		}
	};
	ReleaseAll(LivingEnemies);
	ReleaseAll(PendingSpawns);
	MeleeWindupCount = 0;
	RangedActiveCount = 0;
}

void AQinglanEncounterDirector::SpawnThroughPortal(UQinglanEnemyDataAsset* Data, const FVector& Location)
{
	if (!Data || !GetWorld())
	{
		return;
	}
	FQinglanDirectorExtra& Extra = ExtraFor(this);

	// Enforce the 4 m rule at the final location as well (docs/01 section 6).
	FVector SpawnLocation = Location;
	if (ACharacter* Player = GetPlayer(this))
	{
		const float Distance = FVector::Dist(SpawnLocation, Player->GetActorLocation());
		if (Distance < QinglanAITunables::PortalMinPlayerDistanceCm)
		{
			const FVector Away = (SpawnLocation - Player->GetActorLocation()).GetSafeNormal2D();
			SpawnLocation = Player->GetActorLocation() +
				Away * (QinglanAITunables::PortalMinPlayerDistanceCm + 25.f);
		}
	}

	// Reuse a pooled enemy when one fits; otherwise spawn fresh.
	AQinglanEnemyCharacter* Enemy = nullptr;
	while (!Extra.EnemyPool.IsEmpty())
	{
		AQinglanEnemyCharacter* Candidate = Extra.EnemyPool.Last().Get();
		Extra.EnemyPool.Pop(EAllowShrinking::No);
		if (Candidate && ControllerOf(Candidate))
		{
			Enemy = Candidate;
			break;
		}
	}
	if (!Enemy)
	{
		FActorSpawnParameters Params;
		Params.Owner = this;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Enemy = GetWorld()->SpawnActor<AQinglanEnemyCharacter>(
			AQinglanEnemyCharacter::StaticClass(), SpawnLocation, FRotator::ZeroRotator, Params);
	}
	if (!Enemy)
	{
		UE_LOG(LogQinglanAI, Error, TEXT("SpawnThroughPortal failed for %s"), *Data->EnemyId.ToString());
		return;
	}

	AQinglanAIController* Controller = ControllerOf(Enemy);
	if (!Controller)
	{
		UE_LOG(LogQinglanAI, Error, TEXT("Spawned enemy without an AQinglanAIController"));
		Enemy->Destroy();
		return;
	}

	// Face the room anchor on materialization (deterministic, no randomness).
	const FRotator Facing = (GetActorLocation() - SpawnLocation).Rotation();
	Enemy->SetActorLocation(SpawnLocation);
	Enemy->SetActorRotation(FRotator(0.f, Facing.Yaw, 0.f));
	Enemy->SetActorHiddenInGame(true); // behind the portal until it materializes
	Enemy->SetActorEnableCollision(false);
	Enemy->SetActorTickEnabled(false);

	Enemy->InitializeFromData(Data);
	// Per-enemy behavior stream seeded from the room's encounter stream
	// (docs/01 section 6; determinism contract).
	Controller->Initialize(this, static_cast<int32>(Extra.EncounterStream.GetUnsignedInt()));
	Controller->BeginPortalWarning(PortalWarningDuration);

	PendingSpawns.Add(Enemy);

	// Portal warning: always audible (placeholder asset, null-safe); visual
	// cue ids route through quality tiers on the presentation side
	// (code-contracts; docs/06 section 6).
	if (USoundBase* Cue = Cast<USoundBase>(FSoftObjectPath(GPortalWarningCuePath).TryLoad()))
	{
		UGameplayStatics::PlaySoundAtLocation(this, Cue, SpawnLocation);
	}
	UE_LOG(LogQinglanAI, Log, TEXT("Portal opened for %s (%.1f s warning)"),
		*Data->EnemyId.ToString(), PortalWarningDuration);
}
