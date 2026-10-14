// Base widget classes: C++ logic bases for UMG-authored frontends.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanUIWidgets.generated.h"

class UProgressBar;
class UTextBlock;
class UVerticalBox;

/** Dialogue subtitle + choices presenter; drives via StorySubsystem events. */
UCLASS(Abstract)
class UQinglanDialogueWidget : public UUserWidget
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
	bool SetCurrentLine(const FText& SpeakerName, const FText& LineText);
	/**
	 * Stores the presented choice list and rebuilds Box_Choices (one index-
	 * prefixed TextBlock per entry); an empty array clears the list.
	 * Returns false when Box_Choices does not exist (fallback presenter).
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	bool SetChoiceEntries(const TArray<FQinglanSceneChoice>& Choices);
	/**
	 * Stores one opened lore record (title + body) as the shown line.
	 * Returns the SetCurrentLine presentation result.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	bool ShowRecordEntry(const FText& Title, const FText& Body);

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentSpeaker;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentLineText;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") TArray<FQinglanSceneChoice> CurrentChoices;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordTitle;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordBody;

private:
	/**
	 * Resolves the named elements the bootstrap tree provides. Idempotent:
	 * re-construct (viewport re-add) re-resolves in case the tree was
	 * recompiled. Cast failures leave the slot null and guarded.
	 */
	void BindNamedElements();

	/** Named presenter blocks (null when the authored tree lacks them). */
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SpeakerText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> LineText;
	UPROPERTY(Transient) TObjectPtr<UVerticalBox> ChoicesBox;
};

/** Combat HUD: vitals, techniques+cooldowns, talisman charges, objectives. */
UCLASS(Abstract)
class UQinglanHUDWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	/** Resolves the named vitals/skill elements (see BindNamedElements above). */
	void BindNamedElements();
	/**
	 * Pulls vitals + technique loadout/cooldowns from the owning pawn's
	 * components and pushes them into the bound elements. Missing pawn
	 * (loading order) simply retries on the next refresh.
	 */
	void RefreshFromPlayer();

	/** Named vitals bars (null when the authored tree lacks them). */
	UPROPERTY(Transient) TObjectPtr<UProgressBar> HealthBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> EnergyBar;
	UPROPERTY(Transient) TObjectPtr<UProgressBar> StaminaBar;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> BarrierText;
	/** Technique slots 0..3, authored as Txt_Skill0..Txt_Skill3. */
	UPROPERTY(Transient) TArray<TObjectPtr<UTextBlock>> SkillTexts;

	/** Refresh throttle accumulator (HudRefreshSeconds). */
	float RefreshAccumulator = 0.f;
};

/** Journal: main/side objectives, records (L01-L24), agreement status. */
UCLASS(Abstract)
class UQinglanJournalWidget : public UUserWidget
{
	GENERATED_BODY()
};

/** Ending preview + explicit confirmation dialog. */
UCLASS(Abstract)
class UQinglanEndingWidget : public UUserWidget
{
	GENERATED_BODY()
};
