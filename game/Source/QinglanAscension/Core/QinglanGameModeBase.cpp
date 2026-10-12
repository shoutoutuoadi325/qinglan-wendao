// Hub and realm game modes implementation.
//
// Docs: docs/dev/level-layout.md (hub = sanctuary with training courtyard,
// cultivation stations, quest NPCs; realm maps are 7-room expeditions),
// docs/dev/implementation-blueprint.md section 1.6 ("crash in active room ->
// resume room entrance from pre-room snapshot without respawn of committed
// rewards"; hub is a safe checkpoint boundary) and section 1.3/1.4 (the
// encounter director owns one room's wave lifecycle).
//
// FIXME-CONTRACT: the frozen scaffold headers declare no members, so the
// per-mode encounter director reference lives in a TU-local weak-keyed map
// (house pattern; see AI/QinglanEncounterDirector.cpp).
#include "Core/QinglanGameModeBase.h"

#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanAscension.h"

namespace
{
	// PlayerStart tag convention for room entrances (docs/dev/level-layout.md
	// room 0 "entrance (bamboo gate arrival, loadout shrine)" and each room's
	// arrival point). Exact tag: "Qinglan.RoomEntrance" or the per-room form
	// "Qinglan.RoomEntrance.<Index>".
	// FIXME-CONTRACT(data): no design doc pins the tag names; level authors
	// must place PlayerStarts matching this prefix.
	const TCHAR* GRoomEntranceTagPrefix = TEXT("Qinglan.RoomEntrance");

	TMap<TWeakObjectPtr<AQinglanRealmGameMode>, TWeakObjectPtr<AQinglanEncounterDirector>>& RealmDirectors()
	{
		static TMap<TWeakObjectPtr<AQinglanRealmGameMode>, TWeakObjectPtr<AQinglanEncounterDirector>> Map;
		return Map;
	}

	AQinglanEncounterDirector* FindDirectorInWorld(UWorld* World)
	{
		return World
			? Cast<AQinglanEncounterDirector>(
				UGameplayStatics::GetActorOfClass(World, AQinglanEncounterDirector::StaticClass()))
			: nullptr;
	}

	/**
	 * Best room-entrance PlayerStart: prefer the exact room tag, then any
	 * entrance-tagged start, then null (caller falls back to the default
	 * start selection).
	 */
	APlayerStart* FindRoomEntranceStart(UWorld* World, int32 RoomIndex)
	{
		if (!World)
		{
			return nullptr;
		}
		const FString Prefix(GRoomEntranceTagPrefix);
		APlayerStart* AnyEntrance = nullptr;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			APlayerStart* Start = *It;
			if (!Start)
			{
				continue;
			}
			if (Start->PlayerStartTag == FName(*FString::Printf(TEXT("%s.%d"), *Prefix, RoomIndex)))
			{
				return Start;
			}
			if (!AnyEntrance && Start->PlayerStartTag.ToString().StartsWith(Prefix))
			{
				AnyEntrance = Start;
			}
		}
		return AnyEntrance;
	}
}

// ---------------------------------------------------------------------------
// AQinglanHubGameMode
// ---------------------------------------------------------------------------

AQinglanHubGameMode::AQinglanHubGameMode()
{
	// Pure C++ pawn: no Blueprint variant required (headless-authored content).
	DefaultPawnClass = AQinglanCharacter::StaticClass();
}

void AQinglanHubGameMode::BeginPlay()
{
	Super::BeginPlay();
	UE_LOG(LogQinglan, Log, TEXT("Hub mode active (sanctuary)"));

	UGameInstance* Instance = GetGameInstance();

	// Defensive diagnostic: entering the hub while an expedition is still open
	// means the exit/banking flow was bypassed (blueprint section 1.5 banking
	// rules; docs/03 progression and economy).
	if (UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr)
	{
		if (Run->IsInExpedition())
		{
			UE_LOG(LogQinglan, Warning, TEXT("Hub entered while an expedition is still open; loot stays unbanked until EndExpedition"));
		}
	}

	// Hub arrival is a safe checkpoint boundary (blueprint section 1.6
	// "Safe checkpoints: entrance, completed-room reward screen, exit, hub,
	// trial prep only"): commit the profile now through the save subsystem's
	// durable write path.
	if (UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr)
	{
		FText Notice;
		if (!Save->SaveNow(Notice))
		{
			// Never fatal at a safe boundary: the previous primary remains
			// valid and the UI surfaces the notice (error.save_failed).
			UE_LOG(LogQinglan, Warning, TEXT("Hub autosave did not commit"));
		}
	}
}

// ---------------------------------------------------------------------------
// AQinglanRealmGameMode
// ---------------------------------------------------------------------------

