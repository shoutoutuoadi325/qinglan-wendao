// Status component implementation. Authored semantics: docs/02 §7.
#include "Combat/QinglanStatusComponent.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h"

namespace
{
	// String table keys, mirror localization/en-US.json lines 36-39
	// ("status.wet" ... "status.conduct").
	FName StatusTextKey(EQinglanStatus Status)
	{
		switch (Status)
		{
		case EQinglanStatus::Wet:		return "status.wet";
		case EQinglanStatus::Burn:		return "status.burn";
		case EQinglanStatus::Root:		return "status.root";
		case EQinglanStatus::Conduct:	return "status.conduct";
		default:						return NAME_None;
		}
	}
}

UQinglanStatusComponent::UQinglanStatusComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UQinglanStatusComponent::BeginPlay()
{
	Super::BeginPlay();
	BindCombatDelegates();
}

UQinglanCombatComponent* UQinglanStatusComponent::GetOwningCombat() const
{
	AActor* Owner = GetOwner();
	return Owner ? Owner->FindComponentByClass<UQinglanCombatComponent>() : nullptr;
}

bool UQinglanStatusComponent::OwnerIsBoss() const
{
	if (bTreatAsBoss)
	{
		return true;
	}
	// Bosses are tagged Qinglan.Boss (Bamboo Guardian, Ashen Guardian, trial
	// constructs). Checked dynamically: InitializeFromData may tag after BeginPlay.
	const AActor* Owner = GetOwner();
	return Owner && Owner->ActorHasTag(FName(QinglanCombatTunables::QinglanBossTag));
}

void UQinglanStatusComponent::BindCombatDelegates()
{
	if (bCombatDelegatesBound)
	{
		return;
	}
	UQinglanCombatComponent* Combat = GetOwningCombat();
	if (!Combat)
	{
		return;
	}
	Combat->OnStatusApplied.AddDynamic(this, &UQinglanStatusComponent::HandleCombatStatusApplied);
	Combat->OnStatusCleared.AddDynamic(this, &UQinglanStatusComponent::HandleCombatStatusCleared);
	bCombatDelegatesBound = true;
}

void UQinglanStatusComponent::BroadcastApplied(EQinglanStatus Status, float RemainingSeconds)
{
	OnStatusApplied.Broadcast(Status, RemainingSeconds);
}

void UQinglanStatusComponent::BroadcastRemoved(EQinglanStatus Status)
{
	OnStatusRemoved.Broadcast(Status);
}

void UQinglanStatusComponent::HandleCombatStatusApplied(EQinglanStatus Status)
{
	// Mirror the combat-side authority so GetStatusRemaining stays coherent no
	// matter which entry point applied the status.
	if (Status == EQinglanStatus::Wet)
	{
		WetRemainingMirror = WetDuration;
		BroadcastApplied(Status, WetRemainingMirror);
	}
	else if (Status == EQinglanStatus::Burn)
	{
		BurnRemainingMirror = BurnDuration;
		BroadcastApplied(Status, BurnRemainingMirror);
	}
}

void UQinglanStatusComponent::HandleCombatStatusCleared(EQinglanStatus Status)
{
	if (Status == EQinglanStatus::Wet)
	{
		WetRemainingMirror = 0.f;
		BroadcastRemoved(Status);
	}
	else if (Status == EQinglanStatus::Burn)
	{
		BurnRemainingMirror = 0.f;
		BroadcastRemoved(Status);
	}
}

void UQinglanStatusComponent::ApplyRootMovement(float Duration, bool bBossOwner)
{
	// Root: 1 s ordinary enemies only; bosses get a 10% slow for 1 s instead
	// (docs/02 §7). Never both. Loop-free sink: never calls the combat component.
	const float ClampedDuration = FMath::Max(0.f, Duration);
	if (bBossOwner)
	{
		RootRemaining = 0.f;
		SlowRemaining = ClampedDuration;
		ActiveSlowFraction = 1.f - (BossSlowPercent / 100.f);
	}
	else
	{
		RootRemaining = ClampedDuration;
		SlowRemaining = 0.f;
		ActiveSlowFraction = 1.f;
	}
	BroadcastApplied(EQinglanStatus::Root, ClampedDuration);
}

