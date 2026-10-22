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
class UQinglanEndingWidget;
class UQinglanMenuWidget;
class UQinglanJournalWidget;
class AQinglanInteractable;
class UQinglanGameplayProofRelay;

/**
 * Action classes covered by the unified priority gate (AUD-32). Priority:
 * Dead > forced HitReaction > menu/scene-choice freeze > permitted dodge
 * cancel > committed action > buffered input.
 */
enum class EQinglanActionKind : uint8
{
	Move,
	Sword,
	Dodge,
	Technique,
	LockOn
};

/** Outcome of one dodge input (AUD-32: 0.15 s buffer). */
enum class EQinglanDodgeRequest : uint8
{
	Start,
	Buffer,
	Reject
};

/**
 * Pure action-permission snapshot. Kept plain (not reflected) so automation
 * can exercise the exact rules the character uses at runtime.
 */
struct FQinglanActionGateState
{
	bool bAlive = true;
	bool bRooted = false;
	bool bHitReaction = false;
	bool bDodging = false;
	bool bCasting = false;
	bool bCastAnticipation = false;
	bool bCastRecovery = false;
	bool bSwordSwinging = false;
	bool bSwordWindupOrActive = false;
	bool bSwordActive = false;
	bool bSwordCanDodgeCancel = true;
	bool bSceneChoicePending = false;
	bool bMenuOpen = false;
};

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
class QINGLANASCENSION_API AQinglanCharacter : public ACharacter // linkage-only export for editor automation specs
{
	GENERATED_BODY()

public:
	AQinglanCharacter();

	/**
	 * Boss-room clear commits the realm's current choice-free scene (gameplay
	 * proof). Public so the editor automation specs can bind the production
	 * handler directly with AddDynamic (the runtime binds it in BeginPlay).
	 */
	UFUNCTION() void HandleRoomCompleted(int32 RoomIndex, int64 ShardsAwarded);

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

	// --- Unified action gate (AUD-32) ---

	/** Pure permission query used by every action entry point. */
	static bool IsActionAllowed(EQinglanActionKind Kind, const FQinglanActionGateState& State);
	/**
	 * Dodge request classification. Start when the dodge may run now; Buffer
	 * while a committed action will become cancellable/finish inside the
	 * 0.15 s window; Reject on death/reaction/menu/root/no stamina.
	 */
	static EQinglanDodgeRequest EvaluateDodgeRequest(const FQinglanActionGateState& State, bool bHasStamina);
	/** True while a buffered dodge press is still inside [0, WindowSeconds]. */
	static bool IsDodgeBufferFresh(float BufferedAtSeconds, float NowSeconds, float WindowSeconds);
	/** Snapshot the live component state into the pure gate. */
	void GetActionGateState(FQinglanActionGateState& OutState) const;
	bool CanStartAction(EQinglanActionKind Kind) const;
	/** True while a buffered dodge press waits for its action window to open. */
	UFUNCTION(BlueprintPure) bool IsDodgeInputBuffered() const { return bHasBufferedDodge; }
	/** Forced hit reaction is the highest non-death gate (AUD-32). */
	UFUNCTION(BlueprintPure) bool IsInHitReaction() const { return bInHitReaction; }

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
	/**
	 * AUD-26: interactable currently inside the 300 cm player + camera range,
	 * camera cone, and unobstructed line of sight (null when none). The HUD
	 * hint is refreshed when this selection changes.
	 */
	AQinglanInteractable* GetFocusedInteractable() const;
	/** AUD-26 HUD wiring point: localized "interact" hint for the focus (empty when none). */
	UFUNCTION(BlueprintPure) FText GetInteractPromptText() const;

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

