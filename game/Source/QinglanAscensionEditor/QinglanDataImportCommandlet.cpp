// UQinglanDataImportCommandlet implementation: JSON -> data asset importer +
// content validator. Authoritative contracts:
//   docs/dev/implementation-blueprint.md section 3 "Data Contracts"
//   docs/dev/implementation-blueprint.md sections 1.4-1.9 (numbers)
//   docs/dev/code-contracts.md "Localization keys" (ST_Game regeneration)
//   AUD-48: both locales are also baked into DT_Localization for the runtime
//   bilingual service (no engine .locres gather step in this offline project)
//   docs/03-progression-and-economy.md sections 1-2 (payouts used for parity)
//
// Determinism: file iteration is always sorted, no RNG is used anywhere, and
// every tunable below cites its doc source. The importer runs headless
// (-nullrhi safe): it only touches the file system, JSON, and UObjects.
//
// Cross-module linkage note: the story/technique/realm data asset classes are
// created through the runtime class registry (FindQinglanScriptClass) and then
// filled through typed member access, so this module takes no C++ symbol
// dependency on those (still in-flight) runtime folders. Only their headers
// (inline types) are included.
#include "QinglanDataImportCommandlet.h"

#include "QinglanAscensionEditor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/QinglanLocalizationService.h" // FQinglanLocalizedTextRow (AUD-48)
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/StringTable.h" // UE 5.8 location (was Engine/StringTable.h)
#include "Internationalization/StringTableCore.h" // FStringTable definition (SetSourceString); the header above only forward-declares it
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include "Abilities/QinglanTechniqueTypes.h"
#include "Core/QinglanCoreTypes.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Narrative/QinglanStoryIdentity.h"
#include "Progression/QinglanProgressionTables.h"

namespace
{
	// ---------------------------------------------------------------------------
	// Constants (every tunable cites its doc source)
	// ---------------------------------------------------------------------------

	/** Exactly two shipped locales. docs/dev/implementation-blueprint.md section 1.8. */
	constexpr const TCHAR* RequiredCultureEnUs = TEXT("en-US");
	constexpr const TCHAR* RequiredCultureZhHans = TEXT("zh-Hans");

	/**
	 * The string table holds one source culture; the other locale is validated
	 * for key + placeholder parity and compiled by the localization gather step.
	 * Source culture is en-US (project source language).
	 * docs/dev/implementation-blueprint.md section 3 "Localization" (import
	 * mechanism was undecided; this commandlet is the documented mechanism).
	 */
	constexpr const TCHAR* StringTableSourceCulture = TEXT("en-US");

	/** Single player-facing string table. docs/dev/code-contracts.md "Localization keys". */
	constexpr const TCHAR* StringTableAssetPath = TEXT("/Game/Qinglan/Data/ST_Game");

	/**
	 * AUD-48: packaged bilingual table consumed by FQinglanLocalizationService.
	 * Row name = localization key, columns = en-US + zh-Hans. Baking both
	 * locales into a cooked asset replaces the unavailable .locres pipeline and
	 * keeps the repository JSON out of the shipped build.
	 */
	constexpr const TCHAR* BilingualTableAssetPath = TEXT("/Game/Qinglan/Data/DT_Localization");

	/** Story registry asset. Task contract + QinglanStoryDataTypes.h header note. */
	constexpr const TCHAR* StoryRegistryAssetPath = TEXT("/Game/Qinglan/Data/DA_StoryRegistry");

	/** Story JSON schema version. docs/dev/implementation-blueprint.md section 3 ("schema_version 1 everywhere"). */
	constexpr int32 ExpectedSchemaVersion = 1;

	/** Realm room contract: exactly 7 rooms, entrance first, exit last. docs/dev/implementation-blueprint.md section 1.4. */
	constexpr int32 RealmRoomContractCount = 7;

	/** Stable realm ids. docs/dev/implementation-blueprint.md section 1.4 / docs/03 section 2. */
	constexpr const TCHAR* BambooRealmId = TEXT("bamboo_hollow");
	constexpr const TCHAR* AshenRealmId = TEXT("ashen_peak");
	/** Base payout equals the room shard sum (docs/03 section 2 "base is the expedition total incl. room rewards"). */
	constexpr int32 BambooRoomShardSum = 180;
	constexpr int32 AshenRoomShardSum = 300;
	/** Bamboo first-clear bonus 120 Shards + 3 Herbs + 1 Crystal. docs/03 section 2. */
	constexpr int64 BambooFirstClearShards = 120, BambooFirstClearHerbs = 3, BambooFirstClearCrystals = 1;
	/** Ashen first-clear bonus 200 Shards + 3 Crystals. docs/03 section 2. */
	constexpr int64 AshenFirstClearShards = 200, AshenFirstClearCrystals = 3;

	/** Every authored choice action type. docs/dev/implementation-blueprint.md section 3 + data/story/side-quests.json. */
	const TCHAR* const KnownActionTypes[] =
	{
		TEXT("enter_trial"), TEXT("stay_at_hub"), TEXT("release_guardian"), TEXT("preview_ending"),
		TEXT("confirm_ending"), TEXT("cancel_confirmation"), TEXT("return_to_hub"), TEXT("complete_campaign"),
		TEXT("select_proposal"), TEXT("accept_verified_proposal")
	};

	/** Ending tokens -> committed ending flags (mutually exclusive set). docs/dev/implementation-blueprint.md section 2.3. */
	const TCHAR* const EndingMutuallyExclusiveFlags[] = { TEXT("ending_open"), TEXT("ending_seal"), TEXT("ending_shared") };

	/** All three effective agreements gate ending_shared. docs/dev/implementation-blueprint.md section 2.3; code-contracts.md "Narrative semantics". */
	const TCHAR* const SharedEndingRequiredFlags[] =
	{
		TEXT("sq_spillway_verified"), TEXT("sq_allocation_agreed"), TEXT("sq_handover_accepted")
	};

	/** JSON object fields (string values) that reference string-table keys. */
	const TCHAR* const LocKeyRefFields[] =
	{
		TEXT("text_key"), TEXT("speaker_key"), TEXT("source_kind_key"), TEXT("label_key"),
		TEXT("title_key"), TEXT("summary_key"), TEXT("retry_key"), TEXT("journal_key"),
		TEXT("advance_delivery_key"), TEXT("delivery_time_key"), TEXT("show_after_text_key"),
		TEXT("name_key")
	};

	/** JSON array-of-string fields that reference string-table keys. */
	const TCHAR* const LocKeyRefListFields[] = { TEXT("response_keys"), TEXT("epilogue_keys"), TEXT("player_text_keys") };

	/** Head-count sanity warnings for the imported narrative. docs/dev/implementation-blueprint.md section 1.9. */
	constexpr int32 ExpectedMainSceneCount = 30;
	constexpr int32 ExpectedSideStageCount = 24;
	constexpr int32 ExpectedRecordCount = 24;
	constexpr int32 ExpectedBarkCount = 40;

	/**
	 * Expected shipped techniques with the numbers fixed by
	 * docs/dev/implementation-blueprint.md section 2.5 (table + "Unlock order").
	 * Targeting/Magnitude/Range/UnlockFlag are blueprint-only columns (not in
	 * techniques.json), so the importer authors them from this table.
	 */
	struct FExpectedTechnique
	{
		const TCHAR* Id;
		const TCHAR* Element;
		int32 EnergyCost;
		float CooldownSeconds;
		float PowerCoefficient;
		int32 FlatDamage;
		const TCHAR* Targeting;
		float Magnitude;
		float Range;
		const TCHAR* UnlockFlag;
	};
	const FExpectedTechnique ExpectedTechniques[] =
	{
		{ TEXT("jade_sword_arc"),   TEXT("Metal"),     12, 2.5f, 1.5f, 5, TEXT("SelfArc"),      0.f,   1200.f, nullptr /* starting technique, CH00_S02 */ },
		{ TEXT("ember_lance"),      TEXT("Fire"),      20, 6.0f, 2.0f, 5, TEXT("Projectile"),   0.f,   0.f,    TEXT("mq_ch01_complete") /* post-guided, CH01_S03 */ },
		{ TEXT("flowing_ward"),     TEXT("Water"),     18, 8.0f, 0.0f, 0, TEXT("SelfBarrier"),  25.f,  0.f,    nullptr /* starting technique, CH00_S02 */ },
		{ TEXT("thunder_seal"),     TEXT("Lightning"), 24, 7.0f, 2.2f, 8, TEXT("GroundStrike"), 0.f,   300.f,  TEXT("boss_bamboo_guardian_cleared") /* Bamboo first clear */ },
		{ TEXT("stone_aegis"),      TEXT("Earth"),     22, 10.0f, 0.0f, 0, TEXT("SelfBarrier"),  35.f,  0.f,    TEXT("foundation_established") /* Foundation */ },
		{ TEXT("verdant_pulse"),    TEXT("Wood"),      25, 12.0f, 0.0f, 0, TEXT("SelfHeal"),     20.f,  0.f,    TEXT("foundation_established") /* Foundation */ },
		{ TEXT("mist_step"),        TEXT("Water"),     15, 9.0f, 0.0f, 0, TEXT("SelfDash"),      600.f, 0.f,    TEXT("sq_warning_record_filed") /* SQ04 */ },
		{ TEXT("orbiting_blades"),  TEXT("Metal"),     30, 14.0f, 0.6f, 2, TEXT("SelfPulses"),    6.f,   0.f,    TEXT("sq_iron_reconciled") /* SQ05 */ },
	};

	// ---------------------------------------------------------------------------
	// Small utilities
	// ---------------------------------------------------------------------------

	void AddError(TArray<FString>& Errors, const FString& Context, const FString& Message)
	{
		const FString Line = Context.IsEmpty() ? Message : FString::Printf(TEXT("%s: %s"), *Context, *Message);
		Errors.Add(Line);
		UE_LOG(LogQinglanEditor, Error, TEXT("%s"), *Line);
	}

	/** Deterministic FNV-1a-64 over a string (local copy: QinglanSave::Fnv1a64 is not exported cross-module). */
	uint64 HashFnv1a64(const FString& Value)
	{
		uint64 Hash = 0xcbf29ce484222325ULL;
		const uint64 Prime = 0x100000001b3ULL;
		for (const TCHAR Char : Value)
		{
			Hash ^= static_cast<uint64>(Char);
			Hash *= Prime;
		}
		return Hash;
	}

