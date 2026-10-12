// Dodge locomotion helper for AQinglanCharacter: constant-speed, capsule-swept
// dodge (~450 cm / 0.45 s) with a bounded i-frame window (docs/02 §2).
// The character owns the authored tunables (distance/duration/stamina); this
// component owns the dodge runtime state because the scaffold AQinglanCharacter
// header exposes no dodge members. Velocity is driven through the character
// movement component every tick, so capsule sweeps make wall pass-through
// impossible (docs/02 §2) and the integrated distance is frame-rate independent.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "QinglanDodgeComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanDodgeStarted, float, Duration);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanDodgeFinished);

UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class UQinglanDodgeComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanDodgeComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Starts one dodge along Direction (Z ignored, normalized internally).
	 * Distance/Duration/i-frame bounds come from the character's authored tunables.
	 * Returns false when a dodge is already active or the arguments are invalid.
	 */
	bool StartDodge(const FVector& Direction, float Distance, float Duration, float InIFrameStart, float InIFrameEnd);

	bool IsDodging() const { return bDodgeActive; }
	float GetElapsedSeconds() const { return DodgeElapsed; }
	float GetRemainingSeconds() const { return bDodgeActive ? FMath::Max(0.f, DodgeDuration - DodgeElapsed) : 0.f; }
	/** True inside [IFrameStart, IFrameEnd] of the dodge (docs/02 §2: 0.10-0.28 s of 0.45 s). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Dodge")
	bool IsInvulnerableNow() const;
	/** Unit planar direction of the active dodge. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Dodge")
	FVector GetDodgeDirection() const { return DodgeDirection; }

	UPROPERTY(BlueprintAssignable) FQinglanDodgeStarted OnDodgeStarted;
	/** Character restores walk speed here (AQinglanCharacter::HandleDodgeFinished). */
	UPROPERTY(BlueprintAssignable) FQinglanDodgeFinished OnDodgeFinished;

	/** Face the dodge direction while active (docs do not author this; default on). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Dodge") bool bOrientToDodgeDirection = true;

private:
	void FinishDodge();

	bool bDodgeActive = false;
	float DodgeElapsed = 0.f;
	float DodgeDuration = 0.45f;
	float DodgeSpeed = 1000.f;
	float IFrameStartTime = 0.10f;
	float IFrameEndTime = 0.28f;
	FVector DodgeDirection = FVector::ForwardVector;
};
