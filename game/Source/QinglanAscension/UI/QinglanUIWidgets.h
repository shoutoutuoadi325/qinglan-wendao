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