void UQinglanStatusComponent::ClearRootMovement()
{
	if (RootRemaining > 0.f || SlowRemaining > 0.f)
	{
		RootRemaining = 0.f;
		SlowRemaining = 0.f;
		ActiveSlowFraction = 1.f;
		BroadcastRemoved(EQinglanStatus::Root);
	}
}

void UQinglanStatusComponent::ApplyStatus(EQinglanStatus Status, float Magnitude)
{
	BindCombatDelegates();
	switch (Status)
	{
	case EQinglanStatus::Wet:
	case EQinglanStatus::Burn:
		{
			// Wet/Burn authority is the combat component (Burn ticks deal
			// audited damage; Wet feeds Conduct math). Refresh semantics live
			// there; HandleCombatStatusApplied mirrors and re-broadcasts.
			if (UQinglanCombatComponent* Combat = GetOwningCombat())
			{
				Combat->ApplyStatus(Status, Magnitude);
			}
			else
			{
				// Test dummy without a combat component: track locally so
				// HasStatus/GetStatusRemaining still behave for automation.
				if (Status == EQinglanStatus::Wet)
				{
					WetRemainingMirror = WetDuration;
				}
				else
				{
					BurnRemainingMirror = BurnDuration;
				}
				BroadcastApplied(Status, Status == EQinglanStatus::Wet ? WetRemainingMirror : BurnRemainingMirror);
			}
		}
		break;

	case EQinglanStatus::Root:
		{
			// Route through the combat component so both stores (combat
			// ActiveStatuses for non-boss HasStatus queries + this movement
			// effect) update exactly once, from one entry point.
			const float Duration = FMath::Max(0.f, Magnitude > 0.f ? Magnitude : RootDuration);
			if (UQinglanCombatComponent* Combat = GetOwningCombat())
			{
				Combat->ApplyStatus(EQinglanStatus::Root, Duration);
			}
			else
			{
				// Combat-less test dummy: local movement effect only.
				ApplyRootMovement(Duration, OwnerIsBoss());
			}
		}
		break;

	case EQinglanStatus::Conduct:
		// One-shot reaction: no stored state. The 1.25x damage and Wet
		// consumption happen inside UQinglanCombatComponent::ApplyDamage; this
		// call only publishes the brief readable arc cue (docs/02 §7).
		BroadcastApplied(EQinglanStatus::Conduct, 0.f);
		break;

	case EQinglanStatus::None:
	default:
		break;
	}
}

void UQinglanStatusComponent::RemoveStatus(EQinglanStatus Status)
{
	switch (Status)
	{
	case EQinglanStatus::Wet:
	case EQinglanStatus::Burn:
		if (UQinglanCombatComponent* Combat = GetOwningCombat())
		{
			Combat->RemoveStatus(Status);
			// HandleCombatStatusCleared mirrors and re-broadcasts.
		}
		else
		{
			if (Status == EQinglanStatus::Wet)
			{
				WetRemainingMirror = 0.f;
			}
			else
			{
				BurnRemainingMirror = 0.f;
			}
			BroadcastRemoved(Status);
		}
		break;

	case EQinglanStatus::Root:
		if (UQinglanCombatComponent* Combat = GetOwningCombat())
		{
			Combat->RemoveStatus(EQinglanStatus::Root); // clears both stores
		}
		else
		{
			ClearRootMovement();
		}
		break;

	case EQinglanStatus::Conduct:
	case EQinglanStatus::None:
	default:
		break;
	}
}

