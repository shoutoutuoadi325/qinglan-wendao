// Base widget classes implementation + the UI event relay body.
//
// Docs: docs/11-english-style-and-ui.md (UMG receives FText from string
// tables; live refresh without restart), docs/dev/implementation-blueprint.md
// section 1.1 (typed events only; no global untyped event bus).
//
// FIXME-CONTRACT: the frozen scaffold headers (UI/QinglanUIWidgets.h) declare
// only NativeConstruct overrides, so every dynamic-delegate binding routes
// through UQinglanUIEventRelay (new header in this folder, per the "new
// headers in your folder" rule). The relay is outer-owned by its widget, dies
// with it, and dynamic multicast bindings are weak, so a destructed widget can
// never receive a stale broadcast. The dialogue AdvanceLine/SelectChoice
// passthroughs live on UQinglanHUDStatics for the same reason.
//
// UQinglanJournalWidget and UQinglanEndingWidget are deliberately BP-only in
// this scaffold: journal/ending data flows through UQinglanHUDStatics helpers
// (GameText, FormatGameText*) plus UQinglanProfileSubsystem/UQinglanStorySubsystem
// queries, so they add no C++ surface here.
#include "UI/QinglanUIWidgets.h"

#include "Blueprint/UserWidget.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Progression/QinglanRunSubsystem.h"
#include "UI/QinglanHUDStatics.h"
#include "UI/QinglanUIEventRelay.h"

namespace
{
	// Late pawn-binding poll while the HUD constructs before the player pawn
	// exists (loading-screen order). Authored interval; paused-game-time
	// semantics because it runs on the world timer manager, which freezes with
	// paused game time (code-contracts.md "Timers use paused game time").
	constexpr float LateBindPollSeconds = 0.25f;

	/**
	 * One relay per live widget: re-entrant NativeConstruct (viewport re-add)
	 * reuses the relay after an UnbindAll instead of stacking bindings.
	 */
	TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UQinglanUIEventRelay>>& RelayByWidget()
	{
		static TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UQinglanUIEventRelay>> Map;
		return Map;
	}

	UQinglanUIEventRelay* AcquireRelay(UUserWidget* Owner)
	{
		TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UQinglanUIEventRelay>>& Relays = RelayByWidget();
		for (auto It = Relays.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent(); // the widget (and its outer-owned relay) is gone
			}
		}
		UQinglanUIEventRelay* Relay = nullptr;
		if (TWeakObjectPtr<UQinglanUIEventRelay>* Existing = Relays.Find(Owner))
		{
			Relay = Existing->Get();
		}
		if (!Relay)
		{
			Relay = NewObject<UQinglanUIEventRelay>(Owner);
			Relays.Add(Owner, Relay);
		}
		Relay->UnbindAll(); // re-construct must never double-bind
		return Relay;
	}

	/** Reflectively invokes a BP custom event on the HUD widget (see relay header). */
	void ForwardHUDEvent(UQinglanHUDWidget* Widget, const TCHAR* EventName, void* Params)
	{
		if (!Widget)
		{
			return;
		}
		if (UFunction* Function = Widget->FindFunction(EventName))
		{
			Widget->ProcessEvent(Function, Params);
		}
	}
}

void UQinglanDialogueWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Subscribe OnSceneLine/OnChoicesPresented and forward to the
	// BlueprintImplementableEvents declared on this scaffold class.
	UQinglanUIEventRelay* Relay = AcquireRelay(this);
	Relay->SetupDialogue(this);
}

void UQinglanHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Bind vitals (health/energy/stamina/barrier), technique cooldowns, and
	// room completion; the relay pushes an initial vitals snapshot so bars
	// start filled without waiting for the first change event.
	UQinglanUIEventRelay* Relay = AcquireRelay(this);
	Relay->SetupHUD(this);
}

// ---------------------------------------------------------------------------
// UQinglanUIEventRelay
// ---------------------------------------------------------------------------

UWorld* UQinglanUIEventRelay::GetWorld() const
{
	// UObject::GetWorld for a plain outer-owned relay: follow the owning widget
	// so subsystem/PC lookups resolve inside the correct world (PIE isolation).
	if (const UQinglanDialogueWidget* Dialogue = DialogueWidget.Get())
	{
		return Dialogue->GetWorld();
	}
	if (const UQinglanHUDWidget* HUD = HUDWidget.Get())
	{
		return HUD->GetWorld();
	}
	return nullptr;
}

