// HUD/UI static helpers implementation.
//
// Text: GameText resolves through FQinglanLocalizationService (AUD-48: the
// packaged en-US + zh-Hans DT_Localization table) and falls back to the
// /Game/Qinglan/Data/ST_Game source strings via FText::FromStringTable
// (code-contracts.md "Localization keys"; docs/11-english-style-and-ui.md
// section on keys and formatting). Numbers: FText::AsNumber/AsPercent route
// through the active culture, so en-US and zh-Hans both format correctly after
// a live language switch (docs/11: live switching without restart).
#include "UI/QinglanHUDStatics.h"

#include "Blueprint/UserWidget.h"
#include "Combat/QinglanCharacter.h"
#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanTechniqueComponent.h"
#include "Core/QinglanLocalizationService.h"
#include "Core/QinglanTrialDirector.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h" // IFileManager for the one-shot load-failure diagnostic
#include "Internationalization/StringTable.h" // UE 5.8 location (was Engine/StringTable.h)
#include "Internationalization/StringTableCore.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Blueprint/WidgetBlueprintLibrary.h" // UMG public root (UMG module in Build.cs)
#include "Misc/PackageName.h" // FPackageName for the widget asset filename probe
#include "Narrative/QinglanStorySubsystem.h"
#include "QinglanAscension.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY(LogQinglanUI);

namespace
{
	// String table asset id (code-contracts.md "Localization keys").
	const TCHAR* GGameStringTablePath = TEXT("/Game/Qinglan/Data/ST_Game");

	// Cooldown display keeps one fractional digit (authored HUD readability).
	constexpr int32 CooldownFractionDigits = 1;

	// Clock key for room/buff timers: "hud.clock" = "{minutes}:{seconds}"
	// (present in both localization locale files).
	const TCHAR* GClockFormatKey = TEXT("hud.clock");

	/**
	 * Default widget id -> Blueprint class object path table.
	 * Convention: /Game/Qinglan/UI/<AssetName>.<AssetName>_C, mirroring the
	 * module-wide /Game/Qinglan/{Data,Input,Audio} asset layout.
	 * FIXME-CONTRACT: no design doc pins these asset names; RegisterWidgetClass
	 * overrides any entry without code changes.
	 */
	struct FQinglanWidgetDefault { FName Id; const TCHAR* Path; };
	const FQinglanWidgetDefault GDefaultWidgetAssets[] = {
		{ FName(TEXT("HUD")),      TEXT("/Game/Qinglan/UI/WBP_HUD.WBP_HUD_C") },
		{ FName(TEXT("Dialogue")), TEXT("/Game/Qinglan/UI/WBP_Dialogue.WBP_Dialogue_C") },
		{ FName(TEXT("Journal")),  TEXT("/Game/Qinglan/UI/WBP_Journal.WBP_Journal_C") },
		{ FName(TEXT("Ending")),   TEXT("/Game/Qinglan/UI/WBP_Ending.WBP_Ending_C") },
	};

	TMap<FName, FString>& WidgetPathRegistry()
	{
		static TMap<FName, FString> Registry;
		if (Registry.IsEmpty())
		{
			for (const FQinglanWidgetDefault& Entry : GDefaultWidgetAssets)
			{
				Registry.Add(Entry.Id, FString(Entry.Path));
			}
		}
		return Registry;
	}

	// Resolved-class cache uses weak pointers so stale UClass entries cannot
	// outlive a recompiled/reloaded widget asset.
	TMap<FName, TWeakObjectPtr<UClass>>& WidgetClassCache()
	{
		static TMap<FName, TWeakObjectPtr<UClass>> Cache;
		return Cache;
	}

	UGameInstance* GameInstanceFrom(const UObject* WorldContextObject)
	{
		if (!WorldContextObject)
		{
			return nullptr;
		}
		if (UGameInstance* AsInstance = const_cast<UGameInstance*>(Cast<UGameInstance>(WorldContextObject)))
		{
			return AsInstance;
		}
		UWorld* World = GEngine
			? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull)
			: nullptr;
		return World ? World->GetGameInstance() : nullptr;
	}

	UStringTable* LoadGameStringTable()
	{
		// The table is tiny and requested constantly; cache weakly so a
		// reimport during editor sessions is picked up.
		static TWeakObjectPtr<UStringTable> Cached;
		if (UStringTable* Table = Cached.Get())
		{
			return Table;
		}
		UStringTable* Loaded = Cast<UStringTable>(FSoftObjectPath(GGameStringTablePath).TryLoad());
		Cached = Loaded;
		return Loaded;
	}
}

FName UQinglanHUDStatics::GameStringTableId()
{
	return FName(GGameStringTablePath);
}

