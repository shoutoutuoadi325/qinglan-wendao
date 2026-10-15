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

#include "Core/QinglanAudioSubsystem.h"

#include "AI/QinglanEncounterDirector.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanStatusComponent.h"
#include "Core/QinglanInteractable.h"
#include "Core/QinglanRoomLayout.h"
#include "Misc/PackageName.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h" // FParse
#include "UnrealClient.h" // FScreenshotRequest
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Persistence/QinglanSaveGame.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
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
	static APlayerStart* FindRoomEntranceStart(UWorld* World, int32 RoomIndex)
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
			if (!AnyEntrance && Start->PlayerStartTag == FName(*Prefix))
			{
				AnyEntrance = Start;
			}
		}
		return RoomIndex == 0 ? AnyEntrance : nullptr;
	}
	APlayerStart* EnsureRoomEntrance(UWorld* World, int32 RoomIndex)
	{
		if (!World || RoomIndex < 0 || RoomIndex >= QinglanRoomLayout::RoomCount) { return nullptr; }
		if (APlayerStart* Existing = FindRoomEntranceStart(World, RoomIndex)) { return Existing; }
		// Old committed maps predate the per-room tags. Recreate their known
		// greybox entrance transform without requiring an asset regeneration.
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(),
			QinglanRoomLayout::RoomEntrance(RoomIndex), FRotator::ZeroRotator, Params);
		if (Start) { Start->PlayerStartTag = QinglanRoomLayout::EntranceTag(RoomIndex); }
		return Start;
	}

}


