// Base widget classes: C++ logic bases for UMG-authored frontends.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/QinglanCoreTypes.h"
#include "Narrative/QinglanStorySubsystem.h"
#include "QinglanUIWidgets.generated.h"

class UProgressBar;
class UTextBlock;
class UVerticalBox;
class UButton;
class UCheckBox;
class UBorder;
class UScrollBox;
class USizeBox;
class UCanvasPanel;
class UQinglanDialogueWidget;
class UQinglanJournalWidget;
class UQinglanMenuWidget;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanEndingClosed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanEndingCommitted, FName, EndingId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanDialogueChoiceSelected, FName, ChoiceId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanJournalClosed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FQinglanMenuClosed);

// ---------------------------------------------------------------------------
// AUD-51/52 pure UI state machines.
//
// These carry no Slate/UObject dependencies, so automation tests can drive the
// exact transitions without a rendering world. The widget classes below own one
// instance each and only mirror the state into UMG controls.
// ---------------------------------------------------------------------------

/**
 * One opened lore record as presented by the journal. Records carry no authored
 * source classification in FQinglanRegistryRecord, so the journal infers the
 * speaker/source tag from the record id (see InferRecordSourceKind).
 */
struct FQinglanJournalRecordView
{
	FName RecordId = NAME_None;
	FText Title;
	FText Body;
	FText DateLabel;
	EQinglanSourceKind SourceKind = EQinglanSourceKind::Paper;
	bool bRead = false;
};

/**
 * Journal open/record/close state (AUD-52).
 *
 * Opening a journal shows the record list; opening a record switches to the
 * scrollable body while the list stays logically open. Closing a record returns
 * to the list, and ReopenLastRecord restores the most recently viewed record
 * even after the whole journal was closed.
 */
struct FQinglanJournalState
{
	bool bOpen = false;
	bool bRecordOpen = false;
	FName CurrentRecordId = NAME_None;
	FName LastRecordId = NAME_None;
	int32 SelectedRecordIndex = 0;
	int32 RecordCount = 0;

	void OpenJournal()
	{
		bOpen = true;
		bRecordOpen = false;
		CurrentRecordId = NAME_None;
		SelectedRecordIndex = 0;
	}

	/** True when an open journal was actually closed. */
	bool CloseJournal()
	{
		if (!bOpen)
		{
			return false;
		}
		bOpen = false;
		bRecordOpen = false;
		CurrentRecordId = NAME_None;
		return true;
	}

	bool OpenRecord(FName RecordId)
	{
		if (RecordId.IsNone())
		{
			return false;
		}
		bOpen = true;
		bRecordOpen = true;
		CurrentRecordId = RecordId;
		LastRecordId = RecordId;
		return true;
	}

	/** True when a record was showing and is now closed (journal remains open). */
	bool CloseRecord()
	{
		if (!bRecordOpen)
		{
			return false;
		}
		bRecordOpen = false;
		CurrentRecordId = NAME_None;
		return true;
	}

	bool ReopenLastRecord()
	{
		return OpenRecord(LastRecordId);
	}

	bool MoveSelection(int32 Direction)
	{
		if (RecordCount <= 0)
		{
			SelectedRecordIndex = 0;
			return false;
		}
		SelectedRecordIndex = (SelectedRecordIndex + Direction + RecordCount) % RecordCount;
		return true;
	}

	bool IsOpen() const { return bOpen; }
	bool IsRecordOpen() const { return bOpen && bRecordOpen; }
	FName GetCurrentRecordId() const { return bRecordOpen ? CurrentRecordId : NAME_None; }

	/**
	 * FQinglanRegistryRecord has no source field (see Narrative/
	 * QinglanStoryDataTypes.h). The authored ids are stable, so the journal
	 * classifies them by prefix:
	 *   LIVE- or D-prefixed ids -> live transcript; M- or MEM-prefixed ids ->
	 *   memory; everything else (the shipped L01-L24 lore records) -> paper.
	 * This is an intentional inference, recorded here because the registry
	 * schema gap cannot be fixed from the UI module.
	 */
	static EQinglanSourceKind InferRecordSourceKind(FName RecordId)
	{
		const FString Id = RecordId.ToString();
		if (Id.StartsWith(TEXT("LIVE"), ESearchCase::IgnoreCase)
			|| Id.StartsWith(TEXT("D"), ESearchCase::IgnoreCase))
		{
			return EQinglanSourceKind::Live;
		}
		if (Id.StartsWith(TEXT("M"), ESearchCase::IgnoreCase))
		{
			return EQinglanSourceKind::Memory;
		}
		return EQinglanSourceKind::Paper;
	}