AQinglanRealmGameMode::AQinglanRealmGameMode()
{
	DefaultPawnClass = AQinglanCharacter::StaticClass();
}

void AQinglanRealmGameMode::BeginPlay()
{
	Super::BeginPlay();

	TMap<TWeakObjectPtr<AQinglanRealmGameMode>, TWeakObjectPtr<AQinglanEncounterDirector>>& Directors = RealmDirectors();
	for (auto It = Directors.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// One encounter director per realm world: reuse a level-placed one when
	// present (authored rooms), otherwise spawn at the room-0 entrance.
	AQinglanEncounterDirector* Director = FindDirectorInWorld(GetWorld());
	if (!Director)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			APlayerStart* Anchor = FindRoomEntranceStart(World, 0);
			const FVector Location = Anchor ? Anchor->GetActorLocation() : FVector::ZeroVector;
			const FRotator Rotation = Anchor ? Anchor->GetActorRotation() : FRotator::ZeroRotator;
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.Name = TEXT("QinglanEncounterDirector");
			Director = World->SpawnActor<AQinglanEncounterDirector>(
				AQinglanEncounterDirector::StaticClass(), Location, Rotation, Params);
			UE_LOG(LogQinglan, Log, TEXT("Realm mode spawned encounter director"));
		}
	}
	else
	{
		UE_LOG(LogQinglan, Log, TEXT("Realm mode reused level-placed encounter director"));
	}

	if (Director)
	{
		Directors.Add(this, Director);
	}
	else
	{
		UE_LOG(LogQinglan, Error, TEXT("Realm mode has no encounter director; rooms will not spawn encounters"));
	}
}

void AQinglanRealmGameMode::RestartRoomFromSnapshot()
{
	// Crash-resume path (blueprint section 1.6): restore the player at the
	// room entrance from the run subsystem checkpoint with pre-room
	// attributes; committed room rewards never respawn (run subsystem
	// transaction idempotency).
	UGameInstance* Instance = GetGameInstance();
	UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (!Run || !Run->IsInExpedition())
	{
		UE_LOG(LogQinglan, Warning, TEXT("RestartRoomFromSnapshot: no active expedition to resume"));
		return;
	}

	FQinglanCheckpoint Checkpoint;
	Run->ExportCheckpoint(Checkpoint);
	if (Checkpoint.RealmId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("RestartRoomFromSnapshot: checkpoint carries no realm"));
		return;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;

	// 1) Position: room entrance PlayerStart for the checkpointed room.
	APlayerStart* Entrance = FindRoomEntranceStart(World, Checkpoint.RoomIndex);
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (Entrance && PC)
	{
		if (Pawn)
		{
			Pawn->SetActorLocationAndRotation(Entrance->GetActorLocation(), Entrance->GetActorRotation(), false, nullptr, ETeleportType::TeleportPhysics);
		}
		else
		{
			// Pawn destroyed mid-room (death/crash edge): respawn in place.
			RestartPlayerAtPlayerStart(PC, Entrance);
			Pawn = PC->GetPawn();
		}
	}
	else if (PC)
	{
		RestartPlayer(PC); // default start selection as a last resort
		Pawn = PC->GetPawn();
	}

	// 2) Vitals: pre-room attribute snapshot (health/energy/stamina as they
	// were when the room was entered; FQinglanCheckpoint.PlayerAttributes).
	AQinglanCharacter* Player = Cast<AQinglanCharacter>(Pawn);
	if (UQinglanCombatComponent* Combat = Player ? Player->GetCombat() : nullptr)
	{
		Combat->SetDerivedAttributes(Checkpoint.PlayerAttributes);
	}
	// 3) Statuses: an interrupted active room may have died mid-Burn/Wet/Root;
	// entrance resumes clean (status semantics: code-contracts.md combat).
	if (UQinglanStatusComponent* Status = Player ? Player->GetStatus() : nullptr)
	{
		for (EQinglanStatus St : { EQinglanStatus::Wet, EQinglanStatus::Burn,
				EQinglanStatus::Root, EQinglanStatus::Conduct })
		{
			if (Status->HasStatus(St))
			{
				Status->RemoveStatus(St);
			}
		}
	}

	// 4) Room pipeline: re-arm the checkpointed room when it is not already
	// active. CommittedRoomIds keeps already-awarded room rewards from paying
	// twice (run subsystem idempotency), so this cannot "respawn" committed
	// rewards.
	if (Run->GetCurrentRoomIndex() != Checkpoint.RoomIndex)
	{
		Run->EnterRoom(Checkpoint.RoomIndex);
	}

	UE_LOG(LogQinglan, Log, TEXT("RestartRoomFromSnapshot: realm %s room %d restored"),
		*Checkpoint.RealmId.ToString(), Checkpoint.RoomIndex);
}
