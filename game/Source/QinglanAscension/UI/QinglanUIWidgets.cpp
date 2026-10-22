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
// AUD-51/52 add a native menu container (pause/profile/settings/equipment/
// offers/outcome) and a native journal record viewer. Both build their own
// trees so stale placeholder Blueprints cannot remove required controls; the
// pure state structs at the top of QinglanUIWidgets.h remain automation-safe.
#include "UI/QinglanUIWidgets.h"

#include "Abilities/QinglanTechniqueTypes.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h" // UWidgetTree for named-element lookup/creation
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTalismanComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Core/QinglanLocalizationService.h"
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
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Persistence/QinglanSaveSubsystem.h"
#include "Platform/QinglanGraphicsPresets.h"
#include "Platform/QinglanGraphicsSettingsService.h"
#include "Progression/QinglanEconomyLibrary.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"
#include "Progression/QinglanRunSubsystem.h"
#include "Narrative/QinglanEndingRules.h"
#include "Narrative/QinglanStoryDataTypes.h"
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

	/** AUD-51: ambient bark/record notice lifetime in the HUD text region. */
	constexpr float HudBarkSeconds = 6.f;

	// Menu row colors (selected row matches the dialogue choice highlight).
	const FLinearColor MenuRowColor(0.07f, 0.12f, 0.16f);
	const FLinearColor MenuRowSelectedColor(0.18f, 0.45f, 0.48f);
	const FLinearColor MenuBodyColor(0.92f, 0.95f, 0.97f);
	const FLinearColor MenuMutedColor(0.62f, 0.68f, 0.72f);
	const FLinearColor MenuNoticeColor(1.f, 0.55f, 0.45f);

	/**
	 * String-table keys for the C++ presenter rows (docs/11 "stable keys";
	 * localization/en-US.json + zh-Hans.json, mirrored placeholders).
	 * FIXME-CONTRACT: no design doc pins these key names; they follow the
	 * existing "hud.*"/"dialogue.*" groups.
	 */
	const TCHAR* GBarrierTextKey = TEXT("hud.barrier");
	const TCHAR* GSkillSlotTextKey = TEXT("hud.skill_slot");
	const TCHAR* GChoiceEntryTextKey = TEXT("dialogue.choice_entry");

	/**
	 * AUD-52: long-text helper. Font size is set on FSlateFontInfo (not a
	 * render-scale transform), auto-wrap is always on, and the caller decides
	 * whether the block lives inside a scroll box. The optional name keeps the
	 * bootstrap's named-lookup convention for authored trees.
	 */
	UTextBlock* QinglanUICreateText(UWidgetTree* Tree, int32 FontSize,
		const FLinearColor& Color, FName Name = NAME_None)
	{
		if (!Tree)
		{
			return nullptr;
		}
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Text->SetAutoWrapText(true);
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = FontSize;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(Color));
		return Text;
	}

	UTextBlock* QinglanUIAddText(UWidgetTree* Tree, UVerticalBox* Box, int32 FontSize,
		const FLinearColor& Color, FName Name = NAME_None, float BottomPadding = 6.f)
	{
		UTextBlock* Text = QinglanUICreateText(Tree, FontSize, Color, Name);
		if (Text && Box)
		{
			Box->AddChildToVerticalBox(Text)->SetPadding(FMargin(0.f, 0.f, 0.f, BottomPadding));
		}
		return Text;
	}

	/** FText::Format over one localized pattern; no display string lives in code. */
	FText QinglanUIFormat(FName Key, FFormatNamedArguments Args)
	{
		return FText::Format(UQinglanHUDStatics::GameText(Key), Args);
	}

	/**
	 * AUD-51 input routing: mouse clicks move Slate focus onto buttons; arrow/
	 * Enter/Escape must keep reaching the Character's menu bindings. Returning
	 * focus to the game viewport keeps both paths working (mouse hit-testing is
	 * independent of keyboard focus).
	 */
	void QinglanUIReturnKeyboardToGame()
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetAllUserFocusToGameViewport();
		}
	}

	/** Catalog display name, falling back to the stable id when unauthored. */
	FText QinglanUILocalizedOrId(FName Key, FName FallbackId)
	{
		if (!Key.IsNone() && UQinglanHUDStatics::HasGameText(Key))
		{
			return UQinglanHUDStatics::GameText(Key);
		}
		return FText::FromName(FallbackId);
	}

	const FQinglanRegistryScene* QinglanUIFindScene(const UQinglanStoryRegistry* Registry, FName SceneId)
	{
		if (!Registry || SceneId.IsNone())
		{
			return nullptr;
		}
		for (const FQinglanRegistryScene& Scene : Registry->MainScenes)
		{
			if (Scene.SceneId == SceneId)
			{
				return &Scene;
			}
		}
		for (const FQinglanRegistrySideQuest& Quest : Registry->SideQuests)
		{
			for (const FQinglanRegistryScene& Stage : Quest.Stages)
			{
				if (Stage.SceneId == SceneId)
				{
					return &Stage;
				}
			}
		}
		return nullptr;
	}

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

bool UQinglanDialogueWidget::SetCurrentLine(const FText& InSpeakerName, const FText& InLineText, int32 FontSize)
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
		// AUD-52: long prose scales through the actual font size. FontSize 0
		// keeps the existing authored default for callers that do not opt in.
		if (FontSize > 0)
		{
			FSlateFontInfo Font = LineText->GetFont();
			Font.Size = FontSize;
			LineText->SetFont(Font);
		}
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

bool UQinglanDialogueWidget::ShowRecordEntry(const FText& Title, const FText& Body, int32 BodyFontSize)
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
	return SetCurrentLine(Title, Body, BodyFontSize);
}

// ---------------------------------------------------------------------------
// AUD-52 journal record viewer
// ---------------------------------------------------------------------------

TSharedRef<SWidget> UQinglanJournalWidget::RebuildWidget()
{
	// Native tree so stale placeholder Blueprints cannot remove the scroll,
	// close, source-tag or record-list controls.
	WidgetTree = NewObject<UWidgetTree>(this);
	BuildJournalTree();
	return Super::RebuildWidget();
}

void UQinglanJournalWidget::BuildJournalTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("JournalRuntimeTree"));
	}
	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
	Backdrop->SetBrushColor(FLinearColor(0.015f, 0.025f, 0.035f, 0.97f));
	Backdrop->SetPadding(FMargin(32.f));
	Backdrop->SetHorizontalAlignment(HAlign_Center);
	Backdrop->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Backdrop;

	USizeBox* PanelSize = WidgetTree->ConstructWidget<USizeBox>();
	PanelSize->SetWidthOverride(1200.f);
	PanelSize->SetHeightOverride(860.f);
	PanelSize->SetMaxDesiredHeight(860.f);
	Backdrop->AddChild(PanelSize);
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
	PanelSize->AddChild(Content);

	TitleText = QinglanUIAddText(WidgetTree, Content, 30, MenuBodyColor, TEXT("Txt_Title"), 4.f);
	SourceText = QinglanUIAddText(WidgetTree, Content, 16, MenuMutedColor, TEXT("Txt_SourceKind"), 2.f);
	DateText = QinglanUIAddText(WidgetTree, Content, 15, MenuMutedColor, TEXT("Txt_Date"), 6.f);

	RecordListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	RecordListScroll->SetIsFocusable(false); // keyboard stays on gameplay bindings
	UVerticalBoxSlot* ListSlot = Content->AddChildToVerticalBox(RecordListScroll);
	FSlateChildSize ListSize;
	ListSize.SizeRule = ESlateSizeRule::Fill;
	ListSize.Value = 0.35f;
	ListSlot->SetSize(ListSize);
	ListSlot->SetPadding(FMargin(0.f, 4.f));
	RecordRowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box_Records"));
	RecordListScroll->AddChild(RecordRowsBox);

	BodyScroll = WidgetTree->ConstructWidget<UScrollBox>();
	BodyScroll->SetIsFocusable(false);
	UVerticalBoxSlot* BodySlot = Content->AddChildToVerticalBox(BodyScroll);
	FSlateChildSize BodySize;
	BodySize.SizeRule = ESlateSizeRule::Fill;
	BodySize.Value = 0.55f;
	BodySlot->SetSize(BodySize);
	BodySlot->SetPadding(FMargin(0.f, 8.f));
	BodyText = QinglanUICreateText(WidgetTree, 20, MenuBodyColor, TEXT("Txt_Body"));
	BodyScroll->AddChild(BodyText);

	HintText = QinglanUIAddText(WidgetTree, Content, 14, MenuMutedColor, TEXT("Txt_Hint"), 6.f);
	UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
	Content->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0.f, 4.f));
	CloseButton = WidgetTree->ConstructWidget<UButton>();
	CloseLabel = QinglanUICreateText(WidgetTree, 17, MenuBodyColor);
	CloseButton->AddChild(CloseLabel);
	if (UButtonSlot* CloseSlot = Cast<UButtonSlot>(CloseLabel->Slot))
	{
		CloseSlot->SetPadding(FMargin(12.f, 8.f));
	}
	Actions->AddChildToHorizontalBox(CloseButton);
	CloseButton->OnClicked.AddDynamic(this, &UQinglanJournalWidget::HandleClosePressed);
}

void UQinglanJournalWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RefreshFromStory();
	RefreshPresentation();
}

void UQinglanJournalWidget::SetRecordViews(const TArray<FQinglanJournalRecordView>& InRecords)
{
	RecordViews = InRecords;
	JournalState.RecordCount = RecordViews.Num();
	if (RecordViews.Num() == 0)
	{
		JournalState.SelectedRecordIndex = 0;
	}
	else
	{
		JournalState.SelectedRecordIndex = FMath::Clamp(JournalState.SelectedRecordIndex, 0, RecordViews.Num() - 1);
	}
	RebuildRecordRows();
	RefreshPresentation();
}

void UQinglanJournalWidget::RefreshFromStory()
{
	TArray<FQinglanJournalRecordView> Views;
	UGameInstance* Instance = GetGameInstance();
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (Registry)
	{
		for (const FQinglanRegistryRecord& Record : Registry->Records)
		{
			const bool bRead = Profile && Profile->HasReadRecord(Record.RecordId);
			bool bUnlocked = true;
			for (const FName RequiredFlag : Record.RequiredFlags)
			{
				if (!Story->HasFlag(RequiredFlag))
				{
					bUnlocked = false;
					break;
				}
			}
			if (!bRead && !bUnlocked)
			{
				continue;
			}
			FQinglanJournalRecordView View;
			View.RecordId = Record.RecordId;
			View.Title = Record.TitleKey.IsNone()
				? FText::FromName(Record.RecordId) : UQinglanHUDStatics::GameText(Record.TitleKey);
			View.Body = Record.BodyKey.IsNone()
				? FText::GetEmpty() : UQinglanHUDStatics::GameText(Record.BodyKey);
			View.DateLabel = Record.DateLabelKey.IsNone()
				? FText::GetEmpty() : UQinglanHUDStatics::GameText(Record.DateLabelKey);
			// FQinglanRegistryRecord carries no source field; this inference is
			// documented on FQinglanJournalState::InferRecordSourceKind.
			View.SourceKind = FQinglanJournalState::InferRecordSourceKind(Record.RecordId);
			View.bRead = bRead;
			Views.Add(View);
		}
	}
	RecordViews = Views;
	JournalState.RecordCount = RecordViews.Num();
	if (RecordViews.Num() == 0)
	{
		JournalState.SelectedRecordIndex = 0;
	}
	else
	{
		JournalState.SelectedRecordIndex = FMath::Clamp(JournalState.SelectedRecordIndex, 0, RecordViews.Num() - 1);
	}
	RebuildRecordRows();
	RefreshPresentation();
}

void UQinglanJournalWidget::OpenJournal()
{
	JournalState.OpenJournal();
	RefreshFromStory();
	// Returning players land on the previously viewed record; Enter reopens it
	// (the list stays the visible screen until then).
	if (!JournalState.LastRecordId.IsNone())
	{
		const int32 LastIndex = RecordViews.IndexOfByPredicate([this](const FQinglanJournalRecordView& View)
		{
			return View.RecordId == JournalState.LastRecordId;
		});
		if (LastIndex != INDEX_NONE)
		{
			JournalState.SelectedRecordIndex = LastIndex;
		}
	}
	RefreshPresentation();
	if (RecordListScroll)
	{
		if (RecordButtons.IsValidIndex(JournalState.SelectedRecordIndex)
			&& RecordButtons[JournalState.SelectedRecordIndex])
		{
			RecordListScroll->ScrollWidgetIntoView(RecordButtons[JournalState.SelectedRecordIndex], true,
				EDescendantScrollDestination::IntoView);
		}
		else
		{
			RecordListScroll->ScrollToStart();
		}
	}
}

bool UQinglanJournalWidget::CloseJournal()
{
	if (!JournalState.CloseJournal())
	{
		return false;
	}
	RefreshPresentation();
	// Closing the viewer restores gameplay input: the overlay leaves the
	// viewport before Character clears the AUD-51 gate.
	RemoveFromParent();
	OnJournalClosed.Broadcast();
	return true;
}

bool UQinglanJournalWidget::OpenRecord(FName RecordId)
{
	int32 Index = RecordViews.IndexOfByPredicate(
		[RecordId](const FQinglanJournalRecordView& View) { return View.RecordId == RecordId; });
	if (Index == INDEX_NONE)
	{
		RefreshFromStory();
		Index = RecordViews.IndexOfByPredicate(
			[RecordId](const FQinglanJournalRecordView& View) { return View.RecordId == RecordId; });
	}
	if (Index == INDEX_NONE || !JournalState.OpenRecord(RecordId))
	{
		return false;
	}
	JournalState.SelectedRecordIndex = Index;
	// Viewing through the journal counts as reading the record. The collection
	// flag itself remains owned by the world inspection path.
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UQinglanProfileSubsystem* Profile = Instance->GetSubsystem<UQinglanProfileSubsystem>())
		{
			Profile->MarkRecordRead(RecordId);
		}
	}
	RefreshPresentation();
	if (BodyScroll)
	{
		BodyScroll->ScrollToStart();
	}
	return true;
}

bool UQinglanJournalWidget::CloseRecord()
{
	if (!JournalState.CloseRecord())
	{
		return false;
	}
	RefreshPresentation();
	return true;
}

bool UQinglanJournalWidget::ReopenLastRecord()
{
	if (JournalState.LastRecordId.IsNone())
	{
		return false;
	}
	return OpenRecord(JournalState.LastRecordId);
}

bool UQinglanJournalWidget::MoveRecordSelection(int32 Direction)
{
	if (!JournalState.MoveSelection(Direction))
	{
		return false;
	}
	RefreshPresentation();
	if (RecordListScroll && RecordButtons.IsValidIndex(JournalState.SelectedRecordIndex)
		&& RecordButtons[JournalState.SelectedRecordIndex])
	{
		RecordListScroll->ScrollWidgetIntoView(RecordButtons[JournalState.SelectedRecordIndex], true,
			EDescendantScrollDestination::IntoView);
	}
	return true;
}

bool UQinglanJournalWidget::SelectCurrentRecord()
{
	if (!RecordViews.IsValidIndex(JournalState.SelectedRecordIndex))
	{
		return false;
	}
	return OpenRecord(RecordViews[JournalState.SelectedRecordIndex].RecordId);
}

void UQinglanJournalWidget::ShowRecordView(int32 RecordIndex)
{
	if (!RecordViews.IsValidIndex(RecordIndex))
	{
		return;
	}
	OpenRecord(RecordViews[RecordIndex].RecordId);
}

bool UQinglanJournalWidget::Back()
{
	if (!JournalState.IsOpen())
	{
		return false;
	}
	if (JournalState.IsRecordOpen())
	{
		CloseRecord();
		return true;
	}
	CloseJournal();
	return false;
}

void UQinglanJournalWidget::HandleClosePressed()
{
	Back();
	QinglanUIReturnKeyboardToGame();
}

void UQinglanJournalWidget::RebuildRecordRows()
{
	if (!RecordRowsBox)
	{
		return;
	}
	RecordRowsBox->ClearChildren();
	RecordButtons.Reset();
	RecordClickRelays.Reset();
	for (int32 Index = 0; Index < RecordViews.Num(); ++Index)
	{
		UTextBlock* Label = QinglanUICreateText(WidgetTree, 18, MenuBodyColor);
		FFormatNamedArguments Args;
		Args.Add(TEXT("index"), FText::AsNumber(Index + 1));
		Args.Add(TEXT("title"), RecordViews[Index].Title);
		Label->SetText(QinglanUIFormat(FName(TEXT("menu.record.entry")), Args));
		UButton* Button = WidgetTree->ConstructWidget<UButton>();
		Button->AddChild(Label);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Label->Slot))
		{
			ContentSlot->SetPadding(FMargin(12.f, 8.f));
		}
		Button->SetBackgroundColor(Index == JournalState.SelectedRecordIndex
			? MenuRowSelectedColor : MenuRowColor);
		RecordRowsBox->AddChildToVerticalBox(Button)->SetPadding(FMargin(0.f, 3.f));
		UQinglanJournalRecordClickRelay* Relay = NewObject<UQinglanJournalRecordClickRelay>(this);
		Relay->Initialize(this, Index);
		Button->OnClicked.AddDynamic(Relay, &UQinglanJournalRecordClickRelay::HandleClicked);
		RecordButtons.Add(Button);
		RecordClickRelays.Add(Relay);
	}
}

