#pragma once

#include "CoreMinimal.h"
#include "EnhancedPlayerInput.h"
#include "QinglanPlayerInput.generated.h"

// Keep held digital keys suppressed across a modal flush until their release.
// UE's local-player flush synthesizes releases through virtual InputKey; those
// releases must not count as the player letting go of an action.
UCLASS()
class QINGLANASCENSION_API UQinglanPlayerInput : public UEnhancedPlayerInput
{
	GENERATED_BODY()

public:
	virtual void FlushPressedKeys() override;
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

private:
	TSet<FKey> PhysicallyHeldDigitalKeys;
	TSet<FKey> HeldKeysAwaitingRelease;
	bool bSynthesizingFlushReleases = false;
};
