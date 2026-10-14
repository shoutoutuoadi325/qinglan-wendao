// Player character: movement, input routing, animation hooks.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Combat/QinglanCombatState.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "Progression/QinglanRunSubsystem.h"
#include "QinglanCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class UQinglanCombatComponent;
class UQinglanTechniqueComponent;
class UQinglanStatusComponent;
class USpringArmComponent;
class UCameraComponent;
class UUserWidget;

/** One purchasable sanctuary-shop entry (relic or passive manual). */
struct FQinglanShopOfferEntry
{
	FName ItemId = NAME_None;
	/** True: relic (Vein Crystal price); false: passive manual (study cost). */
	bool bRelic = false;
	FText Title;
	FText CostSummary;
};

/**
 * Third-person player character. Owns no game rules; routes input to components.
 * Camera: 350 cm spring arm, 60 cm shoulder offset, 75 deg FOV (65-95 user range),
 * per-frame sphere-sweep obstruction (in fast, out slow), optional lock-on.
 */
UCLASS()
class AQinglanCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AQinglanCharacter();

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void PossessedBy(AController* NewController) override;

	// --- Input handlers (Enhanced Input) ---
	void InputMove(const FInputActionValue& Value);
	void InputLook(const FInputActionValue& Value);
	void InputSwordAttack(const FInputActionValue& Value);
	void InputDodge(const FInputActionValue& Value);
	void InputLockTarget(const FInputActionValue& Value);
	void InputCycleTarget(const FInputActionValue& Value);
	void InputTechniqueSlot(int32 Slot);
	void InputTalismanSlot(int32 Slot);
	void InputInteract(const FInputActionValue& Value);
	void InputPause(const FInputActionValue& Value);

	// --- Dodge ---
	/** Dodge: ~450 cm over 0.45 s, 25 stamina, i-frames 0.10-0.28 s, no wall pass-through. */
	void TryDodge();
	bool CanDodge() const;
	float GetDodgeIFrameStart() const { return 0.10f; }
	float GetDodgeIFrameEnd() const { return 0.28f; }

	// --- Lock-on ---
	void ToggleLockOn();
	void CycleLockTarget();
	bool IsLockedOn() const { return LockTarget.IsValid(); }
	TWeakObjectPtr<AActor> GetLockTarget() const { return LockTarget; }
	/** Free-aim point for techniques: camera trace, then cast socket to that point. */
	FVector GetAimPoint() const;

	UFUNCTION(BlueprintPure) UQinglanCombatComponent* GetCombat() const { return CombatComponent; }
	UFUNCTION(BlueprintPure) UQinglanTechniqueComponent* GetTechniques() const { return TechniqueComponent; }
	UFUNCTION(BlueprintPure) UQinglanStatusComponent* GetStatus() const { return StatusComponent; }
	UFUNCTION(BlueprintPure) EQinglanCombatState GetCombatState() const;

	// --- Playable-loop glue (interaction, HUD, menus, defeat) ---

	/** Presents a 3-option blessing draft; technique keys 1-3 select (docs/03 section 5). */
	void PresentBlessingOffer(const TArray<FQinglanBlessing>& Offers);
	/** Presents up to four purchasable items; technique keys 1-4 buy. */
	void PresentShopOffer(const TArray<FQinglanShopOfferEntry>& Entries);
	/** Transient localized notice: dialogue widget when open, on-screen message otherwise. */
	void ShowGameMessage(const FText& Message);
	/**
	 * True when SceneId began, delivered at least one line, is choice-free,
	 * and its playback has ended: the script played out, so a follow-up
	 * interaction is the gameplay proof that commits the scene (blueprint
	 * section 1.9 dispatcher; choice-free scenes commit only through
	 * UQinglanStorySubsystem::NotifyObjectiveCompleted).
	 */
	bool WasSceneScriptExhausted(FName SceneId) const;

	UPROPERTY(VisibleAnywhere) USpringArmComponent* CameraBoom = nullptr;
	UPROPERTY(VisibleAnywhere) UCameraComponent* FollowCamera = nullptr;

	// Tunables (defaults from docs/02-combat-and-camera.md; data-driven overrides at runtime)
	UPROPERTY(EditDefaultsOnly) float WalkSpeed = 420.f;
	UPROPERTY(EditDefaultsOnly) float DodgeDistance = 450.f;
	UPROPERTY(EditDefaultsOnly) float DodgeDuration = 0.45f;
	UPROPERTY(EditDefaultsOnly) float DodgeStaminaCost = 25.f;
	UPROPERTY(EditDefaultsOnly) float LockOnRange = 2000.f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY() UQinglanCombatComponent* CombatComponent = nullptr;
	UPROPERTY() UQinglanTechniqueComponent* TechniqueComponent = nullptr;
	UPROPERTY() UQinglanStatusComponent* StatusComponent = nullptr;

	UPROPERTY(EditDefaultsOnly) UInputMappingContext* DefaultMappingContext = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* MoveAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* LookAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* AttackAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* DodgeAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* LockAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* CycleTargetAction = nullptr;
	UPROPERTY(EditDefaultsOnly) TArray<UInputAction*> TechniqueActions;
	UPROPERTY(EditDefaultsOnly) TArray<UInputAction*> TalismanActions;
	UPROPERTY(EditDefaultsOnly) UInputAction* InteractAction = nullptr;
	UPROPERTY(EditDefaultsOnly) UInputAction* PauseAction = nullptr;

	TWeakObjectPtr<AActor> LockTarget;
	float TimeSinceTargetLost = 0.f;

	void UpdateLockOn(float DeltaSeconds);
	void UpdateCameraObstruction(float DeltaSeconds);
	/** Restores walk speed after a dodge finishes. */
	UFUNCTION() void HandleDodgeFinished();

	// --- Playable-loop glue state ---

	/** Lazy combat-HUD creation on the first player-controlled tick. */
	void EnsureHudCreated();
	/** Creates (once) and returns the dialogue presenter; null until first shown. */
	UUserWidget* EnsureDialogueWidget();
	/** Story playback display: line, choices, records (widget or on-screen fallback). */
	UFUNCTION() void HandleSceneLine(const FQinglanScriptLine& Line);
	UFUNCTION() void HandleChoicesPresented(const TArray<FQinglanSceneChoice>& Choices);
	UFUNCTION() void HandleSceneEntered(FName SceneId);
	UFUNCTION() void HandleSceneCompleted(FName SceneId);
	UFUNCTION() void HandleRecordOpened(FName RecordId, const FText& Title, const FText& Body);
	/** Boss-room clear commits the realm's current choice-free scene (gameplay proof). */
	UFUNCTION() void HandleRoomCompleted(int32 RoomIndex, int64 ShardsAwarded);
	/** Story choice-action delegates (Narrative/QinglanStoryRuntime.h). */
	UFUNCTION() void HandleEnterTrialRequested(FName IdempotencyKey, FName SceneId);
	UFUNCTION() void HandleReturnToHub(FName IdempotencyKey, FName SceneId);
	UFUNCTION() void HandleCampaignComplete(FName IdempotencyKey, FName SceneId);
	/** Defeat: banks at 50% and returns to the hub after a short beat (docs/03 section 6). */
	UFUNCTION() void HandleHealthChanged(float Current, float Maximum);
	/** Delayed hub reload after the death beat (timer callback; OpenLevel off the input path). */
	UFUNCTION() void HandleDefeatTravel();
	void BindStoryDelegates();
	void UnbindStoryDelegates();

	FTimerHandle DefeatTravelTimer;

	TWeakObjectPtr<UUserWidget> HudWidget;
	TWeakObjectPtr<UUserWidget> DialogueWidget;
	bool bHudCreated = false;
	bool bStoryDelegatesBound = false;
	bool bDefeatHandled = false;

	/** Open key menus: technique keys 1-4 select while one is active (highest priority first). */
	TArray<FQinglanSceneChoice> PendingSceneChoices;
	TArray<FQinglanBlessing> PendingBlessingChoices;
	TArray<FQinglanShopOfferEntry> PendingShopChoices;

	/** Script-exhaustion tracking for the objective-proof dispatcher. */
	FName LastEnteredSceneId = NAME_None;
	bool bLastEnteredSceneChoiceDriven = false;
	bool bSawLineForEnteredScene = false;

	/** One technique-key press routed into the open menu, if any. */
	bool ConsumeMenuSelection(int32 Slot);
	/** Buys one pending shop entry; auto-equips into a free slot (docs/03 sections 4-5). */
	void BuyShopEntry(const FQinglanShopOfferEntry& Entry);
};