void UQinglanUIEventRelay::BeginDestroy()
{
	StopLateBindTimer();
	Super::BeginDestroy();
}

UGameInstance* UQinglanUIEventRelay::ResolveGameInstance() const
{
	if (const UQinglanDialogueWidget* Dialogue = DialogueWidget.Get())
	{
		return Dialogue->GetGameInstance();
	}
	if (const UQinglanHUDWidget* HUD = HUDWidget.Get())
	{
		return HUD->GetGameInstance();
	}
	return nullptr;
}

UQinglanStorySubsystem* UQinglanUIEventRelay::ResolveStory() const
{
	UGameInstance* Instance = ResolveGameInstance();
	return Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
}

UQinglanRunSubsystem* UQinglanUIEventRelay::ResolveRun() const
{
	UGameInstance* Instance = ResolveGameInstance();
	return Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
}

void UQinglanUIEventRelay::SetupDialogue(UQinglanDialogueWidget* Widget)
{
	if (!Widget)
	{
		return;
	}
	DialogueWidget = Widget;
	if (UQinglanStorySubsystem* Story = ResolveStory())
	{
		Story->OnSceneLine.AddDynamic(this, &UQinglanUIEventRelay::HandleSceneLine);
		Story->OnChoicesPresented.AddDynamic(this, &UQinglanUIEventRelay::HandleChoicesPresented);
	}
	else
	{
		UE_LOG(LogQinglanUI, Warning, TEXT("DialogueWidget: story subsystem unavailable at construct"));
	}
}

void UQinglanUIEventRelay::SetupHUD(UQinglanHUDWidget* Widget)
{
	if (!Widget)
	{
		return;
	}
	HUDWidget = Widget;

	if (UQinglanRunSubsystem* Run = ResolveRun())
	{
		Run->OnRoomCompleted.AddDynamic(this, &UQinglanUIEventRelay::HandleRoomCompleted);
	}
	else
	{
		UE_LOG(LogQinglanUI, Warning, TEXT("HUDWidget: run subsystem unavailable at construct"));
	}

	if (!TryBindPawnComponents())
	{
		// Pawn not yet in the world (loading order): retry on paused game time.
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(LateBindTimer, this,
				&UQinglanUIEventRelay::HandleLateBindPoll, LateBindPollSeconds, true);
		}
	}
}

bool UQinglanUIEventRelay::TryBindPawnComponents()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AQinglanCharacter* Player = PC ? Cast<AQinglanCharacter>(PC->GetPawn()) : nullptr;
	UQinglanCombatComponent* CombatPtr = Player ? Player->GetCombat() : nullptr;
	UQinglanTechniqueComponent* TechniquePtr = Player ? Player->GetTechniques() : nullptr;
	if (!CombatPtr || !TechniquePtr)
	{
		return false;
	}
	if (Combat.Get() == CombatPtr && Techniques.Get() == TechniquePtr)
	{
		return true; // already bound to this pawn
	}

	CombatPtr->OnHealthChanged.AddDynamic(this, &UQinglanUIEventRelay::HandleHealthChanged);
	CombatPtr->OnEnergyChanged.AddDynamic(this, &UQinglanUIEventRelay::HandleEnergyChanged);
	CombatPtr->OnStaminaChanged.AddDynamic(this, &UQinglanUIEventRelay::HandleStaminaChanged);
	CombatPtr->OnBarrierChanged.AddDynamic(this, &UQinglanUIEventRelay::HandleBarrierChanged);
	TechniquePtr->OnCooldownStarted.AddDynamic(this, &UQinglanUIEventRelay::HandleCooldownStarted);
	Combat = CombatPtr;
	Techniques = TechniquePtr;

	// Initial vitals snapshot through the same handlers so the HUD renders
	// correct bars on the first frame (docs/11: widgets must never show stale
	// defaults after a live refresh).
	const FQinglanAttributes& Attributes = CombatPtr->GetAttributes();
	HandleHealthChanged(Attributes.Health, Attributes.MaxHealth);
	HandleEnergyChanged(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy);
	HandleStaminaChanged(Attributes.Stamina, Attributes.MaxStamina);
	const float Barrier = CombatPtr->GetBarrierAmount();
	HandleBarrierChanged(Barrier, Barrier);
	return true;
}

