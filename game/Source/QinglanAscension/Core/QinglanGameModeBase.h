// Base game modes: hub (sanctuary) and realm (expedition).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "TimerManager.h"
#include "QinglanGameModeBase.generated.h"

/**
 * Hub mode: sanctuary with training courtyard, cultivation stations, quest NPCs.
 * Realm mode: expedition rooms; owns the encounter director lifecycle.
 */
UCLASS()
class AQinglanHubGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	AQinglanHubGameMode();
	virtual void BeginPlay() override;
	/**
	 * AUD-01: arrival work (checkpoint resume, talisman refill, autosave,
	 * epilogue entry) that requires an active profile. Cold start defers it
	 * until the player has selected a slot in the profile frontend; ordinary
	 * hub travel calls it directly from BeginPlay.
	 */
	void CompleteHubArrivalAfterProfileSelection();

private:
	bool bHubArrivalCompleted = false;
};

UCLASS()
class AQinglanRealmGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	AQinglanRealmGameMode();
	virtual void BeginPlay() override;
	/** Respawns at room entrance with pre-room snapshot on crash-resume. */
	bool RestartRoomFromSnapshot();
	/** Retries a loaded checkpoint after possession and room authoring are ready. */
	void RequestCheckpointResume();
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

private:
	void TryResumeCheckpoint();
	FTimerHandle ResumeRetryTimer;
	int32 ResumeRetryCount = 0;
};