	static FName SourceKindLabelKey(EQinglanSourceKind SourceKind)
	{
		switch (SourceKind)
		{
		case EQinglanSourceKind::Live: return FName(TEXT("story.speaker.source.live"));
		case EQinglanSourceKind::Paper: return FName(TEXT("story.speaker.source.paper"));
		case EQinglanSourceKind::Letter: return FName(TEXT("story.speaker.source.letter"));
		case EQinglanSourceKind::System: return FName(TEXT("story.speaker.source.system"));
		case EQinglanSourceKind::Memory: return FName(TEXT("story.speaker.source.memory"));
		case EQinglanSourceKind::Recorded: return FName(TEXT("story.speaker.source.recorded"));
		default: return FName(TEXT("story.speaker.source.paper"));
		}
	}
};

/** Native menu screens (AUD-51). */
UENUM(BlueprintType)
enum class EQinglanMenuScreen : uint8
{
	Closed,
	Pause,
	Profile,
	Settings,
	Equipment,
	Journal,
	Offer,
	Outcome
};

/**
 * Keyboard/mouse menu navigation state. Gameplay is frozen while open; the
 * number-row capture list records every slot routed into the menu and is
 * cleared by Close() (AUD-51 "numeric capture clears on close").
 */
struct FQinglanMenuState
{
	EQinglanMenuScreen Screen = EQinglanMenuScreen::Closed;
	EQinglanMenuScreen PreviousScreen = EQinglanMenuScreen::Closed;
	int32 SelectionIndex = 0;
	int32 OptionCount = 0;
	bool bGameplayInputBlocked = false;
	TArray<int32> CapturedNumberKeys;

	void Open(EQinglanMenuScreen InitialScreen = EQinglanMenuScreen::Pause)
	{
		Screen = InitialScreen == EQinglanMenuScreen::Closed ? EQinglanMenuScreen::Pause : InitialScreen;
		PreviousScreen = EQinglanMenuScreen::Closed;
		SelectionIndex = 0;
		OptionCount = 0;
		bGameplayInputBlocked = true;
		ClearNumberCapture();
	}

	bool NavigateTo(EQinglanMenuScreen NewScreen)
	{
		if (NewScreen == EQinglanMenuScreen::Closed)
		{
			Close();
			return false;
		}
		PreviousScreen = Screen;
		Screen = NewScreen;
		SelectionIndex = 0;
		bGameplayInputBlocked = true;
		return true;
	}

	/**
	 * Back returns true while a screen remains open (sub-screens return to
	 * Pause). Pause itself closes: false means the caller should tear down the
	 * widget and restore gameplay input.
	 */
	bool Back()
	{
		if (Screen == EQinglanMenuScreen::Closed)
		{
			return false;
		}
		if (Screen == EQinglanMenuScreen::Pause)
		{
			Close();
			return false;
		}
		Screen = EQinglanMenuScreen::Pause;
		SelectionIndex = 0;
		return true;
	}

	void Close()
	{
		Screen = EQinglanMenuScreen::Closed;
		PreviousScreen = EQinglanMenuScreen::Closed;
		SelectionIndex = 0;
		OptionCount = 0;
		bGameplayInputBlocked = false;
		ClearNumberCapture();
	}

	bool IsOpen() const { return Screen != EQinglanMenuScreen::Closed; }
	bool IsGameplayInputBlocked() const { return IsOpen() && bGameplayInputBlocked; }

	void SetOptionCount(int32 Count)
	{
		OptionCount = FMath::Max(0, Count);
		if (OptionCount == 0)
		{
			SelectionIndex = 0;
		}
		else
		{
			SelectionIndex = FMath::Clamp(SelectionIndex, 0, OptionCount - 1);
		}
	}

	bool MoveSelection(int32 Direction)
	{
		if (OptionCount <= 0)
		{
			SelectionIndex = 0;
			return false;
		}
		SelectionIndex = (SelectionIndex + Direction + OptionCount) % OptionCount;
		return true;
	}

	void CaptureNumberKey(int32 Slot)
	{
		if (IsOpen() && Slot >= 0 && !CapturedNumberKeys.Contains(Slot))
		{
			CapturedNumberKeys.Add(Slot);
		}
	}

