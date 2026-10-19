// Runtime bilingual string service implementation (AUD-48) and startup locale
// policy (AUD-49). See QinglanLocalizationService.h for the design rationale.
#include "Core/QinglanLocalizationService.h"

#include "CoreGlobals.h" // GGameUserSettingsIni
#include "Internationalization/Culture.h" // complete FCulture for GetName()
#include "Internationalization/ILocalizedTextSource.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/LocalizedTextSourceTypes.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Internationalization/TextLocalizationResource.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "QinglanAscension.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/** Cooked bilingual table (both locales; row name == localization key). */
	const TCHAR* const BilingualTableAssetPath = TEXT("/Game/Qinglan/Data/DT_Localization.DT_Localization");

	/**
	 * String-table namespace used by UStringTable display string ids. UStringTable
	 * sets its FStringTable namespace to the asset name; FText::FromStringTable
	 * therefore looks up translations under "ST_Game" + key.
	 */
	const TCHAR* const GameStringTableNamespace = TEXT("ST_Game");

	/** Explicit preference lives outside profile saves so it survives slot changes. */
	const TCHAR* const PreferenceConfigSection = TEXT("QinglanLocalization");
	const TCHAR* const PreferenceConfigKey = TEXT("PreferredCulture");

	const TCHAR* const EnglishCultureName = TEXT("en-US");
	const TCHAR* const ChineseCultureName = TEXT("zh-Hans");

	/** Guards the two tables; the text localization manager may read them from a worker task. */
	FCriticalSection GTableLock;
	TMap<FString, FString> GEnglishText;
	TMap<FString, FString> GChineseText;
	bool bGTableLoaded = false;

	/** One manager-owned source per process; the shared pointer keeps it alive. */
	TSharedPtr<class FQinglanRuntimeTextSource> GTextSource;
	bool bGTextSourceRegistered = false;

	struct FLocalizedEntry
	{
		FString Key;
		FString SourceText;
		FString Translation;
	};

	/**
	 * Active-culture lookup shared by TryResolve/ResolveForCulture.
	 * zh* -> zh-Hans, any other supported value -> en-US.
	 * Returns false only when the key exists in neither locale.
	 */
	bool TryResolveFromTables(const FString& KeyString, const FString& CultureName, FString& OutValue)
	{
		FScopeLock Lock(&GTableLock);
		if (!bGTableLoaded)
		{
			return false;
		}

		if (FQinglanLocalizationService::IsChineseCulture(CultureName))
		{
			if (const FString* Chinese = GChineseText.Find(KeyString))
			{
				OutValue = *Chinese;
				return true;
			}
		}
		if (const FString* English = GEnglishText.Find(KeyString))
		{
			OutValue = *English;
			return true;
		}
		return false;
	}

	/** Copies the requested culture's entries (with en fallback) for the text source. */
	void SnapshotForCulture(const FString& CultureName, TArray<FLocalizedEntry>& OutEntries)
	{
		OutEntries.Reset();

		FScopeLock Lock(&GTableLock);
		if (!bGTableLoaded)
		{
			return;
		}

		const bool bChinese = FQinglanLocalizationService::IsChineseCulture(CultureName);
		OutEntries.Reserve(GEnglishText.Num());
		for (const TPair<FString, FString>& Pair : GEnglishText)
		{
			FLocalizedEntry& Entry = OutEntries.AddDefaulted_GetRef();
			Entry.Key = Pair.Key;
			Entry.SourceText = Pair.Value;
			if (bChinese)
			{
				// Parity validation guarantees the key, but the runtime table must
				// still degrade to English rather than emit a missing string.
				const FString* Chinese = GChineseText.Find(Pair.Key);
				Entry.Translation = Chinese != nullptr ? *Chinese : Pair.Value;
			}
			else
			{
				Entry.Translation = Pair.Value;
			}
		}
	}

	/**
	 * Bridges the bilingual DataTable into the engine's display-string lookup.
	 * This is the "equivalent registered runtime translation implementation"
	 * for projects without a .locres gather/compile step: every existing
	 * FText::FromStringTable consumer resolves through this source.
	 */
	class FQinglanRuntimeTextSource final : public ILocalizedTextSource
	{
	public:
		virtual bool GetNativeCultureName(
			const ELocalizedTextSourceCategory InCategory,
			FString& OutNativeCultureName) override
		{
			if (InCategory != ELocalizedTextSourceCategory::Game)
			{
				return false;
			}
			OutNativeCultureName = FQinglanLocalizationService::GetEnglishCultureName();
			return true;
		}

		virtual void GetLocalizedCultureNames(
			const ELocalizationLoadFlags InLoadFlags,
			TSet<FString>& OutLocalizedCultureNames) override
		{
			if (!ShouldLoadGame(InLoadFlags) && !ShouldLoadNative(InLoadFlags) && !ShouldLoadNativeGameData(InLoadFlags))
			{
				return;
			}
			OutLocalizedCultureNames.Add(FQinglanLocalizationService::GetEnglishCultureName());
			OutLocalizedCultureNames.Add(FQinglanLocalizationService::GetChineseCultureName());
		}

		virtual int32 GetPriority() const override
		{
			// Higher source priority wins over engine locres entries for the same
			// text id; this table is the project's authoritative translation.
			return ELocalizedTextSourcePriority::High;
		}

		virtual void LoadLocalizedResources(
			const ELocalizationLoadFlags InLoadFlags,
			TArrayView<const FString> InPrioritizedCultures,
			FTextLocalizationResource& InOutNativeResource,
			FTextLocalizationResource& InOutLocalizedResource) override
		{
			if (!ShouldLoadGame(InLoadFlags) && !ShouldLoadNative(InLoadFlags) && !ShouldLoadNativeGameData(InLoadFlags))
			{
				return;
			}

			// The prioritized list starts with the active culture and then its
			// language/fallback cultures. Pick the first shipped match.
			FString CultureName = FQinglanLocalizationService::GetEnglishCultureName();
			for (const FString& Candidate : InPrioritizedCultures)
			{
				FString Normalized;
				if (FQinglanLocalizationService::TryNormalizeSupportedCulture(Candidate, Normalized))
				{
					CultureName = Normalized;
					break;
				}
			}

			TArray<FLocalizedEntry> Entries;
			SnapshotForCulture(CultureName, Entries);
			if (Entries.IsEmpty())
			{
				return;
			}

			// Resource priority is inverted relative to source priority: smaller
			// numbers win (mirrors the engine locres source's BaseResourcePriority).
			const int32 ResourcePriority = -GetPriority();
			const FTextKey LocResId(TEXT("QinglanRuntime"));
			const FTextKey Namespace(GameStringTableNamespace);

			auto AddEntries = [&Entries, ResourcePriority, &LocResId, &Namespace](FTextLocalizationResource& Resource)
			{
				for (const FLocalizedEntry& Entry : Entries)
				{
					const FTextKey Key(Entry.Key);
					Resource.AddEntry(Namespace, Key, Entry.SourceText, Entry.Translation, ResourcePriority, LocResId);
				}
			};

			// LoadLocalizationResourcesForPrioritizedCultures passes the same
			// resource for both arguments in UE 5.8; add once in that case.
			AddEntries(InOutLocalizedResource);
			if (&InOutNativeResource != &InOutLocalizedResource)
			{
				AddEntries(InOutNativeResource);
			}
		}
	};

	void EnsureTextSourceRegistered()
	{
		if (bGTextSourceRegistered)
		{
			return;
		}
		GTextSource = MakeShared<FQinglanRuntimeTextSource>();
		FTextLocalizationManager::Get().RegisterTextSource(GTextSource.ToSharedRef(), /*bInRefreshResources*/ true);
		bGTextSourceRegistered = true;
	}
}