	/** Extracts named {format} arguments from a localized string (FText::Format named args, blueprint section 1.8). */
	void ExtractFormatArgs(const FString& Source, TArray<FString>& OutArgs)
	{
		int32 SearchFrom = 0;
		while (SearchFrom < Source.Len())
		{
			const int32 Open = Source.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Open == INDEX_NONE) { break; }
			const int32 Close = Source.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
			if (Close == INDEX_NONE) { break; }
			const FString Inner = Source.Mid(Open + 1, Close - Open - 1);
			// Literal braces are escaped "{{"/"}}"; only identifier-shaped named args count.
			bool bIdentifier = !Inner.IsEmpty();
			for (const TCHAR Char : Inner)
			{
				const bool bOk = FChar::IsAlnum(Char) || Char == TEXT('_');
				if (!bOk) { bIdentifier = false; break; }
			}
			if (bIdentifier)
			{
				OutArgs.AddUnique(Inner);
			}
			SearchFrom = Close + 1;
		}
	}

	// ---------------------------------------------------------------------------
	// JSON helpers (only stable As*/HasField accessors are used)
	// ---------------------------------------------------------------------------

	bool ReadJsonObject(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, TArray<FString>& Errors)
	{
		const FString Context = FPaths::GetCleanFilename(FilePath);
		FString FileText;
		if (!FFileHelper::LoadFileToString(FileText, *FilePath))
		{
			AddError(Errors, Context, TEXT("file could not be read"));
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileText);
		TSharedPtr<FJsonObject> Parsed;
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			AddError(Errors, Context, TEXT("invalid JSON"));
			return false;
		}
		if (!Reader->GetErrorMessage().IsEmpty()) // UE 5.8: TJsonReader has GetErrorMessage(), no GetErrorCode()
		{
			AddError(Errors, Context, TEXT("trailing garbage after JSON document"));
			return false;
		}
		OutObject = Parsed;
		return true;
	}

	bool JHas(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field) { return Obj->HasField(Field); }

	FString JStr(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, const FString& Default = FString())
	{
		// UE 5.8: untyped GetField is GetFieldUntyped (GetField now requires an EJson template arg).
		return Obj->HasField(Field) ? Obj->GetFieldUntyped(Field)->AsString() : Default;
	}

	FName JName(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		const FString Value = JStr(Obj, Field);
		return Value.IsEmpty() ? NAME_None : FName(*Value);
	}

	double JNum(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, double Default = 0.0)
	{
		if (!Obj->HasField(Field)) { return Default; }
		double Out = Default;
		if (Obj->GetFieldUntyped(Field)->TryGetNumber(Out)) { return Out; }
		return Default;
	}

	bool JBool(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field, bool Default = false)
	{
		if (!Obj->HasField(Field)) { return Default; }
		bool Out = Default;
		if (Obj->GetFieldUntyped(Field)->TryGetBool(Out)) { return Out; }
		return Default;
	}

	TArray<TSharedPtr<FJsonValue>> JArr(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		if (!Obj->HasField(Field)) { return TArray<TSharedPtr<FJsonValue>>(); }
		return Obj->GetFieldUntyped(Field)->AsArray();
	}

	TSharedPtr<FJsonObject> JObj(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		if (!Obj->HasField(Field)) { return nullptr; }
		return Obj->GetFieldUntyped(Field)->AsObject();
	}

	TArray<FName> JNameArray(const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FName> Out;
		for (const TSharedPtr<FJsonValue>& Value : JArr(Obj, Field))
		{
			if (Value.IsValid() && !Value->IsNull())
			{
				Out.Add(FName(*Value->AsString()));
			}
		}
		return Out;
	}

	// ---------------------------------------------------------------------------
	// Enum parsing (values fixed by the data contracts; unknown -> error)
	// ---------------------------------------------------------------------------

	bool ParseElement(const FString& Text, EQinglanElement& Out) // {"Metal","Wood","Water","Fire","Earth","Lightning"}; blueprint section 5 item 6
	{
		if (Text == TEXT("None")) { Out = EQinglanElement::None; return true; }
		if (Text == TEXT("Metal")) { Out = EQinglanElement::Metal; return true; }
		if (Text == TEXT("Wood")) { Out = EQinglanElement::Wood; return true; }
		if (Text == TEXT("Water")) { Out = EQinglanElement::Water; return true; }
		if (Text == TEXT("Fire")) { Out = EQinglanElement::Fire; return true; }
		if (Text == TEXT("Earth")) { Out = EQinglanElement::Earth; return true; }
		if (Text == TEXT("Lightning")) { Out = EQinglanElement::Lightning; return true; }
		return false;
	}

	bool ParseRoomKind(const FString& Text, EQinglanRoomKind& Out) // blueprint section 3 realm contract
	{
		if (Text == TEXT("entrance")) { Out = EQinglanRoomKind::Entrance; return true; }
		if (Text == TEXT("combat")) { Out = EQinglanRoomKind::Combat; return true; }
		if (Text == TEXT("shrine")) { Out = EQinglanRoomKind::Shrine; return true; }
		if (Text == TEXT("elite")) { Out = EQinglanRoomKind::Elite; return true; }
		if (Text == TEXT("boss")) { Out = EQinglanRoomKind::Boss; return true; }
		if (Text == TEXT("exit")) { Out = EQinglanRoomKind::Exit; return true; }
		return false;
	}

	bool ParseSourceKind(const FString& Text, EQinglanSourceKind& Out) // blueprint section 3 scene contract
	{
		if (Text == TEXT("live")) { Out = EQinglanSourceKind::Live; return true; }
		if (Text == TEXT("paper")) { Out = EQinglanSourceKind::Paper; return true; }
		if (Text == TEXT("letter")) { Out = EQinglanSourceKind::Letter; return true; }
		if (Text == TEXT("system")) { Out = EQinglanSourceKind::System; return true; }
		if (Text == TEXT("memory")) { Out = EQinglanSourceKind::Memory; return true; }
		if (Text == TEXT("recorded")) { Out = EQinglanSourceKind::Recorded; return true; }
		return false;
	}

	bool ParseTargeting(const FString& Text, EQinglanTargeting& Out) // blueprint section 2.5 delivery column
	{
		if (Text == TEXT("SelfArc")) { Out = EQinglanTargeting::SelfArc; return true; }
		if (Text == TEXT("Projectile")) { Out = EQinglanTargeting::Projectile; return true; }
		if (Text == TEXT("SelfBarrier")) { Out = EQinglanTargeting::SelfBarrier; return true; }
		if (Text == TEXT("SelfHeal")) { Out = EQinglanTargeting::SelfHeal; return true; }
		if (Text == TEXT("GroundStrike")) { Out = EQinglanTargeting::GroundStrike; return true; }
		if (Text == TEXT("SelfDash")) { Out = EQinglanTargeting::SelfDash; return true; }
		if (Text == TEXT("SelfPulses")) { Out = EQinglanTargeting::SelfPulses; return true; }
		return false;
	}

	// ---------------------------------------------------------------------------
	// File discovery (sorted => deterministic import order)
	// ---------------------------------------------------------------------------

	void FindFilesSorted(const FString& Directory, const FString& Pattern, TArray<FString>& OutPaths)
	{
		TArray<FString> FileNames;
		IFileManager::Get().FindFiles(FileNames, *(Directory / Pattern), true, false);
		FileNames.Sort();
		for (const FString& FileName : FileNames)
		{
			OutPaths.Add(Directory / FileName);
		}
	}

	// ---------------------------------------------------------------------------
	// Locale loading + parity
	// ---------------------------------------------------------------------------

	/** culture -> (key -> source string) plus (key -> defining file) for duplicate diagnostics. */
	struct FQinglanLocaleData
	{
		TMap<FString, TMap<FString, FString>> CultureKeys;
		TMap<FString, TMap<FString, FString>> CultureKeySourceFile;
		TArray<FString> Cultures; // sorted
	};

	void LoadLocaleFile(const FString& FilePath, const FString& Culture, FQinglanLocaleData& Out, TArray<FString>& Errors)
	{
		TSharedPtr<FJsonObject> Root;
		if (!ReadJsonObject(FilePath, Root, Errors)) { return; }
		const FString Context = FString::Printf(TEXT("%s/%s"), *Culture, *FPaths::GetCleanFilename(FilePath));
		TMap<FString, FString>& Keys = Out.CultureKeys.FindOrAdd(Culture);
		TMap<FString, FString>& KeyFiles = Out.CultureKeySourceFile.FindOrAdd(Culture);
		// UE 5.8: FJsonObject::Values keys are UE::TSharedString, not FString (auto keeps this key-type agnostic).
		for (const auto& Pair : Root->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->IsNull()) { continue; }
			const FString Key(Pair.Key);
			const FString Value = Pair.Value->AsString();
			if (Key.IsEmpty())
			{
				AddError(Errors, Context, TEXT("empty string-table key"));
				continue;
			}
			if (const FString* Existing = Keys.Find(Key))
			{
				if (*Existing != Value)
				{
					const FString* ExistingFile = KeyFiles.Find(Key);
					AddError(Errors, Context, FString::Printf(TEXT("key '%s' already defined with a different value in '%s'"),
						*Key, ExistingFile ? **ExistingFile : TEXT("?")));
					continue;
				}
				continue; // identical re-declaration across files is tolerated
			}
			Keys.Add(Key, Value);
			KeyFiles.Add(Key, Context);
		}
	}

	void LoadAllLocales(const FString& LocDir, FQinglanLocaleData& Out, TArray<FString>& Errors)
	{
		// Top-level <culture>.json files define the culture set.
		TArray<FString> TopFiles;
		FindFilesSorted(LocDir, TEXT("*.json"), TopFiles);
		TArray<FString> Cultures;
		for (const FString& Path : TopFiles)
		{
			Cultures.Add(FPaths::GetBaseFilename(Path));
		}
		Cultures.Sort();
		// Exactly two locales ship. docs/dev/implementation-blueprint.md section 1.8.
		if (!Cultures.Contains(RequiredCultureEnUs) || !Cultures.Contains(RequiredCultureZhHans))
		{
			AddError(Errors, TEXT("localization"),
				FString::Printf(TEXT("expected exactly the locales en-US and zh-Hans, found %d: [%s]"),
					Cultures.Num(), *FString::Join(Cultures, TEXT(", "))));
		}
		for (const FString& Culture : Cultures)
		{
			const FString CultureFile = LocDir / (Culture + TEXT(".json"));
			LoadLocaleFile(CultureFile, Culture, Out, Errors);
			// Story payload lives in localization/story/<culture>/*.json (blueprint section 3).
			const FString StoryDir = LocDir / TEXT("story") / Culture;
			TArray<FString> StoryFiles;
			FindFilesSorted(StoryDir, TEXT("*.json"), StoryFiles);
			for (const FString& Path : StoryFiles)
			{
				LoadLocaleFile(Path, Culture, Out, Errors);
			}
		}
		Out.Cultures = Cultures;
	}

	/**
	 * Locale parity: identical key sets and identical named {placeholder} sets
	 * per key across every culture. docs/dev/implementation-blueprint.md
	 * section 3 "identical keys/placeholders both locales".
	 */
	void ValidateLocaleParity(const FQinglanLocaleData& Locales, TArray<FString>& Errors)
	{
		if (Locales.Cultures.Num() < 2) { return; }
		const FString& Reference = Locales.Cultures[0];
		const TMap<FString, FString>* RefKeysPtr = Locales.CultureKeys.Find(Reference);
		if (RefKeysPtr == nullptr)
		{
			return; // load errors were already reported
		}
		const TMap<FString, FString>& RefKeys = *RefKeysPtr;
		for (int32 CultureIndex = 1; CultureIndex < Locales.Cultures.Num(); ++CultureIndex)
		{
			const FString& Other = Locales.Cultures[CultureIndex];
			const TMap<FString, FString>* OtherKeysPtr = Locales.CultureKeys.Find(Other);
			if (OtherKeysPtr == nullptr)
			{
				continue;
			}
			const TMap<FString, FString>& OtherKeys = *OtherKeysPtr;
			for (const TPair<FString, FString>& Pair : RefKeys)
			{
				if (!OtherKeys.Contains(Pair.Key))
				{
					AddError(Errors, TEXT("locale parity"), FString::Printf(TEXT("key '%s' missing in '%s'"), *Pair.Key, *Other));
				}
			}
			for (const TPair<FString, FString>& Pair : OtherKeys)
			{
				if (!RefKeys.Contains(Pair.Key))
				{
					AddError(Errors, TEXT("locale parity"), FString::Printf(TEXT("key '%s' missing in '%s'"), *Pair.Key, *Reference));
				}
			}
		}
		// Placeholder parity is checked against every culture.
		for (const TPair<FString, FString>& Pair : RefKeys)
		{
			TArray<FString> RefArgs;
			ExtractFormatArgs(Pair.Value, RefArgs);
			RefArgs.Sort();
			for (int32 CultureIndex = 1; CultureIndex < Locales.Cultures.Num(); ++CultureIndex)
			{
				const FString& Other = Locales.Cultures[CultureIndex];
				const TMap<FString, FString>* OtherKeysPtr = Locales.CultureKeys.Find(Other);
				if (OtherKeysPtr == nullptr)
				{
					continue;
				}
				if (const FString* OtherValue = OtherKeysPtr->Find(Pair.Key))
				{
					TArray<FString> OtherArgs;
					ExtractFormatArgs(*OtherValue, OtherArgs);
					OtherArgs.Sort();
					if (OtherArgs != RefArgs)
					{
						AddError(Errors, TEXT("locale parity"),
							FString::Printf(TEXT("key '%s' format arguments differ: [%s] vs [%s]"),
								*Pair.Key, *FString::Join(RefArgs, TEXT(",")), *FString::Join(OtherArgs, TEXT(","))));
					}
				}
			}
		}
	}

	bool KeyResolvesEverywhere(const FQinglanLocaleData& Locales, const FString& Key)
	{
		for (const TPair<FString, TMap<FString, FString>>& Culture : Locales.CultureKeys)
		{
			if (!Culture.Value.Contains(Key)) { return false; }
		}
		return Locales.CultureKeys.Num() > 0;
	}

	// ---------------------------------------------------------------------------
	// Loc-key reference collection (recursive over the whole story payload)
	// ---------------------------------------------------------------------------

	void CollectLocKeyRefs(const TSharedRef<FJsonValue>& Value, TArray<FString>& OutRefs)
	{
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid()) { return; }
			const TSharedRef<FJsonObject> ObjRef = Obj.ToSharedRef();
			for (const TCHAR* Field : LocKeyRefFields)
			{
				if (ObjRef->HasField(Field) && !ObjRef->GetFieldUntyped(Field)->IsNull())
				{
					OutRefs.Add(ObjRef->GetFieldUntyped(Field)->AsString());
				}
			}
			for (const TCHAR* Field : LocKeyRefListFields)
			{
				for (const TSharedPtr<FJsonValue>& Item : JArr(ObjRef, Field))
				{
					if (Item.IsValid() && !Item->IsNull())
					{
						OutRefs.Add(Item->AsString());
					}
				}
			}
			for (const auto& Pair : ObjRef->Values)
			{
				if (Pair.Value.IsValid())
				{
					CollectLocKeyRefs(Pair.Value.ToSharedRef(), OutRefs);
				}
			}
		}
		else if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (Item.IsValid())
				{
					CollectLocKeyRefs(Item.ToSharedRef(), OutRefs);
				}
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Story validation (task contract: text keys, chapters/scenes, availability
	// flags, choice actions; all with a precise error list)
	// ---------------------------------------------------------------------------

	/** Flag definitions: anything an author can set. Prerequisites/availability must reference these. */
	void BuildFlagUniverse(const TMap<FString, TSharedRef<FJsonObject>>& StoryFiles, TSet<FName>& OutUniverse)
	{
		for (const TPair<FString, TSharedRef<FJsonObject>>& FilePair : StoryFiles)
		{
			const TSharedRef<FJsonObject>& Root = FilePair.Value;
			auto AddSceneFlags = [&OutUniverse](const TSharedRef<FJsonObject>& Scene)
			{
				for (const FName Flag : JNameArray(Scene, TEXT("sets_flags"))) { OutUniverse.Add(Flag); }
				for (const TSharedPtr<FJsonValue>& ChoiceValue : JArr(Scene, TEXT("choices")))
				{
					const TSharedPtr<FJsonObject> Choice = ChoiceValue->AsObject();
					if (!Choice.IsValid()) { continue; }
					const TSharedRef<FJsonObject> ChoiceRef = Choice.ToSharedRef();
					for (const FName Flag : JNameArray(ChoiceRef, TEXT("sets_flags"))) { OutUniverse.Add(Flag); }
					if (const TSharedPtr<FJsonObject> Action = JObj(ChoiceRef, TEXT("action")))
					{
						for (const TSharedPtr<FJsonValue>& FlagValue : JArr(Action.ToSharedRef(), TEXT("sets_only_after_dissolution")))
						{
							if (FlagValue.IsValid() && !FlagValue->IsNull())
							{
								OutUniverse.Add(FName(*FlagValue->AsString()));
							}
						}
					}
				}
			};
			for (const TSharedPtr<FJsonValue>& SceneValue : JArr(Root, TEXT("scenes")))
			{
				if (SceneValue->AsObject().IsValid()) { AddSceneFlags(SceneValue->AsObject().ToSharedRef()); }
			}
			for (const TSharedPtr<FJsonValue>& QuestValue : JArr(Root, TEXT("quests")))
			{
				const TSharedPtr<FJsonObject> Quest = QuestValue->AsObject();
				if (!Quest.IsValid()) { continue; }
				const TSharedRef<FJsonObject> QuestRef = Quest.ToSharedRef();
				for (const FName Flag : JNameArray(QuestRef, TEXT("completion_flags"))) { OutUniverse.Add(Flag); }
				for (const TSharedPtr<FJsonValue>& StageValue : JArr(QuestRef, TEXT("stages")))
				{
					if (StageValue->AsObject().IsValid()) { AddSceneFlags(StageValue->AsObject().ToSharedRef()); }
				}
			}
		}
	}

	bool IsKnownActionType(const FString& Type)
	{
		for (const TCHAR* Known : KnownActionTypes)
		{
			if (Type == Known) { return true; }
		}
		return false;
	}

	const TCHAR* EndingFlagForToken(const FString& Token)
	{
		if (Token == TEXT("END_OPEN")) { return TEXT("ending_open"); }
		if (Token == TEXT("END_SEAL")) { return TEXT("ending_seal"); }
		if (Token == TEXT("END_SHARED")) { return TEXT("ending_shared"); }
		return nullptr;
	}

	/**
	 * Choice action payload well-formedness. Types and required fields per
	 * docs/dev/implementation-blueprint.md section 3 (action payload list) and
	 * section 2.3 (ending rules: preview + warning ack, mutually exclusive,
	 * commit once), plus the SQ02/SQ03 proposal actions from
	 * data/story/side-quests.json (select_proposal / accept_verified_proposal).
	 */
	void ValidateChoiceAction(const FString& Context, const TSharedRef<FJsonObject>& Choice, const TSharedRef<FJsonObject>& Scene,
		const TSet<FName>& FlagUniverse, const TSet<FName>& SceneIds, TArray<FString>& Errors)
	{
		const TSharedPtr<FJsonObject> Action = JObj(Choice, TEXT("action"));
		if (!Action.IsValid())
		{
			return; // plain choices carry only flags
		}
		const TSharedRef<FJsonObject> ActionRef = Action.ToSharedRef();
		const FString Type = JStr(ActionRef, TEXT("type"));
		if (!IsKnownActionType(Type))
		{
			AddError(Errors, Context, FString::Printf(TEXT("unknown action type '%s'"), *Type));
			return;
		}

		if (Type == TEXT("enter_trial"))
		{
			if (JStr(ActionRef, TEXT("requires_progression")).IsEmpty())
			{
				AddError(Errors, Context, TEXT("enter_trial action requires 'requires_progression'"));
			}
		}
		else if (Type == TEXT("release_guardian"))
		{
			if (JStr(ActionRef, TEXT("idempotency_key")).IsEmpty())
			{
				AddError(Errors, Context, TEXT("release_guardian action requires 'idempotency_key'"));
			}
		}
		else if (Type == TEXT("preview_ending"))
		{
			const FString EndingToken = JStr(ActionRef, TEXT("ending_id"));
			if (EndingFlagForToken(EndingToken) == nullptr)
			{
				AddError(Errors, Context, FString::Printf(TEXT("preview_ending has invalid ending_id '%s'"), *EndingToken));
			}
		}
		else if (Type == TEXT("confirm_ending"))
		{
			const FString EndingToken = JStr(ActionRef, TEXT("ending_id"));
			const TCHAR* EndingFlag = EndingFlagForToken(EndingToken);
			if (EndingFlag == nullptr)
			{
				AddError(Errors, Context, FString::Printf(TEXT("confirm_ending has invalid ending_id '%s'"), *EndingToken));
				return;
			}
			if (!JBool(ActionRef, TEXT("requires_warning_acknowledgment"), false))
			{
				AddError(Errors, Context, TEXT("confirm_ending must require warning acknowledgment (blueprint section 2.3)"));
			}
			if (!JBool(ActionRef, TEXT("commit_once"), false))
			{
				AddError(Errors, Context, TEXT("confirm_ending must be commit_once"));
			}
			// The exclusive set is exactly the three ending flags.
			TArray<FName> Exclusive = JNameArray(ActionRef, TEXT("mutually_exclusive_flags"));
			if (Exclusive.Num() != 3)
			{
				AddError(Errors, Context, TEXT("confirm_ending mutually_exclusive_flags must list exactly the three endings"));
			}
			else
			{
				for (const TCHAR* Flag : EndingMutuallyExclusiveFlags)
				{
					if (!Exclusive.Contains(FName(Flag)))
					{
						AddError(Errors, Context, FString::Printf(TEXT("confirm_ending mutually_exclusive_flags missing '%s'"), Flag));
					}
				}
			}
			// The choice must set its own ending flag and the shared ending must
			// additionally carry the three agreement prerequisites.
			if (!JNameArray(Choice, TEXT("sets_flags")).Contains(FName(EndingFlag)))
			{
				AddError(Errors, Context, FString::Printf(TEXT("confirm_ending choice must set flag '%s'"), EndingFlag));
			}
			// requires_preview must point at a preview choice of the SAME ending in the SAME scene.
			const FString RequiresPreview = JStr(ActionRef, TEXT("requires_preview"));
			bool bPreviewFound = false;
			if (RequiresPreview.IsEmpty())
			{
				AddError(Errors, Context, TEXT("confirm_ending requires 'requires_preview'"));
			}
			else
			{
				for (const TSharedPtr<FJsonValue>& OtherValue : JArr(Scene, TEXT("choices")))
				{
					const TSharedPtr<FJsonObject> Other = OtherValue->AsObject();
					if (!Other.IsValid()) { continue; }
					if (JStr(Other.ToSharedRef(), TEXT("id")) != RequiresPreview) { continue; }
					const TSharedPtr<FJsonObject> OtherAction = JObj(Other.ToSharedRef(), TEXT("action"));
					if (OtherAction.IsValid()
						&& JStr(OtherAction.ToSharedRef(), TEXT("type")) == TEXT("preview_ending")
						&& JStr(OtherAction.ToSharedRef(), TEXT("ending_id")) == EndingToken)
					{
						bPreviewFound = true;
					}
				}
				if (!bPreviewFound)
				{
					AddError(Errors, Context,
						FString::Printf(TEXT("confirm_ending requires_preview '%s' does not match a preview_ending('%s') choice in the same scene"),
							*RequiresPreview, *EndingToken));
				}
			}
		}
		else if (Type == TEXT("return_to_hub"))
		{
			const FString ResumeScene = JStr(ActionRef, TEXT("resume_scene_id"));
			if (ResumeScene.IsEmpty() || !SceneIds.Contains(FName(*ResumeScene)))
			{
				AddError(Errors, Context, FString::Printf(TEXT("return_to_hub resume_scene_id '%s' is not a known scene"), *ResumeScene));
			}
			for (const FName Flag : JNameArray(ActionRef, TEXT("preserve_flags")))
			{
				if (!FlagUniverse.Contains(Flag))
				{
					AddError(Errors, Context, FString::Printf(TEXT("return_to_hub preserve_flags references undefined flag '%s'"), *Flag.ToString()));
				}
			}
		}
		else if (Type == TEXT("complete_campaign"))
		{
			if (JStr(ActionRef, TEXT("idempotency_key")).IsEmpty())
			{
				AddError(Errors, Context, TEXT("complete_campaign action requires 'idempotency_key'"));
			}
		}
		else if (Type == TEXT("select_proposal"))
		{
			if (JNameArray(ActionRef, TEXT("replace_choice_flags")).Num() == 0)
			{
				AddError(Errors, Context, TEXT("select_proposal requires non-empty 'replace_choice_flags'"));
			}
		}
		else if (Type == TEXT("accept_verified_proposal"))
		{
			if (!ActionRef->HasField(TEXT("require_matching_delivered_reply")))
			{
				AddError(Errors, Context, TEXT("accept_verified_proposal requires 'require_matching_delivered_reply'"));
			}
		}
	}

	/** AUD-09: identities are unique within an explicit scene/stage owner.
	 * Cross-scene reuse of a local ID is valid; runtime callers must supply ownership.
	 */
	void ValidateSceneMemberIds(const TSharedRef<FJsonObject>& Scene, const FString& Context, TArray<FString>& Errors)
	{
		if (!QinglanStoryIdentity::IsValidComponent(JName(Scene, TEXT("id"))))
		{
			AddError(Errors, Context, TEXT("scene/stage id is empty or contains reserved separator '::'"));
		}
		for (const TCHAR* Field : { TEXT("objectives"), TEXT("choices") })
		{
			TSet<FName> Seen;
			for (const TSharedPtr<FJsonValue>& Value : JArr(Scene, Field))
			{
				const TSharedPtr<FJsonObject> Member = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Member.IsValid())
				{
					AddError(Errors, Context, FString::Printf(TEXT("%s member must be an object"), Field));
					continue;
				}
				const FName Id = JName(Member.ToSharedRef(), TEXT("id"));
				if (!QinglanStoryIdentity::IsValidComponent(Id) || Seen.Contains(Id))
				{
					AddError(Errors, Context, FString::Printf(TEXT("invalid or duplicate %s id '%s' within owner"), Field, *Id.ToString()));
				}
				Seen.Add(Id);
			}
		}
	}

	void ValidateStoryFiles(const TMap<FString, TSharedRef<FJsonObject>>& StoryFiles, const FQinglanLocaleData& Locales,
		TArray<FString>& Errors, int32& OutMainSceneCount, int32& OutSideStageCount, int32& OutRecordCount, int32& OutBarkCount)
	{
		// --- Schema version gate ------------------------------------------------
		for (const TPair<FString, TSharedRef<FJsonObject>>& FilePair : StoryFiles)
		{
			if (static_cast<int32>(JNum(FilePair.Value, TEXT("schema_version"), -1)) != ExpectedSchemaVersion)
			{
				AddError(Errors, FPaths::GetCleanFilename(FilePair.Key),
					FString::Printf(TEXT("schema_version must be %d"), ExpectedSchemaVersion));
			}
		}

		// --- Referenced text keys resolve in BOTH locales ------------------------
		TArray<FString> LocRefs;
		for (const TPair<FString, TSharedRef<FJsonObject>>& FilePair : StoryFiles)
		{
			// UE 5.8: wrap the object in an FJsonValueObject (no implicit TSharedRef<FJsonObject> -> TSharedRef<FJsonValue>).
			CollectLocKeyRefs(MakeShared<FJsonValueObject>(FilePair.Value), LocRefs);
		}
		for (const FString& Key : LocRefs)
		{
			if (!KeyResolvesEverywhere(Locales, Key))
			{
				AddError(Errors, TEXT("story text keys"), FString::Printf(TEXT("key '%s' does not resolve in every locale"), *Key));
			}
		}

		// --- Index scenes/chapters/quests across files ---------------------------
		TSet<FName> SceneIds;          // main scenes
		TSet<FName> ChapterIds;
		TSet<FName> QuestIds;
		TSet<FName> StageIds;
		TMap<FName, FName> SceneToChapter; // main scenes only
		TArray<FName> ChapterSceneRefs;    // (chapter -> scene) pairs flattened twice below

		for (const TPair<FString, TSharedRef<FJsonObject>>& FilePair : StoryFiles)
		{
			const TSharedRef<FJsonObject>& Root = FilePair.Value;
			for (const TSharedPtr<FJsonValue>& ChapterValue : JArr(Root, TEXT("chapters")))
			{
				const TSharedPtr<FJsonObject> Chapter = ChapterValue->AsObject();
				if (!Chapter.IsValid()) { continue; }
				const TSharedRef<FJsonObject> ChapterRef = Chapter.ToSharedRef();
				const FName ChapterId = JName(ChapterRef, TEXT("id"));
				if (ChapterId.IsNone()) { continue; }
				// UE 5.8: TSet::Add returns FSetElementId, not bool; check Contains first.
				if (ChapterIds.Contains(ChapterId))
				{
					AddError(Errors, FPaths::GetCleanFilename(FilePair.Key), FString::Printf(TEXT("duplicate chapter id '%s'"), *ChapterId.ToString()));
				}
				else
				{
					ChapterIds.Add(ChapterId);
				}
				for (const FName SceneId : JNameArray(ChapterRef, TEXT("scene_ids")))
				{
					ChapterSceneRefs.Add(SceneId);
					ChapterSceneRefs.Add(ChapterId); // flattened pair (scene, chapter)
				}
			}
			for (const TSharedPtr<FJsonValue>& SceneValue : JArr(Root, TEXT("scenes")))
			{
				const TSharedPtr<FJsonObject> Scene = SceneValue->AsObject();
				if (!Scene.IsValid()) { continue; }
				const TSharedRef<FJsonObject> SceneRef = Scene.ToSharedRef();
				const FName SceneId = JName(SceneRef, TEXT("id"));
				ValidateSceneMemberIds(SceneRef, FPaths::GetCleanFilename(FilePair.Key) + TEXT("/scene ") + SceneId.ToString(), Errors);
				if (SceneId.IsNone()) { continue; }
				if (SceneIds.Contains(SceneId)) // UE 5.8: TSet::Add returns FSetElementId, not bool
				{
					AddError(Errors, FPaths::GetCleanFilename(FilePair.Key), FString::Printf(TEXT("duplicate scene id '%s'"), *SceneId.ToString()));
				}
				else
				{
					SceneIds.Add(SceneId);
				}
				const FName ChapterId = JName(SceneRef, TEXT("chapter"));
				if (!ChapterId.IsNone())
				{
					SceneToChapter.Add(SceneId, ChapterId);
				}
				++OutMainSceneCount;
			}
			for (const TSharedPtr<FJsonValue>& QuestValue : JArr(Root, TEXT("quests")))
			{
				const TSharedPtr<FJsonObject> Quest = QuestValue->AsObject();
				if (!Quest.IsValid()) { continue; }
				const TSharedRef<FJsonObject> QuestRef = Quest.ToSharedRef();
				const FName QuestId = JName(QuestRef, TEXT("id"));
				if (!QuestId.IsNone())
				{
					if (QuestIds.Contains(QuestId)) // UE 5.8: TSet::Add returns FSetElementId, not bool
					{
						AddError(Errors, FPaths::GetCleanFilename(FilePair.Key), FString::Printf(TEXT("duplicate quest id '%s'"), *QuestId.ToString()));
					}
					else
					{
						QuestIds.Add(QuestId);
					}
				}
				for (const TSharedPtr<FJsonValue>& StageValue : JArr(QuestRef, TEXT("stages")))
				{
					const TSharedPtr<FJsonObject> Stage = StageValue->AsObject();
					if (!Stage.IsValid()) { continue; }
					const FName StageId = JName(Stage.ToSharedRef(), TEXT("id"));
					ValidateSceneMemberIds(Stage.ToSharedRef(), FPaths::GetCleanFilename(FilePair.Key) + TEXT("/stage ") + StageId.ToString(), Errors);
					if (StageId.IsNone()) { continue; }
					if (StageIds.Contains(StageId)) // UE 5.8: TSet::Add returns FSetElementId, not bool
					{
						AddError(Errors, FPaths::GetCleanFilename(FilePair.Key), FString::Printf(TEXT("duplicate stage id '%s'"), *StageId.ToString()));
					}
					else
					{
						StageIds.Add(StageId);
					}
					++OutSideStageCount;
				}
			}
			OutRecordCount += JArr(Root, TEXT("records")).Num();
			OutBarkCount += JArr(Root, TEXT("barks")).Num();
		}
		for (const FName StageId : StageIds)
		{
			if (SceneIds.Contains(StageId))
			{
				AddError(Errors, TEXT("story identities"), FString::Printf(TEXT("main scene and side stage share owner id '%s'"), *StageId.ToString()));
			}
		}

		// --- Chapters reference existing scenes and agree with scene.chapter ----
		for (int32 PairIndex = 0; PairIndex + 1 < ChapterSceneRefs.Num(); PairIndex += 2)
		{
			const FName SceneId = ChapterSceneRefs[PairIndex];
			const FName ChapterId = ChapterSceneRefs[PairIndex + 1];
			if (!SceneIds.Contains(SceneId))
			{
				AddError(Errors, TEXT("chapters"), FString::Printf(TEXT("chapter '%s' references unknown scene '%s'"), *ChapterId.ToString(), *SceneId.ToString()));
			}
			const FName* DeclaredChapter = SceneToChapter.Find(SceneId);
			if (DeclaredChapter != nullptr && *DeclaredChapter != ChapterId)
			{
				AddError(Errors, TEXT("chapters"), FString::Printf(TEXT("scene '%s' declares chapter '%s' but is listed under '%s'"),
					*SceneId.ToString(), *DeclaredChapter->ToString(), *ChapterId.ToString()));
			}
		}
		for (const TPair<FName, FName>& Pair : SceneToChapter)
		{
			if (!ChapterIds.Contains(Pair.Value))
			{
				AddError(Errors, TEXT("chapters"), FString::Printf(TEXT("scene '%s' references unknown chapter '%s'"), *Pair.Key.ToString(), *Pair.Value.ToString()));
			}
		}

		// --- Flag universe: prerequisites + availability + next-scene graph -------
		TSet<FName> FlagUniverse;
		BuildFlagUniverse(StoryFiles, FlagUniverse);
		for (const TPair<FString, TSharedRef<FJsonObject>>& FilePair : StoryFiles)
		{
			const TSharedRef<FJsonObject>& Root = FilePair.Value;
			auto ValidatePrereqs = [&Errors, &FlagUniverse, &FilePair](const FString& Owner, const TArray<FName>& Prerequisites)
			{
				for (const FName Flag : Prerequisites)
				{
					if (!FlagUniverse.Contains(Flag))
					{
						AddError(Errors, TEXT("flags"), FString::Printf(TEXT("%s: prerequisite flag '%s' is never defined"), *Owner, *Flag.ToString()));
					}
				}
			};
			for (const TSharedPtr<FJsonValue>& SceneValue : JArr(Root, TEXT("scenes")))
			{
				const TSharedPtr<FJsonObject> Scene = SceneValue->AsObject();
				if (!Scene.IsValid()) { continue; }
				const TSharedRef<FJsonObject> SceneRef = Scene.ToSharedRef();
				const FString SceneContext = FString::Printf(TEXT("%s/scene %s"), *FPaths::GetCleanFilename(FilePair.Key), *JStr(SceneRef, TEXT("id")));
				ValidatePrereqs(SceneContext, JNameArray(SceneRef, TEXT("prerequisites")));
				for (const FName NextSceneId : JNameArray(SceneRef, TEXT("next_scene_ids")))
				{
					if (!SceneIds.Contains(NextSceneId))
					{
						AddError(Errors, SceneContext, FString::Printf(TEXT("next_scene_ids references unknown scene '%s'"), *NextSceneId.ToString()));
					}
				}
				for (const TSharedPtr<FJsonValue>& ChoiceValue : JArr(SceneRef, TEXT("choices")))
				{
					const TSharedPtr<FJsonObject> Choice = ChoiceValue->AsObject();
					if (!Choice.IsValid()) { continue; }
					const TSharedRef<FJsonObject> ChoiceRef = Choice.ToSharedRef();
					ValidatePrereqs(SceneContext, JNameArray(ChoiceRef, TEXT("prerequisites")));
					ValidateChoiceAction(SceneContext, ChoiceRef, SceneRef, FlagUniverse, SceneIds, Errors);
				}
			}
			for (const TSharedPtr<FJsonValue>& QuestValue : JArr(Root, TEXT("quests")))
			{
				const TSharedPtr<FJsonObject> Quest = QuestValue->AsObject();
				if (!Quest.IsValid()) { continue; }
				const TSharedRef<FJsonObject> QuestRef = Quest.ToSharedRef();
				const FString QuestContext = FString::Printf(TEXT("%s/quest %s"), *FPaths::GetCleanFilename(FilePair.Key), *JStr(QuestRef, TEXT("id")));
				for (const FName Flag : JNameArray(QuestRef, TEXT("availability")))
				{
					if (!FlagUniverse.Contains(Flag))
					{
						AddError(Errors, QuestContext, FString::Printf(TEXT("availability flag '%s' is never defined"), *Flag.ToString()));
					}
				}
				for (const TSharedPtr<FJsonValue>& StageValue : JArr(QuestRef, TEXT("stages")))
				{
					const TSharedPtr<FJsonObject> Stage = StageValue->AsObject();
					if (!Stage.IsValid()) { continue; }
					const TSharedRef<FJsonObject> StageRef = Stage.ToSharedRef();
					const FString StageContext = QuestContext + TEXT("/") + JStr(StageRef, TEXT("id"));
					ValidatePrereqs(StageContext, JNameArray(StageRef, TEXT("prerequisites")));
					for (const TSharedPtr<FJsonValue>& ChoiceValue : JArr(StageRef, TEXT("choices")))
					{
						const TSharedPtr<FJsonObject> Choice = ChoiceValue->AsObject();
						if (!Choice.IsValid()) { continue; }
						const TSharedRef<FJsonObject> ChoiceRef = Choice.ToSharedRef();
						ValidatePrereqs(StageContext, JNameArray(ChoiceRef, TEXT("prerequisites")));
						ValidateChoiceAction(StageContext, ChoiceRef, StageRef, FlagUniverse, SceneIds, Errors);
					}
					// SQ02/SQ03 delivery contract: zero real-time wait (blueprint section 1.9).
					if (const TSharedPtr<FJsonObject> Transition = JObj(StageRef, TEXT("delivery_transition")))
					{
						if (JNum(Transition.ToSharedRef(), TEXT("real_time_wait_seconds"), -1.0) != 0.0)
						{
							AddError(Errors, StageContext, TEXT("delivery_transition must have real_time_wait_seconds == 0"));
						}
					}
				}
			}
		}

		// --- Content head counts (warnings only; docs blueprint section 1.9) ----
		if (OutMainSceneCount != ExpectedMainSceneCount)
		{
			UE_LOG(LogQinglanEditor, Warning, TEXT("main scene count %d differs from the shipped contract %d (blueprint section 1.9)"),
				OutMainSceneCount, ExpectedMainSceneCount);
		}
		if (OutSideStageCount != ExpectedSideStageCount)
		{
			UE_LOG(LogQinglanEditor, Warning, TEXT("side stage count %d differs from the shipped contract %d (blueprint section 1.9)"),
				OutSideStageCount, ExpectedSideStageCount);
		}
		if (OutRecordCount != ExpectedRecordCount)
		{
			UE_LOG(LogQinglanEditor, Warning, TEXT("record count %d differs from the shipped contract %d (blueprint section 1.9)"),
				OutRecordCount, ExpectedRecordCount);
		}
		if (OutBarkCount != ExpectedBarkCount)
		{
			UE_LOG(LogQinglanEditor, Warning, TEXT("bark count %d differs from the shipped contract %d (blueprint section 1.9)"),
				OutBarkCount, ExpectedBarkCount);
		}
	}

	// ---------------------------------------------------------------------------
	// Technique / realm validation
	// ---------------------------------------------------------------------------

	void ValidateTechniqueFile(const FString& FilePath, const TSharedRef<FJsonObject>& Root, const FQinglanLocaleData& Locales,
		TArray<FString>& Errors, int32& OutTechniqueCount)
	{
		const FString Context = FPaths::GetCleanFilename(FilePath);
		if (static_cast<int32>(JNum(Root, TEXT("schema_version"), -1)) != ExpectedSchemaVersion)
		{
			AddError(Errors, Context, FString::Printf(TEXT("schema_version must be %d"), ExpectedSchemaVersion));
		}
		TArray<FName> SeenIds;
		for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("techniques")))
		{
			const TSharedPtr<FJsonObject> Tech = Value->AsObject();
			if (!Tech.IsValid()) { continue; }
			const TSharedRef<FJsonObject> TechRef = Tech.ToSharedRef();
			const FString Id = JStr(TechRef, TEXT("id"));
			const FString Entry = FString::Printf(TEXT("%s/%s"), *Context, *Id);
			if (Id.IsEmpty()) { AddError(Errors, Context, TEXT("technique without id")); continue; }
			if (SeenIds.Contains(FName(*Id))) { AddError(Errors, Context, FString::Printf(TEXT("duplicate technique id '%s'"), *Id)); }
			SeenIds.Add(FName(*Id));
			++OutTechniqueCount;

			EQinglanElement Element = EQinglanElement::None;
			if (!ParseElement(JStr(TechRef, TEXT("element")), Element))
			{
				AddError(Errors, Entry, FString::Printf(TEXT("unknown element '%s'"), *JStr(TechRef, TEXT("element"))));
			}
			if (!KeyResolvesEverywhere(Locales, JStr(TechRef, TEXT("name_key"))))
			{
				AddError(Errors, Entry, FString::Printf(TEXT("name_key '%s' does not resolve in every locale"), *JStr(TechRef, TEXT("name_key"))));
			}
			// Numbers must match the shipped table (docs/dev/implementation-blueprint.md section 2.5).
			const FExpectedTechnique* Expected = nullptr;
			for (const FExpectedTechnique& Candidate : ExpectedTechniques)
			{
				if (Id == Candidate.Id) { Expected = &Candidate; break; }
			}
			if (Expected == nullptr)
			{
				AddError(Errors, Entry, TEXT("id is not one of the eight shipped techniques (blueprint section 2.5)"));
				continue;
			}
			const double Energy = JNum(TechRef, TEXT("energy_cost"), -1);
			const double Cooldown = JNum(TechRef, TEXT("cooldown_seconds"), -1);
			const double Coefficient = JNum(TechRef, TEXT("power_coefficient"), -12345.0);
			const double Flat = JNum(TechRef, TEXT("flat_damage"), -1);
			if (static_cast<int32>(Energy) != Expected->EnergyCost)
			{
				AddError(Errors, Entry, FString::Printf(TEXT("energy_cost %d does not match blueprint section 2.5 (%d)"), static_cast<int32>(Energy), Expected->EnergyCost));
			}
			if (!FMath::IsNearlyEqual(static_cast<float>(Cooldown), Expected->CooldownSeconds, 1e-4f))
			{
				AddError(Errors, Entry, FString::Printf(TEXT("cooldown_seconds %.3f does not match blueprint section 2.5 (%.3f)"), Cooldown, Expected->CooldownSeconds));
			}
			if (!FMath::IsNearlyEqual(static_cast<float>(Coefficient), Expected->PowerCoefficient, 1e-4f))
			{
				AddError(Errors, Entry, FString::Printf(TEXT("power_coefficient %.3f does not match blueprint section 2.5 (%.3f)"), Coefficient, Expected->PowerCoefficient));
			}
			if (static_cast<int32>(Flat) != Expected->FlatDamage)
			{
				AddError(Errors, Entry, FString::Printf(TEXT("flat_damage %d does not match blueprint section 2.5 (%d)"), static_cast<int32>(Flat), Expected->FlatDamage));
			}
		}
		// The file must carry exactly the eight shipped techniques (blueprint sections 2.5 / 1.5).
		if (SeenIds.Num() != 8)
		{
			AddError(Errors, Context, FString::Printf(TEXT("expected exactly 8 techniques (blueprint section 2.5), found %d"), SeenIds.Num()));
		}
		for (const TCHAR* TechniqueText : QinglanProgression::TechniqueIdTexts)
		{
			if (!SeenIds.Contains(FName(TechniqueText)))
			{
				AddError(Errors, Context, FString::Printf(TEXT("shipped technique '%s' missing (blueprint section 2.5)"), TechniqueText));
			}
		}
	}

	void ValidateRealmFile(const FString& FilePath, const TSharedRef<FJsonObject>& Root, const FQinglanLocaleData& Locales, TArray<FString>& Errors)
	{
		const FString Context = FPaths::GetCleanFilename(FilePath);
		if (static_cast<int32>(JNum(Root, TEXT("schema_version"), -1)) != ExpectedSchemaVersion)
		{
			AddError(Errors, Context, FString::Printf(TEXT("schema_version must be %d"), ExpectedSchemaVersion));
		}
		const FString RealmId = JStr(Root, TEXT("id"));
		const FString Entry = FString::Printf(TEXT("%s/%s"), *Context, *RealmId);
		if (RealmId.IsEmpty())
		{
			AddError(Errors, Context, TEXT("realm without id"));
			return;
		}
		if (!KeyResolvesEverywhere(Locales, JStr(Root, TEXT("name_key"))))
		{
			AddError(Errors, Entry, FString::Printf(TEXT("name_key '%s' does not resolve in every locale"), *JStr(Root, TEXT("name_key"))));
		}

		const TArray<TSharedPtr<FJsonValue>> Rooms = JArr(Root, TEXT("rooms"));
		if (Rooms.Num() != RealmRoomContractCount) // docs/dev/implementation-blueprint.md section 1.4 "7-room contract"
		{
			AddError(Errors, Entry, FString::Printf(TEXT("expected %d rooms, found %d"), RealmRoomContractCount, Rooms.Num()));
		}
		int64 ShardSum = 0;
		bool bSawBoss = false;
		for (int32 RoomIndex = 0; RoomIndex < Rooms.Num(); ++RoomIndex)
		{
			const TSharedPtr<FJsonObject> Room = Rooms[RoomIndex]->AsObject();
			if (!Room.IsValid()) { continue; }
			const TSharedRef<FJsonObject> RoomRef = Room.ToSharedRef();
			const FString RoomContext = FString::Printf(TEXT("%s/room %d"), *Entry, RoomIndex);
			if (static_cast<int32>(JNum(RoomRef, TEXT("index"), -1)) != RoomIndex)
			{
				AddError(Errors, RoomContext, TEXT("room index must be contiguous from 0"));
			}
			EQinglanRoomKind Kind = EQinglanRoomKind::Entrance;
			const FString KindText = JStr(RoomRef, TEXT("kind"));
			if (!ParseRoomKind(KindText, Kind))
			{
				AddError(Errors, RoomContext, FString::Printf(TEXT("unknown room kind '%s'"), *KindText));
			}
			bSawBoss |= (Kind == EQinglanRoomKind::Boss);
			const int64 Shards = static_cast<int64>(JNum(RoomRef, TEXT("spirit_shards"), -1));
			if (Shards < 0)
			{
				AddError(Errors, RoomContext, TEXT("spirit_shards must be non-negative"));
			}
			ShardSum += FMath::Max<int64>(Shards, 0);
		}
		if (Rooms.Num() > 0)
		{
			const TSharedPtr<FJsonObject> First = Rooms[0]->AsObject();
			if (First.IsValid() && JStr(First.ToSharedRef(), TEXT("kind")) != TEXT("entrance"))
			{
				AddError(Errors, Entry, TEXT("first room must be 'entrance' (blueprint section 1.4)"));
			}
			const TSharedPtr<FJsonObject> Last = Rooms.Last()->AsObject();
			if (Last.IsValid() && JStr(Last.ToSharedRef(), TEXT("kind")) != TEXT("exit"))
			{
				AddError(Errors, Entry, TEXT("last room must be 'exit' (blueprint section 1.4)"));
			}
		}
		if (!bSawBoss)
		{
			AddError(Errors, Entry, TEXT("realm must contain a boss room (blueprint section 1.4)"));
		}

		// Payout parity for the two shipped realms (docs/03 section 2 table).
		if (RealmId == BambooRealmId)
		{
			if (ShardSum != BambooRoomShardSum)
			{
				AddError(Errors, Entry, FString::Printf(TEXT("room shard sum %lld does not equal the 180 base payout (docs/03 section 2)"), static_cast<long long>(ShardSum)));
			}
			const TSharedPtr<FJsonObject> Bonus = JObj(Root, TEXT("first_clear_bonus"));
			if (!Bonus.IsValid()
				|| static_cast<int64>(JNum(Bonus.ToSharedRef(), TEXT("spirit_shards"))) != BambooFirstClearShards
				|| static_cast<int64>(JNum(Bonus.ToSharedRef(), TEXT("spirit_herb"))) != BambooFirstClearHerbs
				|| static_cast<int64>(JNum(Bonus.ToSharedRef(), TEXT("vein_crystal"))) != BambooFirstClearCrystals)
			{
				AddError(Errors, Entry, TEXT("first_clear_bonus must be 120 shards + 3 herbs + 1 crystal (docs/03 section 2)"));
			}
		}
		else if (RealmId == AshenRealmId)
		{
			if (ShardSum != AshenRoomShardSum)
			{
				AddError(Errors, Entry, FString::Printf(TEXT("room shard sum %lld does not equal the 300 base payout (docs/03 section 2)"), static_cast<long long>(ShardSum)));
			}
			const TSharedPtr<FJsonObject> Bonus = JObj(Root, TEXT("first_clear_bonus"));
			if (!Bonus.IsValid()
				|| static_cast<int64>(JNum(Bonus.ToSharedRef(), TEXT("spirit_shards"))) != AshenFirstClearShards
				|| static_cast<int64>(JNum(Bonus.ToSharedRef(), TEXT("vein_crystal"))) != AshenFirstClearCrystals)
			{
				AddError(Errors, Entry, TEXT("first_clear_bonus must be 200 shards + 3 crystals (docs/03 section 2)"));
			}
		}
	}

	// ---------------------------------------------------------------------------
	// Asset plumbing (reflection-based creation; typed fill)
	// ---------------------------------------------------------------------------

	UClass* FindQinglanScriptClass(const TCHAR* ShortName)
	{
		const FString Path = FString::Printf(TEXT("/Script/QinglanAscension.%s"), ShortName);
		UClass* Class = FindObject<UClass>(nullptr, *Path);
		if (Class == nullptr)
		{
			Class = LoadObject<UClass>(nullptr, *Path);
		}
		return Class;
	}

	/**
	 * Creates (or fully resets) the asset at PackagePath. The importer always
	 * regenerates from data, so a pre-existing asset is renamed out of the
	 * package: stale keys/rows never survive an import.
	 */
	UObject* CreateFreshAsset(const FString& PackagePath, UClass* Class, TArray<FString>& Errors)
	{
		if (Class == nullptr)
		{
			AddError(Errors, TEXT("import"), FString::Printf(TEXT("class for '%s' could not be resolved"), *PackagePath));
			return nullptr;
		}
		// Packages touched by asset-registry discovery can be half-open
		// (!IsFullyLoaded) and SavePackage refuses those; force a full load of
		// anything that already exists before reusing it.
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (Package == nullptr)
		{
			Package = CreatePackage(*PackagePath);
		}
		else if (!Package->IsFullyLoaded())
		{
			FlushAsyncLoading();
			Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
		}
		if (Package == nullptr)
		{
			AddError(Errors, TEXT("import"), FString::Printf(TEXT("package '%s' could not be created"), *PackagePath));
			return nullptr;
		}
		const FString AssetName = FPaths::GetBaseFilename(PackagePath);
		if (UObject* Existing = FindObject<UObject>(Package, *AssetName))
		{
			Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}
		UObject* Asset = NewObject<UObject>(Package, Class, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		return Asset;
	}

	bool SaveImportedAsset(UObject* Asset, TArray<FString>& Errors)
	{
		if (Asset == nullptr) { return false; }
		UPackage* Package = Cast<UPackage>(Asset->GetOuter());
		if (Package == nullptr) { return false; }
		// Same guard as the content bootstrap: half-open discovery packages
		// refuse to save; FullyLoad() closes them.
		Package->FullyLoad();
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		// UE 5.8 SavePackage API: the legacy uint32-flags overload is gone; the
		// args struct keeps defaults (SAVE_None, error output to GError).
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone | RF_Transactional;
		if (!UPackage::SavePackage(Package, Asset, *FileName, SaveArgs))
		{
			AddError(Errors, TEXT("save"), FString::Printf(TEXT("failed to save '%s'"), *Package->GetName()));
			return false;
		}
		Package->SetDirtyFlag(false);
		UE_LOG(LogQinglanEditor, Display, TEXT("Saved %s"), *Package->GetName());
		return true;
	}

	// ---------------------------------------------------------------------------
	// Fill helpers (run only after validation passed; defaults for absent fields)
	// ---------------------------------------------------------------------------

	void FillLine(const TSharedRef<FJsonObject>& Json, FQinglanRegistryLine& Out)
	{
		Out.Speaker = JName(Json, TEXT("speaker"));
		Out.TextKey = JName(Json, TEXT("text_key"));
		Out.SpeakerKey = JName(Json, TEXT("speaker_key"));
		EQinglanSourceKind SourceKind = EQinglanSourceKind::Live;
		ParseSourceKind(JStr(Json, TEXT("source_kind"), TEXT("live")), SourceKind);
		Out.SourceKind = SourceKind;
		Out.SourceKindKey = JName(Json, TEXT("source_kind_key"));
		Out.Event = JName(Json, TEXT("event"));
		Out.Trigger = JName(Json, TEXT("trigger"));
		Out.WhenAll = JNameArray(Json, TEXT("when_all"));
		Out.WhenNone = JNameArray(Json, TEXT("when_none"));
	}

	void FillChoice(const TSharedRef<FJsonObject>& Json, FQinglanRegistryChoice& Out)
	{
		Out.ChoiceId = JName(Json, TEXT("id"));
		Out.LabelKey = JName(Json, TEXT("label_key"));
		Out.ResponseKeys = JNameArray(Json, TEXT("response_keys"));
		Out.SetsFlags = JNameArray(Json, TEXT("sets_flags"));
		if (const TSharedPtr<FJsonObject> Action = JObj(Json, TEXT("action")))
		{
			Out.Action = JName(Action.ToSharedRef(), TEXT("type"));
		}
		Out.Prerequisites = JNameArray(Json, TEXT("prerequisites"));
		Out.PresentationGroup = JName(Json, TEXT("presentation_group"));
		Out.ShowAfterTextKey = JName(Json, TEXT("show_after_text_key"));
	}

	/**
	 * Parses one `all_of` term (AUD-11). Unknown kinds are skipped with a
	 * Warning instead of failing the whole import: the data validator owns the
	 * grammar, so an unrecognized kind is a forward-compatibility signal, not a
	 * reason to drop the scene.
	 */
	bool ParseGuardTerm(const TSharedRef<FJsonObject>& Json, FQinglanRegistryGuardTerm& Out)
	{
		Out.Kind = JName(Json, TEXT("kind"));
		if (Out.Kind.IsNone())
		{
			UE_LOG(LogQinglanEditor, Warning, TEXT("Guard term without a kind skipped"));
			return false;
		}
		if (Out.Kind == FName(TEXT("event")))
		{
			Out.Event = JName(Json, TEXT("event"));
			return !Out.Event.IsNone();
		}
		if (Out.Kind == FName(TEXT("event_count")))
		{
			Out.Event = JName(Json, TEXT("event"));
			Out.Count = FMath::Max(1, static_cast<int32>(JNum(Json, TEXT("count"), 1.0)));
			return !Out.Event.IsNone();
		}
		if (Out.Kind == FName(TEXT("ordered")))
		{
			Out.OrderedSteps = JNameArray(Json, TEXT("steps"));
			return Out.OrderedSteps.Num() > 0;
		}
		if (Out.Kind == FName(TEXT("delivery")))
		{
			Out.Quest = JName(Json, TEXT("quest"));
			return !Out.Quest.IsNone();
		}
		if (Out.Kind == FName(TEXT("time_advance")))
		{
			Out.GameHours = JNum(Json, TEXT("game_hours"));
			return Out.GameHours > 0.0;
		}
		if (Out.Kind == FName(TEXT("flag")))
		{
			Out.Flag = JName(Json, TEXT("flag"));
			return !Out.Flag.IsNone();
		}
		UE_LOG(LogQinglanEditor, Warning, TEXT("Unknown guard term kind '%s' skipped"), *Out.Kind.ToString());
		return false;
	}

	/** Fills a conjunction guard from a JSON object carrying `all_of`. */
	void FillGuard(const TSharedRef<FJsonObject>& Json, FQinglanRegistryGuard& Out)
	{
		Out.AllOf.Reset();
		for (const TSharedPtr<FJsonValue>& Value : JArr(Json, TEXT("all_of")))
		{
			if (!Value.IsValid() || Value->IsNull() || !Value->AsObject().IsValid())
			{
				continue;
			}
			FQinglanRegistryGuardTerm Term;
			if (ParseGuardTerm(Value->AsObject().ToSharedRef(), Term))
			{
				Out.AllOf.Add(Term);
			}
		}
	}

	void FillObjective(const TSharedRef<FJsonObject>& Json, FQinglanRegistryObjective& Out)
	{
		Out.ObjectiveId = JName(Json, TEXT("id"));
		Out.TextKey = JName(Json, TEXT("text_key"));
		Out.Completion = JStr(Json, TEXT("completion"));
		if (const TSharedPtr<FJsonObject> Proof = JObj(Json, TEXT("proof")))
		{
			FillGuard(Proof.ToSharedRef(), Out.Guard);
		}
	}

	void FillScene(const TSharedRef<FJsonObject>& Json, FQinglanRegistryScene& Out)
	{
		Out.SceneId = JName(Json, TEXT("id"));
		Out.ChapterId = JName(Json, TEXT("chapter"));
		Out.Location = JName(Json, TEXT("location"));
		Out.TitleKey = JName(Json, TEXT("title_key"));
		Out.Prerequisites = JNameArray(Json, TEXT("prerequisites"));
		for (const TSharedPtr<FJsonValue>& Value : JArr(Json, TEXT("script")))
		{
			if (Value->AsObject().IsValid())
			{
				FQinglanRegistryLine& Line = Out.Script.AddDefaulted_GetRef();
				FillLine(Value->AsObject().ToSharedRef(), Line);
			}
		}
		for (const TSharedPtr<FJsonValue>& Value : JArr(Json, TEXT("choices")))
		{
			if (Value->AsObject().IsValid())
			{
				FQinglanRegistryChoice& Choice = Out.Choices.AddDefaulted_GetRef();
				FillChoice(Value->AsObject().ToSharedRef(), Choice);
			}
		}
		for (const TSharedPtr<FJsonValue>& Value : JArr(Json, TEXT("objectives")))
		{
			if (Value->AsObject().IsValid())
			{
				FQinglanRegistryObjective& Objective = Out.Objectives.AddDefaulted_GetRef();
				FillObjective(Value->AsObject().ToSharedRef(), Objective);
			}
		}
		Out.SetsFlags = JNameArray(Json, TEXT("sets_flags"));
		Out.NextSceneIds = JNameArray(Json, TEXT("next_scene_ids"));
		Out.RetryKey = JName(Json, TEXT("retry_key"));
		// The scaffold field is a FName, but the JSON payload is either the
		// older English QA prose or the structured {"all_of":[...]} guard
		// (AUD-11). Presence of either form yields the stable derived token;
		// the runtime evaluator reads the structured form from the registry.
		Out.ConfirmationGuard = Json->HasField(TEXT("confirmation_guard"))
			? FName(*FString::Printf(TEXT("guard.%s"), *Out.SceneId.ToString()))
			: NAME_None;
		// Structured guard form (AUD-10/AUD-11). The legacy prose form is not
		// an object, so JObj returns null and the token above stays the only
		// trace; only the structured form gains evaluable terms.
		if (const TSharedPtr<FJsonObject> ConfirmationGuard = JObj(Json, TEXT("confirmation_guard")))
		{
			FillGuard(ConfirmationGuard.ToSharedRef(), Out.ConfirmationGuardTerms);
		}
		Out.ScriptTrigger = JName(Json, TEXT("script_trigger"));
		Out.JournalKey = JName(Json, TEXT("journal_key"));
		Out.AdvanceDeliveryKey = JName(Json, TEXT("advance_delivery_key"));
		Out.DeliveryTimeKey = JName(Json, TEXT("delivery_time_key"));
		if (const TSharedPtr<FJsonObject> Transition = JObj(Json, TEXT("delivery_transition")))
		{
			Out.DeliveryTransition = JName(Transition.ToSharedRef(), TEXT("type"));
		}
		// CH07_S04 scene-extras (optional_epilogue_*): only the epilogue-owning
		// scene carries these; every other scene imports empty arrays.
		Out.OptionalEpilogueQuestIds = JNameArray(Json, TEXT("optional_epilogue_quest_ids"));
		Out.OptionalEpilogueSource = JName(Json, TEXT("optional_epilogue_source"));
	}

	void FillChapter(const TSharedRef<FJsonObject>& Json, FQinglanRegistryChapter& Out)
	{
		Out.ChapterId = JName(Json, TEXT("id"));
		Out.TitleKey = JName(Json, TEXT("title_key"));
		Out.SummaryKey = JName(Json, TEXT("summary_key"));
		Out.SceneIds = JNameArray(Json, TEXT("scene_ids"));
	}

	void FillQuest(const TSharedRef<FJsonObject>& Json, FQinglanRegistrySideQuest& Out)
	{
		Out.QuestId = JName(Json, TEXT("id"));
		Out.TitleKey = JName(Json, TEXT("title_key"));
		Out.SummaryKey = JName(Json, TEXT("summary_key"));
		Out.Availability = JNameArray(Json, TEXT("availability"));
		for (const TSharedPtr<FJsonValue>& Value : JArr(Json, TEXT("stages")))
		{
			if (Value->AsObject().IsValid())
			{
				FQinglanRegistryScene& Stage = Out.Stages.AddDefaulted_GetRef();
				FillScene(Value->AsObject().ToSharedRef(), Stage);
			}
		}
		Out.CompletionFlags = JNameArray(Json, TEXT("completion_flags"));
		Out.EpilogueKeys = JNameArray(Json, TEXT("epilogue_keys"));
		// rewards.technique_access is a scalar id in the current contract (SQ04
		// mist_step, SQ05 orbiting_blades); the array member keeps any future
		// multi-grant data-driven without a schema change.
		if (const TSharedPtr<FJsonObject> Rewards = JObj(Json, TEXT("rewards")))
		{
			const FName TechniqueAccess = JName(Rewards.ToSharedRef(), TEXT("technique_access"));
			if (!TechniqueAccess.IsNone())
			{
				Out.RewardTechniqueIds.Add(TechniqueAccess);
			}
		}
	}

	void FillRecord(const TSharedRef<FJsonObject>& Json, FQinglanRegistryRecord& Out)
	{
		Out.RecordId = JName(Json, TEXT("id"));
		Out.Location = JName(Json, TEXT("location"));
		Out.RequiredFlags = JNameArray(Json, TEXT("required_flags"));
		Out.DateLabelKey = JName(Json, TEXT("date_key"));
		Out.TitleKey = JName(Json, TEXT("title_key"));
		Out.BodyKey = JName(Json, TEXT("body_key"));
		Out.CollectionFlag = JName(Json, TEXT("collection_flag"));
	}

	void FillBark(const TSharedRef<FJsonObject>& Json, FQinglanRegistryBark& Out)
	{
		Out.BarkId = JName(Json, TEXT("id"));
		Out.Speaker = JName(Json, TEXT("speaker"));
		Out.Location = JName(Json, TEXT("location"));
		Out.RequiredFlags = JNameArray(Json, TEXT("required_flags"));
		Out.ForbiddenFlags = JNameArray(Json, TEXT("forbidden_flags"));
		Out.TextKey = JName(Json, TEXT("text_key"));
		Out.CooldownSeconds = static_cast<float>(JNum(Json, TEXT("cooldown_seconds"), 180.0)); // docs blueprint section 2.6
		Out.MaxPerVisit = static_cast<int32>(JNum(Json, TEXT("max_per_visit"), 1.0));
		Out.bSuppressDuringCombat = JBool(Json, TEXT("suppress_during_combat"), true);
	}

	bool ImportStoryRegistry(const TMap<FString, TSharedRef<FJsonObject>>& StoryFiles, TArray<FString>& Errors)
	{
		UObject* Raw = CreateFreshAsset(StoryRegistryAssetPath, FindQinglanScriptClass(TEXT("QinglanStoryRegistry")), Errors);
		if (Raw == nullptr) { return false; }
		UQinglanStoryRegistry* Registry = static_cast<UQinglanStoryRegistry*>(Raw);

		FString RevisionInput; // deterministic content revision (docs blueprint section 1.9 revision gate)
		TArray<FString> SortedFiles;
		StoryFiles.GetKeys(SortedFiles); // UE 5.8: TMap has GetKeys(), not a Keys member
		SortedFiles.Sort();
		for (const FString& FilePath : SortedFiles)
		{
			const TSharedRef<FJsonObject>& Root = StoryFiles.FindChecked(FilePath);
			const FString Context = FPaths::GetCleanFilename(FilePath);
			for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("chapters")))
			{
				if (Value->AsObject().IsValid())
				{
					FQinglanRegistryChapter& Chapter = Registry->Chapters.AddDefaulted_GetRef();
					FillChapter(Value->AsObject().ToSharedRef(), Chapter);
				}
			}
			for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("scenes")))
			{
				if (Value->AsObject().IsValid())
				{
					FQinglanRegistryScene& Scene = Registry->MainScenes.AddDefaulted_GetRef();
					FillScene(Value->AsObject().ToSharedRef(), Scene);
				}
			}
			for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("quests")))
			{
				if (Value->AsObject().IsValid())
				{
					FQinglanRegistrySideQuest& Quest = Registry->SideQuests.AddDefaulted_GetRef();
					FillQuest(Value->AsObject().ToSharedRef(), Quest);
				}
			}
			for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("records")))
			{
				if (Value->AsObject().IsValid())
				{
					FQinglanRegistryRecord& Record = Registry->Records.AddDefaulted_GetRef();
					FillRecord(Value->AsObject().ToSharedRef(), Record);
				}
			}
			for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("barks")))
			{
				if (Value->AsObject().IsValid())
				{
					FQinglanRegistryBark& Bark = Registry->Barks.AddDefaulted_GetRef();
					FillBark(Value->AsObject().ToSharedRef(), Bark);
				}
			}
			RevisionInput += Context + TEXT(":");
			RevisionInput += FString::FromInt(JArr(Root, TEXT("scenes")).Num()) + TEXT(",");
			RevisionInput += FString::FromInt(JArr(Root, TEXT("quests")).Num()) + TEXT(";");
		}
		Registry->NarrativeRevision = FName(*FString::Printf(TEXT("nar-%08x"), static_cast<uint32>(HashFnv1a64(RevisionInput))));
		return SaveImportedAsset(Raw, Errors);
	}

	bool ImportTechnique(const TSharedRef<FJsonObject>& Tech, TArray<FString>& Errors)
	{
		const FString Id = JStr(Tech, TEXT("id"));
		// One technique asset per id. Task contract; ids mirror docs blueprint section 2.5.
		// UE 5.8: FString::Printf needs a literal format string (TCheckedFormatString), so the format is inlined here.
		const FString PackagePath = FString::Printf(TEXT("/Game/Qinglan/Data/Techniques/DA_Tech_%s"), *Id);
		UObject* Raw = CreateFreshAsset(PackagePath, FindQinglanScriptClass(TEXT("QinglanTechniqueDataAsset")), Errors);
		if (Raw == nullptr) { return false; }
		UQinglanTechniqueDataAsset* Asset = static_cast<UQinglanTechniqueDataAsset*>(Raw);

		Asset->TechniqueId = FName(*Id);
		Asset->NameTextKey = JName(Tech, TEXT("name_key"));
		ParseElement(JStr(Tech, TEXT("element")), Asset->Element);
		Asset->EnergyCost = static_cast<int32>(JNum(Tech, TEXT("energy_cost")));
		Asset->CooldownSeconds = static_cast<float>(JNum(Tech, TEXT("cooldown_seconds")));
		Asset->PowerCoefficient = static_cast<float>(JNum(Tech, TEXT("power_coefficient")));
		Asset->FlatDamage = static_cast<int32>(JNum(Tech, TEXT("flat_damage")));
		// Blueprint-only columns (docs/dev/implementation-blueprint.md section 2.5).
		for (const FExpectedTechnique& Expected : ExpectedTechniques)
		{
			if (Id != Expected.Id) { continue; }
			EQinglanTargeting Targeting = EQinglanTargeting::Projectile;
			ParseTargeting(Expected.Targeting, Targeting);
			Asset->Targeting = Targeting;
			Asset->Magnitude = Expected.Magnitude;
			Asset->Range = Expected.Range;
			Asset->UnlockFlag = Expected.UnlockFlag != nullptr ? FName(Expected.UnlockFlag) : NAME_None;
		}
		// Phases stay at their scaffold defaults; animation/cue soft references
		// are authored in the editor on top of the imported numbers.
		return SaveImportedAsset(Raw, Errors);
	}

	bool ImportRealm(const TSharedRef<FJsonObject>& Root, TArray<FString>& Errors)
	{
		const FString Id = JStr(Root, TEXT("id"));
		// One realm asset per realm id. Task contract; realm ids mirror docs blueprint section 1.4.
		// UE 5.8: FString::Printf needs a literal format string (TCheckedFormatString), so the format is inlined here.
		const FString PackagePath = FString::Printf(TEXT("/Game/Qinglan/Data/DA_Realm_%s"), *Id);
		UObject* Raw = CreateFreshAsset(PackagePath, FindQinglanScriptClass(TEXT("QinglanRealmDataAsset")), Errors);
		if (Raw == nullptr) { return false; }
		UQinglanRealmDataAsset* Asset = static_cast<UQinglanRealmDataAsset*>(Raw);

		Asset->RealmId = FName(*Id);
		Asset->NameTextKey = JName(Root, TEXT("name_key"));
		Asset->DefaultSeed = static_cast<int32>(JNum(Root, TEXT("seed")));
		Asset->MaxConcurrentEnemies = static_cast<int32>(JNum(Root, TEXT("max_concurrent_enemies"), 8.0)); // blueprint section 1.3 cap
		for (const TSharedPtr<FJsonValue>& Value : JArr(Root, TEXT("rooms")))
		{
			const TSharedPtr<FJsonObject> Room = Value->AsObject();
			if (!Room.IsValid()) { continue; }
			const TSharedRef<FJsonObject> RoomRef = Room.ToSharedRef();
			FQinglanRealmRoom& RoomOut = Asset->Rooms.AddDefaulted_GetRef();
			EQinglanRoomKind Kind = EQinglanRoomKind::Entrance;
			ParseRoomKind(JStr(RoomRef, TEXT("kind"), TEXT("entrance")), Kind);
			RoomOut.Kind = Kind;
			RoomOut.SpiritShards = static_cast<int32>(JNum(RoomRef, TEXT("spirit_shards")));
		}
		if (const TSharedPtr<FJsonObject> Bonus = JObj(Root, TEXT("first_clear_bonus")))
		{
			const TSharedRef<FJsonObject> BonusRef = Bonus.ToSharedRef();
			Asset->FirstClearBonus.SpiritShards = static_cast<int64>(JNum(BonusRef, TEXT("spirit_shards")));
			Asset->FirstClearBonus.SpiritHerb = static_cast<int64>(JNum(BonusRef, TEXT("spirit_herb")));
			Asset->FirstClearBonus.VeinCrystal = static_cast<int64>(JNum(BonusRef, TEXT("vein_crystal")));
		}
		return SaveImportedAsset(Raw, Errors);
	}

	bool ImportStringTable(const FQinglanLocaleData& Locales, TArray<FString>& Errors)
	{
		UObject* Raw = CreateFreshAsset(StringTableAssetPath, UStringTable::StaticClass(), Errors);
		if (Raw == nullptr) { return false; }
		UStringTable* StringTable = static_cast<UStringTable*>(Raw);
		const TMap<FString, FString>* SourceKeys = Locales.CultureKeys.Find(StringTableSourceCulture);
		if (SourceKeys == nullptr)
		{
			AddError(Errors, TEXT("ST_Game"), FString::Printf(TEXT("source culture '%s' produced no keys"), StringTableSourceCulture));
			return false;
		}
		// UE 5.8 string-table API: GetMutableStringTable returns a non-null
		// FStringTableRef (TSharedRef) and insertion goes through SetSourceString
		// (the editor build carries the dev-notes overload). Sorted insertion =>
		// byte-stable table across machines.
		const FStringTableRef Table = StringTable->GetMutableStringTable();
		TArray<FString> SortedKeys;
		SourceKeys->GetKeys(SortedKeys);
		SortedKeys.Sort();
		for (const FString& Key : SortedKeys)
		{
			Table->SetSourceString(FTextKey(Key), SourceKeys->FindChecked(Key), FString());
		}
		// AUD-50: assert the generated table carries every imported key; a
		// silently truncated table is exactly the drift this audit found.
		int32 WrittenKeys = 0;
		Table->EnumerateKeysAndSourceStrings([&WrittenKeys](const FTextKey&, const FString&) { ++WrittenKeys; return true; });
		UE_LOG(LogQinglanEditor, Display, TEXT("ST_Game: %d keys written (source locale %s)"),
			WrittenKeys, StringTableSourceCulture);
		if (WrittenKeys != SourceKeys->Num())
		{
			AddError(Errors, TEXT("ST_Game"), FString::Printf(
				TEXT("string table holds %d keys but the locale defines %d"), WrittenKeys, SourceKeys->Num()));
			return false;
		}
		return SaveImportedAsset(Raw, Errors);
	}

	/**
	 * AUD-48: bake both shipped locales into DT_Localization so the runtime
	 * service can switch languages without repository JSON and without the
	 * offline container running the engine's gather/compile .locres pipeline.
	 * Parity was validated above; missing zh rows still fall back to en.
	 */
	bool ImportBilingualTable(const FQinglanLocaleData& Locales, TArray<FString>& Errors)
	{
		const TMap<FString, FString>* EnglishKeys = Locales.CultureKeys.Find(RequiredCultureEnUs);
		const TMap<FString, FString>* ChineseKeys = Locales.CultureKeys.Find(RequiredCultureZhHans);
		if (EnglishKeys == nullptr || ChineseKeys == nullptr)
		{
			AddError(Errors, TEXT("DT_Localization"), TEXT("both en-US and zh-Hans key sets are required"));
			return false;
		}

		UObject* Raw = CreateFreshAsset(BilingualTableAssetPath, UDataTable::StaticClass(), Errors);
		if (Raw == nullptr) { return false; }
		UDataTable* Table = static_cast<UDataTable*>(Raw);
		Table->RowStruct = FQinglanLocalizedTextRow::StaticStruct();

		// Sorted row insertion => byte-stable asset across machines.
		TArray<FString> SortedKeys;
		EnglishKeys->GetKeys(SortedKeys);
		SortedKeys.Sort();
		for (const FString& Key : SortedKeys)
		{
			FQinglanLocalizedTextRow Row;
			Row.TextEn = EnglishKeys->FindChecked(Key);
			if (const FString* ChineseText = ChineseKeys->Find(Key))
			{
				Row.TextZh = *ChineseText;
			}
			else
			{
				// ValidateLocaleParity already reported the mismatch; keep the
				// runtime table complete so Chinese never renders blank.
				Row.TextZh = Row.TextEn;
			}
			Table->AddRow(FName(*Key), Row);
		}

		UE_LOG(LogQinglanEditor, Display, TEXT("DT_Localization: %d bilingual rows"), SortedKeys.Num());
		return SaveImportedAsset(Raw, Errors);
	}
} // namespace