FText UQinglanHUDStatics::GameText(FName Key)
{
	// AUD-48: the runtime bilingual service (packaged DT_Localization) is the
	// primary resolver; it follows the active culture and falls back en-US.
	// ST_Game remains the final fallback so a missing/unqueued importer output
	// never turns player-facing text blank.
	FText Result;
	if (FQinglanLocalizationService::TryResolve(Key, Result))
	{
		return Result;
	}

	// FText::FromStringTable is the sanctioned source-string path
	// (code-contracts.md); it logs internally for malformed table ids.
	Result = FText::FromStringTable(GGameStringTablePath, Key.ToString());
	if (Result.IsEmpty())
	{
		UE_LOG(LogQinglanUI, Warning, TEXT("GameText: no runtime translation or string table key '%s'"), *Key.ToString());
	}
	return Result;
}

bool UQinglanHUDStatics::HasGameText(FName Key)
{
	if (FQinglanLocalizationService::HasKey(Key))
	{
		return true;
	}
	if (UStringTable* Table = LoadGameStringTable())
	{
		const TSharedPtr<FStringTable> Root = Table->GetMutableStringTable();
		// UE 5.8: FStringTable has no GetString; FindEntry is the key-exists probe.
		return Root.IsValid() && Root->FindEntry(FTextKey(Key.ToString())) != nullptr;
	}
	return false;
}

FText UQinglanHUDStatics::FormatGameTextWithNumber(FName Key, FName ArgName, int32 Value)
{
	FFormatNamedArguments Args;
	Args.Add(ArgName.ToString(), FText::AsNumber(Value));
	return FText::Format(GameText(Key), Args);
}

FText UQinglanHUDStatics::FormatGameTextWithInt64(FName Key, FName ArgName, int64 Value)
{
	FFormatNamedArguments Args;
	Args.Add(ArgName.ToString(), FText::AsNumber(Value));
	return FText::Format(GameText(Key), Args);
}

FText UQinglanHUDStatics::FormatGameTextWithText(FName Key, FName ArgName, const FText& Value)
{
	FFormatNamedArguments Args;
	Args.Add(ArgName.ToString(), Value);
	return FText::Format(GameText(Key), Args);
}

FText UQinglanHUDStatics::FormatNumber(int32 Value)
{
	return FText::AsNumber(Value);
}

FText UQinglanHUDStatics::FormatNumber64(int64 Value)
{
	return FText::AsNumber(Value);
}

FText UQinglanHUDStatics::FormatSpiritShards(int64 Amount)
{
	// "hud.shards" labels the stat; the amount itself is pure number text so
	// locale grouping applies (docs/11 locale-aware numeric formatting).
	return FText::AsNumber(FMath::Max<int64>(0, Amount));
}

FText UQinglanHUDStatics::FormatPercent(float FractionZeroToOne)
{
	return FText::AsPercent(FMath::Clamp(FractionZeroToOne, 0.f, 1.f));
}

FText UQinglanHUDStatics::FormatCooldownSeconds(float Seconds)
{
	FNumberFormattingOptions Options;
	Options.MinimumFractionalDigits = CooldownFractionDigits;
	Options.MaximumFractionalDigits = CooldownFractionDigits;
	return FText::AsNumber(FMath::Max(0.f, Seconds), &Options);
}

FText UQinglanHUDStatics::FormatMinutesSeconds(int32 TotalSeconds)
{
	const int32 Clamped = FMath::Max(0, TotalSeconds);
	const int32 Minutes = Clamped / 60;
	const int32 Seconds = Clamped % 60;

	FNumberFormattingOptions SecondDigits;
	SecondDigits.MinimumIntegralDigits = 2;
	SecondDigits.MaximumIntegralDigits = 2;

	// Named-argument pattern from the string table; the colon separator lives
	// inside the "hud.clock" pattern, never in code.
	FFormatNamedArguments Args;
	Args.Add(TEXT("minutes"), FText::AsNumber(Minutes));
	Args.Add(TEXT("seconds"), FText::AsNumber(Seconds, &SecondDigits));
	return FText::Format(GameText(GClockFormatKey), Args);
}

