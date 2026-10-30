#include "Combat/QinglanPlayerInput.h"
#include "EnhancedActionKeyMapping.h"
#include "InputTriggers.h"

void UQinglanPlayerInput::FlushPressedKeys()
{
	// The last physical event determines whether a key is held. A release
	// followed by a press in one frame is held; the opposite order is a tap.
	HeldKeysAwaitingRelease.Append(PhysicallyHeldDigitalKeys);

	// Leave the normal engine flush and its synthetic release dispatch intact.
	// Only real releases may remove the project's suppression latch.
	TGuardValue<bool> Guard(bSynthesizingFlushReleases, true);
	Super::FlushPressedKeys();
	for (const FKey& Key : HeldKeysAwaitingRelease)
	{
		if (FKeyState* State = GetKeyState(Key))
		{
			// A flush can precede key-map evaluation. Remove the still-held
			// digital press from both queues and its pending value, so it
			// cannot fire after the modal transition. Preserve release events
			// and completed taps; analog input follows the engine's policy.
			for (const EInputEvent Event : { IE_Pressed, IE_Repeat, IE_DoubleClick })
			{
				State->EventCounts[Event].Reset();
				State->EventAccumulator[Event].Reset();
			}
			State->RawValueAccumulator = FVector::ZeroVector;
			State->SampleCountAccumulator = 0;
		}
	}
}

bool UQinglanPlayerInput::InputKey(const FInputKeyEventArgs& Params)
{
	if (!bSynthesizingFlushReleases)
	{
		if (Params.Event == IE_Released)
		{
			PhysicallyHeldDigitalKeys.Remove(Params.Key);
			if (HeldKeysAwaitingRelease.Remove(Params.Key) > 0)
			{
				// Suppressed mappings may never evaluate their zero-value release,
				// so Pressed retains its old actuated LastValue. A release and
				// re-press in one frame then loses the new actuation edge. Reset
				// the player-instanced edge triggers on the real release only.
				for (const FEnhancedActionKeyMapping& Mapping : GetEnhancedActionMappingsView())
				{
					if (Mapping.Key != Params.Key) { continue; }
					for (UInputTrigger* Trigger : Mapping.Triggers)
					{
						if (Cast<UInputTriggerPressed>(Trigger)) { Trigger->LastValue.Reset(); }
					}
				}
			}
		}
		else if (Params.Key.IsDigital() && (Params.Event == IE_Pressed
			|| Params.Event == IE_Repeat || Params.Event == IE_DoubleClick))
		{
			PhysicallyHeldDigitalKeys.Add(Params.Key);
		}
		if (Params.Event != IE_Released && HeldKeysAwaitingRelease.Contains(Params.Key))
		{
			// Reject repeats before the engine can reconcile them into a new
			// press. This protects both Enhanced actions and native menu keys.
			return false;
		}
	}
	return Super::InputKey(Params);
}