	void ClearNumberCapture() { CapturedNumberKeys.Reset(); }
	bool WasNumberKeyCaptured(int32 Slot) const { return CapturedNumberKeys.Contains(Slot); }
};

/** One purchasable/selectable offer row (shop entry or blessing draft). */
struct FQinglanOfferChoice
{
	FName OfferId = NAME_None;
	FText Title;
	FText Cost;
	bool bBlessing = false;
};

/**
 * Offer persistence (AUD-51): an offer set stays active across screen changes
 * until Select() consumes it or Dismiss() explicitly clears it.
 */
struct FQinglanOfferState
{
	bool bActive = false;
	TArray<FQinglanOfferChoice> Entries;
	int32 SelectedIndex = 0;

	bool BeginOffer(const TArray<FQinglanOfferChoice>& InEntries)
	{
		Entries = InEntries;
		SelectedIndex = 0;
		bActive = Entries.Num() > 0;
		return bActive;
	}

	bool Select(FName OfferId)
	{
		if (!bActive)
		{
			return false;
		}
		const bool bFound = Entries.ContainsByPredicate(
			[OfferId](const FQinglanOfferChoice& Entry) { return Entry.OfferId == OfferId; });
		if (!bFound)
		{
			return false;
		}
		Clear();
		return true;
	}

	bool Dismiss()
	{
		const bool bWasActive = IsActive();
		Clear();
		return bWasActive;
	}

	void Clear()
	{
		bActive = false;
		Entries.Reset();
		SelectedIndex = 0;
	}

	bool IsActive() const { return bActive && Entries.Num() > 0; }

	const FQinglanOfferChoice* Find(FName OfferId) const
	{
		return Entries.FindByPredicate(
			[OfferId](const FQinglanOfferChoice& Entry) { return Entry.OfferId == OfferId; });
	}

	const FQinglanOfferChoice* GetSelected() const
	{
		return Entries.IsValidIndex(SelectedIndex) ? &Entries[SelectedIndex] : nullptr;
	}

	bool MoveSelection(int32 Direction)
	{
		if (Entries.Num() == 0)
		{
			SelectedIndex = 0;
			return false;
		}
		SelectedIndex = (SelectedIndex + Direction + Entries.Num()) % Entries.Num();
		return true;
	}
};

/** Dynamic UButton delegates have no payload; this per-button owner supplies its stable choice id. */
UCLASS()
class UQinglanDialogueChoiceClickRelay : public UObject
{
	GENERATED_BODY()
public:
	void Initialize(UQinglanDialogueWidget* InOwner, FName InChoiceId);
	UFUNCTION() void HandleClicked();
private:
	UPROPERTY(Transient) TWeakObjectPtr<UQinglanDialogueWidget> OwnerWidget;
	FName ChoiceId = NAME_None;
};