TSubclassOf<UUserWidget> UQinglanHUDStatics::ResolveWidgetClass(FName WidgetId)
{
	TMap<FName, TWeakObjectPtr<UClass>>& Cache = WidgetClassCache();
	if (const TWeakObjectPtr<UClass>* Hit = Cache.Find(WidgetId))
	{
		if (UClass* Loaded = Hit->Get())
		{
			return Loaded;
		}
	}
	const FString* Path = WidgetPathRegistry().Find(WidgetId);
	if (!Path)
	{
		UE_LOG(LogQinglanUI, Warning, TEXT("ResolveWidgetClass: unknown widget id '%s'"), *WidgetId.ToString());
		return nullptr;
	}

	UClass* Resolved = StaticLoadClass(UUserWidget::StaticClass(), nullptr, **Path);
	if (!Resolved)
	{
		// One-shot diagnostic per id (StaticLoadClass already logged its own
		// load error above this line): distinguish a missing on-disk asset
		// from a load-time failure so the remediation is obvious.
		static TSet<FName> ReportedLoadFailures;
		if (ReportedLoadFailures.Contains(WidgetId))
		{
			return nullptr;
		}
		ReportedLoadFailures.Add(WidgetId);
		const FString PackageName = FPackageName::ObjectPathToPackageName(*Path);
		const FString AssetFile = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		const bool bFileExists = IFileManager::Get().FileSize(*AssetFile) >= 0;
		const FString Notice = FString::Printf(
			TEXT("ResolveWidgetClass: failed to load '%s' from %s (asset file %s: %s); regenerate with QinglanUIBootstrapCommandlet"),
			*WidgetId.ToString(), **Path, *AssetFile,
			bFileExists ? TEXT("exists, load failed") : TEXT("missing"));
		UE_LOG(LogQinglanUI, Warning, TEXT("%s"), *Notice);
		return nullptr;
	}
	UE_LOG(LogQinglanUI, Verbose, TEXT("ResolveWidgetClass: %s -> %s"),
		*WidgetId.ToString(), *Resolved->GetPathName());
	Cache.Add(WidgetId, Resolved);
	return Resolved;
}

UUserWidget* UQinglanHUDStatics::CreateWidgetById(const UObject* WorldContextObject, FName WidgetId)
{
	TSubclassOf<UUserWidget> Class = ResolveWidgetClass(WidgetId);
	if (!Class)
	{
		return nullptr;
	}
	UWorld* World = GEngine
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull)
		: nullptr;
	APlayerController* OwningPlayer = World ? World->GetFirstPlayerController() : nullptr;
	// Create takes a mutable context (world resolution only); the const here just pins the lookup.
	return UWidgetBlueprintLibrary::Create(const_cast<UObject*>(WorldContextObject), Class, OwningPlayer);
}

void UQinglanHUDStatics::RegisterWidgetClass(FName WidgetId, const FString& ClassObjectPath)
{
	WidgetPathRegistry().Add(WidgetId, ClassObjectPath);
	WidgetClassCache().Remove(WidgetId);
}

void UQinglanHUDStatics::AdvanceDialogueLine(const UObject* WorldContextObject)
{
	UGameInstance* Instance = GameInstanceFrom(WorldContextObject);
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	if (Story)
	{
		Story->AdvanceLine();
	}
	else
	{
		UE_LOG(LogQinglanUI, Warning, TEXT("AdvanceDialogueLine: story subsystem unavailable"));
	}
}

bool UQinglanHUDStatics::SelectDialogueChoice(const UObject* WorldContextObject, FName ChoiceId)
{
	UGameInstance* Instance = GameInstanceFrom(WorldContextObject);
	UQinglanStorySubsystem* Story = Instance ? Instance->GetSubsystem<UQinglanStorySubsystem>() : nullptr;
	return Story ? Story->SelectChoice(ChoiceId) : false;
}

bool UQinglanHUDStatics::GetPlayerVitals(const UObject* WorldContextObject,
	FQinglanAttributes& OutAttributes, float& OutBarrierAmount)
{
	const AQinglanCharacter* Player = Cast<AQinglanCharacter>(
		UGameplayStatics::GetPlayerCharacter(WorldContextObject, 0));
	const UQinglanCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
	if (!Combat)
	{
		OutAttributes = FQinglanAttributes();
		OutBarrierAmount = 0.f;
		return false;
	}
	OutAttributes = Combat->GetAttributes();
	OutBarrierAmount = Combat->GetBarrierAmount();
	return true;
}

float UQinglanHUDStatics::GetTechniqueCooldownRemaining(const UObject* WorldContextObject, FName TechniqueId)
{
	const AQinglanCharacter* Player = Cast<AQinglanCharacter>(
		UGameplayStatics::GetPlayerCharacter(WorldContextObject, 0));
	const UQinglanTechniqueComponent* Techniques = Player ? Player->GetTechniques() : nullptr;
	return Techniques ? Techniques->GetCooldownRemaining(TechniqueId) : 0.f;
}

bool UQinglanHUDStatics::GetTrialRemainingSeconds(const UObject* WorldContextObject, float& OutRemainingSeconds)
{
	// One trial director per arena world (spawned by the trial anchor's proof
	// press); absent everywhere else - the query is a clean false, never a log.
	// AUD-20: the countdown reads as the timed survival phase only; once the
	// clock expires the required-kill fight is its own beat with no timer UI.
	OutRemainingSeconds = 0.f;
	AQinglanTrialDirector* Director = Cast<AQinglanTrialDirector>(
		UGameplayStatics::GetActorOfClass(WorldContextObject, AQinglanTrialDirector::StaticClass()));
	if (Director == nullptr || !Director->IsTimedSurvivalActive())
	{
		return false;
	}
	OutRemainingSeconds = Director->GetRemainingSeconds();
	return true;
}
