// Encounter director implementation: portal spawns with warnings, enemy caps,
// melee/ranged arbitration budgets, exactly-once completion, enemy pooling.
//
// Docs: docs/01 section 6 (encounter selection, caps, portals),
// docs/05 section 5 (AI arbitration, pooled projectiles/portals),
// docs/dev/implementation-blueprint.md section 1.3.
//
// FIXME-CONTRACT: the frozen scaffold header has no members for the
// enemy pool or the seeded encounter stream, and exposes no
// per-enemy release for ReleaseAttackSlots(). That state therefore lives in
// a TU-local registry keyed by weak director pointers (pruned every tick);
// merge it into the class when the headers unfreeze. The scaffold also
// declares private helpers SpawnWave/SpawnThroughPortal only, so shared logic
// (spawn trickle, completion) lives inside member bodies with free-function
// helpers that receive the arrays as parameters.
#include "AI/QinglanEncounterDirector.h"

#include "AI/QinglanAIController.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Core/QinglanAudioSubsystem.h"
#include "Core/QinglanAudioAssets.h"
#include "Core/QinglanRoomLayout.h"
#include "Components/SceneComponent.h"
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

	/** Per-director state that cannot live in the frozen scaffold header. */
	struct FQinglanDirectorExtra
	{
		TArray<TWeakObjectPtr<AQinglanEnemyCharacter>> EnemyPool;
		TArray<TWeakObjectPtr<AQinglanEnemyCharacter>> RetiredEnemies;
		FRandomStream EncounterStream;
		/** Portal ring band around the director; expedition default above. */
		float RingMinCm = PortalSpawnRingMinCm;
		float RingMaxCm = PortalSpawnRingMaxCm;
		bool bConfineToRealmRoom = false;
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
	FVector PickPortalLocation(FRandomStream& Stream, const FVector& Anchor, const ACharacter* Player,
		float RingMinCm, float RingMaxCm, bool bConfineToRealmRoom)
	{
		auto Confine = [&](FVector Candidate)
		{
			if (bConfineToRealmRoom)
			{
				Candidate.X = FMath::Clamp(Candidate.X, Anchor.X - QinglanRoomLayout::PortalHalfExtentCm, Anchor.X + QinglanRoomLayout::PortalHalfExtentCm);
				Candidate.Y = FMath::Clamp(Candidate.Y, Anchor.Y - QinglanRoomLayout::PortalHalfExtentCm, Anchor.Y + QinglanRoomLayout::PortalHalfExtentCm);
			}
			return Candidate;
		};
		for (int32 Attempt = 0; Attempt < PortalPlacementAttempts; ++Attempt)
		{
			// Deterministic ring position from the seeded stream (no RandRange).
			const float AngleDeg = Stream.FRand() * 360.f;
			const float Radius = RingMinCm + Stream.FRand() * (RingMaxCm - RingMinCm);
			const FVector Candidate = Confine(Anchor + FVector(
				FMath::Cos(FMath::DegreesToRadians(AngleDeg)) * Radius,
				FMath::Sin(FMath::DegreesToRadians(AngleDeg)) * Radius, 0.f));
			if (!Player ||
				FVector::Dist2D(Candidate, Player->GetActorLocation()) >= QinglanAITunables::PortalMinPlayerDistanceCm)
			{
				return Candidate;
			}
		}
		// Stay on the floor: choose the ring point opposite the player. Pushing
		// outward from the player's position could land outside the room entirely.
		if (Player)
		{
			FVector Away = (Anchor - Player->GetActorLocation()).GetSafeNormal2D();
			if (Away.IsNearlyZero()) { Away = FVector::ForwardVector; }
			return Confine(Anchor + Away * FMath::Max(RingMaxCm, QinglanAITunables::PortalMinPlayerDistanceCm + 25.f));
		}
		return Anchor;
	}
}

