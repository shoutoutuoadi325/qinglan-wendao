// Animation bootstrap: builds combat montages from imported FBX takes.
// Discovers anim sequences under /Game/Qinglan/Characters, cuts combo montages
// with notify windows, and pairs them to techniques/enemies by naming rules.
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanAnimBootstrap
#include "QinglanAnimBootstrap.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Combat/QinglanAnimNotifies.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Abilities/QinglanTechniqueTypes.h"

/**
 * AUD-45 path contract. Each montage is its own package and its top-level
 * object name is exactly the package's short name, e.g.
 *   /Game/Qinglan/Characters/Human/Montages/AM_Human_SwordCut_1
 *   object AM_Human_SwordCut_1 inside package .../AM_Human_SwordCut_1
 * These functions are consumed by the commandlet and by automation
 * (Qinglan.Anim.MontagePathContract); keep them in sync with the mesh import
 * layout and UQinglanSwordComponent's fallback soft paths.
 */
namespace QinglanAnimBootstrapPaths
{
	FString MontageDirectory(const FString& Group)
	{
		return FString::Printf(TEXT("/Game/Qinglan/Characters/%s/Montages"), *Group);
	}

	FString MontagePackagePath(const FString& Directory, const FString& MontageName)
	{
		return Directory / MontageName;
	}

	FString MontageObjectPath(const FString& PackagePath, const FString& MontageName)
	{
		return FString::Printf(TEXT("%s.%s"), *PackagePath, *MontageName);
	}
}

namespace
{
	/** Montage output root per character group (mirrors mesh import layout). */
	FString MontageDir(const FString& Group)
	{
		return QinglanAnimBootstrapPaths::MontageDirectory(Group);
	}

	/** Case-insensitive substring match on an asset name. */
	bool NameContains(const UObject* Asset, const TCHAR* Needle)
	{
		return Asset && Asset->GetName().Contains(Needle, ESearchCase::IgnoreCase);
	}

	struct FTakeRule
	{
		const TCHAR* Needle;   // substring matched against the sequence name
		const TCHAR* MontageSuffix; // output montage name suffix after "AM_<Group>_"
	};

	/**
	 * Player (Human skeleton) take rules; order matters — first match wins.
	 * The imported Human rig carries three takes (Human_Armature_Idle / _Jump /
	 * _ArmatureAction_001); the action take drives every melee/cast montage,
	 * Jump stands in for the dodge. Misses are logged, never fatal: the sword
	 * component runs its phase timers without montages.
	 */
	const FTakeRule PlayerRules[] = {
		{ TEXT("ArmatureAction"), TEXT("SwordCut_1") },
		{ TEXT("Attack2"),  TEXT("SwordCut_2") },
		{ TEXT("Attack3"),  TEXT("SwordCut_3") },
		{ TEXT("Punch"),    TEXT("SwordCut_1") },
		{ TEXT("Slash"),    TEXT("SwordCut_1") },
		{ TEXT("Attack"),   TEXT("SwordCut_1") },
		{ TEXT("Cast"),     TEXT("Cast") },
		{ TEXT("Spell"),    TEXT("Cast") },
		{ TEXT("Jump"),     TEXT("Dodge") },
		{ TEXT("Roll"),     TEXT("Dodge") },
		{ TEXT("Dodge"),    TEXT("Dodge") },
	};

	/** Monster groups: take rules per skeleton directory. */
	const FTakeRule MonsterRules[] = {
		{ TEXT("Attack"),   TEXT("Attack") },
		{ TEXT("Bite"),     TEXT("Attack") },
		{ TEXT("Hit"),      TEXT("HitReact") },
		{ TEXT("Hurt"),     TEXT("HitReact") },
		{ TEXT("Damage"),   TEXT("HitReact") },
		{ TEXT("React"),    TEXT("HitReact") },
	};