// ---------------------------------------------------------------------------
// UQinglanDataImportCommandlet
// ---------------------------------------------------------------------------

UQinglanDataImportCommandlet::UQinglanDataImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	LogToConsole = true;
	// UE 5.8 UCommandlet help fields (HelpToRunCmdlet no longer exists).
	HelpDescription = TEXT("Regenerates ST_Game, DT_Localization (en-US + zh-Hans), DA_StoryRegistry, DA_Tech_<id>, and DA_Realm_<id> from the repository data directory.");
	HelpUsage = TEXT("QinglanDataImport -data=<repo data dir> [-loc=<localization dir>] [-localizationonly]");
}

int32 UQinglanDataImportCommandlet::Main(const FString& Params)
{
	UE_LOG(LogQinglanEditor, Display, TEXT("QinglanDataImport starting"));

	// --- Parameter resolution -------------------------------------------------
	FString DataDir;
	if (!FParse::Value(*Params, TEXT("data="), DataDir) || DataDir.IsEmpty())
	{
		UE_LOG(LogQinglanEditor, Error, TEXT("Missing required parameter: -data=<repo data dir>"));
		return 1;
	}
	DataDir.ReplaceInline(TEXT("\""), TEXT("")); // strip wrapping quotes from -data="path" (no RemoveSingleInline in 5.8)
	FPaths::NormalizeDirectoryName(DataDir);
	if (!FPaths::DirectoryExists(DataDir))
	{
		UE_LOG(LogQinglanEditor, Error, TEXT("data directory does not exist: %s"), *DataDir);
		return 1;
	}
	FString LocDir;
	if (FParse::Value(*Params, TEXT("loc="), LocDir) && !LocDir.IsEmpty())
	{
		LocDir.ReplaceInline(TEXT("\""), TEXT(""));
		FPaths::NormalizeDirectoryName(LocDir);
	}
	else
	{
		// localization/ is a sibling of data/ in the repository layout.
		LocDir = FPaths::Combine(FPaths::GetPath(DataDir), TEXT("localization"));
	}
	if (!FPaths::DirectoryExists(LocDir))
	{
		UE_LOG(LogQinglanEditor, Error, TEXT("localization directory does not exist: %s (override with -loc=)"), *LocDir);
		return 1;
	}
	UE_LOG(LogQinglanEditor, Display, TEXT("Data: %s  Localization: %s"), *DataDir, *LocDir);

	TArray<FString> Errors;

	// --- Locales ---------------------------------------------------------------
	FQinglanLocaleData Locales;
	LoadAllLocales(LocDir, Locales, Errors);
	ValidateLocaleParity(Locales, Errors);
	if (const TMap<FString, FString>* EnKeys = Locales.CultureKeys.Find(RequiredCultureEnUs))
	{
		UE_LOG(LogQinglanEditor, Display, TEXT("Locale en-US: %d keys"), EnKeys->Num());
	}

	// --- Story ------------------------------------------------------------------
	TMap<FString, TSharedRef<FJsonObject>> StoryFiles;
	{
		TArray<FString> StoryPaths;
		FindFilesSorted(FPaths::Combine(DataDir, TEXT("story")), TEXT("*.json"), StoryPaths);
		for (const FString& Path : StoryPaths)
		{
			TSharedPtr<FJsonObject> Root;
			if (ReadJsonObject(Path, Root, Errors) && Root.IsValid())
			{
				StoryFiles.Add(Path, Root.ToSharedRef());
			}
		}
		if (StoryFiles.Num() == 0)
		{
			AddError(Errors, TEXT("story"), TEXT("no data/story/*.json files found"));
		}
	}
	int32 MainSceneCount = 0, SideStageCount = 0, RecordCount = 0, BarkCount = 0;
	ValidateStoryFiles(StoryFiles, Locales, Errors, MainSceneCount, SideStageCount, RecordCount, BarkCount);

	// --- Techniques ----------------------------------------------------------------
	int32 TechniqueCount = 0;
	{
		TArray<FString> TechPaths;
		FindFilesSorted(FPaths::Combine(DataDir, TEXT("examples")), TEXT("techniques*.json"), TechPaths);
		for (const FString& Path : TechPaths)
		{
			TSharedPtr<FJsonObject> Root;
			if (ReadJsonObject(Path, Root, Errors) && Root.IsValid())
			{
				ValidateTechniqueFile(Path, Root.ToSharedRef(), Locales, Errors, TechniqueCount);
			}
		}
		if (TechniqueCount == 0)
		{
			AddError(Errors, TEXT("techniques"), TEXT("no techniques found under data/examples/techniques*.json"));
		}
	}

	// --- Realms ------------------------------------------------------------------------
	{
		TArray<FString> RealmPaths;
		FindFilesSorted(FPaths::Combine(DataDir, TEXT("examples")), TEXT("realm*.json"), RealmPaths);
		if (RealmPaths.Num() == 0)
		{
			AddError(Errors, TEXT("realms"), TEXT("no realms found under data/examples/realm*.json"));
		}
		for (const FString& Path : RealmPaths)
		{
			TSharedPtr<FJsonObject> Root;
			if (ReadJsonObject(Path, Root, Errors) && Root.IsValid())
			{
				ValidateRealmFile(Path, Root.ToSharedRef(), Locales, Errors);
			}
		}
	}

	// --- Fail fast with the precise error list ----------------------------------------
	if (Errors.Num() > 0)
	{
		UE_LOG(LogQinglanEditor, Error, TEXT("QinglanDataImport FAILED with %d error(s):"), Errors.Num());
		for (int32 ErrorIndex = 0; ErrorIndex < Errors.Num(); ++ErrorIndex)
		{
			UE_LOG(LogQinglanEditor, Error, TEXT("  [%d/%d] %s"), ErrorIndex + 1, Errors.Num(), *Errors[ErrorIndex]);
		}
		return 1;
	}

	// --- Import (validation passed; every asset regenerates from data) ------------------
	bool bAllSaved = ImportStringTable(Locales, Errors);
	// AUD-48: the runtime service reads this bilingual table; keep it in the
	// same regeneration pass so it can never drift from ST_Game's key set.
	if (!ImportBilingualTable(Locales, Errors)) { bAllSaved = false; }
	// Release builds can refresh text without replacing technique assets whose
	// montage/cue references are authored by later bootstrap passes.
	if (FParse::Param(*Params, TEXT("localizationonly")))
	{
		for (const FString& Error : Errors)
		{
			UE_LOG(LogQinglanEditor, Error, TEXT("%s"), *Error);
		}
		UE_LOG(LogQinglanEditor, Display, TEXT("QinglanDataImport localization-only complete: %s"),
			bAllSaved && Errors.IsEmpty() ? TEXT("success") : TEXT("failed"));
		return bAllSaved && Errors.IsEmpty() ? 0 : 1;
	}
	if (!ImportStoryRegistry(StoryFiles, Errors)) { bAllSaved = false; }

	{
		TArray<FString> TechPaths;
		FindFilesSorted(FPaths::Combine(DataDir, TEXT("examples")), TEXT("techniques*.json"), TechPaths);
		for (const FString& Path : TechPaths)
		{
			TSharedPtr<FJsonObject> Root;
			if (ReadJsonObject(Path, Root, Errors) && Root.IsValid())
			{
				for (const TSharedPtr<FJsonValue>& Value : JArr(Root.ToSharedRef(), TEXT("techniques")))
				{
					if (Value->AsObject().IsValid() && !ImportTechnique(Value->AsObject().ToSharedRef(), Errors))
					{
						bAllSaved = false;
					}
				}
			}
		}
	}
	{
		TArray<FString> RealmPaths;
		FindFilesSorted(FPaths::Combine(DataDir, TEXT("examples")), TEXT("realm*.json"), RealmPaths);
		for (const FString& Path : RealmPaths)
		{
			TSharedPtr<FJsonObject> Root;
			if (ReadJsonObject(Path, Root, Errors) && Root.IsValid())
			{
				if (!ImportRealm(Root.ToSharedRef(), Errors))
				{
					bAllSaved = false;
				}
			}
		}
	}

	if (Errors.Num() > 0 || !bAllSaved)
	{
		UE_LOG(LogQinglanEditor, Error, TEXT("QinglanDataImport FAILED with %d error(s):"), Errors.Num());
		for (int32 ErrorIndex = 0; ErrorIndex < Errors.Num(); ++ErrorIndex)
		{
			UE_LOG(LogQinglanEditor, Error, TEXT("  [%d/%d] %s"), ErrorIndex + 1, Errors.Num(), *Errors[ErrorIndex]);
		}
		return 1;
	}

	UE_LOG(LogQinglanEditor, Display,
		TEXT("QinglanDataImport complete: %d chapters-worth of files, %d main scenes, %d side stages, %d records, %d barks, %d techniques"),
		StoryFiles.Num(), MainSceneCount, SideStageCount, RecordCount, BarkCount, TechniqueCount);
	return 0;
}
