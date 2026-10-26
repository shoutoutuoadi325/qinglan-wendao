// Core shared value types, enums, and events for Qinglan: Path of Ascension.
// All identifiers are stable ASCII strings at runtime; presentation text is
// always resolved through localization string tables, never stored in state.
#pragma once

#include "CoreMinimal.h"
#include "QinglanCoreTypes.generated.h"

/** Five Elements plus Lightning as a technique category (not a sixth element). */
UENUM(BlueprintType)
enum class EQinglanElement : uint8
{
	None	UMETA(DisplayName = "None"),
	Metal	UMETA(DisplayName = "Metal"),
	Wood	UMETA(DisplayName = "Wood"),
	Water	UMETA(DisplayName = "Water"),
	Fire	UMETA(DisplayName = "Fire"),
	Earth	UMETA(DisplayName = "Earth"),
	Lightning UMETA(DisplayName = "Lightning")
};

/** Cultivation stages in campaign order. */
UENUM(BlueprintType)
enum class EQinglanStage : uint8
{
	QiRefining			UMETA(DisplayName = "Qi Refining"),
	FoundationEstablishment UMETA(DisplayName = "Foundation Establishment"),
	GoldenCore			UMETA(DisplayName = "Golden Core")
};

/** Realm room kinds, mirrors data/examples/realm.json. */
UENUM(BlueprintType)
enum class EQinglanRoomKind : uint8
{
	Entrance,
	Combat,
	Shrine,
	Elite,
	Boss,
	Exit
};

/** Narrative scene source kinds, mirrors data/story registries. */
UENUM(BlueprintType)
enum class EQinglanSourceKind : uint8
{
	Live,
	Paper,
	Letter,
	System,
	Memory,
	Recorded
};

/** Confirmed endings; mutually exclusive, one commit per profile. */
UENUM(BlueprintType)
enum class EQinglanEnding : uint8
{
	None		UMETA(DisplayName = "None"),
	Open		UMETA(DisplayName = "Open River"),
	Seal		UMETA(DisplayName = "Quiet Summit"),
	Shared		UMETA(DisplayName = "Returning Rain")
};

/** Unique handle for one technique release; prevents reentrant release and duplicate hits. */
USTRUCT(BlueprintType)
struct FQinglanCastId
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 Serial = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 FrameNumber = 0;

	bool IsValid() const { return Serial > 0; }
	friend uint32 GetTypeHash(const FQinglanCastId& C) { return HashCombine(::GetTypeHash(C.Serial), ::GetTypeHash(C.FrameNumber)); }
	bool operator==(const FQinglanCastId& O) const { return Serial == O.Serial && FrameNumber == O.FrameNumber; }
};

/** Full audit record for one damage application. Duplicate (CastId, victim, ordinal) is rejected. */
USTRUCT(BlueprintType)
struct FQinglanDamageEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<AActor> Attacker;
	UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<AActor> Target;
	UPROPERTY(BlueprintReadOnly) FQinglanCastId CastId;
	/** Per-victim hit ordinal; multi-tick effects increment it deliberately. */
	UPROPERTY(BlueprintReadOnly) int32 HitOrdinal = 0;
	UPROPERTY(BlueprintReadOnly) FName TechniqueId = NAME_None;
	UPROPERTY(BlueprintReadOnly) EQinglanElement Element = EQinglanElement::None;
	UPROPERTY(BlueprintReadOnly) float RawMagnitude = 0.f;
	UPROPERTY(BlueprintReadOnly) float FinalMagnitude = 0.f;
	/** Paused-game-time timestamp in seconds. */
	UPROPERTY(BlueprintReadOnly) float Timestamp = 0.f;
	UPROPERTY(BlueprintReadOnly) bool bBlockedByBarrier = false;
};

/** Timed status effects with exact authored semantics. */
UENUM(BlueprintType)
enum class EQinglanStatus : uint8
{
	None,
	Wet,	// 5 s, from authored Water attacks or gameplay puddles
	Burn,	// 3 s, three 4-damage ticks, refresh not stack, max 3 ticks after latest refresh
	Root,	// 1 s ordinary enemies; bosses get 10% slow for 1 s
	Conduct // Thunder Seal vs Wet: one 1.25x application, consumes Wet once
};

/** Currency and material inventory; 64-bit bounded, negatives rejected. */
USTRUCT(BlueprintType)
struct FQinglanInventory
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64 SpiritShards = 0;
	UPROPERTY(BlueprintReadOnly) int64 SpiritHerb = 0;
	UPROPERTY(BlueprintReadOnly) int64 VeinCrystal = 0;
};

/** Player vitals and derived combat attributes. */
USTRUCT(BlueprintType)
struct FQinglanAttributes
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) float Health = 100.f;
	UPROPERTY(BlueprintReadOnly) float MaxHealth = 100.f;
	UPROPERTY(BlueprintReadOnly) float SpiritualEnergy = 100.f;
	UPROPERTY(BlueprintReadOnly) float MaxSpiritualEnergy = 100.f;
	UPROPERTY(BlueprintReadOnly) float Stamina = 100.f;
	UPROPERTY(BlueprintReadOnly) float MaxStamina = 100.f;
	UPROPERTY(BlueprintReadOnly) float TechniquePower = 20.f;
	UPROPERTY(BlueprintReadOnly) float Defense = 10.f;
};

/** One choice presented inside a scene; practical choices may carry requirements. */
USTRUCT(BlueprintType)
struct FQinglanSceneChoice
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FName ChoiceId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FText Label;
	UPROPERTY(BlueprintReadOnly) TArray<FText> Responses;
	UPROPERTY(BlueprintReadOnly) TArray<FName> SetsFlags;
	UPROPERTY(BlueprintReadOnly) bool bPractical = false;
	UPROPERTY(BlueprintReadOnly) TArray<FName> UnmetRequirements;
};

/** Outcome of a profile transaction: atomic, idempotent, and auditable. */
USTRUCT(BlueprintType)
struct FQinglanTransactionResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) bool bCommitted = false;
	UPROPERTY(BlueprintReadOnly) FName TransactionId = NAME_None;
	UPROPERTY(BlueprintReadOnly) FText FailureReason;
};
