// Game instance: bootstrap, profile selection, travel coordination.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "QinglanGameInstance.generated.h"

UCLASS()
class UQinglanGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

	/** Applies the persisted graphics preset early, before any heavy scene loads. */
	void ApplyEarlyPlatformSettings();

	/** Hosts the first map respecting first-launch flow (benchmark offer vs direct hub). */
	void StartFirstMap();
};
