// Sword combo component: three-cut chain, notify-driven hit windows, substepped
// blade-socket sweeps. Timings and multipliers from docs/02-combat-and-camera.md §4.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Combat/QinglanCombatState.h"
#include "Core/QinglanCoreTypes.h"
#include "UObject/SoftObjectPath.h"
#include "QinglanSwordComponent.generated.h"

class UQinglanCombatComponent;
class USkeletalMeshComponent;
class UAnimMontage;
class AActor;

/** One cut of the sword chain: phase durations (s) and a TechniquePower multiplier. */
USTRUCT(BlueprintType)
struct FQinglanSwordCut
{
	GENERATED_BODY()

	/** Windup phase length in seconds before the hit window may open. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float WindupSeconds = 0.f;
	/** Active (hit window) phase length in seconds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float ActiveSeconds = 0.f;
	/** Recovery phase length in seconds; dodge-cancelable after its first 0.10 s (docs/02 §3). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float RecoverySeconds = 0.f;
	/** Multiplier applied to TechniquePower for RawMagnitude (docs/02 §4). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) float PowerMultiplier = 1.f;
};

/** Internal phase of the sword state machine (subset of EQinglanCombatState). */
UENUM(BlueprintType)
enum class EQinglanSwordPhase : uint8
{
	Idle,
	Windup,
	Active,
	Recovery
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanSwordSwingStarted, int32, ComboIndex, FName, SwingId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanSwordSwingEnded, int32, ComboIndex, FName, SwingId);

/**
 * Player sword combo. Owns the Cut1/Cut2/Cut3 chain, the single-slot 0.15 s input
 * buffer, the 0.35 s combo continuation window, and the substepped blade-socket
 * sweeps that deal exactly one damage per victim per swing (docs/02 §4).
 * Phase timers accumulate DeltaTime so paused game time freezes the combo.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class UQinglanSwordComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanSwordComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Input entry point (routed from AQinglanCharacter::InputSwordAttack).
	 * Starts a swing when the state machine allows it (idle, combo window, or
	 * recovery); otherwise stores the press in the single-slot input buffer.
	 * Returns true when a swing started immediately.
	 */
	bool RequestAttack();

	// --- Notify-driven hit window (call from AnimNotifies on the swing montage) ---
	/** AnimNotify "hit window open": begins substepped blade sweeps for the committed swing. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Sword")
	void NotifyWindowOpen();
	/** AnimNotify "hit window closed": stops sweeps early (redundant safety: the timer closes too). */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Sword")
	void NotifyWindowClosed();

	/** True while any phase of a swing is in flight. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	bool IsSwinging() const { return Phase != EQinglanSwordPhase::Idle; }

	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	EQinglanSwordPhase GetPhase() const { return Phase; }

	/** Maps to the shared combat states AttackWindup/AttackActive/AttackRecovery/Idle. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	EQinglanCombatState GetCombatState() const;

	/** Index into Cuts of the current or most recent swing (0 = Cut 1). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	int32 GetComboIndex() const { return ComboIndex; }

	/** Stable ASCII swing id (sword_cut_1..3); presentation names live in ST_Game. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	FName GetSwingId() const;

	/** True while the hit window is open and blade sweeps are running. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	bool IsWindowOpen() const { return bWindowOpen; }

	/** Test hook: number of unique victims damaged by the current swing. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Sword")
	int32 GetSwingVictimCount() const { return SwingVictims.Num(); }

	/**
	 * Dodge-cancel rules (docs/02 §3): windup cancellable any time; active hits
	 * never cancellable; recovery cancellable after its first 0.10 seconds.
	 */
	bool CanDodgeCancelNow() const;

	/** Cancels any in-flight swing (dodge cancel, forced hit reaction, death). */
	void InterruptSwing();

	UPROPERTY(BlueprintAssignable) FQinglanSwordSwingStarted OnSwingStarted;
	UPROPERTY(BlueprintAssignable) FQinglanSwordSwingEnded OnSwingEnded;

