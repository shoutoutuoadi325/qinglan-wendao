// HUD/UI static helpers: string-table text, locale-aware numeric formatting,
// widget class resolution, dialogue passthroughs.
//
// All player-facing text resolves from the string table
// /Game/Qinglan/Data/ST_Game via FText::FromStringTable (code-contracts.md
// "Localization keys"; docs/11-english-style-and-ui.md: dotted keys, named-arg
// FText::Format, locale-aware numeric formatting, no fragment concatenation).
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Core/QinglanCoreTypes.h"
#include "QinglanHUDStatics.generated.h"

class UUserWidget;

DECLARE_LOG_CATEGORY_EXTERN(LogQinglanUI, Log, All);

/**
 * Shared UI glue available to every UMG-authored frontend widget.
 * No hardcoded display strings anywhere in this class: every FText comes
 * from the string table or from culture-aware FText formatting.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanHUDStatics : public UBlueprintFunctionLibrary // linkage-only export for editor automation specs
{
	GENERATED_BODY()

public:
	/** Canonical string-table asset id for all player-facing text (code-contracts.md). */
	static FName GameStringTableId();

	// --- Text resolution (string table only) ---

	/** Resolves one dotted key (e.g. "hud.health") from /Game/Qinglan/Data/ST_Game. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Text")
	static FText GameText(FName Key);

	/** True when the key exists in the table (missing-key lint for widget graphs). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Text")
	static bool HasGameText(FName Key);

	/** FText::Format with one named int32 argument; pattern comes from the string table. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Text")
	static FText FormatGameTextWithNumber(FName Key, FName ArgName, int32 Value);

	/** FText::Format with one named int64 argument (currencies are 64-bit; blueprint section 1.5). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Text")
	static FText FormatGameTextWithInt64(FName Key, FName ArgName, int64 Value);

	/** FText::Format with one named FText argument. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Text")
	static FText FormatGameTextWithText(FName Key, FName ArgName, const FText& Value);

	// --- Locale-aware numeric formatting (docs/11: locale-aware numeric formatting) ---

	/** Culture-aware integer text (grouping and digits follow the active locale). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatNumber(int32 Value);

	/** Culture-aware 64-bit integer text; used for all currency display. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatNumber64(int64 Value);

	/** Spirit shard amount ("hud.shards" is the label key; this formats the number). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatSpiritShards(int64 Amount);

	/** Culture-aware percent for a 0..1 fraction (resource bars, bank multipliers). */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatPercent(float FractionZeroToOne);

	/** Technique cooldown seconds, one fractional digit, never negative. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatCooldownSeconds(float Seconds);

	/** m:ss clock (room timers) from the "hud.clock" pattern with named arguments. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|Format")
	static FText FormatMinutesSeconds(int32 TotalSeconds);

	// --- Widget class resolution helpers ---

	/**
	 * Resolves a stable widget id ("HUD", "Dialogue", "Journal", "Ending") to its
	 * Blueprint class. Code may override any id via RegisterWidgetClass().
	 * FIXME-CONTRACT: no design doc pins the widget asset paths; the defaults
	 * follow the module-wide /Game/Qinglan/UI/<AssetName> convention.
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|UI")
	static TSubclassOf<UUserWidget> ResolveWidgetClass(FName WidgetId);

	/** Resolve + CreateWidget in one step; returns null when the id is unknown. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|UI", meta = (WorldContext = "WorldContextObject"))
	static UUserWidget* CreateWidgetById(const UObject* WorldContextObject, FName WidgetId);

	/** Registers/overrides the soft class for a widget id (called from startup code). */
	static void RegisterWidgetClass(FName WidgetId, const FString& ClassObjectPath);

	// --- Dialogue passthroughs ---
	// FIXME-CONTRACT: the frozen scaffold UQinglanDialogueWidget declares no
	// AdvanceLine/SelectChoice members, so the passthroughs live here as
	// statics; they route exclusively through UQinglanStorySubsystem (the single
	// authoritative narrative mutation path, code-contracts.md).

	/** Continues scene playback to the next line. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue", meta = (WorldContext = "WorldContextObject"))
	static void AdvanceDialogueLine(const UObject* WorldContextObject);

	/** Selects a presented choice; returns false when no scene/choice is active. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Dialogue", meta = (WorldContext = "WorldContextObject"))
	static bool SelectDialogueChoice(const UObject* WorldContextObject, FName ChoiceId);

	// --- HUD data queries (initial bar/widget state) ---

	/** Player vitals + barrier snapshot for one-time HUD initialization. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|HUD", meta = (WorldContext = "WorldContextObject"))
	static bool GetPlayerVitals(const UObject* WorldContextObject, FQinglanAttributes& OutAttributes, float& OutBarrierAmount);

	/** Remaining paused-game-time cooldown for one technique id. */
	UFUNCTION(BlueprintPure, Category = "Qinglan|HUD", meta = (WorldContext = "WorldContextObject"))
	static float GetTechniqueCooldownRemaining(const UObject* WorldContextObject, FName TechniqueId);

	/**
	 * Seconds left in the running arena trial (Core/QinglanTrialDirector),
	 * formatted for display with FormatMinutesSeconds. False when no trial
	 * runs; HUD widgets poll this every frame (no per-tick messages).
	 */
	UFUNCTION(BlueprintPure, Category = "Qinglan|HUD", meta = (WorldContext = "WorldContextObject"))
	static bool GetTrialRemainingSeconds(const UObject* WorldContextObject, float& OutRemainingSeconds);
};