/** Dialogue subtitle + choices presenter; drives via StorySubsystem events. */
UCLASS(Abstract)
class QINGLANASCENSION_API UQinglanDialogueWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	UFUNCTION(BlueprintImplementableEvent) void BP_OnLineShown(const FQinglanScriptLine& Line);
	UFUNCTION(BlueprintImplementableEvent) void BP_OnChoicesShown(const TArray<FQinglanSceneChoice>& Choices);

	// --- C++ text setters (playable-loop glue) ---
	// The scaffold shipped no members for line/choice/record text, so the C++
	// interaction layer had nowhere to put what the story subsystem broadcasts.
	// These setters store the current presentation on the widget itself; UMG
	// graphs bind to the BlueprintReadOnly members below (the BP_* events above
	// keep firing for graph-driven frontends). Since the UI bootstrap pass
	// (QinglanUIBootstrapCommandlet) every setter ALSO writes into the named
	// tree elements it authored (docs/11 "UMG widgets receive FText from String
	// Tables"); the bool return says whether those named elements exist, so the
	// caller can keep the on-screen-message fallback for unauthored trees.
	/**
	 * Stores the currently shown speaker + line (scene script or record body)
	 * and writes both into Txt_Speaker/Txt_Line when the tree provides them.
	 * Returns false when neither named block exists (fallback presenter).
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	bool SetCurrentLine(const FText& SpeakerName, const FText& LineText, int32 FontSize = 0);
	/** Scene subtitles expire when playback finishes; notices/records do not. */
	bool SetScenePlaybackLine(const FText& SpeakerName, const FText& LineText);
	/** Clears only finished scene subtitles, leaving gameplay proof pending. */
	void RefreshScenePlayback(bool bScenePlaying);
	/** Present the authored live/letter/paper/system/memory/recorded classification. */
	void SetSourceKind(EQinglanSourceKind SourceKind);
	/**
	 * Stores the presented choice list and rebuilds Box_Choices with clickable,
	 * index-prefixed buttons; an empty array clears the list.
	 * Returns false when Box_Choices does not exist (fallback presenter).
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	bool SetChoiceEntries(const TArray<FQinglanSceneChoice>& Choices);
	/** Controller/keyboard selection uses the same visible button highlight and focus. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue") void SetSelectedChoiceIndex(int32 Index);
	/** Emits a selection request; Character clears the old menu before invoking the story authority. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue") void RequestChoiceSelection(FName ChoiceId);
	UPROPERTY(BlueprintAssignable) FQinglanDialogueChoiceSelected OnChoiceSelected;
	/**
	 * Stores one opened lore record (title + body) as the shown line.
	 * BodyFontSize 0 keeps the authored default (AUD-52: long text scales via
	 * the font size, not only a render transform).
	 * Returns the SetCurrentLine presentation result.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	bool ShowRecordEntry(const FText& Title, const FText& Body, int32 BodyFontSize = 0);

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentSpeaker;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentLineText;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") TArray<FQinglanSceneChoice> CurrentChoices;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordTitle;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordBody;

protected:
	/** Responsive native layout also repairs stale Blueprint trees with fixed choice rectangles. */
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	/**
	 * Resolves the named elements the bootstrap tree provides. Idempotent:
	 * re-construct (viewport re-add) re-resolves in case the tree was
	 * recompiled. Cast failures leave the slot null and guarded.
	 */
	void BindNamedElements();

	/** Named presenter blocks (null when the authored tree lacks them). */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeakerText;
	FText CurrentSourceLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LineText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ChoicesBox;
	UPROPERTY(Transient) TObjectPtr<UBorder> DialoguePanel;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> LineScroll;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> ChoiceScroll;
	UPROPERTY(Transient) TArray<TObjectPtr<UButton>> ChoiceButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UQinglanDialogueChoiceClickRelay>> ChoiceClickRelays;
	int32 SelectedChoiceIndex = INDEX_NONE;
	bool bShowingScenePlayback = false;
};

/** Combat HUD: vitals, techniques+cooldowns, talisman charges, objectives. */
UCLASS(Abstract)
class UQinglanHUDWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** AUD-51 live info: one ambient bark line (also used for record notices). */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|HUD")
	void ShowBarkLine(const FText& Speaker, const FText& Text);
	/** AUD-52: surface a collected record title in the HUD text region. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|HUD")
	void ShowRecordNotice(const FText& Title);

	/** Story objective updates (AUD-51 objective row). */
	UFUNCTION() void HandleObjectiveUpdated(FName ObjectiveId, const FText& Text);
	/** Runtime bark broadcasts (AUD-51 bark text region). */
	UFUNCTION() void HandleBarkLine(FName BarkId, const FQinglanScriptLine& Line);

private:
	/** Resolves the named vitals/skill elements (see BindNamedElements above). */
	void BindNamedElements();
	/**
	 * Pulls vitals + technique loadout/cooldowns from the owning pawn's
	 * components and pushes them into the bound elements. Missing pawn
	 * (loading order) simply retries on the next refresh.
	 */
	void RefreshFromPlayer();
	/** AUD-51 objective/talisman/trial/bark rows (throttled with the rest). */
	void RefreshLiveInfo(class AQinglanCharacter* Player);
	/** Finds an authored text block, or creates one on the HUD canvas. */
	UTextBlock* EnsureNamedText(FName Name, const FVector2D& Position, const FVector2D& Size, int32 FontSize);

	/** Named vitals bars (null when the authored tree lacks them). */
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> EnergyBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> StaminaBar;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BarrierText;
	/** Technique slots 0..3, authored as Txt_Skill0..Txt_Skill3. */
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> SkillTexts;

	/** AUD-51 live rows; Txt_Trial is created when the authored tree lacks it. */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ObjectiveText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GuideText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ObjectiveMarker;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> TrialText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BarkText;
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> TalismanTexts;

	FText CachedObjectiveText;
	FText CachedBarkText;
	double BarkExpiresAt = 0.0;
	bool bBoundStoryDelegates = false;

	/** Refresh throttle accumulator (HudRefreshSeconds). */
	float RefreshAccumulator = 0.f;
};

