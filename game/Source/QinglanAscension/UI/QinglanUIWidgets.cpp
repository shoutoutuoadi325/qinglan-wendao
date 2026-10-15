// Base widget classes implementation + the UI event relay body.
//
// Docs: docs/11-english-style-and-ui.md (UMG receives FText from string
// tables; live refresh without restart), docs/dev/implementation-blueprint.md
// section 1.1 (typed events only; no global untyped event bus).
//
// Dialogue/HUD subsystem bindings use their outer-owned event relay. Choice
// buttons emit requests to Character, which clears the old menu before selecting
// so synchronous story events can safely present the next group.
//
// The journal remains Blueprint-authored. Ending confirmation has a native
// runtime tree so stale placeholder Blueprint assets cannot remove its controls.
#include "UI/QinglanUIWidgets.h"

#include "Abilities/QinglanTechniqueTypes.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h" // UWidgetTree for named-element lookup/creation
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Components/ProgressBar.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CheckBox.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryRuntime.h"
#include "Narrative/QinglanStorySubsystem.h"
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

TSharedRef<SWidget> UQinglanDialogueWidget::RebuildWidget()
{
	// A fresh tree avoids old authored widgets with identical names retaining
	// fixed Canvas geometry. Keep presenter names for existing lookups and assets.
	WidgetTree = NewObject<UWidgetTree>(this);
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Root;
	DialoguePanel = WidgetTree->ConstructWidget<UBorder>();
	DialoguePanel->SetBrushColor(FLinearColor(0.015f, 0.025f, 0.035f, 0.96f));
	DialoguePanel->SetPadding(FMargin(20.f, 12.f));
	UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(DialoguePanel);
	PanelSlot->SetAnchors(FAnchors(0.06f, 0.48f, 0.94f, 0.97f));
	PanelSlot->SetOffsets(FMargin(0.f));
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
	DialoguePanel->AddChild(Content);
	SpeakerText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Txt_Speaker"));
	SpeakerText->SetAutoWrapText(true);
	FSlateFontInfo SpeakerFont = SpeakerText->GetFont();
	SpeakerFont.Size = 22;
	SpeakerText->SetFont(SpeakerFont);
	Content->AddChildToVerticalBox(SpeakerText)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	UTextBlock* SourceKind = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Txt_SourceKind"));
	SourceKind->SetAutoWrapText(true);
	FSlateFontInfo SourceFont = SourceKind->GetFont();
	SourceFont.Size = 14;
	SourceKind->SetFont(SourceFont);
	Content->AddChildToVerticalBox(SourceKind);
	LineScroll = WidgetTree->ConstructWidget<UScrollBox>();
	UVerticalBoxSlot* LineSlot = Content->AddChildToVerticalBox(LineScroll);
	FSlateChildSize LineSize;
	LineSize.SizeRule = ESlateSizeRule::Fill;
	LineSize.Value = 0.4f;
	LineSlot->SetSize(LineSize);
	LineSlot->SetPadding(FMargin(0.f, 4.f));
	LineText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Txt_Line"));
	LineText->SetAutoWrapText(true);
	FSlateFontInfo LineFont = LineText->GetFont();
	LineFont.Size = 18;
	LineText->SetFont(LineFont);
	LineScroll->AddChild(LineText);
	ChoiceScroll = WidgetTree->ConstructWidget<UScrollBox>();
	ChoiceScroll->SetScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll);
	UVerticalBoxSlot* ChoiceSlot = Content->AddChildToVerticalBox(ChoiceScroll);
	FSlateChildSize ChoiceSize;
	ChoiceSize.SizeRule = ESlateSizeRule::Fill;
	ChoiceSize.Value = 0.6f;
	ChoiceSlot->SetSize(ChoiceSize);
	ChoiceSlot->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	ChoicesBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box_Choices"));
	ChoiceScroll->AddChild(ChoicesBox);
	return Super::RebuildWidget();
}

void UQinglanDialogueWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// Subscribe OnSceneLine/OnChoicesPresented and forward to the
	// BlueprintImplementableEvents declared on this scaffold class.
	UQinglanUIEventRelay* Relay = AcquireRelay(this);
	Relay->SetupDialogue(this);
	BindNamedElements();
	SetCurrentLine(CurrentSpeaker, CurrentLineText);
	const int32 PreviousSelection = SelectedChoiceIndex;
	SetChoiceEntries(CurrentChoices);
	SetSelectedChoiceIndex(PreviousSelection);
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
		SourceKindText->SetText(CurrentSourceLabel);
	}
}

void UQinglanDialogueWidget::SetSourceKind(EQinglanSourceKind SourceKind)
{
	const TCHAR* Token = TEXT("live");
	switch (SourceKind)
	{
	case EQinglanSourceKind::Paper: Token = TEXT("paper"); break;
	case EQinglanSourceKind::Letter: Token = TEXT("letter"); break;
	case EQinglanSourceKind::System: Token = TEXT("system"); break;
	case EQinglanSourceKind::Memory: Token = TEXT("memory"); break;
	case EQinglanSourceKind::Recorded: Token = TEXT("recorded"); break;
	default: break;
	}
	CurrentSourceLabel = UQinglanHUDStatics::GameText(FName(*FString::Printf(TEXT("story.speaker.source.%s"), Token)));
	if (UTextBlock* Text = WidgetTree ? WidgetTree->FindWidget<UTextBlock>(TEXT("Txt_SourceKind")) : nullptr)
	{
		Text->SetText(CurrentSourceLabel);
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
	if (LineScroll) { LineScroll->ScrollToStart(); }
	if (DialoguePanel)
	{
		DialoguePanel->SetVisibility(CurrentSpeaker.IsEmpty() && CurrentLineText.IsEmpty() && CurrentChoices.IsEmpty()
			? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	return SpeakerText || LineText;
}

bool UQinglanDialogueWidget::SetChoiceEntries(const TArray<FQinglanSceneChoice>& Choices)
{
	CurrentChoices = Choices;
	ChoiceButtons.Reset();
	ChoiceButtons.SetNum(Choices.Num());
	ChoiceClickRelays.Reset();
	SelectedChoiceIndex = INDEX_NONE;
	if (ChoiceScroll)
	{
		ChoiceScroll->SetVisibility(Choices.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		ChoiceScroll->ScrollToStart();
	}
	if (DialoguePanel)
	{
		DialoguePanel->SetVisibility(CurrentSpeaker.IsEmpty() && CurrentLineText.IsEmpty() && CurrentChoices.IsEmpty()
			? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
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
		EntryText->SetAutoWrapText(true);
		FSlateFontInfo ChoiceFont = EntryText->GetFont();
		ChoiceFont.Size = 17;
		EntryText->SetFont(ChoiceFont);
		UButton* Button = Tree->ConstructWidget<UButton>();
		Button->AddChild(EntryText);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(EntryText->Slot))
		{
			ContentSlot->SetPadding(FMargin(12.f, 8.f));
		}
		Button->SetIsEnabled(Choices[Index].UnmetRequirements.IsEmpty());
		Button->SetBackgroundColor(FLinearColor(0.07f, 0.12f, 0.16f));
		ChoicesBox->AddChildToVerticalBox(Button)->SetPadding(FMargin(0.f, 4.f));
		UQinglanDialogueChoiceClickRelay* ClickRelay = NewObject<UQinglanDialogueChoiceClickRelay>(this);
		ClickRelay->Initialize(this, Choices[Index].ChoiceId);
		Button->OnClicked.AddDynamic(ClickRelay, &UQinglanDialogueChoiceClickRelay::HandleClicked);
		ChoiceButtons[Index] = Button;
		ChoiceClickRelays.Add(ClickRelay);
	}
	return true;
}

void UQinglanDialogueChoiceClickRelay::Initialize(UQinglanDialogueWidget* InOwner, FName InChoiceId)
{
	OwnerWidget = InOwner;
	ChoiceId = InChoiceId;
}

void UQinglanDialogueChoiceClickRelay::HandleClicked()
{
	if (UQinglanDialogueWidget* Owner = OwnerWidget.Get())
	{
		Owner->RequestChoiceSelection(ChoiceId);
	}
}

void UQinglanDialogueWidget::RequestChoiceSelection(FName ChoiceId)
{
	for (const FQinglanSceneChoice& Choice : CurrentChoices)
	{
		if (Choice.ChoiceId == ChoiceId && Choice.UnmetRequirements.IsEmpty())
		{
			// Request only. SelectChoice can synchronously present the next menu;
			// Character owns clear-before-select ordering for every input surface.
			OnChoiceSelected.Broadcast(ChoiceId);
			return;
		}
	}
}

void UQinglanDialogueWidget::SetSelectedChoiceIndex(int32 Index)
{
	SelectedChoiceIndex = CurrentChoices.IsValidIndex(Index) ? Index : INDEX_NONE;
	for (int32 ButtonIndex = 0; ButtonIndex < ChoiceButtons.Num(); ++ButtonIndex)
	{
		if (UButton* Button = ChoiceButtons[ButtonIndex])
		{
			Button->SetBackgroundColor(ButtonIndex == SelectedChoiceIndex
				? FLinearColor(0.18f, 0.45f, 0.48f) : FLinearColor(0.07f, 0.12f, 0.16f));
		}
	}
	if (ChoiceButtons.IsValidIndex(SelectedChoiceIndex) && ChoiceButtons[SelectedChoiceIndex]
		&& ChoiceButtons[SelectedChoiceIndex]->GetIsEnabled() && GetOwningPlayer())
	{
		ChoiceButtons[SelectedChoiceIndex]->SetUserFocus(GetOwningPlayer());
	}
	if (ChoiceScroll && ChoiceButtons.IsValidIndex(SelectedChoiceIndex) && ChoiceButtons[SelectedChoiceIndex])
	{
		ChoiceScroll->ScrollWidgetIntoView(ChoiceButtons[SelectedChoiceIndex], true, EDescendantScrollDestination::IntoView);
	}
}

bool UQinglanDialogueWidget::ShowRecordEntry(const FText& Title, const FText& Body)
{
	LastRecordTitle = Title;
	LastRecordBody = Body;
	// The current record registry has no source-kind field; do not carry a
	// previous live dialogue's label onto an unclassified collected record.
	CurrentSourceLabel = FText::GetEmpty();
	if (UTextBlock* Text = WidgetTree ? WidgetTree->FindWidget<UTextBlock>(TEXT("Txt_SourceKind")) : nullptr)
	{
		Text->SetText(CurrentSourceLabel);
	}
	// Records reuse the line presenter: the title reads as the speaker slot.
	return SetCurrentLine(Title, Body);
}

TSharedRef<SWidget> UQinglanEndingWidget::RebuildWidget()
{
	BuildEndingTree();
	return Super::RebuildWidget();
}

void UQinglanEndingWidget::BuildEndingTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("EndingRuntimeTree"));
	}
	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
	Backdrop->SetBrushColor(FLinearColor(0.015f, 0.025f, 0.035f, 0.96f));
	Backdrop->SetPadding(FMargin(32.f));
	Backdrop->SetHorizontalAlignment(HAlign_Center);
	Backdrop->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Backdrop;
	USizeBox* PanelSize = WidgetTree->ConstructWidget<USizeBox>();
	PanelSize->SetWidthOverride(800.f);
	PanelSize->SetMaxDesiredHeight(660.f);
	Backdrop->AddChild(PanelSize);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	PanelSize->AddChild(Scroll);
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
	Scroll->AddChild(Content);
	const auto AddText = [this, Content](int32 FontSize, FLinearColor Color)
	{
		UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
		Text->SetAutoWrapText(true);
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = FontSize;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(Color));
		Content->AddChildToVerticalBox(Text)->SetPadding(FMargin(0.f, 8.f));
		return Text;
	};
	EndingTitle = AddText(26, FLinearColor::White);
	EndingPreview = AddText(17, FLinearColor::White);
	EndingRequirements = AddText(16, FLinearColor(1.f, 0.8f, 0.35f));
	EndingWarning = AddText(16, FLinearColor(1.f, 0.8f, 0.35f));
	WarningAcknowledgment = WidgetTree->ConstructWidget<UCheckBox>();
	AcknowledgmentLabel = WidgetTree->ConstructWidget<UTextBlock>();
	AcknowledgmentLabel->SetAutoWrapText(true);
	WarningAcknowledgment->AddChild(AcknowledgmentLabel);
	Content->AddChildToVerticalBox(WarningAcknowledgment)->SetPadding(FMargin(0.f, 12.f));
	EndingError = AddText(16, FLinearColor(1.f, 0.45f, 0.4f));
	UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
	Content->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0.f, 12.f));
	ConfirmButton = WidgetTree->ConstructWidget<UButton>();
	ConfirmLabel = WidgetTree->ConstructWidget<UTextBlock>();
	ConfirmButton->AddChild(ConfirmLabel);
	Actions->AddChildToHorizontalBox(ConfirmButton)->SetPadding(FMargin(0.f, 0.f, 20.f, 0.f));
	CancelButton = WidgetTree->ConstructWidget<UButton>();
	CancelLabel = WidgetTree->ConstructWidget<UTextBlock>();
	CancelButton->AddChild(CancelLabel);
	Actions->AddChild(CancelButton);
	ConfirmButton->OnClicked.AddDynamic(this, &UQinglanEndingWidget::HandleConfirmPressed);
	CancelButton->OnClicked.AddDynamic(this, &UQinglanEndingWidget::CancelPreview);
	WarningAcknowledgment->OnCheckStateChanged.AddDynamic(this, &UQinglanEndingWidget::HandleAcknowledgmentChanged);
}

void UQinglanEndingWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RefreshPresentation();
	if (ConfirmButton && GetOwningPlayer())
	{
		if (bConfirmRequested) { WarningAcknowledgment->SetUserFocus(GetOwningPlayer()); }
		else { ConfirmButton->SetUserFocus(GetOwningPlayer()); }
	}
}

void UQinglanEndingWidget::ShowEnding(FName EndingId, bool bConfirmationRequested)
{
	CurrentEndingId = QinglanEnding::EndingEnumToId(QinglanEnding::EndingIdToEnum(EndingId));
	bConfirmRequested = bConfirmationRequested;
	bWarningAcknowledged = false; // every request requires its own deliberate acknowledgment
	bClosing = false;
	LastError = FText::GetEmpty();
	if (WarningAcknowledgment)
	{
		WarningAcknowledgment->SetIsChecked(false);
	}
	RefreshPresentation();
	if (ConfirmButton && GetOwningPlayer())
	{
		if (bConfirmRequested) { WarningAcknowledgment->SetUserFocus(GetOwningPlayer()); }
		else { ConfirmButton->SetUserFocus(GetOwningPlayer()); }
	}
}

void UQinglanEndingWidget::FocusDefaultControl()
{
	if (!GetOwningPlayer()) { return; }
	if (bConfirmRequested && WarningAcknowledgment) { WarningAcknowledgment->SetUserFocus(GetOwningPlayer()); }
	else if (ConfirmButton) { ConfirmButton->SetUserFocus(GetOwningPlayer()); }
}

