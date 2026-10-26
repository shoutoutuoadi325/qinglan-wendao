// Combat state machine states shared by character and components.
#pragma once

#include "CoreMinimal.h"
#include "QinglanCombatState.generated.h"

UENUM(BlueprintType)
enum class EQinglanCombatState : uint8
{
	Idle,
	Move,
	AttackWindup,
	AttackActive,
	AttackRecovery,
	Casting,
	Dodging,
	HitReaction,
	Dead
};

/**
 * Priority resolution: Dead > forced HitReaction > allowed DodgeCancel >
 * current committed action > buffered input. One buffered input, 0.15 s.
 */
UENUM(BlueprintType)
enum class EQinglanActionPriority : uint8
{
	Buffered = 0,
	Committed = 1,
	DodgeCancel = 2,
	ForcedReaction = 3,
	Dead = 4
};
