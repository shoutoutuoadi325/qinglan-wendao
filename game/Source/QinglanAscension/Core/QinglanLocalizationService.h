// Runtime bilingual string service (AUD-48) and startup-locale policy
// (AUD-49).
//
// AUD-48 design: the engine's .locres/ICU gather pipeline is not available in
// this offline project, so the data importer bakes both shipped locales into a
// single cooked UDataTable (/Game/Qinglan/Data/DT_Localization). At boot this
// service loads that asset and does two things:
//   1. Resolve(Key) answers FText lookups from the table for the active
//      culture (en-US / zh-Hans), falling back to en-US and then the key.
//   2. It registers an ILocalizedTextSource so existing
//      FText::FromStringTable("/Game/Qinglan/Data/ST_Game", Key) consumers
//      (story, endings, profiles, save migration, blessings, status) resolve
//      translations too, without every call site changing.
//
// AUD-49 design: startup culture is a pure decision over captured inputs so
// the GameInstance can sample "first launch" before it creates profile 0:
//   explicit preference > first-launch OS detection > en-US.
// The preferred culture is persisted in GameUserSettings.ini (outside profile
// saves) and re-applied on every launch.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "QinglanLocalizationService.generated.h"

/**
 * One row of the packaged bilingual table. The row name is the localization
 * key (e.g. "hud.health"); both shipped cultures are columns.
 */
USTRUCT(BlueprintType)
struct QINGLANASCENSION_API FQinglanLocalizedTextRow : public FTableRowBase
{
	GENERATED_BODY()

	/** en-US source text; also the fallback for every missing key. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Localization")
	FString TextEn;

	/** zh-Hans translation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Localization")
	FString TextZh;
};

/**
 * Registered runtime translation implementation for the two shipped locales.
 * All functions are static: the service is process-wide, matching the
 * process-wide string table registry and text localization manager.
 */
class QINGLANASCENSION_API FQinglanLocalizationService
{
public:
	/** Shipped source culture. */
	static const TCHAR* GetEnglishCultureName();

	/** Shipped translation culture. */
	static const TCHAR* GetChineseCultureName();

	/** True for any BCP-47 culture whose language subtag is "zh" (zh, zh-Hans, zh-Hans-CN). */
	static bool IsChineseCulture(const FString& CultureName);

	/**
	 * Normalizes a supported culture name to exactly en-US or zh-Hans.
	 * Returns false (and leaves OutCultureName untouched) for unsupported
	 * languages, so callers can distinguish "explicit preference" from junk.
	 */
	static bool TryNormalizeSupportedCulture(const FString& CultureName, FString& OutCultureName);

	/** OS language as reported by FInternationalization::GetDefaultLanguage(). */
	static FString GetOSDefaultCultureName();

	/** Normalized culture currently applied to FInternationalization. */
	static FString GetRuntimeCultureName();

	/**
	 * Loads the bilingual DataTable and registers the localized text source.
	 * Safe to call repeatedly. Returns false when the asset is unavailable;
	 * text then falls back to the en-US ST_Game source strings.
	 */
	static bool InitializeRuntimeResources();

	/** True once a non-empty bilingual table is loaded (or test data injected). */
	static bool AreResourcesLoaded();

	/** Resolves Key for the active culture; falls back to en-US, then the key itself. */
	static FText Resolve(FName Key);

	/**
	 * Resolves Key for the active culture. Returns false when the key exists in
	 * neither locale, letting callers fall back to the ST_Game source strings.
	 */
	static bool TryResolve(FName Key, FText& OutText);

	/** Resolves Key for an explicit culture (zh* => zh-Hans, otherwise en-US). */
	static FText ResolveForCulture(FName Key, const FString& CultureName);

	/** True when either locale defines Key. */
	static bool HasKey(FName Key);

	/** Applies a supported culture to FInternationalization; defaults to en-US for junk. */
	static bool ApplyCulture(const FString& CultureName);

	/**
	 * AUD-49 ordering policy. bFirstLaunch MUST be sampled before profile 0 is
	 * created. bOutShouldPersistDetected is true only when no explicit
	 * preference exists and the first-launch branch produced the result,
	 * telling the caller to persist that culture for later launches (this
	 * includes the en-US default for an unrecognized OS language).
	 */
	static FString ResolveStartupCulture(
		bool bFirstLaunch,
		const FString& ExplicitPreference,
		const FString& OSLanguage,
		bool& bOutShouldPersistDetected);

	/** Reads the persisted explicit preference (GameUserSettings.ini). */
	static bool ReadPreferredCulture(FString& OutCultureName);

	/** Persists an explicit preference outside profile saves. */
	static bool WritePreferredCulture(const FString& CultureName);

	/** Settings-flow entry point: persist then apply immediately (live switch). */
	static bool SetPreferredCulture(const FString& CultureName);

#if WITH_DEV_AUTOMATION_TESTS
	/** Test seam: replaces the loaded tables without touching disk. */
	static void SetTranslationTablesForTesting(
		const TMap<FString, FString>& EnglishText,
		const TMap<FString, FString>& ChineseText);

	/** Test seam: clears the loaded/injected tables. */
	static void ResetTranslationTablesForTesting();
#endif
};
