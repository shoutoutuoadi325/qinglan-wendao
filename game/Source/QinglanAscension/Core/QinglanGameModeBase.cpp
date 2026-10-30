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
#include "Core/QinglanTrialDirector.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
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
	 * and exit. -QinglanShotDelay=<seconds> defers that capture so a timed QA
	 * route (for example -QinglanTrial=90 wave 2) can be recorded; the hub
	 * world's own timer dies with the map travel. Used for rendered visual
	 * verification (docs/dev/build-and-run) and CI image regression; harmless
	 * without the flag.
	 */
	void SetupQinglanShotMode(AActor* Mode)
	{
		FString ShotId;
		const bool bNamedShot = FParse::Value(FCommandLine::Get(), TEXT("QinglanShot="), ShotId);
		// Param accepts bare switches only; the documented =<id> form must
		// be recognized through Value before checking the optional bare flag.
		if (!bNamedShot && !FParse::Param(FCommandLine::Get(), TEXT("QinglanShot")))
		{
			return;
		}
		if (ShotId.IsEmpty()) { ShotId = TEXT("map"); }
		float ShotDelaySeconds = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("QinglanShotDelay="), ShotDelaySeconds);
		ShotDelaySeconds = FMath::Max(0.f, ShotDelaySeconds);
		UWorld* World = Mode ? Mode->GetWorld() : nullptr;
		if (!World)
		{
			return;
		}
		FTimerHandle Unused;
		World->GetTimerManager().SetTimer(Unused, FTimerDelegate::CreateWeakLambda(Mode, [World, ShotId]()
		{
			// Sandboxed Mac packages cannot write outside their container, so an
			// absolute /tmp path fails silently. Keep the capture in the
			// project Saved tree (honors -UserDir) where the process can write.
			const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"),
				FString::Printf(TEXT("qinglan_shot_%s"), *ShotId));
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
		}), ShotDelaySeconds, /*bLoop=*/false);
	}

	/**
	 * -QinglanHandShot=<id>: QA close-up of the hero's sword hand for the
	 * AUD-55 UV/palette art check. A SceneCapture2D renders the hand at a near
	 * and a normal distance into Saved/Screenshots, then the process quits.
	 * It needs no player input, so it also works on a locked display.
	 * -QinglanHandShotDelay=<seconds> defaults to 3.
	 */
	void SetupQinglanHandShotMode(AActor* Mode)
	{
		FString ShotId;
		if (!FParse::Value(FCommandLine::Get(), TEXT("QinglanHandShot="), ShotId))
		{
			return;
		}
		if (ShotId.IsEmpty()) { ShotId = TEXT("hands"); }
		float DelaySeconds = 3.f;
		FParse::Value(FCommandLine::Get(), TEXT("QinglanHandShotDelay="), DelaySeconds);
		UWorld* World = Mode ? Mode->GetWorld() : nullptr;
		if (!World) { return; }
		FTimerHandle Unused;
		World->GetTimerManager().SetTimer(Unused, FTimerDelegate::CreateWeakLambda(Mode, [World, ShotId]()
		{
			AQinglanCharacter* Player = Cast<AQinglanCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
			USkeletalMeshComponent* Mesh = Player ? Player->GetMesh() : nullptr;
			if (!Mesh) { return; }
			const FName HandSocket(TEXT("sword_base"));
			FVector Anchor = FVector::ZeroVector;
			if (Mesh->DoesSocketExist(HandSocket))
			{
				Anchor = Mesh->GetSocketLocation(HandSocket);
			}
			else if (Mesh->GetBoneIndex(TEXT("hand_r")) != INDEX_NONE)
			{
				Anchor = Mesh->GetBoneLocation(TEXT("hand_r"));
			}
			else
			{
				return;
			}
			const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			// Three-quarter view from the character's front-right so both the
			// palm-side UV seam and the palette bands are readable.
			const FVector ViewDirection = (Player->GetActorForwardVector() * 0.7f
				+ Player->GetActorRightVector() * 0.7f + FVector(0.f, 0.f, 0.35f)).GetSafeNormal();
			struct FDistance { const TCHAR* Suffix; float Distance; };
			const FDistance Distances[] = { {TEXT("near"), 35.f}, {TEXT("normal"), 150.f} };
			for (const FDistance& Entry : Distances)
			{
				const FVector CameraLocation = Anchor + ViewDirection * Entry.Distance;
				ASceneCapture2D* Capture = World->SpawnActor<ASceneCapture2D>(
					ASceneCapture2D::StaticClass(), CameraLocation,
					(Anchor - CameraLocation).Rotation());
				if (!Capture) { continue; }
				UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Capture);
				// 8-bit LDR target so ExportRenderTarget writes PNG. The export
				// helper keys off RenderTargetFormat, which defaults to RGBA16f and
				// would otherwise write OpenEXR regardless of the extension.
				Target->RenderTargetFormat = RTF_RGBA8;
				Target->InitCustomFormat(512, 512, PF_B8G8R8A8, /*bForceLinearGamma=*/false);
				Target->ClearColor = FLinearColor::Black;
				USceneCaptureComponent2D* CaptureComponent = Capture->GetCaptureComponent2D();
				CaptureComponent->TextureTarget = Target;
				CaptureComponent->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
				CaptureComponent->bCaptureEveryFrame = false;
				CaptureComponent->bCaptureOnMovement = false;
				CaptureComponent->FOVAngle = 45.f;
				CaptureComponent->CaptureScene();
				UKismetRenderingLibrary::ExportRenderTarget(World, Target, Directory,
					FString::Printf(TEXT("qinglan_hand_%s_%s.png"), *ShotId, Entry.Suffix));
				Capture->Destroy();
			}
			UE_LOG(LogQinglan, Log, TEXT("QinglanHandShot: wrote near/normal hand captures for %s"), *ShotId);
			// Let both PNG writers finish before exiting (a same-tick quit can
			// truncate the second export).
			FTimerHandle QuitHandle;
			World->GetTimerManager().SetTimer(QuitHandle, FTimerDelegate::CreateWeakLambda(World, [World]()
			{
				if (APlayerController* PC = World->GetFirstPlayerController())
				{
					PC->ConsoleCommand(TEXT("quit"));
				}
			}), 1.f, /*bLoop=*/false);
		}), FMath::Max(0.f, DelaySeconds), /*bLoop=*/false);
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
	SetupQinglanHandShotMode(this);

	// QA capture (`-QinglanTrial=`): travel straight to the arena. Packaged
	// builds ignore a positional map URL when GameDefaultMap is set, so the
	// capture flag performs the travel itself; the realm mode then starts the
	// trial (see AQinglanRealmGameMode::BeginPlay).
	FString TrialCaptureValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("QinglanTrial="), TrialCaptureValue))
	{
		UE_LOG(LogQinglan, Log, TEXT("QinglanTrial QA: traveling to L_TribulationArena"));
		UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/Qinglan/Maps/L_TribulationArena")));
		return;
	}

	UGameInstance* Instance = GetGameInstance();

	// Sanctuary music (docs/04 audio pass); null-safe without imported audio.
	if (UQinglanAudioSubsystem* Audio = Instance ? Instance->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
	{
		Audio->SetMusicState(EQinglanMusicState::Hub);
	}

	// AUD-01: cold start never auto-loads or auto-creates a profile. The slot
	// is chosen once in the profile frontend (opened by AQinglanCharacter);
	// until then the arrival work below must not run, because there is no
	// active slot to autosave or resume into.
	if (Instance)
	{
		if (UQinglanSaveSubsystem* Save = Instance->GetSubsystem<UQinglanSaveSubsystem>())
		{
			if (!Save->IsProfileSessionActive())
			{
				UE_LOG(LogQinglan, Log,
					TEXT("Hub cold start: awaiting profile selection at the frontend (AUD-01)"));
				return;
			}
		}
	}

	CompleteHubArrivalAfterProfileSelection();
}

