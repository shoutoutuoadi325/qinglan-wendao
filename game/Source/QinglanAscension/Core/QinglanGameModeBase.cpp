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
			bool bAnyProfile = false;
			for (int32 Slot = 0; Slot < UQinglanSaveSubsystem::ProfileSlotCount; ++Slot)
			{
				bAnyProfile |= Save->HasProfile(Slot);
			}
			if (!bAnyProfile)
			{
				UE_LOG(LogQinglan, Log, TEXT("First launch: creating profile slot 0"));
				Save->CreateNewProfile(0);
			}
			else if (Save->HasProfile(0))
			{
				// Boot default: continue from slot 0 (the profile-select menu is
				// a UI feature; without a load the safe-boundary autosave would
				// have no active slot on every non-first launch).
				FText Notice;
				if (Save->LoadProfile(0, Notice))
				{
					UE_LOG(LogQinglan, Log, TEXT("Continuing from profile slot 0"));
				}
				else
				{
					UE_LOG(LogQinglan, Warning, TEXT("Profile slot 0 failed to load: %s"), *Notice.ToString());
				}
			}
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
		const TArray<FName> EpilogueChapters = { FName(TEXT("CH07")) };
		QinglanInteraction::BeginFirstSceneOfChapters(Story, EpilogueChapters);
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

	// Arm the room pipeline: StartExpedition leaves the room index unset, and
	// the room-0 door can only advance from the current room (blueprint 1.4
	// "doors and rewards stay driven by exactly one trigger"). Room 0 is the
	// entrance (no wave), so this never starts a fight on arrival.
	if (Run && Run->IsInExpedition() && Run->GetCurrentRoomIndex() == INDEX_NONE)
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