	// --- Tunables; defaults from docs/02-combat-and-camera.md §4 (Cut1 0.14/0.10/0.22 x0.8,
	//     Cut2 0.16/0.10/0.24 x0.9, Cut3 0.22/0.12/0.32 x1.3) ---
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") TArray<FQinglanSwordCut> Cuts;
	/** Combo continuation window measured from recovery begin (docs/02 §4: 0.35 s). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float ComboWindowSeconds = 0.35f;
	/** Single-slot input buffer lifetime (docs/02 §3: 0.15 s). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float InputBufferSeconds = 0.15f;
	/** Recovery becomes dodge-cancelable after this much of it elapsed (docs/02 §3: 0.10 s). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float RecoveryCancelAfterSeconds = 0.10f;
	/** Raw = TechniquePower * SwordBaseCoefficient * Cut.PowerMultiplier + SwordFlatDamage (docs/02 §5 form; §4 authors only the multiplier, coefficient defaults to 1). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float SwordBaseCoefficient = 1.f;
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float SwordFlatDamage = 0.f;
	/** Blade sockets swept between previous and current positions (docs/02 §4). Names not authored; match the weapon rig. */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") FName BladeStartSocketName = TEXT("sword_base");
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") FName BladeTipSocketName = TEXT("sword_tip");
	/** Sweep sphere radius around each blade socket (cm); not authored, keep conservative. */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float BladeSweepRadius = 7.f;
	/** Substep sweep segments longer than this (cm) so fast swings cannot tunnel (docs/02 §4). */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") float MaxSubstepDistance = 15.f;
	/** Optional cosmetic montages; phase timers in code stay authoritative when null. */
	UPROPERTY(EditDefaultsOnly, Category = "Qinglan|Sword") TArray<TObjectPtr<UAnimMontage>> SwingMontages;
	/**
	 * Fallback montage paths used when SwingMontages is empty (pure-C++ pawn,
	 * no Blueprint defaults). QinglanAnimBootstrap authors these packages;
	 * missing assets degrade to no montage. Order mirrors Cuts (cut 1..3).
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Qinglan|Sword")
	TArray<FSoftObjectPath> SwingMontageSoftPaths;

protected:
	virtual void BeginPlay() override;

private:
	bool CanStartSwing() const;
	void StartSwing(int32 CutIndex);
	void EnterPhase(EQinglanSwordPhase NewPhase);
	/** Current phase duration from Cuts (0 when idle/unknown). */
	float GetPhaseDuration() const;
	void OpenWindow();
	void CloseWindow();
	/** Consumes a buffered attack at a state boundary (combo window open / swing finished). */
	void ConsumeBufferedInput();
	void TickSweep();
	void SweepSocketSegment(const FVector& From, const FVector& To);
	void ApplyHit(AActor* Victim);
	/** Blessings + permanent equipment sword-damage percent, resolved once per swing (docs/02 §4; docs/03 sections 4-5). */
	float ResolveSwingSwordDamagePercent();
	UQinglanCombatComponent* GetOwnerCombat() const;
	USkeletalMeshComponent* GetOwnerMesh() const;
	FVector GetBladeSocketLocation(FName SocketName) const;
	void PlaySwingMontage();
	/** Loads SwingMontageSoftPaths[CutIndex]; null when absent (no montage is fine). */
	UAnimMontage* ResolveSwingMontageSoft(int32 CutIndex);
	void StopSwingMontage();
	/** Paused-game-time clock: accumulates DeltaTime only while unpaused. */
	float Now = 0.f;

	EQinglanSwordPhase Phase = EQinglanSwordPhase::Idle;
	int32 ComboIndex = 0;
	float PhaseElapsed = 0.f;
	/** Clock stamp of the most recent recovery begin; drives the 0.35 s combo window (docs/02 §4). */
	float RecoveryBeganAt = -1.e9f;
	bool bHasBufferedAttack = false;
	float BufferedAttackAt = -1.e9f;
	bool bWindowOpen = false;
	bool bHavePreviousSocketLocations = false;
	FVector PreviousTipLocation = FVector::ZeroVector;
	FVector PreviousBaseLocation = FVector::ZeroVector;
	/** One damage per victim per swing (docs/02 §4). */
	TArray<TWeakObjectPtr<AActor>> SwingVictims;
	FQinglanCastId SwingCastId;
	int32 SwingSerialCounter = 0;
	/** Sword-damage percent for the committed swing (blessings + relic + passives, docs/03 sections 4-5). */
	float SwingSwordDamagePercent = 0.f;
};
