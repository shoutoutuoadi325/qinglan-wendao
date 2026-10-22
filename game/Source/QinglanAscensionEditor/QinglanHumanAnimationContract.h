#pragma once

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Factories/FbxAnimSequenceImportData.h"

// Required runtime takes from the original FBX. ArmatureAction.001 is
// constant over 1/24 s; ArmatureAction.002 duplicates Run and is not an attack.
namespace QinglanHumanAnimation
{
	struct FSourceTake { const TCHAR* Suffix; float Seconds; };
	inline const FSourceTake RequiredTakes[] = {
		{ TEXT("Idle"), 10.f }, { TEXT("Jump"), 1.f },
		{ TEXT("Walk"), 31.f / 30.f }, { TEXT("Run"), 19.f / 30.f },
		{ TEXT("Punch"), 31.f / 30.f },
	};

	inline FString AssetName(const TCHAR* Suffix)
	{
		return FString(TEXT("SK_HumanHuman_Armature_")) + Suffix;
	}

	inline FString ObjectPath(const TCHAR* Suffix)
	{
		const FString Name = AssetName(Suffix);
		return TEXT("/Game/Qinglan/Characters/Human/") + Name + TEXT(".") + Name;
	}

	inline bool HasChangingBoneTrack(const UAnimSequence* Sequence)
	{
		const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
		if (!Model) { return false; }
		TArray<FName> Names;
		Model->GetBoneTrackNames(Names);
		for (FName Name : Names)
		{
			TArray<FTransform> Transforms;
			Model->GetBoneTrackTransforms(Name, Transforms);
			for (int32 Index = 1; Index < Transforms.Num(); ++Index)
			{
				if (!Transforms[Index].Equals(Transforms[0], 1.e-4f)) { return true; }
			}
		}
		return false;
	}

	inline bool Validate(const UAnimSequence* Sequence, const USkeleton* Skeleton,
		const FSourceTake& Take, FString& Reason)
	{
		const UFbxAnimSequenceImportData* Import = Sequence
			? Cast<UFbxAnimSequenceImportData>(Sequence->AssetImportData) : nullptr;
		const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
		const bool bMotion = HasChangingBoneTrack(Sequence);
		const FString Metrics = FString::Printf(TEXT("expected changing 30 Hz take %.6fs; got %.3f Hz, %d frames, %.6fs, motion=%d"),
			Take.Seconds, Model ? Model->GetFrameRate().AsDecimal() : 0.0,
			Model ? Model->GetNumberOfFrames() : 0, Sequence ? Sequence->GetPlayLength() : 0.f, bMotion);
		if (!Sequence || !Skeleton || Sequence->GetSkeleton() != Skeleton)
		{
			Reason = TEXT("missing sequence or mismatched Human skeleton; ") + Metrics;
			return false;
		}
		if (!Import || Import->SourceAnimationName != FString(TEXT("Human Armature|")) + Take.Suffix)
		{
			Reason = TEXT("sequence does not identify the required original FBX take; ") + Metrics;
			return false;
		}
		if (!Model || !FMath::IsNearlyEqual(Model->GetFrameRate().AsDecimal(), 30.0, 0.001)
			|| !FMath::IsNearlyEqual(Sequence->GetPlayLength(), Take.Seconds, 0.001f)
			|| !bMotion)
		{
			Reason = Metrics;
			return false;
		}
		Reason = Metrics;
		return true;
	}
}