void UQinglanJournalWidget::RefreshPresentation()
{
	if (!TitleText || !BodyText)
	{
		return; // called before RebuildWidget (AddToViewport)
	}
	const FQinglanJournalRecordView* View = nullptr;
	if (JournalState.IsRecordOpen())
	{
		View = RecordViews.FindByPredicate([this](const FQinglanJournalRecordView& Entry)
		{
			return Entry.RecordId == JournalState.CurrentRecordId;
		});
	}

	if (View)
	{
		TitleText->SetText(View->Title);
		SourceText->SetText(UQinglanHUDStatics::GameText(
			FQinglanJournalState::SourceKindLabelKey(View->SourceKind)));
		SourceText->SetVisibility(ESlateVisibility::Visible);
		DateText->SetText(View->DateLabel);
		DateText->SetVisibility(View->DateLabel.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		BodyText->SetText(View->Body);
		BodyScroll->SetVisibility(ESlateVisibility::Visible);
		RecordListScroll->SetVisibility(ESlateVisibility::Collapsed);
		CloseLabel->SetText(UQinglanHUDStatics::GameText(FName(TEXT("menu.record.back"))));
	}
	else
	{
		TitleText->SetText(UQinglanHUDStatics::GameText(FName(TEXT("menu.journal.title"))));
		SourceText->SetVisibility(ESlateVisibility::Collapsed);
		DateText->SetVisibility(ESlateVisibility::Collapsed);
		BodyText->SetText(RecordViews.Num() == 0
			? UQinglanHUDStatics::GameText(FName(TEXT("menu.journal.empty"))) : FText::GetEmpty());
		BodyScroll->SetVisibility(RecordViews.Num() == 0 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		RecordListScroll->SetVisibility(RecordViews.Num() == 0 ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		CloseLabel->SetText(UQinglanHUDStatics::GameText(FName(TEXT("menu.close"))));
	}
	if (HintText)
	{
		HintText->SetText(UQinglanHUDStatics::GameText(FName(TEXT("menu.journal.hint"))));
	}

	// Selection highlight follows keyboard navigation without rebuilding rows.
	for (int32 Index = 0; Index < RecordButtons.Num(); ++Index)
	{
		if (UButton* Button = RecordButtons[Index])
		{
			Button->SetBackgroundColor(Index == JournalState.SelectedRecordIndex
				? MenuRowSelectedColor : MenuRowColor);
		}
	}
}

void UQinglanJournalRecordClickRelay::Initialize(UQinglanJournalWidget* InOwner, int32 InRecordIndex)
{
	OwnerWidget = InOwner;
	RecordIndex = InRecordIndex;
}

void UQinglanJournalRecordClickRelay::HandleClicked()
{
	if (UQinglanJournalWidget* Owner = OwnerWidget.Get())
	{
		Owner->ShowRecordView(RecordIndex);
		QinglanUIReturnKeyboardToGame();
	}
}

// ---------------------------------------------------------------------------
// AUD-51 native menu container
// ---------------------------------------------------------------------------

TSharedRef<SWidget> UQinglanMenuWidget::RebuildWidget()
{
	WidgetTree = NewObject<UWidgetTree>(this);
	BuildMenuTree();
	return Super::RebuildWidget();
}

void UQinglanMenuWidget::BuildMenuTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("MenuRuntimeTree"));
	}
	MenuPanel = WidgetTree->ConstructWidget<UBorder>();
	MenuPanel->SetBrushColor(FLinearColor(0.015f, 0.025f, 0.035f, 0.97f));
	MenuPanel->SetPadding(FMargin(32.f));
	MenuPanel->SetHorizontalAlignment(HAlign_Center);
	MenuPanel->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = MenuPanel;

	USizeBox* PanelSize = WidgetTree->ConstructWidget<USizeBox>();
	PanelSize->SetWidthOverride(1000.f);
	PanelSize->SetHeightOverride(800.f);
	PanelSize->SetMaxDesiredHeight(800.f);
	MenuPanel->AddChild(PanelSize);
	UVerticalBox* Content = WidgetTree->ConstructWidget<UVerticalBox>();
	PanelSize->AddChild(Content);

	MenuTitle = QinglanUIAddText(WidgetTree, Content, 30, MenuBodyColor, TEXT("Txt_MenuTitle"), 4.f);
	MenuSummary = QinglanUIAddText(WidgetTree, Content, 17, MenuMutedColor, TEXT("Txt_MenuSummary"), 4.f);
	MenuNotice = QinglanUIAddText(WidgetTree, Content, 16, MenuNoticeColor, TEXT("Txt_MenuNotice"), 4.f);

	RowScroll = WidgetTree->ConstructWidget<UScrollBox>();
	RowScroll->SetIsFocusable(false);
	UVerticalBoxSlot* ScrollSlot = Content->AddChildToVerticalBox(RowScroll);
	FSlateChildSize ScrollSize;
	ScrollSize.SizeRule = ESlateSizeRule::Fill;
	ScrollSize.Value = 0.7f;
	ScrollSlot->SetSize(ScrollSize);
	ScrollSlot->SetPadding(FMargin(0.f, 4.f));
	RowBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box_MenuRows"));
	RowScroll->AddChild(RowBox);

	MenuHint = QinglanUIAddText(WidgetTree, Content, 14, MenuMutedColor, TEXT("Txt_MenuHint"), 6.f);
	BackButton = WidgetTree->ConstructWidget<UButton>();
	BackLabel = QinglanUICreateText(WidgetTree, 17, MenuBodyColor);
	BackButton->AddChild(BackLabel);
	if (UButtonSlot* BackSlot = Cast<UButtonSlot>(BackLabel->Slot))
	{
		BackSlot->SetPadding(FMargin(12.f, 8.f));
	}
	Content->AddChildToVerticalBox(BackButton)->SetPadding(FMargin(0.f, 4.f));
	BackButton->OnClicked.AddDynamic(this, &UQinglanMenuWidget::HandleBackPressed);
}

void UQinglanMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();
	RebuildRows();
	RefreshPresentation();
}

AQinglanCharacter* UQinglanMenuWidget::GetQinglanCharacter() const
{
	return Cast<AQinglanCharacter>(GetOwningPlayerPawn());
}

void UQinglanMenuWidget::ShowScreen(EQinglanMenuScreen NewScreen)
{
	if (NewScreen == EQinglanMenuScreen::Closed)
	{
		CloseMenu();
		return;
	}
	if (!MenuState.IsOpen())
	{
		MenuState.Open(NewScreen);
	}
	else
	{
		MenuState.NavigateTo(NewScreen);
	}
	Notice = FText::GetEmpty();
	RebuildRows();
	RefreshPresentation();
	if (GetOwningPlayer() && !IsInViewport())
	{
		AddToViewport(50);
	}
}

void UQinglanMenuWidget::ShowOffers(const TArray<FQinglanOfferChoice>& Offers)
{
	if (!OfferState.BeginOffer(Offers))
	{
		CloseOffer();
		return;
	}
	if (!MenuState.IsOpen())
	{
		MenuState.Open(EQinglanMenuScreen::Offer);
	}
	else
	{
		MenuState.NavigateTo(EQinglanMenuScreen::Offer);
	}
	Notice = FText::GetEmpty();
	RebuildRows();
	RefreshPresentation();
	if (GetOwningPlayer() && !IsInViewport())
	{
		AddToViewport(50);
	}
}

void UQinglanMenuWidget::CloseOffer()
{
	OfferState.Clear();
	Notice = FText::GetEmpty();
	if (MenuState.Screen == EQinglanMenuScreen::Offer)
	{
		MenuState.NavigateTo(EQinglanMenuScreen::Pause);
	}
	RebuildRows();
	RefreshPresentation();
}

void UQinglanMenuWidget::CloseMenu()
{
	MenuState.Close();
	RemoveFromParent();
	OnMenuClosed.Broadcast();
}

bool UQinglanMenuWidget::Back()
{
	if (!MenuState.IsOpen())
	{
		return false;
	}
	// AUD-01: the cold-start slot selector cannot be backed out of; there is
	// no session to return to (BeginPlay deferred all hub arrival work).
	if (MenuState.Screen == EQinglanMenuScreen::Profile)
	{
		UGameInstance* Instance = GetGameInstance();
		UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
		if (Save && !Save->IsProfileSessionActive())
		{
			SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.profile_required"))));
			RefreshPresentation();
			return false;
		}
	}
	if (MenuState.Screen == EQinglanMenuScreen::Offer)
	{
		if (AQinglanCharacter* Character = GetQinglanCharacter())
		{
			return Character->DismissMenuOffer();
		}
		OfferState.Dismiss();
		MenuState.NavigateTo(EQinglanMenuScreen::Pause);
		RebuildRows();
		RefreshPresentation();
		return true;
	}
	Notice = FText::GetEmpty();
	if (MenuState.Back())
	{
		RebuildRows();
		RefreshPresentation();
		return true;
	}
	CloseMenu();
	return false;
}

