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
