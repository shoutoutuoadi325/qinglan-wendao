// Base widget classes: C++ logic bases for UMG-authored frontends.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanUIWidgets.generated.h"

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
	// keep firing for graph-driven frontends).
	/** Stores the currently shown speaker + line (scene script or record body). */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	void SetCurrentLine(const FText& SpeakerName, const FText& LineText);
	/** Stores the presented choice list; an empty array clears the list. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	void SetChoiceEntries(const TArray<FQinglanSceneChoice>& Choices);
	/** Stores one opened lore record (title + body) as the shown line. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue")
	void ShowRecordEntry(const FText& Title, const FText& Body);

	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentSpeaker;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText CurrentLineText;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") TArray<FQinglanSceneChoice> CurrentChoices;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordTitle;
	UPROPERTY(BlueprintReadOnly, Category = "Qinglan|Dialogue") FText LastRecordBody;
};

/** Combat HUD: vitals, techniques+cooldowns, talisman charges, objectives. */
UCLASS(Abstract)
class UQinglanHUDWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	virtual void NativeConstruct() override;
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