void UQinglanMenuWidget::HandleBackPressed()
{
	Back();
	QinglanUIReturnKeyboardToGame();
}

bool UQinglanMenuWidget::MoveSelection(int32 Direction)
{
	if (!MenuState.MoveSelection(Direction))
	{
		return false;
	}
	RefreshPresentation();
	return true;
}

bool UQinglanMenuWidget::ConfirmSelection()
{
	if (MenuState.Screen == EQinglanMenuScreen::Offer)
	{
		if (AQinglanCharacter* Character = GetQinglanCharacter())
		{
			return Character->SelectMenuOption(MenuState.SelectionIndex);
		}
		if (const FQinglanOfferChoice* Selected = OfferState.GetSelected())
		{
			return OfferState.Select(Selected->OfferId);
		}
		return false;
	}
	if (!Rows.IsValidIndex(MenuState.SelectionIndex))
	{
		return false;
	}
	ActivateRow(MenuState.SelectionIndex);
	return true;
}

void UQinglanMenuWidget::SetNotice(const FText& InNotice)
{
	Notice = InNotice;
	// Text-only update: this path runs from row click handlers, so it must not
	// destroy the button whose event is still on the stack.
	if (MenuNotice)
	{
		MenuNotice->SetText(Notice);
		MenuNotice->SetVisibility(Notice.IsEmpty()
			? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
}

void UQinglanMenuWidget::Refresh()
{
	RebuildRows();
	RefreshPresentation();
}

void UQinglanMenuWidget::ResetRows()
{
	Rows.Reset();
	RowButtons.Reset();
	RowClickRelays.Reset();
}

void UQinglanMenuWidget::RebuildRows()
{
	ResetRows();
	switch (MenuState.Screen)
	{
	case EQinglanMenuScreen::Pause: ApplyPauseRows(); break;
	case EQinglanMenuScreen::Profile: ApplyProfileRows(); break;
	case EQinglanMenuScreen::Settings: ApplySettingsRows(); break;
	case EQinglanMenuScreen::Equipment: ApplyEquipmentRows(); break;
	case EQinglanMenuScreen::Journal: ApplyJournalRows(); break;
	case EQinglanMenuScreen::Offer: ApplyOfferRows(); break;
	case EQinglanMenuScreen::Outcome: ApplyOutcomeRows(); break;
	default: break;
	}
	MenuState.SetOptionCount(Rows.Num());
}

void UQinglanMenuWidget::ApplyPauseRows()
{
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.resume"))), FName(TEXT("resume")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.settings"))), FName(TEXT("settings")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.profile"))), FName(TEXT("profile")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment"))), FName(TEXT("equipment")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.journal"))), FName(TEXT("journal")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.outcome"))), FName(TEXT("outcome")), NAME_None });
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.quit_sanctuary"))), FName(TEXT("quit")), NAME_None });
}

void UQinglanMenuWidget::ApplyProfileRows()
{
	UGameInstance* Instance = GetGameInstance();
	UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
	if (!Save)
	{
		return;
	}
	for (int32 Slot = 0; Slot < UQinglanSaveSubsystem::ProfileSlotCount; ++Slot)
	{
		const bool bHas = Save->HasProfile(Slot);
		FFormatNamedArguments Args;
		Args.Add(TEXT("index"), FText::AsNumber(Slot + 1));
		Args.Add(TEXT("status"), UQinglanHUDStatics::GameText(
			FName(bHas ? TEXT("menu.profile.occupied") : TEXT("menu.profile.empty"))));
		Rows.Add({ QinglanUIFormat(FName(TEXT("menu.profile.slot")), Args),
			FName(bHas ? TEXT("profile.load") : TEXT("profile.create")),
			FName(*FString::FromInt(Slot)) });
	}
}

void UQinglanMenuWidget::ApplySettingsRows()
{
	UGameInstance* Instance = GetGameInstance();
	UQinglanGraphicsSettingsService* Graphics = Instance ? Instance->GetSubsystem<UQinglanGraphicsSettingsService>() : nullptr;
	if (!Graphics)
	{
		return;
	}
	const FQinglanGraphicsSettings Settings = Graphics->GetCurrentSettings();

	FFormatNamedArguments FlashArgs;
	FlashArgs.Add(TEXT("state"), UQinglanHUDStatics::GameText(
		FName(Settings.bLowFlashMode ? TEXT("menu.settings.on") : TEXT("menu.settings.off"))));
	Rows.Add({ QinglanUIFormat(FName(TEXT("menu.settings.low_flash")), FlashArgs),
		FName(TEXT("settings.lowflash")), NAME_None });

	const FString ActiveCulture = FQinglanLocalizationService::GetRuntimeCultureName();
	for (const TCHAR* Culture : { TEXT("en-US"), TEXT("zh-Hans") })
	{
		FFormatNamedArguments LanguageArgs;
		LanguageArgs.Add(TEXT("language"), UQinglanHUDStatics::GameText(
			FName(FQinglanLocalizationService::IsChineseCulture(Culture)
				? TEXT("language.chinese") : TEXT("language.english"))));
		const bool bActive = FQinglanLocalizationService::IsChineseCulture(Culture)
			? FQinglanLocalizationService::IsChineseCulture(ActiveCulture)
			: !FQinglanLocalizationService::IsChineseCulture(ActiveCulture);
		FText Label = QinglanUIFormat(FName(TEXT("menu.settings.language")), LanguageArgs);
		if (bActive)
		{
			// The active culture is marked without a second localized fragment.
			FFormatNamedArguments ActiveArgs;
			ActiveArgs.Add(TEXT("text"), Label);
			Label = QinglanUIFormat(FName(TEXT("menu.settings.active")), ActiveArgs);
		}
		Rows.Add({ Label, FName(TEXT("settings.language")), FName(Culture) });
	}

	const TArray<EQinglanQualityPreset> Presets = {
		EQinglanQualityPreset::Low, EQinglanQualityPreset::Balanced, EQinglanQualityPreset::High };
	for (const EQinglanQualityPreset Preset : Presets)
	{
		FFormatNamedArguments PresetArgs;
		PresetArgs.Add(TEXT("preset"), UQinglanHUDStatics::GameText(
			FName(QinglanGraphics::PresetDisplayKey(Preset))));
		FText Label = QinglanUIFormat(FName(TEXT("menu.settings.preset")), PresetArgs);
		if (Settings.Preset == Preset)
		{
			FFormatNamedArguments ActiveArgs;
			ActiveArgs.Add(TEXT("text"), Label);
			Label = QinglanUIFormat(FName(TEXT("menu.settings.active")), ActiveArgs);
		}
		Rows.Add({ Label, FName(TEXT("settings.preset")),
			FName(*FString::FromInt(static_cast<int32>(Preset))) });
	}
}