const TCHAR* FQinglanLocalizationService::GetEnglishCultureName()
{
	return EnglishCultureName;
}

const TCHAR* FQinglanLocalizationService::GetChineseCultureName()
{
	return ChineseCultureName;
}

bool FQinglanLocalizationService::IsChineseCulture(const FString& CultureName)
{
	FString Trimmed = CultureName.TrimStartAndEnd();
	return Trimmed.StartsWith(TEXT("zh"), ESearchCase::IgnoreCase);
}

bool FQinglanLocalizationService::TryNormalizeSupportedCulture(const FString& CultureName, FString& OutCultureName)
{
	FString Trimmed = CultureName.TrimStartAndEnd();
	if (IsChineseCulture(Trimmed))
	{
		OutCultureName = ChineseCultureName;
		return true;
	}
	if (Trimmed.StartsWith(TEXT("en"), ESearchCase::IgnoreCase))
	{
		OutCultureName = EnglishCultureName;
		return true;
	}
	return false;
}

FString FQinglanLocalizationService::GetOSDefaultCultureName()
{
	return FInternationalization::Get().GetDefaultLanguage()->GetName();
}

FString FQinglanLocalizationService::GetRuntimeCultureName()
{
	const FString CurrentCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	return IsChineseCulture(CurrentCulture) ? FString(ChineseCultureName) : FString(EnglishCultureName);
}