/** Per-record button owner; UButton delegates carry no payload. */
UCLASS()
class UQinglanJournalRecordClickRelay : public UObject
{
	GENERATED_BODY()
public:
	void Initialize(UQinglanJournalWidget* InOwner, int32 InRecordIndex);
	UFUNCTION() void HandleClicked();
private:
	UPROPERTY(Transient) TWeakObjectPtr<UQinglanJournalWidget> OwnerWidget;
	int32 RecordIndex = INDEX_NONE;
};

/**
 * Journal (AUD-52): scrollable record list + record body, source tag, close and
 * reopen. The pure FQinglanJournalState above is the authoritative transition
 * machine; this widget mirrors it into UMG and can be driven headlessly through
 * the public Open/Close/Reopen functions.
 */
UCLASS()
class UQinglanJournalWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;

	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") void OpenJournal();
	/** Closes the whole journal; broadcasts OnJournalClosed when it was open. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool CloseJournal();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool OpenRecord(FName RecordId);
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool CloseRecord();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool ReopenLastRecord();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool MoveRecordSelection(int32 Direction);
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool SelectCurrentRecord();
	/** Keyboard Back: record -> list -> closed. Returns true while still open. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") bool Back();

	/** Rebuilds the list from the story registry + profile read state. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Journal") void RefreshFromStory();
	/** Test/gameplay seam: replace the presented record list directly. */
	void SetRecordViews(const TArray<FQinglanJournalRecordView>& InRecords);
	void ShowRecordView(int32 RecordIndex);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Journal") bool IsJournalOpen() const { return JournalState.IsOpen(); }
	UFUNCTION(BlueprintPure, Category = "Qinglan|Journal") bool IsRecordOpen() const { return JournalState.IsRecordOpen(); }
	UFUNCTION(BlueprintPure, Category = "Qinglan|Journal") FName GetCurrentRecordId() const { return JournalState.GetCurrentRecordId(); }
	const FQinglanJournalState& GetJournalState() const { return JournalState; }

	UPROPERTY(BlueprintAssignable) FQinglanJournalClosed OnJournalClosed;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildJournalTree();
	void RefreshPresentation();
	void RebuildRecordRows();
	UFUNCTION() void HandleClosePressed();

	UPROPERTY(Transient) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SourceText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> DateText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BodyText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> HintText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CloseLabel;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> RecordListScroll;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> BodyScroll;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RecordRowsBox;
	UPROPERTY(Transient) TObjectPtr<UButton> CloseButton;
	UPROPERTY(Transient) TArray<TObjectPtr<UButton>> RecordButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UQinglanJournalRecordClickRelay>> RecordClickRelays;

	FQinglanJournalState JournalState;
	TArray<FQinglanJournalRecordView> RecordViews;
};

/** Per-row button owner for the native menu container. */
UCLASS()
class UQinglanMenuRowClickRelay : public UObject
{
	GENERATED_BODY()
public:
	void Initialize(UQinglanMenuWidget* InOwner, int32 InRowIndex);
	UFUNCTION() void HandleClicked();
private:
	UPROPERTY(Transient) TWeakObjectPtr<UQinglanMenuWidget> OwnerWidget;
	int32 RowIndex = INDEX_NONE;
};

/**
 * Native menu container (AUD-51): Pause, Profiles, Settings/accessibility,
 * Equipment/talismans, Journal, offers and expedition outcome. Keyboard and
 * mouse share the same pure FQinglanMenuState selection/capture rules.
 */