void UQinglanMenuWidget::ApplyEquipmentRows()
{
	UGameInstance* Instance = GetGameInstance();
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (!Profile)
	{
		return;
	}
	const TArray<FName> Equipped = Profile->GetEquippedTechniques();
	for (const TCHAR* TechniqueIdText : QinglanProgression::TechniqueIdTexts)
	{
		const FName TechniqueId(TechniqueIdText);
		if (!Profile->IsTechniqueUnlocked(TechniqueId))
		{
			continue;
		}
		UQinglanTechniqueDataAsset* CatalogEntry = nullptr;
		FName NameKey(*FString::Printf(TEXT("technique.%s.name"), *TechniqueId.ToString()));
		if (const AQinglanCharacter* Character = GetQinglanCharacter())
		{
			if (UQinglanTechniqueComponent* Techniques = Character->GetTechniques())
			{
				CatalogEntry = Techniques->ResolveTechniqueById(TechniqueId);
			}
		}
		if (CatalogEntry && !CatalogEntry->NameTextKey.IsNone())
		{
			NameKey = CatalogEntry->NameTextKey;
		}
		const FText Name = QinglanUILocalizedOrId(NameKey, TechniqueId);
		const bool bEquipped = Equipped.Contains(TechniqueId);
		FFormatNamedArguments Args;
		Args.Add(TEXT("name"), Name);
		Rows.Add({ QinglanUIFormat(FName(bEquipped
				? TEXT("menu.equipment.unequip") : TEXT("menu.equipment.equip")), Args),
			FName(bEquipped ? TEXT("equipment.technique.unequip") : TEXT("equipment.technique.equip")),
			TechniqueId });
	}

	for (const FName RelicId : Profile->GetOwnedRelics())
	{
		FQinglanRelicDef Relic;
		const bool bKnown = UQinglanEconomyLibrary::FindRelic(RelicId, Relic);
		const FText Name = bKnown
			? QinglanUILocalizedOrId(Relic.NameTextKey, RelicId) : FText::FromName(RelicId);
		const bool bEquipped = Profile->GetEquippedRelic() == RelicId;
		FFormatNamedArguments Args;
		Args.Add(TEXT("name"), Name);
		Rows.Add({ QinglanUIFormat(FName(bEquipped
				? TEXT("menu.equipment.relic_unequip") : TEXT("menu.equipment.relic_equip")), Args),
			FName(bEquipped ? TEXT("equipment.relic.unequip") : TEXT("equipment.relic.equip")),
			RelicId });
	}

	const TArray<FName> EquippedPassives = Profile->GetEquippedPassives();
	for (const FName PassiveId : Profile->GetOwnedPassives())
	{
		FQinglanPassiveCatalogEntry Passive;
		const bool bKnown = UQinglanEconomyLibrary::FindPassive(PassiveId, Passive);
		const FText Name = bKnown
			? QinglanUILocalizedOrId(Passive.NameTextKey, PassiveId) : FText::FromName(PassiveId);
		const bool bEquipped = EquippedPassives.Contains(PassiveId);
		FFormatNamedArguments Args;
		Args.Add(TEXT("name"), Name);
		Rows.Add({ QinglanUIFormat(FName(bEquipped
				? TEXT("menu.equipment.passive_unequip") : TEXT("menu.equipment.passive_equip")), Args),
			FName(bEquipped ? TEXT("equipment.passive.unequip") : TEXT("equipment.passive.equip")),
			PassiveId });
	}

	// Talisman slots use the component's equipped list (the profile owns the
	// remaining charges); owned = at least one charge remains.
	AQinglanCharacter* Character = GetQinglanCharacter();
	UQinglanTalismanComponent* TalismanComponent =
		Character ? Character->FindComponentByClass<UQinglanTalismanComponent>() : nullptr;
	if (TalismanComponent)
	{
		const TArray<FName> EquippedTalismans = TalismanComponent->GetEquippedTalismans();
		for (const FQinglanTalismanCatalogEntry& Entry : UQinglanEconomyLibrary::GetTalismanCatalog())
		{
			if (Profile->GetTalismanChargeCount(Entry.TalismanId) <= 0)
			{
				continue;
			}
			const bool bEquipped = EquippedTalismans.Contains(Entry.TalismanId);
			const FText Name = QinglanUILocalizedOrId(Entry.NameTextKey, Entry.TalismanId);
			FFormatNamedArguments Args;
			Args.Add(TEXT("name"), Name);
			Rows.Add({ QinglanUIFormat(FName(bEquipped
					? TEXT("menu.equipment.talisman_unequip") : TEXT("menu.equipment.talisman_equip")), Args),
				FName(bEquipped ? TEXT("equipment.talisman.unequip") : TEXT("equipment.talisman.equip")),
				Entry.TalismanId });
		}
	}
}

void UQinglanMenuWidget::ApplyJournalRows()
{
	UGameInstance* Instance = GetGameInstance();
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	if (!Registry)
	{
		return;
	}
	for (const FQinglanRegistryRecord& Record : Registry->Records)
	{
		const bool bRead = Profile && Profile->HasReadRecord(Record.RecordId);
		bool bUnlocked = true;
		for (const FName RequiredFlag : Record.RequiredFlags)
		{
			if (!Story->HasFlag(RequiredFlag))
			{
				bUnlocked = false;
				break;
			}
		}
		if (!bRead && !bUnlocked)
		{
			continue;
		}
		Rows.Add({ Record.TitleKey.IsNone()
				? FText::FromName(Record.RecordId) : UQinglanHUDStatics::GameText(Record.TitleKey),
			FName(TEXT("journal.open")), Record.RecordId });
	}
}

void UQinglanMenuWidget::ApplyOfferRows()
{
	if (!OfferState.IsActive())
	{
		return;
	}
	for (int32 Index = 0; Index < OfferState.Entries.Num(); ++Index)
	{
		const FQinglanOfferChoice& Entry = OfferState.Entries[Index];
		FFormatNamedArguments Args;
		Args.Add(TEXT("index"), FText::AsNumber(Index + 1));
		Args.Add(TEXT("title"), Entry.Title);
		Args.Add(TEXT("cost"), Entry.Cost);
		Rows.Add({ QinglanUIFormat(FName(Entry.Cost.IsEmpty()
				? TEXT("menu.offer.entry_nocost") : TEXT("menu.offer.entry")), Args),
			FName(TEXT("offer.select")), Entry.OfferId });
	}
}

void UQinglanMenuWidget::ApplyOutcomeRows()
{
	Rows.Add({ UQinglanHUDStatics::GameText(FName(TEXT("menu.back"))), FName(TEXT("back")), NAME_None });
}

FText UQinglanMenuWidget::ResolveTitle() const
{
	switch (MenuState.Screen)
	{
	case EQinglanMenuScreen::Pause: return UQinglanHUDStatics::GameText(FName(TEXT("menu.pause.title")));
	case EQinglanMenuScreen::Profile: return UQinglanHUDStatics::GameText(FName(TEXT("menu.profile.title")));
	case EQinglanMenuScreen::Settings: return UQinglanHUDStatics::GameText(FName(TEXT("menu.settings.title")));
	case EQinglanMenuScreen::Equipment: return UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment.title")));
	case EQinglanMenuScreen::Journal: return UQinglanHUDStatics::GameText(FName(TEXT("menu.journal.title")));
	case EQinglanMenuScreen::Offer: return UQinglanHUDStatics::GameText(FName(TEXT("menu.offer.title")));
	case EQinglanMenuScreen::Outcome: return UQinglanHUDStatics::GameText(FName(TEXT("menu.outcome.title")));
	default: return FText::GetEmpty();
	}
}