	UAnimMontage* SaveMontage(UAnimMontage* Montage, const FString& PackagePath)
	{
		UPackage* Package = Montage->GetOutermost();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Montage, *FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension()), Args))
		{
			UE_LOG(LogTemp, Error, TEXT("failed to save montage %s"), *PackagePath);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Montage);
		return Montage;
	}

	/**
	 * AUD-45: after saving, prove the asset can be found by its contract path and
	 * carries the expected identity. Fails when the package/object names diverge
	 * (the old bug created one "Montages" package for several montages), when the
	 * skeleton is missing, or when the skeleton does not match the source take
	 * (Monsters has multiple rigs, so this is checked per montage).
	 */
	bool VerifyMontageContract(const FString& Directory, const FString& MontageName,
		const USkeleton* ExpectedSkeleton)
	{
		const FString PackagePath = QinglanAnimBootstrapPaths::MontagePackagePath(Directory, MontageName);
		const FString ObjectPath = QinglanAnimBootstrapPaths::MontageObjectPath(PackagePath, MontageName);
		UAnimMontage* Loaded = LoadObject<UAnimMontage>(nullptr, *ObjectPath);
		if (!Loaded)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-45 verify: montage not loadable at %s"), *ObjectPath);
			return false;
		}
		if (Loaded->GetName() != MontageName)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-45 verify: object name '%s' != file name '%s'"),
				*Loaded->GetName(), *MontageName);
			return false;
		}
		if (Loaded->GetOutermost()->GetName() != PackagePath)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-45 verify: package '%s' != expected '%s'"),
				*Loaded->GetOutermost()->GetName(), *PackagePath);
			return false;
		}
		if (Loaded->GetSkeleton() == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-45 verify: montage %s has no skeleton"), *ObjectPath);
			return false;
		}
		if (ExpectedSkeleton != nullptr && Loaded->GetSkeleton() != ExpectedSkeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-45 verify: montage %s skeleton '%s' != source skeleton '%s'"),
				*ObjectPath, *Loaded->GetSkeleton()->GetName(), *ExpectedSkeleton->GetName());
			return false;
		}
		return true;
	}
} // namespace

UQinglanAnimBootstrapCommandlet::UQinglanAnimBootstrapCommandlet()
{
	IsClient = false;
}

UAnimMontage* UQinglanAnimBootstrapCommandlet::BuildComboMontage(const FString& OutPath, const FString& MontageName,
	UAnimSequence* Sequence, float WindupFraction, float ActiveFraction)
{
	if (Sequence == nullptr)
	{
		return nullptr;
	}
	if (Sequence->GetSkeleton() == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("AUD-45: source sequence %s has no skeleton; montage %s not built"),
			*Sequence->GetName(), *MontageName);
		return nullptr;
	}

	// AUD-45: OutPath is the montage *directory*; each montage gets its own full
	// package path and its object name matches the file name. The previous code
	// used the directory itself as the package, so several montages collided in
	// one package and could not be loaded by their advertised paths.
	const FString PackagePath = QinglanAnimBootstrapPaths::MontagePackagePath(OutPath, MontageName);
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogTemp, Error, TEXT("AUD-45: invalid montage package path '%s'"), *PackagePath);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackagePath);
	if (Package == nullptr)
	{
		return nullptr;
	}
	UAnimMontage* Montage = NewObject<UAnimMontage>(Package, *MontageName, RF_Public | RF_Standalone | RF_Transactional);
	if (Montage == nullptr)
	{
		return nullptr;
	}

	// AUD-45: explicit skeleton from the source take. Monsters can have multiple
	// rigs, so each montage follows its own source sequence rather than a shared
	// group-level skeleton.
	Montage->SetSkeleton(Sequence->GetSkeleton());

	// Single slot track over the whole take; montage time == sequence time.
	const float PlayLength = FMath::Max(Sequence->GetPlayLength(), 0.01f);
	FSlotAnimationTrack& Track = Montage->SlotAnimTracks.AddDefaulted_GetRef();
	Track.SlotName = TEXT("DefaultSlot");
	FAnimSegment& Segment = Track.AnimTrack.AnimSegments.AddDefaulted_GetRef();
	Segment.SetAnimReference(Sequence, /*bInitialize=*/true);
	Segment.StartPos = 0.f;
	Segment.AnimStartTime = 0.f;
	Segment.AnimEndTime = PlayLength;
	Segment.AnimPlayRate = 1.f;
	Segment.LoopingCount = 1;
	Montage->GetController().SetPlayLength(PlayLength); // UE 5.8: SequenceLength is protected
	Montage->RateScale = 1.f;

	// Hit-window notifies at the phase boundaries (docs/02 section 4). The
	// sword component's timers stay authoritative; notifies only tighten the
	// cosmetic sweep window to the visual swing.
	const float WindowOpenTime = PlayLength * WindupFraction;
	const float WindowCloseTime = PlayLength * (WindupFraction + ActiveFraction);
	auto AddNotify = [Montage](UClass* NotifyClass, FName NotifyName, float Time)
	{
		FAnimNotifyEvent& Event = Montage->Notifies.AddDefaulted_GetRef();
		Event.NotifyName = NotifyName;
		Event.Notify = NewObject<UAnimNotify>(Montage, NotifyClass);
		Event.Link(Montage, Time);
	};
	AddNotify(UQinglanAnimNotify_SwordWindowOpen::StaticClass(), TEXT("QinglanSwordWindowOpen"), WindowOpenTime);
	AddNotify(UQinglanAnimNotify_SwordWindowClosed::StaticClass(), TEXT("QinglanSwordWindowClosed"), WindowCloseTime);

	return SaveMontage(Montage, PackagePath);
}

int32 UQinglanAnimBootstrapCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("QinglanAnimBootstrap starting"));
	bool bOk = true;

	// --- Discover imported anim sequences by character group. ---
	FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> AllAssets;
	AssetRegistry.Get().GetAssetsByClass(
		FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("AnimSequence")), AllAssets, /*bSearchSubClasses=*/false);

	// group -> (rule table, matched suffixes already built)
	struct FGroupPlan
	{
		FString Group;
		bool bPlayer;
		TMap<FName, TObjectPtr<UAnimSequence>> Takes; // matched suffix -> sequence
	};
	TArray<FGroupPlan> Groups;
	auto GroupOf = [&Groups](const FString& Group, bool bPlayer) -> FGroupPlan&
	{
		for (FGroupPlan& Plan : Groups)
		{
			if (Plan.Group.Equals(Group, ESearchCase::IgnoreCase)) { return Plan; }
		}
		FGroupPlan& NewPlan = Groups.AddDefaulted_GetRef();
		NewPlan.Group = Group;
		NewPlan.bPlayer = bPlayer;
		return NewPlan;
	};

	for (const FAssetData& Asset : AllAssets)
	{
		// PackageName is the reliable "/Game/Qinglan/Characters/<Group>/<Asset>" form.
		const FString Package = Asset.PackageName.ToString();
		if (!Package.StartsWith(TEXT("/Game/Qinglan/Characters/")))
		{
			continue;
		}
		// <Group> is the first path segment after the content root.
		FString Rest = Package;
		Rest.RemoveFromStart(TEXT("/Game/Qinglan/Characters/"));
		const FString Group = Rest.Contains(TEXT("/")) ? Rest.Left(Rest.Find(TEXT("/"))) : FString();
		if (Group.IsEmpty() || Group.Equals(TEXT("Montages"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const bool bPlayer = Group.Equals(TEXT("Human"), ESearchCase::IgnoreCase);
		const FTakeRule* Rules = bPlayer ? PlayerRules : MonsterRules;
		const int32 RuleCount = bPlayer ? UE_ARRAY_COUNT(PlayerRules) : UE_ARRAY_COUNT(MonsterRules);

		UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
		if (Sequence == nullptr)
		{
			continue;
		}
		FGroupPlan& Plan = GroupOf(Group, bPlayer);
		for (int32 Index = 0; Index < RuleCount; ++Index)
		{
			if (NameContains(Sequence, Rules[Index].Needle))
			{
				const FName Suffix(Rules[Index].MontageSuffix);
				// First match per suffix wins; later takes logged and skipped.
				if (!Plan.Takes.Contains(Suffix))
				{
					Plan.Takes.Add(Suffix, Sequence);
				}
				break;
			}
		}
	}

	if (Groups.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("no anim sequences found under /Game/Qinglan/Characters; run mesh import first"));
		return 1;
	}

	// --- Build montages per group. ---
	int32 BuiltCount = 0;
	struct FBuiltMontageRecord
	{
		FString Directory;
		FString MontageName;
		TWeakObjectPtr<USkeleton> SourceSkeleton;
	};
	TArray<FBuiltMontageRecord> BuiltMontageRecords; // AUD-45 end-of-run verification
	TMap<FName, TObjectPtr<UAnimMontage>> CastArchetypes; // player cast montage set shared by techniques
	for (FGroupPlan& Plan : Groups)
	{
		const FString Dir = MontageDir(Plan.Group);
		// The imported Human rig has a single action take; clone it into the
		// missing combo cuts with distinct notify windows so all three player
		// montages exist (visual repetition is acceptable; timing still differs).
		if (Plan.bPlayer)
		{
			const TObjectPtr<UAnimSequence>* CutOne = Plan.Takes.Find(FName(TEXT("SwordCut_1")));
			if (CutOne)
			{
				for (const TCHAR* Missing : { TEXT("SwordCut_2"), TEXT("SwordCut_3") })
				{
					Plan.Takes.FindOrAdd(FName(Missing)) = *CutOne;
				}
				if (!Plan.Takes.Contains(FName(TEXT("Cast"))))
				{
					Plan.Takes.FindOrAdd(FName(TEXT("Cast"))) = *CutOne;
				}
			}
		}
		for (const TPair<FName, TObjectPtr<UAnimSequence>>& Pair : Plan.Takes)
		{
			const FString MontageName = FString::Printf(TEXT("AM_%s_%s"), *Plan.Group, *Pair.Key.ToString());
			// Player combo cut 1..3 carry the windup/active notify pair; other
			// montages are cosmetic-only and get no gameplay notifies.
			float WindupFraction = -1.f;
			float ActiveFraction = 0.f;
			const FString Suffix = Pair.Key.ToString();
			if (Suffix.StartsWith(TEXT("SwordCut")))
			{
				// Visual approximation of docs/02 section 4 phases; later cuts
				// wind up a little longer to match their longer windup timers.
				const float CutIndex = Suffix.EndsWith(TEXT("1")) ? 0.f : (Suffix.EndsWith(TEXT("2")) ? 1.f : 2.f);
				WindupFraction = 0.30f + 0.05f * CutIndex;
				ActiveFraction = 0.25f;
			}
			UAnimMontage* Montage = BuildComboMontage(Dir, MontageName, Pair.Value, FMath::Max(WindupFraction, 0.f), ActiveFraction);
			if (Montage)
			{
				++BuiltCount;
				FBuiltMontageRecord& Record = BuiltMontageRecords.AddDefaulted_GetRef();
				Record.Directory = Dir;
				Record.MontageName = MontageName;
				Record.SourceSkeleton = Pair.Value ? Pair.Value->GetSkeleton() : nullptr;
				if (Plan.bPlayer && Suffix.StartsWith(TEXT("Cast")))
				{
					CastArchetypes.Add(Pair.Key, Montage);
				}
			}
			else
			{
				bOk = false;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("group %s: %d takes matched"), *Plan.Group, Plan.Takes.Num());
	}

	// --- Pair cast montages into technique data assets by targeting shape. ---
	int32 PairedCount = 0;
	if (CastArchetypes.Num() > 0)
	{
		TArray<FAssetData> TechniqueAssets;
		AssetRegistry.Get().GetAssetsByClass(
			FTopLevelAssetPath(TEXT("/Script/QinglanAscension"), TEXT("QinglanTechniqueDataAsset")), TechniqueAssets, false);
		for (const FAssetData& Asset : TechniqueAssets)
		{
			UQinglanTechniqueDataAsset* Technique = Cast<UQinglanTechniqueDataAsset>(Asset.GetAsset());
			if (Technique == nullptr)
			{
				continue;
			}
			// Blueprint section 2.5 "4 cast archetypes": projectile throws,
			// self-buffs/dashes use the body-cast pose; arcs/strikes throw too.
			const TCHAR* Preferred =
				(Technique->Targeting == EQinglanTargeting::Projectile ||
				 Technique->Targeting == EQinglanTargeting::SelfArc ||
				 Technique->Targeting == EQinglanTargeting::GroundStrike) ? TEXT("CastThrow") :
				TEXT("Cast");
			TObjectPtr<UAnimMontage>* Match = CastArchetypes.Find(FName(Preferred));
			if (Match == nullptr)
			{
				Match = CastArchetypes.Find(FName(TEXT("Cast")));
			}
			if (Match && Match->Get())
			{
				Technique->CastMontage = TSoftObjectPtr<UAnimMontage>(*Match);
				UPackage* Package = Technique->GetOutermost();
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				if (UPackage::SavePackage(Package, Technique, *FPackageName::LongPackageNameToFilename(Asset.PackageName.ToString(), FPackageName::GetAssetPackageExtension()), Args))
				{
					++PairedCount;
				}
				else
				{
					bOk = false;
				}
			}
		}
	}

	// --- AUD-45: enumerate and verify every montage produced this run. ---
	int32 VerifiedCount = 0;
	for (const FBuiltMontageRecord& Record : BuiltMontageRecords)
	{
		if (VerifyMontageContract(Record.Directory, Record.MontageName, Record.SourceSkeleton.Get()))
		{
			++VerifiedCount;
		}
		else
		{
			bOk = false;
		}
	}
	if (VerifiedCount != BuiltCount)
	{
		bOk = false;
	}

	UE_LOG(LogTemp, Log, TEXT("QinglanAnimBootstrap finished: %d montages built, %d verified, %d techniques paired, ok=%d"),
		BuiltCount, VerifiedCount, PairedCount, bOk);
	return bOk ? 0 : 1;
}