	// --- AUD-51 native menus ---
	/** Opens the pause menu (offers re-present themselves when pending). */
	UFUNCTION(BlueprintCallable) void OpenGameMenu();
	/** Closes every menu overlay and restores gameplay input. */
	UFUNCTION(BlueprintCallable) void CloseGameMenu();
	/** Opens the journal viewer, optionally directly on one record. */
	UFUNCTION(BlueprintCallable) void OpenJournal(FName RecordId = NAME_None);
	/** Numeric/row selection forwarded from the menu widget. */
	UFUNCTION(BlueprintCallable) bool SelectMenuOption(int32 Index);
	/** Escape on the offer screen: shop offers dismiss, blessings are mandatory. */
	UFUNCTION(BlueprintCallable) bool DismissMenuOffer();
	/** Save-before-travel exit used by the pause menu; never leaves unsaved state. */
	UFUNCTION(BlueprintCallable) void QuitToSanctuary();
	UFUNCTION(BlueprintPure) bool IsMenuInputActive() const { return bMenuOpen; }
	/**
	 * AUD-01: cold-start slot selection. Opens the Profile screen without a
	 * profile session; the game does not auto-load or auto-create a save.
	 * No-op when a profile session is already active (ordinary hub travel).
	 */
	UFUNCTION(BlueprintCallable) void OpenProfileFrontendIfNeeded();
	UFUNCTION(BlueprintPure) bool IsProfileFrontendOpen() const { return bProfileFrontendOpen; }
	/**
	 * True when SceneId began, delivered at least one line, is choice-free,
	 * and its playback has ended: the script played out, so a follow-up
	 * interaction is the gameplay proof that commits the scene (blueprint
	 * section 1.9 dispatcher; choice-free scenes commit only through
	 * UQinglanStorySubsystem::NotifyObjectiveCompleted).
	 */
	bool WasSceneScriptExhausted(FName SceneId) const;
	/** Inspection follows the complete exchange, including choice responses. */
	bool HasSceneScriptExhaustedForInspection(FName SceneId) const;

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
	/** AUD-26: refresh the aimed interactable and show its localized hint on change. */
	void UpdateInteractionFocus();
	/** Restores walk speed after a dodge finishes. */
	UFUNCTION() void HandleDodgeFinished();

	// --- AUD-27/32 runtime state ---

	/** Executes a dodge immediately (stamina committed here, not at buffer time). */
	void StartDodgeNow();
	/** Fires a buffered dodge at the first frame the action gate permits it. */
	void ProcessBufferedDodge();
	/** AUD-27: sync runtime slots from the active profile (possession/travel/commit). */
	UFUNCTION() void HandleProfileCommitted();
	/** AUD-27: binds the profile's commit broadcast; safe when no profile exists yet. */
	void BindProfileDelegates();
	void UnbindProfileDelegates();
	/** AUD-32: forced reaction on accepted damage; cancels committed actions. */
	UFUNCTION() void HandleDamageResolved(const FQinglanDamageEvent& Event);
	/** Cosmetic hit-reaction montage; the input gate is authoritative if absent. */
	void PlayHitReactionMontage();
	/**
	 * AUD-44: derive the animation action layer (Attack/Cast/Dodge/Hit/Death)
	 * from the live component state and push it to the native anim instance.
	 * Polled once per tick plus at the key transition sites.
	 */
	void UpdateAnimationActionState();

	// --- Playable-loop glue state ---