bool FQinglanLocalizationService::InitializeRuntimeResources()
{
	if (!AreResourcesLoaded())
	{
		// Load the asset outside the table lock: asset loading may trigger
		// module/UObject work that should never run under a service lock.
		UDataTable* Table = Cast<UDataTable>(FSoftObjectPath(BilingualTableAssetPath).TryLoad());
		if (Table == nullptr)
		{
			UE_LOG(LogQinglan, Warning,
				TEXT("Localization: bilingual table '%s' is not cooked/loadable; runtime text falls back to ST_Game en-US source strings"),
				BilingualTableAssetPath);
			return false;
		}
		if (Table->GetRowStruct() != FQinglanLocalizedTextRow::StaticStruct())
		{
			UE_LOG(LogQinglan, Error,
				TEXT("Localization: '%s' uses row struct '%s' instead of FQinglanLocalizedTextRow; run QinglanDataImport"),
				BilingualTableAssetPath,
				Table->GetRowStruct() != nullptr ? *Table->GetRowStruct()->GetName() : TEXT("<null>"));
			return false;
		}

		TMap<FString, FString> EnglishText;
		TMap<FString, FString> ChineseText;
		const TArray<FName> RowNames = Table->GetRowNames();
		EnglishText.Reserve(RowNames.Num());
		ChineseText.Reserve(RowNames.Num());
		for (const FName& RowName : RowNames)
		{
			const FQinglanLocalizedTextRow* Row = Table->FindRow<FQinglanLocalizedTextRow>(
				RowName, TEXT("QinglanLocalizationService"), /*bWarnIfRowMissing*/ false);
			if (Row == nullptr || Row->TextEn.IsEmpty())
			{
				UE_LOG(LogQinglan, Warning, TEXT("Localization: skipping malformed row '%s'"), *RowName.ToString());
				continue;
			}
			const FString Key = RowName.ToString();
			EnglishText.Add(Key, Row->TextEn);
			ChineseText.Add(Key, Row->TextZh.IsEmpty() ? Row->TextEn : Row->TextZh);
		}

		if (EnglishText.IsEmpty())
		{
			UE_LOG(LogQinglan, Error,
				TEXT("Localization: '%s' has no usable rows; run QinglanDataImport"), BilingualTableAssetPath);
			return false;
		}

		{
			FScopeLock Lock(&GTableLock);
			GEnglishText = MoveTemp(EnglishText);
			GChineseText = MoveTemp(ChineseText);
			bGTableLoaded = true;
		}
	}

	// Register outside the lock: RegisterTextSource refreshes resources, which
	// re-enters LoadLocalizedResources and takes the lock itself.
	EnsureTextSourceRegistered();
	UE_LOG(LogQinglan, Log, TEXT("Localization: runtime bilingual table loaded (en-US + zh-Hans)"));
	return true;
}

bool FQinglanLocalizationService::AreResourcesLoaded()
{
	FScopeLock Lock(&GTableLock);
	return bGTableLoaded;
}

FText FQinglanLocalizationService::Resolve(FName Key)
{
	FText ResolvedText;
	if (TryResolve(Key, ResolvedText))
	{
		return ResolvedText;
	}
	// Last resort: the key itself keeps UI debuggable instead of rendering blank.
	return FText::FromString(Key.ToString());
}

bool FQinglanLocalizationService::TryResolve(FName Key, FText& OutText)
{
	FString Value;
	if (!TryResolveFromTables(Key.ToString(), GetRuntimeCultureName(), Value))
	{
		return false;
	}
	OutText = FText::FromString(Value);
	return true;
}