void UQinglanEndingWidget::RefreshPresentation()
{
	if (!EndingTitle)
	{
		return; // ShowEnding may run before AddToViewport/RebuildWidget.
	}
	const EQinglanEnding Ending = QinglanEnding::EndingIdToEnum(CurrentEndingId);
	const TCHAR* Token = Ending == EQinglanEnding::Open ? TEXT("open")
		: Ending == EQinglanEnding::Seal ? TEXT("seal") : TEXT("shared");
	EndingTitle->SetText(Ending == EQinglanEnding::None
		? UQinglanHUDStatics::GameText(FName(TEXT("ending.unavailable")))
		: UQinglanHUDStatics::GameText(FName(*FString::Printf(TEXT("story.ch06.s04.%s_title"), Token))));
	EndingPreview->SetText(Ending == EQinglanEnding::None ? FText::GetEmpty()
		: UQinglanHUDStatics::GameText(FName(*FString::Printf(TEXT("story.ch06.s04.%s_preview"), Token))));
	EndingWarning->SetText(UQinglanHUDStatics::GameText(FName(TEXT("story.ch06.s04.confirm_warning"))));
	AcknowledgmentLabel->SetText(UQinglanHUDStatics::GameText(FName(TEXT("ending.warning_acknowledgment"))));
	ConfirmLabel->SetText(UQinglanHUDStatics::GameText(FName(bConfirmRequested ? TEXT("action.confirm") : TEXT("action.continue"))));
	CancelLabel->SetText(UQinglanHUDStatics::GameText(FName(TEXT("action.cancel"))));
	UQinglanStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	FText Requirements;
	const bool bAvailable = Story && Story->IsEndingAvailable(CurrentEndingId, Requirements) && !Story->IsEndingConfirmed();
	const bool bPreviewed = Story && QinglanStoryRuntime::WasEndingPreviewed(Story, CurrentEndingId);
	EndingRequirements->SetText(bAvailable ? FText::GetEmpty() : Requirements);
	EndingRequirements->SetVisibility(bAvailable ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	if (!Story || Ending == EQinglanEnding::None)
	{
		LastError = UQinglanHUDStatics::GameText(FName(TEXT("ending.unavailable")));
	}
	else if (bConfirmRequested && !bPreviewed && LastError.IsEmpty())
	{
		LastError = UQinglanHUDStatics::GameText(FName(TEXT("ending.preview_required")));
	}
	EndingError->SetText(LastError);
	EndingError->SetVisibility(LastError.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	WarningAcknowledgment->SetVisibility(bConfirmRequested ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	ConfirmButton->SetIsEnabled(!bClosing && (!bConfirmRequested || (bWarningAcknowledged && bAvailable && bPreviewed)));
}

void UQinglanEndingWidget::HandleAcknowledgmentChanged(bool bChecked)
{
	bWarningAcknowledged = bConfirmRequested && bChecked;
	LastError = FText::GetEmpty();
	RefreshPresentation();
}

void UQinglanEndingWidget::ToggleWarningAcknowledgment()
{
	if (bConfirmRequested && !bClosing && WarningAcknowledgment)
	{
		const bool bChecked = !bWarningAcknowledged;
		WarningAcknowledgment->SetIsChecked(bChecked);
		// SetIsChecked does not guarantee a user-style check-state event on every UMG version.
		HandleAcknowledgmentChanged(bChecked);
	}
}

void UQinglanEndingWidget::HandleConfirmPressed()
{
	ConfirmCurrentEnding();
}

bool UQinglanEndingWidget::ConfirmCurrentEnding()
{
	if (bClosing)
	{
		return false;
	}
	if (!bConfirmRequested)
	{
		// Continue returns to ending choices while preserving the inspected preview.
		// Cancel has a separate button/path that discards pending confirmation state.
		bClosing = true;
		RemoveFromParent();
		OnEndingClosed.Broadcast();
		return false;
	}
	if (!bWarningAcknowledged)
	{
		LastError = UQinglanHUDStatics::GameText(FName(TEXT("story.ch06.s04.confirm_warning")));
		RefreshPresentation();
		return false;
	}
	UQinglanStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	FText Error;
	if (!Story || !Story->ConfirmEnding(CurrentEndingId, bWarningAcknowledged, Error))
	{
		LastError = Error.IsEmpty() ? UQinglanHUDStatics::GameText(FName(TEXT("ending.unavailable"))) : Error;
		RefreshPresentation();
		return false;
	}
	bClosing = true;
	OnEndingCommitted.Broadcast(CurrentEndingId);
	RemoveFromParent();
	OnEndingClosed.Broadcast();
	return true;
}

void UQinglanEndingWidget::CancelPreview()
{
	if (bClosing)
	{
		return;
	}
	bClosing = true;
	if (UQinglanStorySubsystem* Story = GetGameInstance() ? GetGameInstance()->GetSubsystem<UQinglanStorySubsystem>() : nullptr)
	{
		Story->CancelEndingPreview(); // transient confirmation state only; no campaign flags
	}
	bWarningAcknowledged = false;
	RemoveFromParent();
	OnEndingClosed.Broadcast();
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