	/** Lazy combat-HUD creation on the first player-controlled tick. */
	void EnsureHudCreated();
	/** Creates (once) and returns the dialogue presenter; null until first shown. */
	UUserWidget* EnsureDialogueWidget();
	UQinglanEndingWidget* EnsureEndingWidget();
	/** AUD-51: lazily created native menu/journal widgets. */
	UQinglanMenuWidget* EnsureMenuWidget();
	UQinglanJournalWidget* EnsureJournalWidget();
	/** Re-applies focus/cursor mode from the active modal (menu > journal > choices). */
	void RefreshMenuInputState();
	void UpdateMenuInputMode();
	void MoveMenuSelection(int32 Direction);
	void ConfirmMenuSelection();
	void CancelMenu();
	UFUNCTION() void HandleDialogueChoiceSelected(FName ChoiceId);
	UFUNCTION() void HandleEndingPreviewed(FName EndingId, FName SceneId);
	UFUNCTION() void HandleConfirmEndingRequested(FName EndingId, FName SceneId);
	UFUNCTION() void HandleCancelConfirmation(FName SceneId);
	UFUNCTION() void HandleEndingClosed();
	UFUNCTION() void HandleEndingCommitted(FName EndingId);
	UFUNCTION() void HandleMenuClosed();
	UFUNCTION() void HandleJournalClosed();
	/** Story playback display: line, choices, records (widget or on-screen fallback). */
	UFUNCTION() void HandleSceneLine(const FQinglanScriptLine& Line);
	UFUNCTION() void HandleChoicesPresented(const TArray<FQinglanSceneChoice>& Choices);
	UFUNCTION() void HandleSceneEntered(FName SceneId);
	UFUNCTION() void HandleSceneCompleted(FName SceneId);
	UFUNCTION() void HandleRecordOpened(FName RecordId, const FText& Title, const FText& Body);
	/** Story choice-action delegates (Narrative/QinglanStoryRuntime.h). */
	UFUNCTION() void HandleEnterTrialRequested(FName IdempotencyKey, FName SceneId);
	UFUNCTION() void HandleStayAtHub(FName IdempotencyKey, FName SceneId);
	UFUNCTION() void HandleSaveFailed(FName ProfileId, FText Reason);
	UFUNCTION() void HandleBankingFailed();
	UFUNCTION() void HandleReturnToHub(FName IdempotencyKey, FName SceneId);
	UFUNCTION() void HandleCampaignComplete(FName IdempotencyKey, FName SceneId);
	/** Courier reply arrived: shows the authored arrival notice (the rest
	 *  station itself only reports the hours passed). */
	UFUNCTION() void HandleDeliveryArrived(FName QuestId, FName Version);
	/** Defeat: banks at 50% and returns to the hub after a short beat (docs/03 section 6). */
	UFUNCTION() void HandleHealthChanged(float Current, float Maximum);
	/** Delayed hub reload after the death beat (timer callback; OpenLevel off the input path). */
	UFUNCTION() void HandleDefeatTravel();
	void BindStoryDelegates();
	void UnbindStoryDelegates();

	FTimerHandle DefeatTravelTimer;

	TWeakObjectPtr<UUserWidget> HudWidget;
	TWeakObjectPtr<UUserWidget> DialogueWidget;
	TWeakObjectPtr<UQinglanEndingWidget> EndingWidget;
	TWeakObjectPtr<UQinglanMenuWidget> MenuWidget;
	TWeakObjectPtr<UQinglanJournalWidget> JournalWidget;
	int32 MenuSelectionIndex = 0;
	bool bHudCreated = false;
	bool bStoryDelegatesBound = false;
	/** Owned by this actor; a process-wide strong root would retain its entire world. */
	UPROPERTY(Transient) TObjectPtr<UQinglanGameplayProofRelay> GameplayProofRelay;
	bool bDefeatHandled = false;
	/** AUD-51 gameplay input gate; mirrors any open menu/journal overlay. */
	bool bMenuOpen = false;
	/** AUD-01: the menu currently on screen is the cold-start slot selector. */
	bool bProfileFrontendOpen = false;

	/** AUD-26 focus cache; the prompt is shown only when the selection changes. */
	TWeakObjectPtr<AQinglanInteractable> FocusedInteractable;

	// AUD-32: paused-game-time action clock (advances in Tick only).
	float ActionClock = 0.f;
	/** Single-slot dodge buffer (docs/02 §3: 0.15 s). */
	bool bHasBufferedDodge = false;
	float BufferedDodgeAt = -1.e9f;
	/** Forced hit reaction window; no action is legal while true. */
	bool bInHitReaction = false;
	float HitReactionEndsAt = -1.e9f;
	/** AUD-27: profile commit delegate binding (retried on possession). */
	bool bProfileDelegatesBound = false;

	UPROPERTY(EditDefaultsOnly) float DodgeInputBufferSeconds = 0.15f;
	UPROPERTY(EditDefaultsOnly) float HitReactionSeconds = 0.35f;

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
	/** Buys one pending shop entry; auto-equips into a free slot (docs/03 sections 4-5).
	 *  False leaves the offer pending so a failed purchase is retryable. */
	bool BuyShopEntry(const FQinglanShopOfferEntry& Entry);
};
