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
#include "Abilities/QinglanTechniqueTypes.h"

namespace
{
	/** Montage output root per character group (mirrors mesh import layout). */
	FString MontageDir(const FString& Group)
	{
		return FString::Printf(TEXT("/Game/Qinglan/Characters/%s/Montages"), *Group);
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
	 * Quaternius Universal Animation Library take names vary between packs, so
	 * every rule lists synonyms. Misses are logged, never fatal: the sword
	 * component runs its phase timers without montages.
	 */
	const FTakeRule PlayerRules[] = {
		{ TEXT("Punch2"),   TEXT("SwordCut_2") },
		{ TEXT("Punch3"),   TEXT("SwordCut_3") },
		{ TEXT("Punch"),    TEXT("SwordCut_1") },
		{ TEXT("Attack2"),  TEXT("SwordCut_2") },
		{ TEXT("Attack3"),  TEXT("SwordCut_3") },
		{ TEXT("Attack"),   TEXT("SwordCut_1") },
		{ TEXT("Slash"),    TEXT("SwordCut_1") },
		{ TEXT("Cast"),     TEXT("Cast") },
		{ TEXT("Spell"),    TEXT("Cast") },
		{ TEXT("Magic"),    TEXT("Cast") },
		{ TEXT("Shoot"),    TEXT("CastThrow") },
		{ TEXT("Throw"),    TEXT("CastThrow") },
		{ TEXT("Summon"),   TEXT("CastSummon") },
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
} // namespace

UQinglanAnimBootstrap::UQinglanAnimBootstrap()
{
	IsClient = false;
}

UAnimMontage* UQinglanAnimBootstrap::BuildComboMontage(const FString& OutPath, const FString& MontageName,
	UAnimSequence* Sequence, float WindupFraction, float ActiveFraction)
{
	if (Sequence == nullptr)
	{
		return nullptr;
	}
	UPackage* Package = CreatePackage(*OutPath);
	if (Package == nullptr)
	{
		return nullptr;
	}
	UAnimMontage* Montage = NewObject<UAnimMontage>(Package, *MontageName, RF_Public | RF_Standalone | RF_Transactional);
	if (Montage == nullptr)
	{
		return nullptr;
	}

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

	return SaveMontage(Montage, OutPath / MontageName);
}

int32 UQinglanAnimBootstrap::Main(const FString& Params)
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
		return Groups.AddDefaulted_GetRef();
	};

	for (const FAssetData& Asset : AllAssets)
	{
		const FString ObjectPath = Asset.GetObjectPathString();
		if (!ObjectPath.StartsWith(TEXT("/Game/Qinglan/Characters/")))
		{
			continue;
		}
		// /Game/Qinglan/Characters/<Group>/<...>/AS_Take -> group is the first path segment.
		FString Rest = ObjectPath;
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
	TMap<FName, TObjectPtr<UAnimMontage>> CastArchetypes; // player cast montage set shared by techniques
	for (FGroupPlan& Plan : Groups)
	{
		const FString Dir = MontageDir(Plan.Group);
		for (const TPair<FName, TObjectPtr<UAnimSequence>>& Pair : Plan.Takes)
		{
			const FString MontageName = FString::Printf(TEXT("AM_%s_%s"), *Plan.Group, *Pair.Key.ToString());
			// Player combo cut 1..3 carry the windup/active notify pair; other
			// montages are cosmetic-only and get no gameplay notifies.
			float WindupFraction = -1.f;
			float ActiveFraction = 0.f;
			const bool bIsSwordCut = Pair.Key.ToString().StartsWith(TEXT("SwordCut"));
			if (bIsSwordCut)
			{
				WindupFraction = 0.35f;  // visual approximation of docs/02 section 4 phases
				ActiveFraction = 0.25f;
			}
			UAnimMontage* Montage = BuildComboMontage(Dir, MontageName, Pair.Value, FMath::Max(WindupFraction, 0.f), ActiveFraction);
			if (Montage)
			{
				++BuiltCount;
				if (Plan.bPlayer && Pair.Key.ToString().StartsWith(TEXT("Cast")))
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

	UE_LOG(LogTemp, Log, TEXT("QinglanAnimBootstrap finished: %d montages built, %d techniques paired, ok=%d"),
		BuiltCount, PairedCount, bOk);
	return bOk ? 0 : 1;
}