UCLASS()
class UQinglanMenuWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;

	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") void ShowScreen(EQinglanMenuScreen NewScreen);
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") bool Back();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") bool MoveSelection(int32 Direction);
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") bool ConfirmSelection();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") void CloseMenu();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") void Refresh();

	/** Shows the shop/blessing offer screen; entries persist until select/dismiss. */
	void ShowOffers(const TArray<FQinglanOfferChoice>& Offers);
	/** Selection consumed or offer dismissed: clear and return to Pause. */
	void CloseOffer();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Menu") void SetNotice(const FText& InNotice);
	void CaptureNumberKey(int32 InSlot) { MenuState.CaptureNumberKey(InSlot); }
	/** Row activation entry point for the per-row click relay. */
	void ActivateRow(int32 RowIndex);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Menu") bool IsMenuOpen() const { return MenuState.IsOpen(); }
	UFUNCTION(BlueprintPure, Category = "Qinglan|Menu") bool IsGameplayInputBlocked() const { return MenuState.IsGameplayInputBlocked(); }
	UFUNCTION(BlueprintPure, Category = "Qinglan|Menu") EQinglanMenuScreen GetScreen() const { return MenuState.Screen; }
	const FQinglanMenuState& GetMenuState() const { return MenuState; }
	const FQinglanOfferState& GetOfferState() const { return OfferState; }

	UPROPERTY(BlueprintAssignable) FQinglanMenuClosed OnMenuClosed;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	/** One actionable menu row; ActionId selects the behavior, Payload its id. */
	struct FQinglanMenuRow
	{
		FText Label;
		FName ActionId = NAME_None;
		FName Payload = NAME_None;
	};

	void BuildMenuTree();
	void RebuildRows();
	void RefreshPresentation();
	void ResetRows();
	void ApplyPauseRows();
	void ApplyProfileRows();
	void ApplySettingsRows();
	void ApplyEquipmentRows();
	void ApplyJournalRows();
	void ApplyOfferRows();
	void ApplyOutcomeRows();
	UFUNCTION() void HandleBackPressed();
	FText ResolveTitle() const;
	FText ResolveSummary() const;
	FText ResolveHint() const;
	class AQinglanCharacter* GetQinglanCharacter() const;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> MenuTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MenuSummary;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MenuNotice;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> MenuHint;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BackLabel;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> RowBox;
	UPROPERTY(Transient) TObjectPtr<UScrollBox> RowScroll;
	UPROPERTY(Transient) TObjectPtr<UButton> BackButton;
	UPROPERTY(Transient) TObjectPtr<UBorder> MenuPanel;
	UPROPERTY(Transient) TArray<TObjectPtr<UButton>> RowButtons;
	UPROPERTY(Transient) TArray<TObjectPtr<UQinglanMenuRowClickRelay>> RowClickRelays;

	FQinglanMenuState MenuState;
	FQinglanOfferState OfferState;
	TArray<FQinglanMenuRow> Rows;
	FText Notice;
};

/** Ending preview + explicit confirmation dialog. */
UCLASS()
class QINGLANASCENSION_API UQinglanEndingWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	/** Presentation only: the story action owns preview authorization and its events. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Ending")
	void ShowEnding(FName EndingId, bool bConfirmationRequested);
	/** Shared input path. Preview mode closes back to review (false = no commit);
	 * confirmation mode calls the story authority only after explicit acknowledgment. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Ending") bool ConfirmCurrentEnding();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Ending") void CancelPreview();
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Ending") void ToggleWarningAcknowledgment();
	void FocusDefaultControl();
	UFUNCTION(BlueprintPure, Category = "Qinglan|Ending") bool IsWarningAcknowledged() const { return bWarningAcknowledged; }
	UFUNCTION(BlueprintPure, Category = "Qinglan|Ending") bool IsConfirmationRequested() const { return bConfirmRequested; }

	UPROPERTY(BlueprintAssignable) FQinglanEndingClosed OnEndingClosed;
	UPROPERTY(BlueprintAssignable) FQinglanEndingCommitted OnEndingCommitted;

protected:
	/** Always builds working controls, even when an older WBP_Ending has only placeholder text. */
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildEndingTree();
	void RefreshPresentation();
	UFUNCTION() void HandleConfirmPressed();
	UFUNCTION() void HandleAcknowledgmentChanged(bool bChecked);

	UPROPERTY(Transient) TObjectPtr<UTextBlock> EndingTitle;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EndingPreview;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EndingRequirements;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EndingWarning;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> EndingError;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> AcknowledgmentLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ConfirmLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CancelLabel;
	UPROPERTY(Transient) TObjectPtr<UCheckBox> WarningAcknowledgment;
	UPROPERTY(Transient) TObjectPtr<UButton> ConfirmButton;
	UPROPERTY(Transient) TObjectPtr<UButton> CancelButton;
	FName CurrentEndingId = NAME_None;
	FText LastError;
	bool bConfirmRequested = false;
	bool bWarningAcknowledged = false;
	bool bClosing = false;
};
