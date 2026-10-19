// AUD-44: native locomotion animation instance shared by the player pawn and
// the humanoid enemy rigs. It replaces the single-node looping idle with a
// small speed-driven state machine (Idle/Walk/Run/Airborne) plus an explicit
// action layer (Attack/Cast/Dodge/Hit/Death).
//
// The class intentionally has no dependency on an AnimBlueprint: a native
// UAnimInstance can compute Speed/state and drive slot animations on its own.
// A shipping presentation still needs an editor AnimBP asset whose graph
// contains a "DefaultSlot" slot node (and, for layered combat, an upper-body
// slot); without that graph the slot montages have no pose output.
//
// Remaining asset work (AUD-44, tracked here because the code path is ready):
//   - The Human rig ships Idle/Jump/ArmatureAction only. QinglanAnimBootstrap
//     now generates "程序化占位行走/跑步" (procedural placeholder walk/run) at
//     SK_HumanHuman_Armature_Walk / _Run using the UE 5.8 animation data model
//     API (1.0 s / 0.6 s loops, local-X limb swing, 20-35 deg knee flexion).
//     The axis/sign is not visually tuned; replace with authored takes when
//     available. Until the commandlet runs, Walk/Run fall back to Idle.
//     (The Knight rig has Walking/Run takes, but its skeleton is a different
//     rig and cannot be played on the Human mesh.)
//   - No AnimBP editor asset exists yet (slot graph, aim offsets, layered
//     combat slot, transition rules).
//   - Socket authoring/validation is still missing: sword_base/sword_tip for
//     the blade sweep and cast_source for technique aim.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "QinglanCharacterAnimInstance.generated.h"

class UAnimSequence;

/** Speed band selected from the owning pawn's planar velocity. */
UENUM(BlueprintType)
enum class EQinglanLocomotionState : uint8
{
	Idle,
	Walk,
	Run,
	Airborne
};

/** Explicit action layer; non-Idle states pause locomotion selection. */
UENUM(BlueprintType)
enum class EQinglanAnimActionState : uint8
{
	Idle,
	Attack,
	Cast,
	Dodge,
	Hit,
	Death
};

/**
 * Native anim instance for the Qinglan humanoid rig.
 *
 * Speed thresholds are public constants so headless automation can assert the
 * exact boundaries without a world. Idle below 10 cm/s; Walk from 10 cm/s to
 * 200 cm/s; Run from 200 cm/s upward. RunSpeedThreshold (500 cm/s) is the
 * authored full-run pace used to scale the run clip's playback rate, so the
 * two example thresholds requested by AUD-44 (200/500) both have meaning.
 */
UCLASS(transient, Blueprintable, BlueprintType)
class QINGLANASCENSION_API UQinglanCharacterAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UQinglanCharacterAnimInstance();

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	// --- Pure state selection (AUD-44 test seam) ---

	static constexpr float IdleSpeedThreshold = 10.f;
	static constexpr float WalkSpeedThreshold = 200.f;
	static constexpr float RunSpeedThreshold = 500.f;

	/**
	 * Pure locomotion classification. Airborne wins over every speed band so a
	 * fall/jump cannot be masked by residual planar velocity. The same function
	 * is used at runtime and by Qinglan.Anim.NativeStateMachineExists.
	 */
	static EQinglanLocomotionState SelectLocomotionState(float InSpeed, bool bAirborne);

	/** Planar speed as a pure getter (the Speed property is also read-only). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Animation")
	float GetSpeed() const { return Speed; }

	UFUNCTION(BlueprintPure, Category = "Qinglan|Animation")
	EQinglanLocomotionState GetLocomotionState() const { return LocomotionState; }

	UFUNCTION(BlueprintPure, Category = "Qinglan|Animation")
	EQinglanAnimActionState GetActionState() const { return ActionState; }

	UFUNCTION(BlueprintPure, Category = "Qinglan|Animation")
	bool IsMoving() const { return bIsMoving; }

	UFUNCTION(BlueprintPure, Category = "Qinglan|Animation")
	bool IsInAir() const { return bIsAirborne; }

	/**
	 * Action layer entry point. Non-Idle values freeze locomotion selection
	 * (Speed/airborne keep updating for later); Idle re-enables selection and
	 * forces the current locomotion clip to replay, because combat montages
	 * stop the transient slot montage while they run.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Animation")
	void SetActionState(EQinglanAnimActionState NewState);

	/**
	 * Runtime override for the rig's takes (enemy archetypes carry their own
	 * IdleAnimation). Null entries leave the current asset unchanged; Walk/Run
	 * continue to fall back through Walk -> Run -> Idle when absent.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Animation")
	void SetLocomotionAnimations(UAnimSequence* InIdle, UAnimSequence* InWalk, UAnimSequence* InRun);

	// --- Blueprint-visible state (also read by automation) ---

	/** Planar (XY) speed of the owning pawn, cm/s. */
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Animation")
	float Speed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Animation")
	bool bIsMoving = false;

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Animation")
	bool bIsAirborne = false;

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Animation")
	EQinglanLocomotionState LocomotionState = EQinglanLocomotionState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Animation")
	EQinglanAnimActionState ActionState = EQinglanAnimActionState::Idle;

	// --- Presentation tunables ---

	/**
	 * Convention paths authored by the mesh import. The Human pack has Idle and
	 * Jump; the Walk/Run paths deliberately point at takes that do not exist
	 * yet, so the resolver falls back to Idle until the art pass lands.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	TSoftObjectPtr<UAnimSequence> DefaultIdleAnimationPath;

	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	TSoftObjectPtr<UAnimSequence> DefaultWalkAnimationPath;

	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	TSoftObjectPtr<UAnimSequence> DefaultRunAnimationPath;

	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	TSoftObjectPtr<UAnimSequence> DefaultAirborneAnimationPath;

	/** Montage slot expected in the shipping AnimBP graph (set in the constructor). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	FName LocomotionSlotName;

	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	float LocomotionBlendInTime = 0.2f;

	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Animation")
	float LocomotionBlendOutTime = 0.2f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleAnimation = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkAnimation = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> RunAnimation = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> AirborneAnimation = nullptr;

	/** Set by initialization/action transitions; consumed by the next update. */
	bool bLocomotionPlaybackDirty = true;

	/** Loads the soft convention paths once per instance. */
	void LoadDefaultAnimations();

	/** Refreshes Speed/bIsMoving/bIsAirborne from the owning pawn. */
	void UpdateOwnerKinematics();

	/** Walk -> Run -> Idle fallback chain for a locomotion band. */
	UAnimSequence* ResolveLocomotionAnimation(EQinglanLocomotionState State) const;

	/** Speed-scaled playback rate for the selected band (1.0 for Idle). */
	float ResolveLocomotionPlayRate() const;

	/** Plays the current band as a looping dynamic slot montage. */
	void PlayLocomotionAnimation();
};