bool UQinglanStatusComponent::HasStatus(EQinglanStatus Status) const
{
	switch (Status)
	{
	case EQinglanStatus::Wet:
	case EQinglanStatus::Burn:
	{
		// Combat component is the authority; the local mirror only covers
		// combat-less test dummies.
		const UQinglanCombatComponent* Combat = GetOwningCombat();
		if (Combat)
		{
			return Combat->HasStatus(Status);
		}
		return (Status == EQinglanStatus::Wet) ? WetRemainingMirror > 0.f : BurnRemainingMirror > 0.f;
	}

	case EQinglanStatus::Root:
		return RootRemaining > 0.f || SlowRemaining > 0.f;

	case EQinglanStatus::Conduct:
	case EQinglanStatus::None:
	default:
		return false;
	}
}

float UQinglanStatusComponent::GetStatusRemaining(EQinglanStatus Status) const
{
	switch (Status)
	{
	case EQinglanStatus::Wet:
		return WetRemainingMirror;

	case EQinglanStatus::Burn:
		return BurnRemainingMirror;

	case EQinglanStatus::Root:
		return FMath::Max(RootRemaining, SlowRemaining);

	case EQinglanStatus::Conduct:
	case EQinglanStatus::None:
	default:
		return 0.f;
	}
}

FText UQinglanStatusComponent::GetStatusDisplayName(EQinglanStatus Status) const
{
	// All player-facing text resolves through /Game/Qinglan/Data/ST_Game;
	// keys mirror localization/en-US.json ("status.wet" ... "status.conduct").
	const FName Key = StatusTextKey(Status);
	if (Key == NAME_None)
	{
		return FText::GetEmpty();
	}
	return FText::FromStringTable(TEXT("/Game/Qinglan/Data/ST_Game"), Key.ToString());
}

bool UQinglanStatusComponent::IsRooted() const
{
	return RootRemaining > 0.f;
}

bool UQinglanStatusComponent::IsSlowed() const
{
	return SlowRemaining > 0.f;
}

float UQinglanStatusComponent::GetMoveSpeedMultiplier() const
{
	if (RootRemaining > 0.f)
	{
		return 0.f;
	}
	if (SlowRemaining > 0.f)
	{
		return FMath::Clamp(ActiveSlowFraction, 0.f, 1.f);
	}
	return 1.f;
}

void UQinglanStatusComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Paused-game-time semantics: TickComponent does not run while the game is
	// paused, and DeltaTime accumulation keeps countdowns frame-rate stable.
	if (!bCombatDelegatesBound)
	{
		BindCombatDelegates();
	}

	if (RootRemaining > 0.f)
	{
		RootRemaining = FMath::Max(0.f, RootRemaining - DeltaTime);
		if (RootRemaining <= 0.f)
		{
			ActiveSlowFraction = 1.f;
			BroadcastRemoved(EQinglanStatus::Root);
		}
	}
	if (SlowRemaining > 0.f)
	{
		SlowRemaining = FMath::Max(0.f, SlowRemaining - DeltaTime);
		if (SlowRemaining <= 0.f)
		{
			ActiveSlowFraction = 1.f;
			BroadcastRemoved(EQinglanStatus::Root);
		}
	}

	// Mirror countdowns for Wet/Burn. The combat component clears them on
	// expiry; the local mirrors only tick down and snap to zero if the
	// authority no longer reports the status (defensive sync).
	WetRemainingMirror = FMath::Max(0.f, WetRemainingMirror - DeltaTime);
	BurnRemainingMirror = FMath::Max(0.f, BurnRemainingMirror - DeltaTime);
	const UQinglanCombatComponent* Combat = GetOwningCombat();
	if (Combat)
	{
		if (!Combat->HasStatus(EQinglanStatus::Wet) && WetRemainingMirror > 0.f)
		{
			WetRemainingMirror = 0.f;
		}
		if (!Combat->HasStatus(EQinglanStatus::Burn) && BurnRemainingMirror > 0.f)
		{
			BurnRemainingMirror = 0.f;
		}
	}
}