void AQinglanHubGameMode::CompleteHubArrivalAfterProfileSelection()
{
	if (bHubArrivalCompleted)
	{
		return;
	}
	UGameInstance* Instance = GetGameInstance();
	UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	if (!Save || !Save->IsProfileSessionActive())
	{
		UE_LOG(LogQinglan, Warning,
			TEXT("Hub arrival work skipped: no active profile session"));
		return;
	}
	bHubArrivalCompleted = true;

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
	// after the ending decision. CH05 rides along so its hub-anchored beats
	// (e.g. CH05_S04 at the trial gate) still surface on later arrivals even
	// if the arena was entered from another route. BeginScene itself validates
	// the chapter prerequisites, so earlier visits are a Verbose no-op.
	if (UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		if (Story->HasFlag(FName(QinglanProgression::FoundationTrialPassedStoryFlag))
			&& !Story->IsSceneCompleted(FName(TEXT("CH03_S03"))))
		{
			Story->BeginScene(FName(TEXT("CH03_S03")));
		}
		else
		{
			QinglanInteraction::BeginFirstSceneOfChapters(Story, { FName(TEXT("CH05")), FName(TEXT("CH07")) });
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
	SetupQinglanHandShotMode(this);

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

	// QA capture hook (AUD-42/55): `-QinglanTrial=<seconds>` starts the
	// foundation survival trial in the arena world once the pawn exists, so
	// lightning hazards, telegraphs and wave enemies can be captured without
	// an interactive run. Harmless unless the flag is passed on the command
	// line (used by docs/dev/build-and-run rendered-QA commands).
	FString TrialCaptureSeconds;
	if (FParse::Value(FCommandLine::Get(), TEXT("QinglanTrial="), TrialCaptureSeconds))
	{
		// Foundation authored contract: 90 s, tier 1. The hook bypasses the
		// progression gate only (admission checks stay in every gameplay call).
		const float CaptureDuration = 90.f;
		FTimerHandle TrialQaHandle;
		GetWorldTimerManager().SetTimer(TrialQaHandle, FTimerDelegate::CreateWeakLambda(this, [this, CaptureDuration]()
		{
			UWorld* World = GetWorld();
			AQinglanCharacter* Player = World
				? Cast<AQinglanCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0)) : nullptr;
			if (!World || !Player)
			{
				UE_LOG(LogQinglan, Warning, TEXT("QinglanTrial QA: no player pawn; trial not started"));
				return;
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Params.Name = TEXT("QinglanTrialDirector_QA");
			AQinglanTrialDirector* Director = World->SpawnActor<AQinglanTrialDirector>(
				AQinglanTrialDirector::StaticClass(), Player->GetActorLocation(), FRotator::ZeroRotator, Params);
			if (Director && Director->BeginTrial(FName(TEXT("CH03_S02")),
					FName(TEXT("CH03_S02_OBJECTIVE")), CaptureDuration, /*Tier=*/1,
					/*bSkipAdmissionForCapture=*/true))
			{
				UE_LOG(LogQinglan, Log, TEXT("QinglanTrial QA: foundation trial started (%.0fs)"), CaptureDuration);
			}
			else
			{
				UE_LOG(LogQinglan, Error, TEXT("QinglanTrial QA: BeginTrial failed"));
			}
		}), 3.f, /*bLoop=*/false);
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
		Combat->RestoreCheckpointAttributes(Checkpoint.PlayerAttributes);
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