AQinglanEncounterDirector::AQinglanEncounterDirector()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("EncounterRoot")));
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PortalWarningDuration = 0.8f; // "0.8-second warning" (docs/01 section 6)
}

void AQinglanEncounterDirector::SetRoomOrigin(const FVector& Origin)
{
	SetActorLocation(Origin);
	ExtraFor(this).bConfineToRealmRoom = true;
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
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : LivingEnemies)
		{
			if (AQinglanEnemyCharacter* Enemy = Ref.Get())
			{
				Enemy->ResetForPool();
				Extra.EnemyPool.AddUnique(Enemy);
			}
		}
		for (const TWeakObjectPtr<AQinglanEnemyCharacter>& Ref : PendingSpawns)
		{
			if (AQinglanEnemyCharacter* Enemy = Ref.Get())
			{
				Enemy->ResetForPool();
				Extra.EnemyPool.AddUnique(Enemy);
			}
		}
	}

	LivingEnemies.Reset();
	PendingSpawns.Reset();
	MeleeWindupCount = 0;
	RangedActiveCount = 0;
	CurrentRoomIndex = RoomIndex;
	bBossRoom = bIsBossRoom;
	bActive = true;
	bCompleted = false;

	// Music state follows the encounter (docs/04 audio pass); null-safe.
	if (UQinglanAudioSubsystem* Audio = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
	{
		Audio->SetMusicState(bIsBossRoom ? EQinglanMusicState::Boss : EQinglanMusicState::Combat);
	}

	WaveQueue.Reset();
	for (UQinglanEnemyDataAsset* Data : Wave)
	{
		if (Data)
		{
			WaveQueue.Add(Data);
		}
	}

	// Deterministic room seed (see FIXME-CONTRACT above).
	const uint32 RoomSeed = FCrc::StrCrc32(
		*FString::Printf(TEXT("qinglan.encounter.room.%d"), RoomIndex), EncounterDomainSalt);
	Extra.EncounterStream.Initialize(static_cast<int32>(RoomSeed));

	UE_LOG(LogQinglanAI, Log, TEXT("Encounter begin: room %d, boss room %d, wave size %d"),
		RoomIndex, static_cast<int32>(bIsBossRoom), WaveQueue.Num());

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

	FQinglanDirectorExtra& Extra = ExtraFor(this);
	// Death removes an enemy from encounter completion immediately, but its
	// body still needs collection after the delay, including after the last
	// kill made the director inactive or the player entered another room.
	for (int32 Index = Extra.RetiredEnemies.Num() - 1; Index >= 0; --Index)
	{
		AQinglanEnemyCharacter* Enemy = Extra.RetiredEnemies[Index].Get();
		AQinglanAIController* Controller = ControllerOf(Enemy);
		if (!Enemy || !Controller)
		{
			Extra.RetiredEnemies.RemoveAtSwap(Index);
		}
		else if (Controller->GetDeathAgeSeconds() >= PoolReturnDelaySeconds)
		{
			Enemy->ResetForPool();
			Extra.EnemyPool.AddUnique(Enemy);
			Extra.RetiredEnemies.RemoveAtSwap(Index);
		}
	}
	if (!bActive)
	{
		return;
	}

	// Paused-game-time semantics: accumulate DeltaTime, never wall clock.
	SpawnTimer += DeltaTime;

	// 1) Trickle the wave through portals while caps allow.
	if (SpawnTimer >= PortalStaggerIntervalSeconds)
	{
		SpawnTimer = 0.f;
		if (!WaveQueue.IsEmpty())
		{
			UQinglanEnemyDataAsset* Next = WaveQueue[0].Get();
			if (!Next)
			{
				WaveQueue.RemoveAt(0);
			}
			else if (CanSpawnWith(Next, LivingEnemies, PendingSpawns, bBossRoom))
			{
				SpawnThroughPortal(Next, PickPortalLocation(Extra.EncounterStream,
					GetActorLocation() + (Extra.bConfineToRealmRoom ? FVector(0.f, 0.f, QinglanRoomLayout::PortalHeightCm) : FVector::ZeroVector),
					GetPlayer(this), Extra.RingMinCm, Extra.RingMaxCm, Extra.bConfineToRealmRoom));
				// Keep the queue reference until the new pawn owns its archetype.
				WaveQueue.RemoveAt(0);
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
	if (!bCompleted && GetLivingEnemyCount() == 0 && PendingSpawns.Num() == 0 && WaveQueue.IsEmpty())
	{
		bCompleted = true;
		bActive = false;
		ReleaseAttackSlots();
		OnEncounterCompleted.Broadcast();
		UE_LOG(LogQinglanAI, Log, TEXT("Encounter completed in room %d (living=%d livingAlive=%d pending=%d queued=%d bossRoom=%d)"),
			CurrentRoomIndex, LivingEnemies.Num(), GetLivingEnemyCount(), PendingSpawns.Num(),
			WaveQueue.Num(), bBossRoom ? 1 : 0);

		// Back to the exploration loop after a cleared room (docs/04 audio pass).
		if (UQinglanAudioSubsystem* Audio = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
		{
			Audio->SetMusicState(EQinglanMusicState::Explore);
		}

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
	ExtraFor(this).RetiredEnemies.AddUnique(Enemy);

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

	// PickPortalLocation already enforces exclusion and floor bounds in this
	// same tick. Never displace outward again after that validated selection.
	const FVector SpawnLocation = Location;

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
		// The pawn's automatic SpawnDefaultController can come up empty in
		// worlds without a full spawn context (bare automation fixtures).
		// The director owns enemy controller provisioning anyway, so supply
		// one explicitly instead of dropping the enemy. No Owner: a controller
		// owned by its pawn makes GetNetConnection() recurse forever
		// (pawn -> controller -> owner pawn -> ...).
		FActorSpawnParameters ControllerParams;
		ControllerParams.Instigator = Enemy->GetInstigator();
		ControllerParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ControllerParams.ObjectFlags |= RF_Transient;
		AQinglanAIController* Provisioned = GetWorld()->SpawnActor<AQinglanAIController>(ControllerParams);
		UE_LOG(LogQinglanAI, Warning, TEXT("Auto controller missing (begunPlay=%d); explicit provisioning %s"),
			GetWorld() ? (GetWorld()->HasBegunPlay() ? 1 : 0) : -1,
			Provisioned ? TEXT("succeeded") : TEXT("failed"));
		if (Provisioned != nullptr)
		{
			Provisioned->Possess(Enemy);
			Controller = Provisioned;
		}
	}
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

	// Portal warning: the imported telegraph cue is retained at every quality; visual
	// cue ids route through quality tiers on the presentation side
	// (code-contracts; docs/06 section 6).
	if (USoundBase* Cue = Cast<USoundBase>(FSoftObjectPath(QinglanAudioAssets::TelegraphWarning).TryLoad()))
	{
		UGameplayStatics::PlaySoundAtLocation(this, Cue, SpawnLocation);
	}
	UE_LOG(LogQinglanAI, Log, TEXT("Portal opened for %s (%.1f s warning)"),
		*Data->EnemyId.ToString(), PortalWarningDuration);
}

void SetEncounterSpawnRingOverride(AQinglanEncounterDirector* Director, float MinCm, float MaxCm)
{
	// Trial-only override (see the header); the expedition default band lives
	// in the constants above. Clamped so a bad call can never invert the band
	// or collapse it onto the anchor.
	if (!Director)
	{
		return;
	}
	FQinglanDirectorExtra& Extra = ExtraFor(Director);
	Extra.RingMinCm = FMath::Clamp(FMath::Min(MinCm, MaxCm), 100.f, 100000.f);
	Extra.RingMaxCm = FMath::Max(Extra.RingMinCm, FMath::Clamp(FMath::Max(MinCm, MaxCm), 100.f, 100000.f));
}
