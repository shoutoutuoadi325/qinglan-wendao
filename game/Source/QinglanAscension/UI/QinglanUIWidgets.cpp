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

#include "Abilities/QinglanTechniqueTypes.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h" // UWidgetTree for named-element lookup/creation
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
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

	// Named refresh cadence for the C++ HUD presenter (docs/11 section 1
	// "Active skill | Technique" terminology; bars/cooldowns read the owning
	// pawn each pass, so live language switches apply on the next refresh).
	constexpr float HudRefreshSeconds = 0.1f;

	// Cooldown shading: full opacity while ready, dimmed while on cooldown
	// (UWidget::SetRenderOpacity; authored HUD readability).
	constexpr float SkillReadyOpacity = 1.f;
	constexpr float SkillCooldownOpacity = 0.35f;

	// Fixed loadout slot count authored by the bootstrap (Txt_Skill0..3).
	constexpr int32 SkillSlotCount = 4;

	/**
	 * String-table keys for the C++ presenter rows (docs/11 "stable keys";
	 * localization/en-US.json + zh-Hans.json, mirrored placeholders).
	 * FIXME-CONTRACT: no design doc pins these key names; they follow the
	 * existing "hud.*"/"dialogue.*" groups.
	 */
	const TCHAR* GBarrierTextKey = TEXT("hud.barrier");
	const TCHAR* GSkillSlotTextKey = TEXT("hud.skill_slot");
	const TCHAR* GChoiceEntryTextKey = TEXT("dialogue.choice_entry");

	/** 0..1 fraction guarded against zero/non-positive maximums. */
	float SafeFraction(float Current, float Maximum)
	{
		return Maximum > 0.f ? FMath::Clamp(Current / Maximum, 0.f, 1.f) : 0.f;
	}

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
	BindNamedElements();
}

void UQinglanDialogueWidget::BindNamedElements()
{
	// Named blocks authored by QinglanUIBootstrapCommandlet's WBP_Dialogue
	// tree; every lookup is cast-guarded so foreign trees simply keep the
	// on-screen-message fallback (return-false setters).
	UWidgetTree* Tree = WidgetTree;
	if (!Tree)
	{
		return;
	}
	SpeakerText = Tree->FindWidget<UTextBlock>(TEXT("Txt_Speaker"));
	LineText = Tree->FindWidget<UTextBlock>(TEXT("Txt_Line"));
	ChoicesBox = Tree->FindWidget<UVerticalBox>(TEXT("Box_Choices"));
	if (UTextBlock* SourceKindText = Tree->FindWidget<UTextBlock>(TEXT("Txt_SourceKind")))
	{
		// Nothing feeds the source-kind column yet (records/scenes carry no
		// key to the widget); clear the authored placeholder so it cannot read
		// as live data during play.
		SourceKindText->SetText(FText::GetEmpty());
	}
}

bool UQinglanDialogueWidget::SetCurrentLine(const FText& InSpeakerName, const FText& InLineText)
{
	CurrentSpeaker = InSpeakerName;
	CurrentLineText = InLineText;
	if (SpeakerText)
	{
		SpeakerText->SetText(InSpeakerName);
	}
	if (LineText)
	{
		LineText->SetText(InLineText);
	}
	return SpeakerText || LineText;
}

bool UQinglanDialogueWidget::SetChoiceEntries(const TArray<FQinglanSceneChoice>& Choices)
{
	CurrentChoices = Choices;
	if (!ChoicesBox)
	{
		return false; // no authored choice box: caller keeps the fallback list
	}
	ChoicesBox->ClearChildren();
	UWidgetTree* Tree = WidgetTree;
	for (int32 Index = 0; Index < Choices.Num(); ++Index)
	{
		UTextBlock* EntryText = Tree
			? Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
				*FString::Printf(TEXT("Txt_Choice%d"), Index))
			: nullptr;
		if (!EntryText)
		{
			continue;
		}
		// Index prefix matches the on-screen fallback list ("[1] ...") so the
		// number keys keep their meaning in both presenters; the pattern lives
		// in the string table, never in code (docs/11 "named arguments with
		// FText::Format").
		FFormatNamedArguments Args;
		Args.Add(TEXT("index"), FText::AsNumber(Index + 1));
		Args.Add(TEXT("label"), Choices[Index].Label);
		EntryText->SetText(FText::Format(UQinglanHUDStatics::GameText(GChoiceEntryTextKey), Args));
		ChoicesBox->AddChild(EntryText);
	}
	return true;
}

bool UQinglanDialogueWidget::ShowRecordEntry(const FText& Title, const FText& Body)
{
	LastRecordTitle = Title;
	LastRecordBody = Body;
	// Records reuse the line presenter: the title reads as the speaker slot.
	return SetCurrentLine(Title, Body);
}

void UQinglanHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Bind vitals (health/energy/stamina/barrier), technique cooldowns, and
	// room completion; the relay pushes an initial vitals snapshot so bars
	// start filled without waiting for the first change event.
	UQinglanUIEventRelay* Relay = AcquireRelay(this);
	Relay->SetupHUD(this);
	BindNamedElements();
	RefreshFromPlayer(); // correct bars on the first visible frame
}

void UQinglanHUDWidget::BindNamedElements()
{
	// Named elements authored by QinglanUIBootstrapCommandlet's WBP_HUD tree;
	// every lookup is cast-guarded so partial/foreign trees render what they
	// can and the character-level notice fallbacks stay available.
	UWidgetTree* Tree = WidgetTree;
	if (!Tree)
	{
		return;
	}
	HealthBar = Tree->FindWidget<UProgressBar>(TEXT("Bar_Health"));
	EnergyBar = Tree->FindWidget<UProgressBar>(TEXT("Bar_Energy"));
	StaminaBar = Tree->FindWidget<UProgressBar>(TEXT("Bar_Stamina"));
	BarrierText = Tree->FindWidget<UTextBlock>(TEXT("Txt_Barrier"));
	SkillTexts.Reset();
	for (int32 Index = 0; Index < SkillSlotCount; ++Index)
	{
		SkillTexts.Add(Tree->FindWidget<UTextBlock>(*FString::Printf(TEXT("Txt_Skill%d"), Index)));
	}
}

void UQinglanHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Throttled pull (HudRefreshSeconds): the tree ticks every frame because
	// the bootstrap compiles with TickPrediction WillTick; state itself comes
	// from the owning pawn each pass.
	RefreshAccumulator += InDeltaTime;
	if (RefreshAccumulator < HudRefreshSeconds)
	{
		return;
	}
	RefreshAccumulator = 0.f;
	RefreshFromPlayer();
}

void UQinglanHUDWidget::RefreshFromPlayer()
{
	if (!HealthBar && !EnergyBar && !StaminaBar && !BarrierText && SkillTexts.Num() == 0)
	{
		return; // no named elements bound: presentation stays with the fallbacks
	}
	AQinglanCharacter* Player = GetOwningPlayerPawn<AQinglanCharacter>();
	UQinglanCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
	if (!Combat)
	{
		return; // pawn not in the world yet (loading order): next refresh retries
	}

	// Vitals (docs/02 section 3 vitals block; percentages clamped 0..1).
	const FQinglanAttributes& Attributes = Combat->GetAttributes();
	if (HealthBar)
	{
		HealthBar->SetPercent(SafeFraction(Attributes.Health, Attributes.MaxHealth));
	}
	if (EnergyBar)
	{
		EnergyBar->SetPercent(SafeFraction(Attributes.SpiritualEnergy, Attributes.MaxSpiritualEnergy));
	}
	if (StaminaBar)
	{
		StaminaBar->SetPercent(SafeFraction(Attributes.Stamina, Attributes.MaxStamina));
	}
	if (BarrierText)
	{
		// "hud.barrier" = "Barrier {amount}"; amounts are whole barrier points.
		BarrierText->SetText(UQinglanHUDStatics::FormatGameTextWithNumber(
			GBarrierTextKey, TEXT("amount"), FMath::RoundToInt(Combat->GetBarrierAmount())));
	}

	// Technique loadout: slot name from the string table via NameTextKey,
	// dimmed while its cooldown runs (docs/11 "Active skill | Technique").
	UQinglanTechniqueComponent* Techniques = Player->GetTechniques();
	if (!Techniques || SkillTexts.Num() == 0)
	{
		return;
	}
	const TArray<UQinglanTechniqueDataAsset*> Loadout = Techniques->GetLoadout();
	for (int32 Index = 0; Index < SkillTexts.Num(); ++Index)
	{
		UTextBlock* SlotText = SkillTexts[Index];
		if (!SlotText)
		{
			continue;
		}
		UQinglanTechniqueDataAsset* Technique = Loadout.IsValidIndex(Index) ? Loadout[Index] : nullptr;
		if (!Technique)
		{
			SlotText->SetText(FText::GetEmpty()); // unequipped slot renders blank
			continue;
		}
		FFormatNamedArguments Args;
		Args.Add(TEXT("index"), FText::AsNumber(Index + 1));
		Args.Add(TEXT("name"), UQinglanHUDStatics::GameText(Technique->NameTextKey));
		SlotText->SetText(FText::Format(UQinglanHUDStatics::GameText(GSkillSlotTextKey), Args));
		const bool bOnCooldown = Techniques->GetCooldownRemaining(Technique->TechniqueId) > 0.f;
		SlotText->SetRenderOpacity(bOnCooldown ? SkillCooldownOpacity : SkillReadyOpacity);
	}
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