FText FQinglanLocalizationService::ResolveForCulture(FName Key, const FString& CultureName)
{
	FString Value;
	if (TryResolveFromTables(Key.ToString(), CultureName, Value))
	{
		return FText::FromString(Value);
	}
	return FText::FromString(Key.ToString());
}

bool FQinglanLocalizationService::HasKey(FName Key)
{
	FText Unused;
	return TryResolve(Key, Unused);
}

bool FQinglanLocalizationService::ApplyCulture(const FString& CultureName)
{
	FString Normalized;
	if (!TryNormalizeSupportedCulture(CultureName, Normalized))
	{
		Normalized = EnglishCultureName;
	}

	FInternationalization& I18N = FInternationalization::Get();
	const bool bApplied = I18N.SetCurrentCulture(Normalized);
	if (bApplied)
	{
		UE_LOG(LogQinglan, Log, TEXT("Localization: culture applied: %s"), *Normalized);
	}
	else
	{
		UE_LOG(LogQinglan, Warning, TEXT("Localization: failed to apply culture '%s'"), *Normalized);
	}
	return bApplied;
}

FString FQinglanLocalizationService::ResolveStartupCulture(
	bool bFirstLaunch,
	const FString& ExplicitPreference,
	const FString& OSLanguage,
	bool& bOutShouldPersistDetected)
{
	bOutShouldPersistDetected = false;

	// 1. Explicit player preference wins on every launch (AUD-49).
	FString ExplicitCulture;
	if (TryNormalizeSupportedCulture(ExplicitPreference, ExplicitCulture))
	{
		return ExplicitCulture;
	}

	// 2. First launch: detect from the OS language. The caller captured
	//    bFirstLaunch before creating the profile, which is the ordering fix.
	if (bFirstLaunch)
	{
		bOutShouldPersistDetected = true;
		FString DetectedCulture;
		if (TryNormalizeSupportedCulture(OSLanguage, DetectedCulture))
		{
			return DetectedCulture;
		}
	}

	// 3. Stable default.
	return EnglishCultureName;
}

bool FQinglanLocalizationService::ReadPreferredCulture(FString& OutCultureName)
{
	if (GConfig == nullptr || GGameUserSettingsIni.IsEmpty())
	{
		return false;
	}
	FString StoredCulture;
	if (!GConfig->GetString(PreferenceConfigSection, PreferenceConfigKey, StoredCulture, GGameUserSettingsIni))
	{
		return false;
	}
	return TryNormalizeSupportedCulture(StoredCulture, OutCultureName);
}

bool FQinglanLocalizationService::WritePreferredCulture(const FString& CultureName)
{
	FString Normalized;
	if (!TryNormalizeSupportedCulture(CultureName, Normalized) || GConfig == nullptr || GGameUserSettingsIni.IsEmpty())
	{
		return false;
	}
	GConfig->SetString(PreferenceConfigSection, PreferenceConfigKey, *Normalized, GGameUserSettingsIni);
	GConfig->Flush(/*bRemoveFromCache*/ false, GGameUserSettingsIni);
	return true;
}

bool FQinglanLocalizationService::SetPreferredCulture(const FString& CultureName)
{
	FString Normalized;
	if (!TryNormalizeSupportedCulture(CultureName, Normalized))
	{
		return false;
	}
	// Persist first: a live switch must survive the next launch.
	if (!WritePreferredCulture(Normalized))
	{
		return false;
	}
	return ApplyCulture(Normalized);
}

#if WITH_DEV_AUTOMATION_TESTS
void FQinglanLocalizationService::SetTranslationTablesForTesting(
	const TMap<FString, FString>& EnglishText,
	const TMap<FString, FString>& ChineseText)
{
	FScopeLock Lock(&GTableLock);
	GEnglishText = EnglishText;
	GChineseText = ChineseText;
	bGTableLoaded = !GEnglishText.IsEmpty();
}

void FQinglanLocalizationService::ResetTranslationTablesForTesting()
{
	FScopeLock Lock(&GTableLock);
	GEnglishText.Reset();
	GChineseText.Reset();
	bGTableLoaded = false;
}
#endif