namespace
{
	/**
	 * -QinglanShot=<id>: capture one gameplay screenshot after a short settle
	 * and exit. Used for headless visual verification (docs/dev/build-and-run
	 * "GPU rendering") and CI image regression; harmless without the flag.
	 */
	void SetupQinglanShotMode(AActor* Mode)
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("QinglanShot")))
		{
			return;
		}
		FString ShotId = TEXT("map");
		FParse::Value(FCommandLine::Get(), TEXT("QinglanShot="), ShotId);
		UWorld* World = Mode ? Mode->GetWorld() : nullptr;
		if (!World)
		{
			return;
		}
		FTimerHandle Unused;
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(Mode, [World, ShotId]()
		{
			// Second tick so the first rendered frame is complete, then shoot.
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(World, [World, ShotId]()
			{
				const FString Path = FString::Printf(TEXT("/tmp/qinglan_shot_%s"), *ShotId);
				FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
				UE_LOG(LogQinglan, Log, TEXT("QinglanShot: requested %s.png"), *Path);
				// Give the readback two frames, then quit so batch runs terminate.
				World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(World, [World]()
				{
					World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(World, [World]()
					{
						if (APlayerController* PC = World->GetFirstPlayerController())
						{
							PC->ConsoleCommand(TEXT("quit"));
						}
					}));
				}));
			}));
		}));
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
	SetupQinglanShotMode(this);

	UGameInstance* Instance = GetGameInstance();

	// Sanctuary music (docs/04 audio pass); null-safe without imported audio.
	if (UQinglanAudioSubsystem* Audio = Instance ? Instance->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
	{
		Audio->SetMusicState(EQinglanMusicState::Hub);
	}

	// First arrival with no profile in any slot creates profile 0. This runs
	// here rather than in the game instance Init because subsystem creation
	// may not be complete during Init on the GameDefaultMap boot path; without
	// an active slot the safe-boundary autosave below has nothing to commit.
	if (Instance)
	{
		if (UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>())
		{
			// AUD-01: ordinary hub travel must retain the active in-memory profile.
			// TODO(AUD-01/AUD-51): move cold-start slot selection to the profile
			// frontend; this guard only prevents arrival from reloading slot 0.
			if (!Save->IsProfileSessionActive())
			{
				int32 FirstExistingSlot = INDEX_NONE;
				for (int32 Slot = 0; Slot < UQinglanSaveSubsystem::ProfileSlotCount; ++Slot)
				{
					if (Save->HasProfile(Slot)) { FirstExistingSlot = Slot; break; }
				}
				if (FirstExistingSlot == INDEX_NONE)
				{
					Save->CreateNewProfile(0);
				}
				else
				{
					FText Notice;
					if (!Save->LoadProfile(FirstExistingSlot, Notice))
					{
						UE_LOG(LogQinglan, Warning, TEXT("Initial profile load failed: %s"), *Notice.ToString());
					}
				}
			}
		}
	}

	// Resume loaded expeditions before hub refills/autosave can change their
	// entrance state. Failed save-before-travel leaves the request pending;
	// the player's interact key retries it after the disk issue is resolved.
	if (UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr)
	{
		if (Run->NeedsCheckpointResume())
		{
			const FName Map = Run->GetPendingCheckpointMap();
			if (!Map.IsNone())
			{
				QinglanInteraction::TravelToMap(this, *Map.ToString());
			}
			else
			{
				UE_LOG(LogQinglan, Error, TEXT("Cannot resume unsupported checkpoint realm"));
			}
			return;
		}
	}

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

	// Talisman charges refill for free at the sanctuary (docs/03 section 5);
	// the refill lands before the hub autosave below so it persists at this
	// safe boundary.
	if (UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr)
	{
		Profile->RefillTalismans();
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

	// Epilogue entry: the CH07 retrospective lives in the sanctuary (its
	// locations are authored prose, so no hub anchor matches them); beginning
	// the earliest uncompleted epilogue scene here keeps the chapter reachable
	// after the ending decision. BeginScene itself validates the CH06
	// prerequisites, so earlier visits are a Verbose no-op.
	if (UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		if (Story->HasFlag(FName(QinglanProgression::FoundationTrialPassedStoryFlag))
			&& !Story->IsSceneCompleted(FName(TEXT("CH03_S03"))))
		{
			Story->BeginScene(FName(TEXT("CH03_S03")));
		}
		else
		{
			QinglanInteraction::BeginFirstSceneOfChapters(Story, { FName(TEXT("CH07")) });
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
	SetupQinglanShotMode(this);

	// Realm exploration starts on the prepare loop; the encounter director
	// switches to Combat/Boss when waves begin (docs/04 audio pass).
	if (UQinglanAudioSubsystem* Audio = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
	{
		Audio->SetMusicState(EQinglanMusicState::Explore);
	}

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

	UGameInstance* Instance = GetGameInstance();
	UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;

	// Arm the room pipeline: StartExpedition captures a pre-travel entrance, and
	// the room-0 door can only advance from the current room (blueprint 1.4
	// "doors and rewards stay driven by exactly one trigger"). Room 0 is the
	// entrance (no wave), so this never starts a fight on arrival.
	if (Run && Run->NeedsCheckpointResume())
	{
		RequestCheckpointResume();
		// Defer chapter playback until the pawn and saved encounter are ready.
		return;
	}
	if (Run && Run->IsInExpedition() && Run->GetCurrentRoomIndex() <= 0)
	{
		Run->EnterRoom(0);
	}

	// Realm chapter entry: entering a realm map begins that realm's current
	// chapter scene (task contract; docs/dev/implementation-blueprint.md 1.9).
	// The active expedition names the realm; trial travel without an expedition
	// (the arena) falls back to the map's chapter set. BeginScene defers while
	// an encounter is active and validates prerequisites itself.
	if (UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		TArray<FName> Chapters;
		const UQinglanRealmDataAsset* Realm = (Run && Run->IsInExpedition()) ? Run->GetCurrentRealm() : nullptr;
		const FName RealmId = Realm ? Realm->RealmId : NAME_None;
		if (RealmId != NAME_None)
		{
			Chapters = QinglanInteraction::ChaptersForRealm(RealmId);
		}
		if (Chapters.IsEmpty())
		{
			Chapters = QinglanInteraction::ChaptersForMapName(GetWorld() ? GetWorld()->GetMapName() : FString());
		}
		if (!Chapters.IsEmpty())
		{
			QinglanInteraction::BeginFirstSceneOfChapters(Story, Chapters);
		}
	}
}

AActor* AQinglanRealmGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	UQinglanRunSubsystem* Run = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (Run && Run->IsInExpedition())
	{
		if (APlayerStart* Entrance = EnsureRoomEntrance(GetWorld(), FMath::Max(0, Run->GetCurrentRoomIndex())))
		{
			return Entrance;
		}
	}
	return Super::ChoosePlayerStart_Implementation(Player);
}

void AQinglanRealmGameMode::RequestCheckpointResume()
{
	ResumeRetryCount = 0;
	TryResumeCheckpoint();
}

void AQinglanRealmGameMode::TryResumeCheckpoint()
{
	UWorld* World = GetWorld();
	UQinglanRunSubsystem* Run = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
	if (!World || !Run || !Run->NeedsCheckpointResume()) { return; }
	const FName ExpectedMap = Run->GetPendingCheckpointMap();
	if (ExpectedMap.IsNone() || UGameplayStatics::GetCurrentLevelName(this, true) != FPackageName::GetShortName(ExpectedMap.ToString()))
	{
		UE_LOG(LogQinglan, Error, TEXT("Checkpoint resume refused in the wrong realm map"));
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	AQinglanCharacter* Player = PC ? Cast<AQinglanCharacter>(PC->GetPawn()) : nullptr;
	if (Player && Player->GetCombat() && RestartRoomFromSnapshot())
	{
		World->GetTimerManager().ClearTimer(ResumeRetryTimer);
		Run->MarkCheckpointResumed();
		TArray<FQinglanBlessing> Offers;
		if (Run->GetBlessingOffer(Offers)) { Player->PresentBlessingOffer(Offers); }
		else if (UQinglanStorySubsystem* Story = GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>())
		{
			const UQinglanRealmDataAsset* Realm = Run->GetCurrentRealm();
			if (Realm) { QinglanInteraction::BeginFirstSceneOfChapters(Story, QinglanInteraction::ChaptersForRealm(Realm->RealmId)); }
		}
		return;
	}
	if (++ResumeRetryCount < 100)
	{
		World->GetTimerManager().SetTimer(ResumeRetryTimer, this, &AQinglanRealmGameMode::TryResumeCheckpoint, 0.1f, false);
	}
	else
	{
		UE_LOG(LogQinglan, Error, TEXT("Checkpoint resume could not obtain its player/entrance; request remains pending"));
	}
}

bool AQinglanRealmGameMode::RestartRoomFromSnapshot()
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
		return false;
	}

	FQinglanCheckpoint Checkpoint;
	Run->ExportCheckpoint(Checkpoint);
	if (Checkpoint.RealmId == NAME_None)
	{
		UE_LOG(LogQinglan, Warning, TEXT("RestartRoomFromSnapshot: checkpoint carries no realm"));
		return false;
	}

	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;

	// 1) Position: room entrance PlayerStart for the checkpointed room.
	APlayerStart* Entrance = EnsureRoomEntrance(World, Checkpoint.RoomIndex);
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!PC || !Entrance) { return false; }
	if (!Pawn)
	{
		RestartPlayerAtPlayerStart(PC, Entrance);
		Pawn = PC->GetPawn();
	}
	AQinglanCharacter* Player = Cast<AQinglanCharacter>(Pawn);
	if (!Player || !Player->GetCombat()) { return false; }
	if (!Pawn->SetActorLocationAndRotation(Entrance->GetActorLocation(), Entrance->GetActorRotation(), false, nullptr, ETeleportType::TeleportPhysics))
	{
		return false;
	}

	// 2) Vitals: pre-room attribute snapshot (health/energy/stamina as they
	// were when the room was entered; FQinglanCheckpoint.PlayerAttributes).
	if (UQinglanCombatComponent* Combat = Player->GetCombat())
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
	// Import already set CurrentRoomIndex: equality does not mean the new
	// world's encounter was spawned. Re-enter after restoring the pawn;
	// committed rooms skip spawning, unfinished rooms recreate their wave.
	if (!Run->EnterRoom(Checkpoint.RoomIndex)) { return false; }

	UE_LOG(LogQinglan, Log, TEXT("RestartRoomFromSnapshot: realm %s room %d restored"),
		*Checkpoint.RealmId.ToString(), Checkpoint.RoomIndex);
	return true;
}
