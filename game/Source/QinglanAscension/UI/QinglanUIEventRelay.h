// UI event relay: binds typed dynamic delegates (subsystems, combat and
// technique components) and forwards them into the frozen scaffold widget
// classes.
//
// Dynamic multicast delegates bind UFUNCTIONs on this adapter. Each widget
// retains its relay through a reflected property; an Outer pointer alone does
// not keep a UObject alive during garbage collection. Construction rebinds the
// adapter and destruction unsubscribes it. One relay exists per widget and
// re-emits only what that widget subscribed to.
#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "UObject/Object.h"
#include "Core/QinglanCoreTypes.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "QinglanUIEventRelay.generated.h"

class UQinglanDialogueWidget;
class UQinglanHUDWidget;
class UQinglanCombatComponent;
class UQinglanTechniqueComponent;
class UQinglanRunSubsystem;
class UGameInstance;

/**
 * Delegate-to-widget adapter retained by one UMG widget instance.
 *
 * HUD forwarding contract (FIXME-CONTRACT: the frozen scaffold header for
 * UQinglanHUDWidget declares no BlueprintImplementableEvent update hooks, so
 * widget graphs implement custom events with these exact names; they are
 * invoked reflectively via ProcessEvent and are safe to omit):
 *   void BP_OnHealthChanged(float Current, float Maximum);
 *   void BP_OnEnergyChanged(float Current, float Maximum);
 *   void BP_OnStaminaChanged(float Current, float Maximum);
 *   void BP_OnBarrierChanged(float Current, float Maximum);
 *   void BP_OnCooldownStarted(FName TechniqueId, float Seconds);
 *   void BP_OnRoomCompleted(int32 RoomIndex, int64 ShardsAwarded);
 */
UCLASS()
class UQinglanUIEventRelay : public UObject
{
	GENERATED_BODY()

public:
	/** Subscribes to UQinglanStorySubsystem::OnSceneLine/OnChoicesPresented. */
	void SetupDialogue(UQinglanDialogueWidget* Widget);

	/**
	 * Subscribes to UQinglanRunSubsystem::OnRoomCompleted, then the owning
	 * player pawn's combat (health/energy/stamina/barrier) and technique
	 * (cooldown) delegates. When the pawn does not exist yet, retries on a
	 * paused-game-time world timer until it does.
	 */
	void SetupHUD(UQinglanHUDWidget* Widget);

	/** Removes every dynamic binding and clears the late-bind timer. */
	void UnbindAll();

	virtual UWorld* GetWorld() const override;
	virtual void BeginDestroy() override;

	// --- Dialogue handlers ---
	UFUNCTION() void HandleSceneLine(const FQinglanScriptLine& Line);
	UFUNCTION() void HandleChoicesPresented(const TArray<FQinglanSceneChoice>& Choices);

	// --- HUD handlers ---
	UFUNCTION() void HandleHealthChanged(float Current, float Maximum);
	UFUNCTION() void HandleEnergyChanged(float Current, float Maximum);
	UFUNCTION() void HandleStaminaChanged(float Current, float Maximum);
	UFUNCTION() void HandleBarrierChanged(float Current, float Maximum);
	UFUNCTION() void HandleCooldownStarted(FName TechniqueId, float Seconds);
	UFUNCTION() void HandleRoomCompleted(int32 RoomIndex, int64 ShardsAwarded);

private:
	/** Binds the pawn's combat/technique components; true on success. */
	bool TryBindPawnComponents();
	/** Timer callback while waiting for the player pawn to exist. */
	void HandleLateBindPoll();
	void StopLateBindTimer();
	/** Subsystem resolvers through the owning widget's game instance. */
	UQinglanStorySubsystem* ResolveStory() const;
	UQinglanRunSubsystem* ResolveRun() const;
	UGameInstance* ResolveGameInstance() const;

	TWeakObjectPtr<UQinglanDialogueWidget> DialogueWidget;
	TWeakObjectPtr<UQinglanHUDWidget> HUDWidget;
	TWeakObjectPtr<UQinglanCombatComponent> Combat;
	TWeakObjectPtr<UQinglanTechniqueComponent> Techniques;
	FTimerHandle LateBindTimer;
};
