// Animation bootstrap: builds combat montages from imported FBX takes.
// Original Human locomotion/action clips are required; optional procedural
// locomotion authoring is retained only as a diagnostic tool.
// Discovers anim sequences under /Game/Qinglan/Characters, cuts combo montages
// with notify windows, and pairs them to techniques/enemies by naming rules.
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanAnimBootstrap
#include "QinglanAnimBootstrap.h"
#include "QinglanHumanAnimationContract.h"
#include "Engine/SkeletalMeshSocket.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Combat/QinglanAnimNotifies.h"
#include "Combat/QinglanCharacterAnimInstance.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Abilities/QinglanTechniqueTypes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"

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

	bool RepairHumanBladeSocket(USkeleton* Skeleton)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Qinglan/Characters/Human/SK_Human"));
		if (!Skeleton || !Mesh) { return false; }
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		FTransform Hand = FTransform::Identity;
		for (int32 Bone = Ref.FindBoneIndex(TEXT("RightHand")); Bone != INDEX_NONE; Bone = Ref.GetParentIndex(Bone))
		{
			Hand = Hand * Ref.GetRefBonePose()[Bone];
		}
		const double NormalizedMeshScale = 88.0 / FMath::Max(1.0, Mesh->GetBounds().BoxExtent.Z);
		const double HandScale = FMath::Abs(Hand.GetScale3D().X) * NormalizedMeshScale;
		if (HandScale < UE_SMALL_NUMBER) { return false; }
		for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
		{
			if (Socket && Socket->SocketName == FName(TEXT("sword_tip")))
			{
				// Imported hand bones include their FBX scale. Express a 90cm
				// blade in that space instead of inheriting a 14m socket offset.
				// The imported grip points down the hand's negative X axis.
				Socket->RelativeLocation = FVector(-90.0 / HandScale, 0.0, 0.0);
				Socket->RelativeRotation = FRotator::ZeroRotator;
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				Args.SaveFlags = SAVE_NoError;
				return UPackage::SavePackage(Skeleton->GetOutermost(), Skeleton,
					*FPackageName::LongPackageNameToFilename(Skeleton->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()), Args);
			}
		}
		return false;
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

	// -----------------------------------------------------------------------
	// AUD-44: procedural placeholder Walk/Run animation sequences.
	//
	// The Quaternius Human pack contains real Walk/Run takes, imported by
	// MeshImport. This optional diagnostic can generate two
	// independent one-second / 0.6-second loop assets with hand-authored limb
	// swing so UQinglanCharacterAnimInstance's convention paths resolve.
	//
	// These are placeholders: the rotation axis/sign below follows the AUD-44
	// prescription (local X) but has not been visually tuned against the rig;
	// the assets are separate names and can be replaced/removed freely.
	// -----------------------------------------------------------------------
	const TCHAR* const HumanCharacterDirectory = TEXT("/Game/Qinglan/Characters/Human");
	const TCHAR* const HumanSkeletonObjectPath = TEXT("/Game/Qinglan/Characters/Human/SK_Human_Skeleton");
	const TCHAR* const HumanMeshObjectPath = TEXT("/Game/Qinglan/Characters/Human/SK_Human");

	USkeleton* LoadHumanSkeleton()
	{
		if (USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, HumanSkeletonObjectPath))
		{
			return Skeleton;
		}
		if (USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, HumanMeshObjectPath))
		{
			return Mesh->GetSkeleton();
		}
		return nullptr;
	}

	struct FProceduralLimbBones
	{
		FName LeftThigh;
		FName RightThigh;
		FName LeftCalf;
		FName RightCalf;
		FName LeftUpperArm;
		FName RightUpperArm;
	};

	/**
	 * Case-insensitive role matching against the reference skeleton. The Human
	 * rig uses Mixamo-style names (LeftUpLeg/LeftLeg/LeftArm), but the matcher
	 * also accepts thigh/calf/leg/arm/upperarm spellings from other packs.
	 * Missing roles fall back to the first reference bones with a warning.
	 */
	FProceduralLimbBones ResolveProceduralLimbBones(const FReferenceSkeleton& RefSkeleton)
	{
		FProceduralLimbBones Bones;
		const int32 NumBones = RefSkeleton.GetRawBoneNum();

		// "前若干骨骼" fallback pool: skip the root so a fallback never rotates
		// the whole character around its origin.
		TArray<FName> FallbackBones;
		for (int32 BoneIndex = 1; BoneIndex < NumBones && FallbackBones.Num() < 8; ++BoneIndex)
		{
			FallbackBones.Add(RefSkeleton.GetBoneName(BoneIndex));
		}
		if (FallbackBones.Num() == 0 && NumBones > 0)
		{
			FallbackBones.Add(RefSkeleton.GetBoneName(0));
		}

		TSet<FName> Assigned;
		auto Assign = [&Assigned](FName Candidate, FName& OutBone)
		{
			if (!Candidate.IsNone() && OutBone.IsNone() && !Assigned.Contains(Candidate))
			{
				OutBone = Candidate;
				Assigned.Add(Candidate);
			}
		};

		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
			const FString Lower = BoneName.ToString().ToLower();

			const bool bLeft = Lower.Contains(TEXT("left"))
				|| Lower.StartsWith(TEXT("l_")) || Lower.EndsWith(TEXT("_l"))
				|| Lower.EndsWith(TEXT(".l"));
			const bool bRight = Lower.Contains(TEXT("right"))
				|| Lower.StartsWith(TEXT("r_")) || Lower.EndsWith(TEXT("_r"))
				|| Lower.EndsWith(TEXT(".r"));
			if (!bLeft && !bRight)
			{
				continue;
			}

			const bool bThigh = Lower.Contains(TEXT("thigh"))
				|| Lower.Contains(TEXT("upleg")) || Lower.Contains(TEXT("upperleg"));
			const bool bCalf = !bThigh && (Lower.Contains(TEXT("calf")) || Lower.Contains(TEXT("leg")));
			const bool bUpperArm = Lower.Contains(TEXT("upperarm")) || Lower.Contains(TEXT("upper_arm"))
				|| (Lower.Contains(TEXT("arm"))
					&& !Lower.Contains(TEXT("forearm"))
					&& !Lower.Contains(TEXT("lowerarm"))
					&& !Lower.Contains(TEXT("hand"))
					&& !Lower.Contains(TEXT("shoulder"))
					&& !Lower.Contains(TEXT("clavicle")));

			if (bThigh)
			{
				Assign(BoneName, bLeft ? Bones.LeftThigh : Bones.RightThigh);
			}
			else if (bCalf)
			{
				Assign(BoneName, bLeft ? Bones.LeftCalf : Bones.RightCalf);
			}
			else if (bUpperArm)
			{
				Assign(BoneName, bLeft ? Bones.LeftUpperArm : Bones.RightUpperArm);
			}
		}

		auto FillMissing = [&FallbackBones, &Assigned](FName& OutBone, const TCHAR* Role)
		{
			if (!OutBone.IsNone())
			{
				return;
			}
			for (const FName Candidate : FallbackBones)
			{
				if (!Assigned.Contains(Candidate))
				{
					OutBone = Candidate;
					Assigned.Add(Candidate);
					UE_LOG(LogTemp, Warning,
						TEXT("AUD-44: no %s bone matched; using reference bone '%s'"),
						Role, *Candidate.ToString());
					return;
				}
			}
			if (FallbackBones.Num() > 0)
			{
				OutBone = FallbackBones[0];
				UE_LOG(LogTemp, Warning,
					TEXT("AUD-44: no %s bone matched and fallback pool exhausted; reusing '%s'"),
					Role, *OutBone.ToString());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("AUD-44: no bones available for %s role"), Role);
			}
		};
		FillMissing(Bones.LeftThigh, TEXT("left thigh"));
		FillMissing(Bones.RightThigh, TEXT("right thigh"));
		FillMissing(Bones.LeftCalf, TEXT("left calf"));
		FillMissing(Bones.RightCalf, TEXT("right calf"));
		FillMissing(Bones.LeftUpperArm, TEXT("left upper-arm"));
		FillMissing(Bones.RightUpperArm, TEXT("right upper-arm"));

		UE_LOG(LogTemp, Log,
			TEXT("AUD-44 limb bones: LT=%s RT=%s LC=%s RC=%s LA=%s RA=%s"),
			*Bones.LeftThigh.ToString(), *Bones.RightThigh.ToString(),
			*Bones.LeftCalf.ToString(), *Bones.RightCalf.ToString(),
			*Bones.LeftUpperArm.ToString(), *Bones.RightUpperArm.ToString());
		return Bones;
	}

	/** One limb track: keyed local-X swing (thigh/arm) or 20-35 deg knee bend. */
	bool AddProceduralSwingTrack(
		IAnimationDataController& Controller,
		const FReferenceSkeleton& RefSkeleton,
		FName BoneName,
		float SwingDegrees,
		float PhaseOffsetRadians,
		bool bKneeBend,
		int32 NumFrames,
		int32 NumKeys)
	{
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogTemp, Warning, TEXT("AUD-44: bone '%s' not found in reference skeleton; track skipped"),
				*BoneName.ToString());
			return false;
		}

		const FTransform& RefPose = RefSkeleton.GetRawRefBonePose()[BoneIndex];
		TArray<FVector> PositionalKeys;
		TArray<FQuat> RotationalKeys;
		TArray<FVector> ScalingKeys;
		PositionalKeys.Reserve(NumKeys);
		RotationalKeys.Reserve(NumKeys);
		ScalingKeys.Reserve(NumKeys);

		constexpr float TwoPi = 2.f * UE_PI;
		constexpr float KneeMinDegrees = 20.f;
		constexpr float KneeMaxDegrees = 35.f;

		for (int32 KeyIndex = 0; KeyIndex < NumKeys; ++KeyIndex)
		{
			// Key 0 and key NumFrames share the same phase for a seamless loop.
			const float Alpha = static_cast<float>(KeyIndex) / static_cast<float>(FMath::Max(1, NumFrames));
			const float Phase = Alpha * TwoPi + PhaseOffsetRadians;

			float AngleDegrees = SwingDegrees * FMath::Sin(Phase);
			if (bKneeBend)
			{
				// Positive-only knee flexion, phase-lagged by the caller.
				const float BendAlpha = 0.5f + 0.5f * FMath::Sin(Phase); // 0..1
				AngleDegrees = FMath::Lerp(KneeMinDegrees, KneeMaxDegrees, BendAlpha);
			}

			// Post-multiply applies the delta in the bone's local space.
			const FQuat Swing(FVector::ForwardVector, FMath::DegreesToRadians(AngleDegrees));
			PositionalKeys.Add(RefPose.GetLocation());
			RotationalKeys.Add((RefPose.GetRotation() * Swing).GetNormalized());
			ScalingKeys.Add(RefPose.GetScale3D());
		}

		if (!Controller.AddBoneCurve(BoneName, /*bShouldTransact=*/false))
		{
			UE_LOG(LogTemp, Warning, TEXT("AUD-44: AddBoneCurve failed for '%s'"), *BoneName.ToString());
			return false;
		}
		if (!Controller.SetBoneTrackKeys(BoneName, PositionalKeys, RotationalKeys, ScalingKeys, /*bShouldTransact=*/false))
		{
			UE_LOG(LogTemp, Warning, TEXT("AUD-44: SetBoneTrackKeys failed for '%s'"), *BoneName.ToString());
			return false;
		}
		return true;
	}

	/** Builds and saves one procedural locomotion sequence. */
	UAnimSequence* GenerateProceduralLocomotionSequence(
		USkeleton* Skeleton,
		const FString& AssetName,
		int32 NumFrames,
		float ThighSwingDegrees,
		float ArmSwingDegrees,
		const TCHAR* Label)
	{
		if (!Skeleton || NumFrames <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44: cannot generate %s (skeleton=%s frames=%d)"),
				*AssetName, *GetNameSafe(Skeleton), NumFrames);
			return nullptr;
		}

		const FString PackagePath = FString(HumanCharacterDirectory) / AssetName;
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

		// Placeholders are generated only when the convention path is empty: an
		// authored replacement must survive later bootstrap runs.
		if (FPackageName::DoesPackageExist(PackagePath))
		{
			if (UAnimSequence* Existing = LoadObject<UAnimSequence>(nullptr, *ObjectPath))
			{
				UE_LOG(LogTemp, Log,
					TEXT("AUD-44: %s already exists; leaving it untouched"), *AssetName);
				return Existing;
			}
			UE_LOG(LogTemp, Warning,
				TEXT("AUD-44: %s exists on disk but failed to load; regenerating"), *AssetName);
		}

		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44: could not create package %s"), *PackagePath);
			return nullptr;
		}

		UAnimSequence* Sequence = FindObject<UAnimSequence>(Package, *AssetName);
		if (!Sequence)
		{
			Sequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		if (!Sequence)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44: could not create UAnimSequence %s"), *PackagePath);
			return nullptr;
		}

		Sequence->SetSkeleton(Skeleton);
		const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
		const FProceduralLimbBones Limbs = ResolveProceduralLimbBones(RefSkeleton);
		const int32 NumKeys = NumFrames + 1; // frames 0..NumFrames, last == first
		constexpr float TwoPi = 2.f * UE_PI;
		const float CalfPhaseLag = -0.3f * TwoPi;

		// Engine recipe (Editor/SequenceRecorder AnimationRecorder.cpp): the
		// asset-owned controller must be pointed at the sequence's data model
		// and initialized before AddBoneCurve; the raw model controller path
		// emits "No Movie Scene found for SequencerDataModel".
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.SetModel(Sequence->GetDataModelInterface());
		Controller.OpenBracket(
			FText::FromString(FString::Printf(TEXT("Generating procedural %s locomotion"), Label)),
			/*bShouldTransact=*/false);
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(30, 1), /*bShouldTransact=*/false);
		Controller.SetNumberOfFrames(FFrameNumber(NumFrames), /*bShouldTransact=*/false);

		int32 TrackCount = 0;
		TSet<FName> AddedTracks;
		auto AddTrackOnce = [&](FName BoneName, float SwingDegrees, float PhaseOffset, bool bKneeBend)
		{
			// The fallback pool can (only when exhausted) map two roles to one
			// bone; never add/overwrite the same track twice.
			if (BoneName.IsNone() || AddedTracks.Contains(BoneName))
			{
				return;
			}
			AddedTracks.Add(BoneName);
			if (AddProceduralSwingTrack(Controller, RefSkeleton, BoneName, SwingDegrees,
					PhaseOffset, bKneeBend, NumFrames, NumKeys))
			{
				++TrackCount;
			}
		};

		AddTrackOnce(Limbs.LeftThigh, ThighSwingDegrees, 0.f, /*bKneeBend=*/false);
		AddTrackOnce(Limbs.RightThigh, ThighSwingDegrees, UE_PI, /*bKneeBend=*/false);
		AddTrackOnce(Limbs.LeftCalf, 0.f, CalfPhaseLag, /*bKneeBend=*/true);
		AddTrackOnce(Limbs.RightCalf, 0.f, UE_PI + CalfPhaseLag, /*bKneeBend=*/true);
		// Arms counter-swing against the same-side leg (contralateral gait).
		AddTrackOnce(Limbs.LeftUpperArm, ArmSwingDegrees, UE_PI, /*bKneeBend=*/false);
		AddTrackOnce(Limbs.RightUpperArm, ArmSwingDegrees, 0.f, /*bKneeBend=*/false);

		if (TrackCount == 0)
		{
			Controller.CloseBracket(/*bShouldTransact=*/false);
			UE_LOG(LogTemp, Error, TEXT("AUD-44: no limb tracks generated for %s; asset not saved"), *AssetName);
			return nullptr;
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket(/*bShouldTransact=*/false);
		Sequence->MarkPackageDirty();

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Sequence,
				*FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension()), Args))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44: failed to save procedural sequence %s"), *PackagePath);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Sequence);

		UE_LOG(LogTemp, Log, TEXT("AUD-44: generated %s (%d frames, %d tracks, %.2fs)"),
			*AssetName, NumFrames, TrackCount, Sequence->GetPlayLength());
		return Sequence;
	}

	/** Post-save contract check for the generated locomotion assets. */
	bool VerifyProceduralLocomotionSequence(const FString& AssetName, const USkeleton* ExpectedSkeleton)
	{
		const FString PackagePath = FString(HumanCharacterDirectory) / AssetName;
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
		UAnimSequence* Loaded = LoadObject<UAnimSequence>(nullptr, *ObjectPath);
		if (!Loaded)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence not loadable at %s"), *ObjectPath);
			return false;
		}
		if (Loaded->GetName() != AssetName || Loaded->GetOutermost()->GetName() != PackagePath)
		{
			// The anim instance resolves these assets by convention path, so a
			// package/object name mismatch must fail the bootstrap.
			UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence identity mismatch at %s (package=%s object=%s)"),
				*ObjectPath, *Loaded->GetOutermost()->GetName(), *Loaded->GetName());
			return false;
		}
		if (!Loaded->GetSkeleton())
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence %s has no skeleton"), *ObjectPath);
			return false;
		}
		if (ExpectedSkeleton && Loaded->GetSkeleton() != ExpectedSkeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence %s skeleton '%s' != expected '%s'"),
				*ObjectPath, *Loaded->GetSkeleton()->GetName(), *ExpectedSkeleton->GetName());
			return false;
		}
		if (Loaded->GetPlayLength() <= 0.f)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence %s has zero play length"), *ObjectPath);
			return false;
		}
		if (const IAnimationDataModel* Model = Loaded->GetDataModel())
		{
			if (Model->GetNumBoneTracks() <= 0)
			{
				UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: sequence %s has no bone tracks"), *ObjectPath);
				return false;
			}
		}
		UE_LOG(LogTemp, Log, TEXT("AUD-44 verify: %s ok (%.2fs, %d bone tracks)"),
			*ObjectPath, Loaded->GetPlayLength(),
			Loaded->GetDataModel() ? Loaded->GetDataModel()->GetNumBoneTracks() : 0);
		return true;
	}

	// -----------------------------------------------------------------------
	// AUD-44: minimal AnimBlueprint (Root + DefaultSlot).
	//
	// The native anim instance drives locomotion/action state, but slot
	// montages (sword cuts, casts, hits, locomotion clips) only produce pose
	// when the compiled graph contains a Slot node. This authors:
	//
	//   [Slot 'DefaultSlot' Pose] --> [Root Result]
	//
	// The graph is created through FBlueprintEditorUtils with the AnimGraph
	// classes resolved by path. This keeps the editor module's Build.cs
	// unchanged (no AnimGraph include or link dependency) while still using the
	// engine's real UAnimationGraphSchema (which creates the Root node) and
	// UAnimGraphNode_Slot class. The slot name is written through reflection
	// on the node's FAnimNode_Slot property.
	// -----------------------------------------------------------------------
	const TCHAR* const HumanAnimBlueprintName = TEXT("ABP_Human");
	const TCHAR* const HumanAnimBlueprintPackagePath = TEXT("/Game/Qinglan/Characters/Human/ABP_Human");
	const TCHAR* const HumanAnimBlueprintObjectPath = TEXT("/Game/Qinglan/Characters/Human/ABP_Human.ABP_Human");
	const TCHAR* const HumanAnimBlueprintClassPath = TEXT("/Game/Qinglan/Characters/Human/ABP_Human.ABP_Human_C");

	/**
	 * AUD-44: one compiled slot graph per rig. ABP_Human covers the player and
	 * Human-skeleton enemies; non-Human enemies each get a sibling
	 * ABP_<MeshBase> in their mesh folder so their montages produce pose too.
	 */
	UAnimBlueprint* GenerateSlotAnimBlueprint(USkeleton* Skeleton, const FString& BlueprintName,
		const FString& BlueprintPackagePath, const FString& BlueprintObjectPath)
	{
		if (!Skeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: no skeleton; cannot generate %s"), *BlueprintName);
			return nullptr;
		}

		// Never overwrite an authored/previous blueprint.
		if (FPackageName::DoesPackageExist(BlueprintPackagePath))
		{
			if (UAnimBlueprint* Existing = LoadObject<UAnimBlueprint>(nullptr, *BlueprintObjectPath))
			{
				UE_LOG(LogTemp, Log, TEXT("AUD-44 ABP: %s already exists; leaving it untouched"), *BlueprintName);
				return Existing;
			}
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: %s exists but failed to load; not overwriting"), *BlueprintName);
			return nullptr;
		}

		// The AnimGraph module registers the AnimBlueprint compiler and node
		// classes; load it before resolving classes by path.
		FModuleManager::Get().LoadModule(TEXT("AnimGraph"));
		UClass* const AnimationGraphClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimationGraph"));
		UClass* const AnimationGraphSchemaClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimationGraphSchema"));
		UClass* const RootNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNode_Root"));
		UClass* const SlotNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNode_Slot"));
		if (!AnimationGraphClass || !AnimationGraphSchemaClass || !RootNodeClass || !SlotNodeClass)
		{
			UE_LOG(LogTemp, Error,
				TEXT("AUD-44 ABP: AnimGraph classes unavailable (graph=%s schema=%s root=%s slot=%s); abandoning"),
				*GetNameSafe(AnimationGraphClass), *GetNameSafe(AnimationGraphSchemaClass),
				*GetNameSafe(RootNodeClass), *GetNameSafe(SlotNodeClass));
			return nullptr;
		}

		UPackage* const Package = CreatePackage(*BlueprintPackagePath);
		if (!Package)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: could not create package %s"), *BlueprintPackagePath);
			return nullptr;
		}

		UAnimBlueprintFactory* const Factory = NewObject<UAnimBlueprintFactory>();
		if (!Factory)
		{
			return nullptr;
		}
		Factory->ParentClass = UQinglanCharacterAnimInstance::StaticClass();
		Factory->TargetSkeleton = Skeleton;

		// Call through the exported UFactory base so MinimalAPI on the concrete
		// factory does not require a derived-method import.
		UObject* const CreatedAsset = static_cast<UFactory*>(Factory)->FactoryCreateNew(
			UAnimBlueprint::StaticClass(), Package, FName(*BlueprintName),
			RF_Public | RF_Standalone, nullptr, GWarn);
		UAnimBlueprint* const AnimBlueprint = Cast<UAnimBlueprint>(CreatedAsset);
		if (!AnimBlueprint)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: factory failed to create %s"), *BlueprintName);
			return nullptr;
		}
		// Factory sets this too; keep it explicit because the compiler needs it.
		AnimBlueprint->TargetSkeleton = Skeleton;
		AnimBlueprint->bIsTemplate = false;

		// Default AnimGraph. The schema implementation creates the Root node.
		UEdGraph* AnimGraph = FindObject<UEdGraph>(AnimBlueprint, TEXT("AnimGraph"));
		if (!AnimGraph)
		{
			AnimGraph = FBlueprintEditorUtils::CreateNewGraph(
				AnimBlueprint, FName(TEXT("AnimGraph")), AnimationGraphClass, AnimationGraphSchemaClass);
			if (!AnimGraph)
			{
				UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: could not create AnimGraph"));
				return nullptr;
			}
			FBlueprintEditorUtils::AddDomainSpecificGraph(AnimBlueprint, AnimGraph);
		}

		UEdGraphNode* RootNode = nullptr;
		for (UEdGraphNode* const Node : AnimGraph->Nodes)
		{
			if (Node && Node->IsA(RootNodeClass))
			{
				RootNode = Node;
				break;
			}
		}
		if (!RootNode)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: AnimGraph has no Root node; abandoning"));
			return nullptr;
		}

		// UEdGraph::CreateNode is protected; FGraphNodeCreator (EdGraph.h) does
		// exactly this sequence and then AddNode. The node class is resolved by
		// reflection, so create it directly instead of the templated creator.
		UEdGraphNode* const SlotNode = NewObject<UEdGraphNode>(AnimGraph, SlotNodeClass);
		if (!SlotNode)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: could not create Slot node"));
			return nullptr;
		}
		SlotNode->NodePosX = 320;
		SlotNode->NodePosY = 0;
		SlotNode->CreateNewGuid();
		SlotNode->PostPlacedNewNode();
		SlotNode->SetFlags(RF_Transactional);
		if (SlotNode->Pins.Num() == 0)
		{
			SlotNode->AllocateDefaultPins();
		}
		AnimGraph->AddNode(SlotNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);

		// Write SlotName through the FAnimNode_Slot struct property without
		// including AnimGraphRuntime headers in this module.
		FStructProperty* const NodeProperty =
			FindFProperty<FStructProperty>(SlotNodeClass, TEXT("Node"));
		FNameProperty* const SlotNameProperty = NodeProperty
			? FindFProperty<FNameProperty>(NodeProperty->Struct, TEXT("SlotName"))
			: nullptr;
		if (!NodeProperty || !SlotNameProperty)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: Slot node reflection lookup failed; abandoning"));
			return nullptr;
		}
		void* const NodeValue = NodeProperty->ContainerPtrToValuePtr<void>(SlotNode);
		SlotNameProperty->SetPropertyValue_InContainer(NodeValue, FName(TEXT("DefaultSlot")));

		UEdGraphPin* const PosePin = SlotNode->FindPin(TEXT("Pose"), EGPD_Output);
		UEdGraphPin* const ResultPin = RootNode->FindPin(TEXT("Result"), EGPD_Input);
		if (!PosePin || !ResultPin)
		{
			UE_LOG(LogTemp, Error,
				TEXT("AUD-44 ABP: pin lookup failed (slot Pose=%s, root Result=%s); abandoning"),
				PosePin ? TEXT("found") : TEXT("missing"),
				ResultPin ? TEXT("found") : TEXT("missing"));
			return nullptr;
		}
		if (!AnimGraph->GetSchema()->TryCreateConnection(PosePin, ResultPin))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: could not connect Slot.Pose -> Root.Result; abandoning"));
			return nullptr;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBlueprint);

		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(AnimBlueprint, EBlueprintCompileOptions::None, &Results);
		UE_LOG(LogTemp, Log,
			TEXT("AUD-44 ABP: compile finished with %d error(s), %d warning(s); parent=%s generated=%s"),
			Results.NumErrors, Results.NumWarnings,
			*GetNameSafe(AnimBlueprint->ParentClass.Get()), *GetNameSafe(AnimBlueprint->GeneratedClass));
		if (Results.NumErrors > 0
			|| AnimBlueprint->ParentClass.Get() != UQinglanCharacterAnimInstance::StaticClass()
			|| !AnimBlueprint->GeneratedClass
			|| !AnimBlueprint->GeneratedClass->IsChildOf(UQinglanCharacterAnimInstance::StaticClass())
			|| AnimBlueprint->TargetSkeleton.Get() != Skeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: compile validation failed; asset not saved"));
			return nullptr;
		}

		UPackage* const OutermostPackage = AnimBlueprint->GetOutermost();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			OutermostPackage->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(OutermostPackage, AnimBlueprint, *FileName, SaveArgs))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: failed to save %s"), *FileName);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(AnimBlueprint);

		// Disk proof: the generated class and slot name must be in the package.
		TArray<uint8> PackageBytes;
		if (!FFileHelper::LoadFileToArray(PackageBytes, *FileName))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: could not read back %s"), *FileName);
			return nullptr;
		}
		auto ContainsToken = [&PackageBytes](const char* Token)
		{
			const SIZE_T TokenLen = FPlatformString::Strlen(Token);
			for (int32 Offset = 0; Offset + static_cast<SIZE_T>(TokenLen) <= static_cast<SIZE_T>(PackageBytes.Num()); ++Offset)
			{
				if (FMemory::Memcmp(PackageBytes.GetData() + Offset, Token, TokenLen) == 0)
				{
					return true;
				}
			}
			return false;
		};
		const FTCHARToUTF8 GeneratedNameANSI(*(BlueprintName + TEXT("_C")));
		const FTCHARToUTF8 GeneratedTypeANSI(TEXT("AnimBlueprintGeneratedClass"));
		const FTCHARToUTF8 SlotNameANSI(TEXT("DefaultSlot"));
		if (!ContainsToken(GeneratedNameANSI.Get()) || !ContainsToken(GeneratedTypeANSI.Get()) || !ContainsToken(SlotNameANSI.Get()))
		{
			UE_LOG(LogTemp, Error,
				TEXT("AUD-44 ABP: disk verification failed for %s (class=%d type=%d slot=%d)"),
				*FileName, ContainsToken(GeneratedNameANSI.Get()) ? 1 : 0,
				ContainsToken(GeneratedTypeANSI.Get()) ? 1 : 0, ContainsToken(SlotNameANSI.Get()) ? 1 : 0);
			return nullptr;
		}

		UE_LOG(LogTemp, Log, TEXT("AUD-44 ABP: %s generated, compiled, saved to %s"), *BlueprintName, *FileName);
		return AnimBlueprint;
	}

	/** Post-save contract check for a generated slot blueprint. */
	bool VerifySlotAnimBlueprint(const USkeleton* ExpectedSkeleton,
		const FString& BlueprintObjectPath, const FString& BlueprintClassPath)
	{
		UAnimBlueprint* const Loaded = LoadObject<UAnimBlueprint>(nullptr, *BlueprintObjectPath);
		if (!Loaded)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP verify: could not load %s"), *BlueprintObjectPath);
			return false;
		}
		if (Loaded->ParentClass.Get() != UQinglanCharacterAnimInstance::StaticClass())
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP verify: parent class is %s, expected UQinglanCharacterAnimInstance"),
				*GetNameSafe(Loaded->ParentClass.Get()));
			return false;
		}
		if (!Loaded->GeneratedClass || !Loaded->GeneratedClass->IsChildOf(UQinglanCharacterAnimInstance::StaticClass()))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP verify: missing/invalid GeneratedClass"));
			return false;
		}
		const UAnimBlueprintGeneratedClass* const GeneratedClass =
			Cast<UAnimBlueprintGeneratedClass>(Loaded->GeneratedClass);
		if (!GeneratedClass || GeneratedClass->GetTargetSkeleton() != ExpectedSkeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP verify: target skeleton mismatch (%s)"),
				*GetNameSafe(GeneratedClass ? GeneratedClass->GetTargetSkeleton() : nullptr));
			return false;
		}
		if (!LoadClass<UQinglanCharacterAnimInstance>(nullptr, *BlueprintClassPath))
		{
			UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP verify: runtime class path does not load: %s"),
				*BlueprintClassPath);
			return false;
		}
		UE_LOG(LogTemp, Log, TEXT("AUD-44 ABP verify: %s ok (generated class %s)"),
			*BlueprintObjectPath, *GetNameSafe(Loaded->GeneratedClass));
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
	// Refresh existing generated assets in place: matching skeleton/slot alone
	// must not keep a montage referencing the obsolete static action take.
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *PackagePath);
	UPackage* Package = Montage ? Montage->GetOutermost() : CreatePackage(*PackagePath);
	if (Package == nullptr)
	{
		return nullptr;
	}
	if (!Montage)
	{
		Montage = NewObject<UAnimMontage>(Package, *MontageName, RF_Public | RF_Standalone | RF_Transactional);
	}
	if (Montage == nullptr)
	{
		return nullptr;
	}
	Montage->SlotAnimTracks.Reset();
	Montage->Notifies.Reset();

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
	if (Montage->CompositeSections.IsEmpty())
	{
		Montage->AddAnimCompositeSection(TEXT("Default"), 0.f);
	}
	if (MontageName.StartsWith(TEXT("AM_Human_SwordCut_")))
	{
		Montage->BlendIn.SetBlendTime(0.05f);
		Montage->BlendOut.SetBlendTime(0.08f);
	}

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

	// The asset registry is needed both by the procedural asset creation below
	// (FAssetRegistryModule::AssetCreated) and by the montage discovery pass.
	FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	// --- Optional diagnostic placeholder authoring, never the default. ---
	// These use conventions matching the existing SK_HumanHuman_Armature_Idle
	// asset, so UQinglanCharacterAnimInstance picks them up automatically.
	// The engine recipe (asset controller + SetModel + InitializeModel) makes
	// the procedural authoring path work; existing assets are left untouched so
	// a source take can replace them. The required-source validation below
	// rejects placeholders as runtime acceptance evidence.
	const bool bAuthorProceduralLocomotion = FParse::Param(*Params, TEXT("procedural-locomotion"));
	USkeleton* const HumanSkeleton = LoadHumanSkeleton();
	if (FParse::Param(*Params, TEXT("repair-combat-assets")))
	{
		// Repair existing assets in place, retaining their original takes and
		// technique references. No mesh import or AnimBlueprint regeneration.
		AssetRegistry.Get().SearchAllAssets(true);
		TArray<FAssetData> Assets;
		AssetRegistry.Get().GetAssetsByClass(UAnimMontage::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/Qinglan/Characters/"))) { continue; }
			UAnimMontage* Montage = Cast<UAnimMontage>(Asset.GetAsset());
			if (!Montage) { bOk = false; continue; }
			if (Montage->CompositeSections.IsEmpty()) { Montage->AddAnimCompositeSection(TEXT("Default"), 0.f); }
			if (Montage->GetName().StartsWith(TEXT("AM_Human_SwordCut_")))
			{
				Montage->BlendIn.SetBlendTime(0.05f);
				Montage->BlendOut.SetBlendTime(0.08f);
				const int32 Cut = Montage->GetName().EndsWith(TEXT("1")) ? 0 : Montage->GetName().EndsWith(TEXT("2")) ? 1 : 2;
				const float Windup[] = {0.14f, 0.16f, 0.22f};
				const float Active[] = {0.10f, 0.10f, 0.12f};
				const float Total[] = {0.46f, 0.50f, 0.66f};
				for (FAnimNotifyEvent& Notify : Montage->Notifies)
				{
					if (Notify.NotifyName == FName(TEXT("QinglanSwordWindowOpen"))) { Notify.Link(Montage, Montage->GetPlayLength() * Windup[Cut] / Total[Cut]); }
					if (Notify.NotifyName == FName(TEXT("QinglanSwordWindowClosed"))) { Notify.Link(Montage, Montage->GetPlayLength() * (Windup[Cut] + Active[Cut]) / Total[Cut]); }
				}
			}
			bOk = SaveMontage(Montage, Asset.PackageName.ToString()) != nullptr && bOk;
		}
		bOk = RepairHumanBladeSocket(HumanSkeleton) && bOk;
		return bOk ? 0 : 1;
	}
	if (bAuthorProceduralLocomotion && HumanSkeleton)
	{
		const TCHAR* const WalkName = TEXT("SK_HumanHuman_Armature_Walk");
		const TCHAR* const RunName = TEXT("SK_HumanHuman_Armature_Run");
		UAnimSequence* const WalkSequence =
			GenerateProceduralLocomotionSequence(HumanSkeleton, WalkName, /*NumFrames=*/30,
				/*ThighSwingDegrees=*/25.f, /*ArmSwingDegrees=*/18.f, TEXT("walk"));
		UAnimSequence* const RunSequence =
			GenerateProceduralLocomotionSequence(HumanSkeleton, RunName, /*NumFrames=*/18,
				/*ThighSwingDegrees=*/40.f, /*ArmSwingDegrees=*/28.f, TEXT("run"));
		if (!WalkSequence || !RunSequence)
		{
			bOk = false;
		}
		if (!VerifyProceduralLocomotionSequence(WalkName, HumanSkeleton))
		{
			bOk = false;
		}
		if (!VerifyProceduralLocomotionSequence(RunName, HumanSkeleton))
		{
			bOk = false;
		}
	}
	else if (!bAuthorProceduralLocomotion)
	{
		UE_LOG(LogTemp, Log, TEXT("AUD-44: procedural Walk/Run authoring skipped (opt-in -procedural-locomotion)"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("AUD-44: Human skeleton unavailable; procedural Walk/Run generation skipped"));
	}

	// --- AUD-44: minimal Root + DefaultSlot AnimBlueprint. ---
	// This is independent of the locomotion placeholder flag: without a slot
	// node in a compiled graph, every action montage stays invisible.
	if (HumanSkeleton)
	{
		UAnimBlueprint* const AnimBlueprint = GenerateSlotAnimBlueprint(HumanSkeleton,
			HumanAnimBlueprintName, HumanAnimBlueprintPackagePath, HumanAnimBlueprintObjectPath);
		if (!AnimBlueprint || !VerifySlotAnimBlueprint(HumanSkeleton,
				HumanAnimBlueprintObjectPath, HumanAnimBlueprintClassPath))
		{
			bOk = false;
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("AUD-44 ABP: Human skeleton unavailable; ABP_Human generation skipped"));
	}

	// AUD-44: non-Human enemies span several rigs (Knight, Slime, Skeleton,
	// Bat, Dragon). A slot graph is skeleton-specific, so author one sibling
	// ABP_<MeshBase> per rig beside its mesh. EnemyCharacter resolves it by
	// deriving this same path from the mesh package.
	{
		TArray<FAssetData> CharacterMeshes;
		AssetRegistry.Get().GetAssetsByClass(
			FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh")), CharacterMeshes, /*bSearchSubClasses=*/false);
		for (const FAssetData& Asset : CharacterMeshes)
		{
			const FString PackageName = Asset.PackageName.ToString();
			if (!PackageName.StartsWith(TEXT("/Game/Qinglan/Characters/")) || PackageName.Contains(TEXT("/Montages/")))
			{
				continue;
			}
			const FString MeshName = Asset.AssetName.ToString();
			if (MeshName == TEXT("SK_Human") || MeshName.Contains(TEXT("Armature")))
			{
				continue; // player rig is covered by ABP_Human
			}
			USkeletalMesh* const Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
			USkeleton* const MeshSkeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
			if (!MeshSkeleton || MeshSkeleton == HumanSkeleton)
			{
				continue;
			}
			FString AbpName = MeshName;
			AbpName.RemoveFromStart(TEXT("SK_"));
			AbpName = FString::Printf(TEXT("ABP_%s"), *AbpName);
			const FString Folder = FPackageName::GetLongPackagePath(PackageName);
			const FString AbpPackage = Folder / AbpName;
			const FString AbpObject = FString::Printf(TEXT("%s.%s"), *AbpPackage, *AbpName);
			const FString AbpClass = FString::Printf(TEXT("%s.%s_C"), *AbpPackage, *AbpName);
			UAnimBlueprint* const AnimBlueprint = GenerateSlotAnimBlueprint(MeshSkeleton,
				AbpName, AbpPackage, AbpObject);
			if (!AnimBlueprint || !VerifySlotAnimBlueprint(MeshSkeleton, AbpObject, AbpClass))
			{
				UE_LOG(LogTemp, Error, TEXT("AUD-44 ABP: %s for %s failed"), *AbpName, *MeshName);
				bOk = false;
			}
		}
	}

	// --- AUD-44: weapon/cast sockets on the Human skeleton ------------------
	// The sword sweep and technique casting resolve these names on the mesh;
	// skeleton sockets are inherited by every mesh using the skeleton.
	if (HumanSkeleton)
	{
		struct FSockSpec { const TCHAR* Name; const TCHAR* Bone; FVector Location; };
		const FSockSpec Sockets[] = {
			{ TEXT("sword_base"), TEXT("RightHand"), FVector(0.f, 0.f, 0.f) },
			{ TEXT("sword_tip"), TEXT("RightHand"), FVector(0.f, 0.f, 60.f) },
			{ TEXT("cast_source"), TEXT("RightHand"), FVector(0.f, 0.f, 0.f) },
		};
		bool bSocketsChanged = false;
		for (const FSockSpec& Spec : Sockets)
		{
			const FName SocketName(Spec.Name);
			const bool bExists = HumanSkeleton->Sockets.ContainsByPredicate(
				[&SocketName](const USkeletalMeshSocket* Existing)
				{
					return Existing && Existing->SocketName == SocketName;
				});
			if (bExists)
			{
				continue;
			}
			USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(HumanSkeleton);
			Socket->SocketName = SocketName;
			Socket->BoneName = FName(Spec.Bone);
			Socket->RelativeLocation = Spec.Location;
			Socket->RelativeRotation = FRotator::ZeroRotator;
			HumanSkeleton->Sockets.Add(Socket);
			bSocketsChanged = true;
			UE_LOG(LogTemp, Log, TEXT("AUD-44: authored skeleton socket %s on %s"), Spec.Name, Spec.Bone);
		}
		if (bSocketsChanged)
		{
			HumanSkeleton->PreEditChange(nullptr);
			HumanSkeleton->PostEditChange();
			UPackage* const SkeletonPackage = HumanSkeleton->GetOutermost();
			const FString SkeletonFileName = FPackageName::LongPackageNameToFilename(
				SkeletonPackage->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(SkeletonPackage, HumanSkeleton, *SkeletonFileName, Args))
			{
				UE_LOG(LogTemp, Error, TEXT("AUD-44: failed to save skeleton sockets"));
				bOk = false;
			}
		}
		// Contract verification: every consumer (sword sweep, cast aim) resolves
		// these through USkeletalMeshComponent, so the skeleton must report them.
		for (const FSockSpec& Spec : Sockets)
		{
			const USkeletalMeshSocket* const Socket = HumanSkeleton->FindSocket(FName(Spec.Name));
			if (Socket == nullptr || Socket->BoneName != FName(Spec.Bone))
			{
				UE_LOG(LogTemp, Error, TEXT("AUD-44 verify: socket %s missing or not on bone %s"),
					Spec.Name, Spec.Bone);
				bOk = false;
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("AUD-44 verify: socket %s on %s at %s ok"),
					Spec.Name, Spec.Bone, *Socket->RelativeLocation.ToString());
			}
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("AUD-44: Human skeleton unavailable; sockets not authored"));
		bOk = false;
	}

	// --- Discover imported anim sequences by character group. ---
	bOk = RepairHumanBladeSocket(HumanSkeleton) && bOk;
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
	FGroupPlan& HumanPlan = GroupOf(TEXT("Human"), true);
	for (const QinglanHumanAnimation::FSourceTake& Take : QinglanHumanAnimation::RequiredTakes)
	{
		UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *QinglanHumanAnimation::ObjectPath(Take.Suffix));
		FString Reason;
		if (!QinglanHumanAnimation::Validate(Sequence, HumanSkeleton, Take, Reason))
		{
			UE_LOG(LogTemp, Error, TEXT("Required Human source take %s invalid: %s"), Take.Suffix, *Reason);
			bOk = false;
			continue;
		}
		if (FString(Take.Suffix) == TEXT("Punch"))
		{
			// The pack has no authored sword/cast clip. Punch is an explicit
			// moving cosmetic approximation; neither ArmatureAction take is
			// valid here (.001 is static, .002 duplicates Run).
			HumanPlan.Takes.Add(FName(TEXT("SwordCut_1")), Sequence);
			HumanPlan.Takes.Add(FName(TEXT("SwordCut_2")), Sequence);
			HumanPlan.Takes.Add(FName(TEXT("SwordCut_3")), Sequence);
			HumanPlan.Takes.Add(FName(TEXT("Cast")), Sequence);
		}
		else if (FString(Take.Suffix) == TEXT("Jump"))
		{
			HumanPlan.Takes.Add(FName(TEXT("Dodge")), Sequence);
		}
	}
	if (!bOk)
	{
		return 1; // A missing source action must not regenerate static fallback montages.
	}

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
		if (bPlayer) { continue; } // Human sources are validated and selected explicitly above.
		const FTakeRule* Rules = MonsterRules;
		const int32 RuleCount = UE_ARRAY_COUNT(MonsterRules);

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
		// Every group's data asset references AM_<Group>_HitReact. Human and
		// Knight rigs ship no hurt take, so clone the closest body motion
		// (player dodge jump / knight attack) into a hit-react montage instead
		// of leaving the catalog path dangling.
		if (!Plan.Takes.Contains(FName(TEXT("HitReact"))))
		{
			const TCHAR* FallbackSuffixes[] = { TEXT("Dodge"), TEXT("Attack"), TEXT("SwordCut_1") };
			for (const TCHAR* Fallback : FallbackSuffixes)
			{
				if (const TObjectPtr<UAnimSequence>* Source = Plan.Takes.Find(FName(Fallback)))
				{
					Plan.Takes.FindOrAdd(FName(TEXT("HitReact"))) = *Source;
					UE_LOG(LogTemp, Log, TEXT("AUD-44 montage: %s HitReact cloned from %s"),
						*Plan.Group, Fallback);
					break;
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
				const int32 CutIndex = Suffix.EndsWith(TEXT("1")) ? 0 : (Suffix.EndsWith(TEXT("2")) ? 1 : 2);
				const float Windup[] = {0.14f, 0.16f, 0.22f};
				const float Active[] = {0.10f, 0.10f, 0.12f};
				const float Total[] = {0.46f, 0.50f, 0.66f};
				WindupFraction = Windup[CutIndex] / Total[CutIndex];
				ActiveFraction = Active[CutIndex] / Total[CutIndex];
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