FText UQinglanMenuWidget::ResolveSummary() const
{
	UGameInstance* Instance = GetGameInstance();
	if (MenuState.Screen == EQinglanMenuScreen::Equipment)
	{
		UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		if (!Profile)
		{
			return FText::GetEmpty();
		}
		// Four equipped slots, including explicit empty placeholders.
		TArray<FString> TechniqueSlots;
		const TArray<FName> EquippedTechniques = Profile->GetEquippedTechniques();
		for (int32 Slot = 0; Slot < QinglanProgression::EquippedTechniqueLimit; ++Slot)
		{
			FText SlotName = UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment.empty")));
			if (EquippedTechniques.IsValidIndex(Slot))
			{
				const FName TechniqueId = EquippedTechniques[Slot];
				FName NameKey(*FString::Printf(TEXT("technique.%s.name"), *TechniqueId.ToString()));
				SlotName = QinglanUILocalizedOrId(NameKey, TechniqueId);
			}
			FFormatNamedArguments SlotArgs;
			SlotArgs.Add(TEXT("index"), FText::AsNumber(Slot + 1));
			SlotArgs.Add(TEXT("name"), SlotName);
			TechniqueSlots.Add(QinglanUIFormat(FName(TEXT("menu.equipment.slot")), SlotArgs).ToString());
		}
		FString RelicText;
		if (!Profile->GetEquippedRelic().IsNone())
		{
			FQinglanRelicDef Relic;
			const bool bKnown = UQinglanEconomyLibrary::FindRelic(Profile->GetEquippedRelic(), Relic);
			RelicText = (bKnown ? QinglanUILocalizedOrId(Relic.NameTextKey, Profile->GetEquippedRelic())
				: FText::FromName(Profile->GetEquippedRelic())).ToString();
		}
		TArray<FString> PassiveNames;
		TArray<FName> EquippedPassives = Profile->GetEquippedPassives();
		EquippedPassives.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		for (const FName PassiveId : EquippedPassives)
		{
			FQinglanPassiveCatalogEntry Passive;
			const bool bKnown = UQinglanEconomyLibrary::FindPassive(PassiveId, Passive);
			PassiveNames.Add((bKnown ? QinglanUILocalizedOrId(Passive.NameTextKey, PassiveId)
				: FText::FromName(PassiveId)).ToString());
		}
		TMap<FName, int32> TalismanCharges;
		Profile->GetTalismanCharges(TalismanCharges);
		TArray<FName> TalismanIds;
		TalismanCharges.GetKeys(TalismanIds);
		TalismanIds.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		TArray<FString> TalismanLines;
		for (const FName TalismanId : TalismanIds)
		{
			FQinglanTalismanCatalogEntry Talisman;
			const bool bKnown = UQinglanEconomyLibrary::FindTalisman(TalismanId, Talisman);
			const FText Name = bKnown
				? QinglanUILocalizedOrId(Talisman.NameTextKey, TalismanId) : FText::FromName(TalismanId);
			FFormatNamedArguments Args;
			Args.Add(TEXT("name"), Name);
			Args.Add(TEXT("charges"), FText::AsNumber(TalismanCharges.FindRef(TalismanId)));
			TalismanLines.Add(QinglanUIFormat(FName(TEXT("hud.talisman")), Args).ToString());
		}
		FFormatNamedArguments Args;
		Args.Add(TEXT("techniques"), FText::FromString(FString::Join(TechniqueSlots, TEXT(", "))));
		Args.Add(TEXT("relic"), FText::FromString(RelicText.IsEmpty()
			? UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment.empty"))).ToString() : RelicText));
		Args.Add(TEXT("passives"), FText::FromString(PassiveNames.Num() > 0
			? FString::Join(PassiveNames, TEXT(", "))
			: UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment.empty"))).ToString()));
		Args.Add(TEXT("talismans"), FText::FromString(TalismanLines.Num() > 0
			? FString::Join(TalismanLines, TEXT(", "))
			: UQinglanHUDStatics::GameText(FName(TEXT("menu.equipment.empty"))).ToString()));
		return QinglanUIFormat(FName(TEXT("menu.equipment.summary")), Args);
	}
	if (MenuState.Screen == EQinglanMenuScreen::Journal)
	{
		return Rows.Num() == 0
			? UQinglanHUDStatics::GameText(FName(TEXT("menu.journal.empty"))) : FText::GetEmpty();
	}
	if (MenuState.Screen == EQinglanMenuScreen::Offer)
	{
		UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
		if (OfferState.Entries.Num() > 0 && OfferState.Entries[0].bBlessing)
		{
			return UQinglanHUDStatics::GameText(FName(TEXT("menu.offer.blessing")));
		}
		FFormatNamedArguments Args;
		Args.Add(TEXT("shards"), FText::AsNumber(Profile ? Profile->GetInventory().SpiritShards : 0));
		Args.Add(TEXT("crystals"), FText::AsNumber(Profile ? Profile->GetInventory().VeinCrystal : 0));
		return QinglanUIFormat(FName(TEXT("menu.offer.balance")), Args);
	}
	if (MenuState.Screen == EQinglanMenuScreen::Outcome)
	{
		UQinglanRunSubsystem* Run = Instance ? Instance->GetSubsystem<UQinglanRunSubsystem>() : nullptr;
		if (Run && Run->IsInExpedition())
		{
			const FQinglanInventory Loot = Run->GetUnbankedLoot();
			FFormatNamedArguments Args;
			Args.Add(TEXT("shards"), FText::AsNumber(Loot.SpiritShards));
			Args.Add(TEXT("herbs"), FText::AsNumber(Loot.SpiritHerb));
			Args.Add(TEXT("crystals"), FText::AsNumber(Loot.VeinCrystal));
			return QinglanUIFormat(FName(TEXT("menu.outcome.unbanked")), Args);
		}
		return UQinglanHUDStatics::GameText(FName(TEXT("menu.outcome.returned")));
	}
	return FText::GetEmpty();
}

FText UQinglanMenuWidget::ResolveHint() const
{
	return UQinglanHUDStatics::GameText(FName(MenuState.Screen == EQinglanMenuScreen::Offer
		? TEXT("menu.offer.hint") : TEXT("menu.hint")));
}

void UQinglanMenuWidget::RefreshPresentation()
{
	if (!MenuTitle || !RowBox)
	{
		return;
	}
	MenuTitle->SetText(ResolveTitle());
	const FText Summary = ResolveSummary();
	MenuSummary->SetText(Summary);
	MenuSummary->SetVisibility(Summary.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	MenuNotice->SetText(Notice);
	MenuNotice->SetVisibility(Notice.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	MenuHint->SetText(ResolveHint());
	BackLabel->SetText(UQinglanHUDStatics::GameText(FName(MenuState.Screen == EQinglanMenuScreen::Offer
		? TEXT("menu.offer.dismiss") : TEXT("menu.back"))));

	RowBox->ClearChildren();
	RowButtons.Reset();
	RowClickRelays.Reset();
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		UTextBlock* Label = QinglanUICreateText(WidgetTree, 18, MenuBodyColor);
		Label->SetText(Rows[Index].Label);
		UButton* Button = WidgetTree->ConstructWidget<UButton>();
		Button->AddChild(Label);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Label->Slot))
		{
			ContentSlot->SetPadding(FMargin(12.f, 9.f));
		}
		Button->SetBackgroundColor(Index == MenuState.SelectionIndex ? MenuRowSelectedColor : MenuRowColor);
		RowBox->AddChildToVerticalBox(Button)->SetPadding(FMargin(0.f, 3.f));
		UQinglanMenuRowClickRelay* Relay = NewObject<UQinglanMenuRowClickRelay>(this);
		Relay->Initialize(this, Index);
		Button->OnClicked.AddDynamic(Relay, &UQinglanMenuRowClickRelay::HandleClicked);
		RowButtons.Add(Button);
		RowClickRelays.Add(Relay);
	}
	if (RowButtons.IsValidIndex(MenuState.SelectionIndex) && MenuState.SelectionIndex < RowButtons.Num()
		&& RowScroll)
	{
		RowScroll->ScrollWidgetIntoView(RowButtons[MenuState.SelectionIndex], true,
			EDescendantScrollDestination::IntoView);
	}
}

void UQinglanMenuWidget::ActivateRow(int32 RowIndex)
{
	if (!Rows.IsValidIndex(RowIndex))
	{
		return;
	}
	const FQinglanMenuRow Row = Rows[RowIndex];
	MenuState.SelectionIndex = RowIndex;
	AQinglanCharacter* Character = GetQinglanCharacter();

	const FName Action = Row.ActionId;
	if (Action == FName(TEXT("resume")))
	{
		if (Character) { Character->CloseGameMenu(); }
		else { CloseMenu(); }
		return;
	}
	if (Action == FName(TEXT("settings")))
	{
		ShowScreen(EQinglanMenuScreen::Settings);
		return;
	}
	if (Action == FName(TEXT("profile")))
	{
		ShowScreen(EQinglanMenuScreen::Profile);
		return;
	}
	if (Action == FName(TEXT("equipment")))
	{
		ShowScreen(EQinglanMenuScreen::Equipment);
		return;
	}
	if (Action == FName(TEXT("outcome")))
	{
		ShowScreen(EQinglanMenuScreen::Outcome);
		return;
	}
	if (Action == FName(TEXT("journal")))
	{
		if (Character)
		{
			Character->OpenJournal();
		}
		else
		{
			ShowScreen(EQinglanMenuScreen::Journal);
		}
		return;
	}
	if (Action == FName(TEXT("quit")))
	{
		if (Character) { Character->QuitToSanctuary(); }
		return;
	}
	if (Action == FName(TEXT("back")))
	{
		Back();
		return;
	}
	if (Action == FName(TEXT("journal.open")))
	{
		if (Character) { Character->OpenJournal(Row.Payload); }
		return;
	}
	if (Action == FName(TEXT("offer.select")))
	{
		if (Character) { Character->SelectMenuOption(RowIndex); }
		return;
	}

	UGameInstance* Instance = GetGameInstance();
	if (Action == FName(TEXT("profile.load")) || Action == FName(TEXT("profile.create")))
	{
		UQinglanSaveSubsystem* Save = Instance ? Instance->GetSubsystem<UQinglanSaveSubsystem>() : nullptr;
		if (!Save)
		{
			SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.save_unavailable"))));
			return;
		}
		const int32 Slot = FCString::Atoi(*Row.Payload.ToString());
		FText Result;
		bool bOk = true;
		if (Action == FName(TEXT("profile.load")))
		{
			bOk = Save->LoadProfile(Slot, Result);
		}
		else
		{
			Save->CreateNewProfile(Slot);
			bOk = Save->HasProfile(Slot);
		}
		if (Result.IsEmpty() && !bOk)
		{
			Result = UQinglanHUDStatics::GameText(FName(Action == FName(TEXT("profile.load"))
				? TEXT("menu.notice.load_failed") : TEXT("menu.notice.create_failed")));
		}
		SetNotice(Result);
		RebuildRows();
		RefreshPresentation();
		// AUD-01: a successful cold-start selection starts the session; close
		// the selector so the deferred hub arrival work runs (HandleMenuClosed).
		if (bOk && Character && Character->IsProfileFrontendOpen())
		{
			CloseMenu();
		}
		return;
	}
	if (Action == FName(TEXT("settings.lowflash")))
	{
		UQinglanGraphicsSettingsService* Graphics = Instance ? Instance->GetSubsystem<UQinglanGraphicsSettingsService>() : nullptr;
		if (Graphics)
		{
			FQinglanGraphicsSettings Settings = Graphics->GetCurrentSettings();
			Settings.bLowFlashMode = !Settings.bLowFlashMode;
			Graphics->ApplySettings(Settings);
		}
		RebuildRows();
		RefreshPresentation();
		return;
	}
	if (Action == FName(TEXT("settings.language")))
	{
		if (!FQinglanLocalizationService::SetPreferredCulture(Row.Payload.ToString()))
		{
			SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.language_failed"))));
		}
		RebuildRows();
		RefreshPresentation();
		return;
	}
	if (Action == FName(TEXT("settings.preset")))
	{
		UQinglanGraphicsSettingsService* Graphics = Instance ? Instance->GetSubsystem<UQinglanGraphicsSettingsService>() : nullptr;
		if (Graphics)
		{
			const EQinglanQualityPreset Preset =
				static_cast<EQinglanQualityPreset>(FCString::Atoi(*Row.Payload.ToString()));
			Graphics->ApplySettings(QinglanGraphics::MakePresetSettings(Preset));
		}
		RebuildRows();
		RefreshPresentation();
		return;
	}

	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;
	if (!Profile)
	{
		return;
	}
	bool bChanged = false;
	if (Action == FName(TEXT("equipment.technique.equip")) || Action == FName(TEXT("equipment.technique.unequip")))
	{
		TArray<FName> Equipped = Profile->GetEquippedTechniques();
		if (Action == FName(TEXT("equipment.technique.unequip")))
		{
			Equipped.Remove(Row.Payload);
		}
		else if (Equipped.Num() < QinglanProgression::EquippedTechniqueLimit && !Equipped.Contains(Row.Payload))
		{
			Equipped.Add(Row.Payload);
		}
		bChanged = Profile->TrySetEquippedTechniques(Equipped);
	}
	else if (Action == FName(TEXT("equipment.relic.equip")))
	{
		bChanged = Profile->TryEquipRelic(Row.Payload);
	}
	else if (Action == FName(TEXT("equipment.relic.unequip")))
	{
		bChanged = Profile->TryEquipRelic(NAME_None);
	}
	else if (Action == FName(TEXT("equipment.passive.equip")) || Action == FName(TEXT("equipment.passive.unequip")))
	{
		TArray<FName> Equipped = Profile->GetEquippedPassives();
		if (Action == FName(TEXT("equipment.passive.unequip")))
		{
			Equipped.Remove(Row.Payload);
		}
		else if (Equipped.Num() < QinglanProgression::EquippedPassiveLimit && !Equipped.Contains(Row.Payload))
		{
			Equipped.Add(Row.Payload);
		}
		bChanged = Profile->TrySetEquippedPassives(Equipped);
	}
	else if (Action == FName(TEXT("equipment.talisman.equip")) || Action == FName(TEXT("equipment.talisman.unequip")))
	{
		UQinglanTalismanComponent* TalismanComponent =
			GetQinglanCharacter() ? GetQinglanCharacter()->FindComponentByClass<UQinglanTalismanComponent>() : nullptr;
		if (TalismanComponent)
		{
			TArray<FName> Equipped = TalismanComponent->GetEquippedTalismans();
			if (Action == FName(TEXT("equipment.talisman.unequip")))
			{
				Equipped.Remove(Row.Payload);
			}
			else if (Equipped.Num() < QinglanProgression::EquippedTalismanLimit && !Equipped.Contains(Row.Payload))
			{
				Equipped.Add(Row.Payload);
			}
			bChanged = TalismanComponent->SetEquippedTalismans(Equipped);
		}
	}
	if (!bChanged)
	{
		SetNotice(UQinglanHUDStatics::GameText(FName(TEXT("menu.notice.equip_failed"))));
	}
	else
	{
		Notice = FText::GetEmpty();
	}
	RebuildRows();
	RefreshPresentation();
}

void UQinglanMenuRowClickRelay::Initialize(UQinglanMenuWidget* InOwner, int32 InRowIndex)
{
	OwnerWidget = InOwner;
	RowIndex = InRowIndex;
}

void UQinglanMenuRowClickRelay::HandleClicked()
{
	if (UQinglanMenuWidget* Owner = OwnerWidget.Get())
	{
		Owner->ActivateRow(RowIndex);
		QinglanUIReturnKeyboardToGame();
	}
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
	// AUD-52: shared helper sets the true font size (not a render transform).
	EndingTitle = QinglanUIAddText(WidgetTree, Content, 26, FLinearColor::White, NAME_None, 8.f);
	EndingPreview = QinglanUIAddText(WidgetTree, Content, 17, FLinearColor::White, NAME_None, 8.f);
	EndingRequirements = QinglanUIAddText(WidgetTree, Content, 16, FLinearColor(1.f, 0.8f, 0.35f), NAME_None, 8.f);
	EndingWarning = QinglanUIAddText(WidgetTree, Content, 16, FLinearColor(1.f, 0.8f, 0.35f), NAME_None, 8.f);
	WarningAcknowledgment = WidgetTree->ConstructWidget<UCheckBox>();
	AcknowledgmentLabel = WidgetTree->ConstructWidget<UTextBlock>();
	AcknowledgmentLabel->SetAutoWrapText(true);
	WarningAcknowledgment->AddChild(AcknowledgmentLabel);
	Content->AddChildToVerticalBox(WarningAcknowledgment)->SetPadding(FMargin(0.f, 12.f));
	EndingError = QinglanUIAddText(WidgetTree, Content, 16, FLinearColor(1.f, 0.45f, 0.4f), NAME_None, 8.f);
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

	// AUD-51: objective and bark text update from typed story delegates. These
	// are widget-owned bindings (the shared relay predates AUD-51) and are
	// removed in NativeDestruct.
	if (UGameInstance* Instance = GetGameInstance())
	{
		if (UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
		{
			Story->OnObjectiveUpdated.AddDynamic(this, &UQinglanHUDWidget::HandleObjectiveUpdated);
			QinglanStoryRuntime::Delegates(Story).OnBarkLine.AddDynamic(this, &UQinglanHUDWidget::HandleBarkLine);
			bBoundStoryDelegates = true;
		}
	}
	RefreshFromPlayer(); // correct bars on the first visible frame
}

void UQinglanHUDWidget::NativeDestruct()
{
	if (bBoundStoryDelegates)
	{
		if (UGameInstance* Instance = GetGameInstance())
		{
			if (UQinglanStorySubsystem* Story = Instance->GetSubsystem<UQinglanStorySubsystem>())
			{
				Story->OnObjectiveUpdated.RemoveDynamic(this, &UQinglanHUDWidget::HandleObjectiveUpdated);
				QinglanStoryRuntime::Delegates(Story).OnBarkLine.RemoveDynamic(this, &UQinglanHUDWidget::HandleBarkLine);
			}
		}
		bBoundStoryDelegates = false;
	}
	Super::NativeDestruct();
}

UTextBlock* UQinglanHUDWidget::EnsureNamedText(FName Name, const FVector2D& Position,
	const FVector2D& Size, int32 FontSize)
{
	if (!WidgetTree || Name.IsNone())
	{
		return nullptr;
	}
	if (UWidget* Existing = WidgetTree->FindWidget(Name))
	{
		// Authored element: cast-guard only (never duplicate a name).
		return Cast<UTextBlock>(Existing);
	}
	UCanvasPanel* Root = Cast<UCanvasPanel>(WidgetTree->RootWidget);
	if (!Root)
	{
		return nullptr;
	}
	UTextBlock* Text = QinglanUICreateText(WidgetTree, FontSize, MenuBodyColor, Name);
	if (!Text)
	{
		return nullptr;
	}
	if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Text))
	{
		Slot->SetPosition(Position);
		Slot->SetSize(Size);
	}
	return Text;
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

	// AUD-51: the bootstrap authored objective/talisman/bark placeholders; the
	// trial row is created here when absent so no bootstrap rebuild is needed.
	ObjectiveText = Tree->FindWidget<UTextBlock>(TEXT("Txt_Objective"));
	TrialText = EnsureNamedText(TEXT("Txt_Trial"), FVector2D(40.f, 100.f), FVector2D(700.f, 40.f), 18);
	BarkText = Tree->FindWidget<UTextBlock>(TEXT("Txt_Bark"));
	TalismanTexts.Reset();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (UTextBlock* Talisman = Tree->FindWidget<UTextBlock>(*FString::Printf(TEXT("Txt_Talisman%d"), Index)))
		{
			Talisman->SetAutoWrapText(true);
			FSlateFontInfo Font = Talisman->GetFont();
			Font.Size = 16;
			Talisman->SetFont(Font);
			TalismanTexts.Add(Talisman);
		}
	}
	// AUD-52: override the bootstrap's render-scale-only sizing with real font
	// sizes for the long-text HUD rows.
	if (ObjectiveText)
	{
		ObjectiveText->SetAutoWrapText(true);
		FSlateFontInfo Font = ObjectiveText->GetFont();
		Font.Size = 18;
		ObjectiveText->SetFont(Font);
	}
	if (BarkText)
	{
		BarkText->SetAutoWrapText(true);
		FSlateFontInfo Font = BarkText->GetFont();
		Font.Size = 18;
		BarkText->SetFont(Font);
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
	const bool bHasBoundRows = HealthBar || EnergyBar || StaminaBar || BarrierText
		|| SkillTexts.Num() > 0 || ObjectiveText || TrialText || BarkText || TalismanTexts.Num() > 0;
	if (!bHasBoundRows)
	{
		return; // no named elements bound: presentation stays with the fallbacks
	}
	AQinglanCharacter* Player = GetOwningPlayerPawn<AQinglanCharacter>();
	if (!IsValid(Player))
	{
		return; // pawn not in the world yet (loading order): next refresh retries
	}

	UQinglanCombatComponent* Combat = Player->GetCombat();
	if (IsValid(Combat))
	{
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
	}

	// Technique loadout: slot name from the string table via NameTextKey,
	// dimmed while its cooldown runs (docs/11 "Active Skill | Technique").
	UQinglanTechniqueComponent* Techniques = Player->GetTechniques();
	if (IsValid(Techniques) && SkillTexts.Num() > 0)
	{
		const TArray<UQinglanTechniqueDataAsset*> Loadout = Techniques->GetLoadout();
		for (int32 Index = 0; Index < SkillTexts.Num(); ++Index)
		{
			UTextBlock* SlotText = SkillTexts[Index];
			if (!SlotText)
			{
				continue;
			}
			UQinglanTechniqueDataAsset* Technique = Loadout.IsValidIndex(Index) ? Loadout[Index] : nullptr;
			if (!IsValid(Technique))
			{
				SlotText->SetText(FText::GetEmpty()); // unequipped slot renders blank
				continue;
			}
			// A corrupted/uninitialized FName would crash FName::ToString in
			// the keying/fallback below (observed after input; see the
			// invalid-FName HUD crash). Blank the slot instead of crashing.
			const FName TechniqueId = Technique->TechniqueId;
			if (!TechniqueId.IsValid())
			{
				UE_LOG(LogQinglanUI, Warning, TEXT("HUD: technique slot %d has an invalid id; slot left blank"), Index);
				SlotText->SetText(FText::GetEmpty());
				continue;
			}
			FFormatNamedArguments Args;
			Args.Add(TEXT("index"), FText::AsNumber(Index + 1));
			// Authored data keys the display name; fall back to the convention
			// key when a legacy/imported asset carries NAME_None instead of
			// rendering the missing-string marker in the skill HUD.
			FName NameKey = Technique->NameTextKey;
			if (!NameKey.IsValid() || NameKey.IsNone())
			{
				NameKey = FName(*FString::Printf(TEXT("technique.%s.name"), *TechniqueId.ToString()));
			}
			// Readable fallback: never render the missing-string marker into
			// the skill bar; the id with separators turned into spaces is
			// preferable to a red placeholder and keeps the slot identifiable.
			FText NameText = UQinglanHUDStatics::GameText(NameKey);
			if (NameText.ToString().Contains(TEXT("MISSING STRING TABLE ENTRY")))
			{
				FString Pretty = TechniqueId.ToString();
				Pretty.ReplaceInline(TEXT("_"), TEXT(" "));
				NameText = FText::FromString(Pretty);
			}
			Args.Add(TEXT("name"), NameText);
			SlotText->SetText(QinglanUIFormat(FName(GSkillSlotTextKey), Args));
			const bool bOnCooldown = Techniques->GetCooldownRemaining(Technique->TechniqueId) > 0.f;
			SlotText->SetRenderOpacity(bOnCooldown ? SkillCooldownOpacity : SkillReadyOpacity);
		}
	}

	RefreshLiveInfo(Player);
}

void UQinglanHUDWidget::RefreshLiveInfo(AQinglanCharacter* Player)
{
	UGameInstance* Instance = GetGameInstance();
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	UQinglanProfileSubsystem* Profile = Instance ? Instance->GetSubsystem<UQinglanProfileSubsystem>() : nullptr;

	// Objective: the playing scene's first incomplete authored objective wins;
	// otherwise keep the last completed-objective broadcast as context.
	FText Objective;
	if (Story)
	{
		const FName SceneId = Story->GetCurrentSceneId();
		if (const FQinglanRegistryScene* Scene = QinglanUIFindScene(Story->GetRegistry(), SceneId))
		{
			for (const FQinglanRegistryObjective& Entry : Scene->Objectives)
			{
				if (!QinglanStoryRuntime::IsSceneObjectiveCompleted(Story, SceneId, Entry.ObjectiveId))
				{
					if (!Entry.TextKey.IsNone())
					{
						Objective = UQinglanHUDStatics::GameText(Entry.TextKey);
					}
					break;
				}
			}
		}
	}
	if (Objective.IsEmpty())
	{
		Objective = CachedObjectiveText;
	}
	if (ObjectiveText)
	{
		if (Objective.IsEmpty())
		{
			ObjectiveText->SetText(UQinglanHUDStatics::GameText(FName(TEXT("hud.objective.none"))));
		}
		else
		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("text"), Objective);
			ObjectiveText->SetText(QinglanUIFormat(FName(TEXT("hud.objective")), Args));
		}
	}

	// Talisman charges: equipped slot ids come from the component, remaining
	// charges from the profile (AUD-51 live info).
	if (TalismanTexts.Num() > 0)
	{
		const UQinglanTalismanComponent* Talisman =
			Player ? Player->FindComponentByClass<UQinglanTalismanComponent>() : nullptr;
		const TArray<FName> Equipped = Talisman ? Talisman->GetEquippedTalismans() : TArray<FName>();
		for (int32 Index = 0; Index < TalismanTexts.Num(); ++Index)
		{
			UTextBlock* Text = TalismanTexts[Index];
			if (!Text)
			{
				continue;
			}
			if (!Equipped.IsValidIndex(Index) || Equipped[Index].IsNone() || !Equipped[Index].IsValid())
			{
				Text->SetText(FText::GetEmpty());
				continue;
			}
			FQinglanTalismanCatalogEntry CatalogEntry;
			const bool bKnown = UQinglanEconomyLibrary::FindTalisman(Equipped[Index], CatalogEntry);
			const FText Name = bKnown
				? QinglanUILocalizedOrId(CatalogEntry.NameTextKey, Equipped[Index])
				: FText::FromName(Equipped[Index]);
			FFormatNamedArguments Args;
			Args.Add(TEXT("name"), Name);
			Args.Add(TEXT("charges"), FText::AsNumber(Profile ? Profile->GetTalismanChargeCount(Equipped[Index]) : 0));
			Text->SetText(QinglanUIFormat(FName(TEXT("hud.talisman")), Args));
		}
	}

	// Trial countdown from the existing helper (AUD-20 timed-survival phase).
	if (TrialText)
	{
		float Remaining = 0.f;
		if (UQinglanHUDStatics::GetTrialRemainingSeconds(this, Remaining))
		{
			FFormatNamedArguments Args;
			Args.Add(TEXT("time"), UQinglanHUDStatics::FormatMinutesSeconds(FMath::Max(0, FMath::CeilToInt(Remaining))));
			TrialText->SetText(QinglanUIFormat(FName(TEXT("hud.trial")), Args));
			TrialText->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			TrialText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	// Bark/record notice (event-driven text with a short display lifetime).
	if (BarkText)
	{
		const UWorld* World = GetWorld();
		const double Now = World ? World->GetTimeSeconds() : 0.0;
		if (!CachedBarkText.IsEmpty() && Now < BarkExpiresAt)
		{
			BarkText->SetText(CachedBarkText);
			BarkText->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			BarkText->SetText(FText::GetEmpty());
			BarkText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UQinglanHUDWidget::HandleObjectiveUpdated(FName ObjectiveId, const FText& Text)
{
	(void)ObjectiveId; // HUD presents the latest objective text, not its id
	CachedObjectiveText = Text;
}

void UQinglanHUDWidget::HandleBarkLine(FName BarkId, const FQinglanScriptLine& Line)
{
	(void)BarkId; // the localized line already carries speaker/text
	ShowBarkLine(Line.SpeakerName, Line.Text);
}

void UQinglanHUDWidget::ShowBarkLine(const FText& Speaker, const FText& Text)
{
	if (Text.IsEmpty())
	{
		return;
	}
	FFormatNamedArguments Args;
	Args.Add(TEXT("speaker"), Speaker);
	Args.Add(TEXT("text"), Text);
	CachedBarkText = QinglanUIFormat(FName(TEXT("hud.bark")), Args);
	const UWorld* World = GetWorld();
	BarkExpiresAt = (World ? World->GetTimeSeconds() : 0.0) + HudBarkSeconds;
	if (BarkText)
	{
		BarkText->SetText(CachedBarkText);
		BarkText->SetVisibility(ESlateVisibility::Visible);
	}
}

void UQinglanHUDWidget::ShowRecordNotice(const FText& Title)
{
	if (Title.IsEmpty())
	{
		return;
	}
	FFormatNamedArguments Args;
	Args.Add(TEXT("title"), Title);
	CachedBarkText = QinglanUIFormat(FName(TEXT("hud.record")), Args);
	const UWorld* World = GetWorld();
	BarkExpiresAt = (World ? World->GetTimeSeconds() : 0.0) + HudBarkSeconds;
	if (BarkText)
	{
		BarkText->SetText(CachedBarkText);
		BarkText->SetVisibility(ESlateVisibility::Visible);
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