void UQinglanUIEventRelay::HandleLateBindPoll()
{
	if (TryBindPawnComponents())
	{
		StopLateBindTimer();
	}
}

void UQinglanUIEventRelay::StopLateBindTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LateBindTimer);
	}
	LateBindTimer.Invalidate();
}

void UQinglanUIEventRelay::UnbindAll()
{
	StopLateBindTimer();
	if (UQinglanStorySubsystem* Story = ResolveStory())
	{
		Story->OnSceneLine.RemoveDynamic(this, &UQinglanUIEventRelay::HandleSceneLine);
		Story->OnChoicesPresented.RemoveDynamic(this, &UQinglanUIEventRelay::HandleChoicesPresented);
	}
	if (UQinglanRunSubsystem* Run = ResolveRun())
	{
		Run->OnRoomCompleted.RemoveDynamic(this, &UQinglanUIEventRelay::HandleRoomCompleted);
	}
	if (UQinglanCombatComponent* CombatPtr = Combat.Get())
	{
		CombatPtr->OnHealthChanged.RemoveDynamic(this, &UQinglanUIEventRelay::HandleHealthChanged);
		CombatPtr->OnEnergyChanged.RemoveDynamic(this, &UQinglanUIEventRelay::HandleEnergyChanged);
		CombatPtr->OnStaminaChanged.RemoveDynamic(this, &UQinglanUIEventRelay::HandleStaminaChanged);
		CombatPtr->OnBarrierChanged.RemoveDynamic(this, &UQinglanUIEventRelay::HandleBarrierChanged);
	}
	if (UQinglanTechniqueComponent* TechniquePtr = Techniques.Get())
	{
		TechniquePtr->OnCooldownStarted.RemoveDynamic(this, &UQinglanUIEventRelay::HandleCooldownStarted);
	}
	Combat = nullptr;
	Techniques = nullptr;
}

void UQinglanUIEventRelay::HandleSceneLine(const FQinglanScriptLine& Line)
{
	if (UQinglanDialogueWidget* Widget = DialogueWidget.Get())
	{
		Widget->BP_OnLineShown(Line);
	}
	else
	{
		UnbindAll(); // owner gone: stop receiving broadcasts
	}
}

void UQinglanUIEventRelay::HandleChoicesPresented(const TArray<FQinglanSceneChoice>& Choices)
{
	if (UQinglanDialogueWidget* Widget = DialogueWidget.Get())
	{
		Widget->BP_OnChoicesShown(Choices);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleHealthChanged(float Current, float Maximum)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { float Current; float Maximum; } Params = { Current, Maximum };
		ForwardHUDEvent(Widget, TEXT("BP_OnHealthChanged"), &Params);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleEnergyChanged(float Current, float Maximum)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { float Current; float Maximum; } Params = { Current, Maximum };
		ForwardHUDEvent(Widget, TEXT("BP_OnEnergyChanged"), &Params);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleStaminaChanged(float Current, float Maximum)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { float Current; float Maximum; } Params = { Current, Maximum };
		ForwardHUDEvent(Widget, TEXT("BP_OnStaminaChanged"), &Params);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleBarrierChanged(float Current, float Maximum)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { float Current; float Maximum; } Params = { Current, Maximum };
		ForwardHUDEvent(Widget, TEXT("BP_OnBarrierChanged"), &Params);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleCooldownStarted(FName TechniqueId, float Seconds)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { FName TechniqueId; float Seconds; } Params = { TechniqueId, Seconds };
		ForwardHUDEvent(Widget, TEXT("BP_OnCooldownStarted"), &Params);
	}
	else
	{
		UnbindAll();
	}
}

void UQinglanUIEventRelay::HandleRoomCompleted(int32 RoomIndex, int64 ShardsAwarded)
{
	if (UQinglanHUDWidget* Widget = HUDWidget.Get())
	{
		struct { int32 RoomIndex; int64 ShardsAwarded; } Params = { RoomIndex, ShardsAwarded };
		ForwardHUDEvent(Widget, TEXT("BP_OnRoomCompleted"), &Params);
	}
	else
	{
		UnbindAll();
	}
}
