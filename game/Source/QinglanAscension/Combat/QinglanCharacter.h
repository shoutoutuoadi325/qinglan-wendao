// Player character: movement, input routing, animation hooks.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Combat/QinglanCombatState.h"
#include "QinglanCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class UQinglanCombatComponent;
class UQinglanTechniqueComponent;
class UQinglanStatusComponent;
class USpringArmComponent;
class UCameraComponent;

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
};
