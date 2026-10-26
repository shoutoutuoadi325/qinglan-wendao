#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "QinglanHumanAnimationContract.h"
#include "Combat/QinglanCharacterAnimInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHeroLocomotionAssets,
	"Qinglan.Animation.HeroLocomotionAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanHeroLocomotionAssets::RunTest(const FString& Parameters)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
	UClass* Blueprint = LoadClass<UQinglanCharacterAnimInstance>(nullptr,
		TEXT("/Game/Qinglan/Characters/Human/ABP_Human.ABP_Human_C"));
	if (!TestNotNull(TEXT("Shipped hero mesh loads"), Mesh) ||
		!TestNotNull(TEXT("Shipped hero animation class loads"), Blueprint))
	{
		return false;
	}
	const UAnimBlueprintGeneratedClass* Generated = Cast<UAnimBlueprintGeneratedClass>(Blueprint);
	if (!TestNotNull(TEXT("Hero class is a compiled animation blueprint"), Generated))
	{
		return false;
	}
	TestTrue(TEXT("Hero animation graph targets the actual mesh skeleton"),
		Generated->GetTargetSkeleton() == Mesh->GetSkeleton());
	const UQinglanCharacterAnimInstance* Defaults = Blueprint->GetDefaultObject<UQinglanCharacterAnimInstance>();
	const TSoftObjectPtr<UAnimSequence> Takes[] = {
		Defaults->DefaultIdleAnimationPath, Defaults->DefaultWalkAnimationPath,
		Defaults->DefaultRunAnimationPath, Defaults->DefaultAirborneAnimationPath,
	};
	const TCHAR* Labels[] = { TEXT("Idle"), TEXT("Walk"), TEXT("Run"), TEXT("Airborne") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Takes); ++Index)
	{
		UAnimSequence* Sequence = Takes[Index].LoadSynchronous();
		if (!TestNotNull(FString::Printf(TEXT("%s loads through the runtime soft reference"), Labels[Index]), Sequence))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s skeleton matches the hero"), Labels[Index]),
			Sequence->GetSkeleton() == Mesh->GetSkeleton());
		TestTrue(FString::Printf(TEXT("%s has a playable duration"), Labels[Index]), Sequence->GetPlayLength() > 0.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHeroOriginalSourceTakes,
	"Qinglan.Animation.HeroOriginalSourceTakes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanHeroOriginalSourceTakes::RunTest(const FString& Parameters)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
	if (!TestNotNull(TEXT("Hero mesh loads"), Mesh)) { return false; }
	for (const QinglanHumanAnimation::FSourceTake& Take : QinglanHumanAnimation::RequiredTakes)
	{
		UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *QinglanHumanAnimation::ObjectPath(Take.Suffix));
		FString Reason;
		const bool bValid = QinglanHumanAnimation::Validate(Sequence, Mesh->GetSkeleton(), Take, Reason);
		TestTrue(FString::Printf(TEXT("%s is the original changing 30 Hz take: %s"), Take.Suffix, *Reason),
			bValid);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHeroActionSourceSelection,
	"Qinglan.Animation.HeroActionSourceSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanHeroActionSourceSelection::RunTest(const FString& Parameters)
{
	UAnimSequence* Punch = LoadObject<UAnimSequence>(nullptr, *QinglanHumanAnimation::ObjectPath(TEXT("Punch")));
	if (!TestNotNull(TEXT("Original Punch loads"), Punch)) { return false; }
	for (const TCHAR* Suffix : { TEXT("SwordCut_1"), TEXT("SwordCut_2"), TEXT("SwordCut_3"), TEXT("Cast") })
	{
		const FString Name = FString(TEXT("AM_Human_")) + Suffix;
		const FString Path = TEXT("/Game/Qinglan/Characters/Human/Montages/") + Name + TEXT(".") + Name;
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *Path);
		if (!TestNotNull(*Name, Montage)) { continue; }
		if (!TestEqual(*FString::Printf(TEXT("%s has one action track"), *Name), Montage->SlotAnimTracks.Num(), 1)) { continue; }
		const TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments;
		if (!TestEqual(*FString::Printf(TEXT("%s has one source segment"), *Name), Segments.Num(), 1)) { continue; }
		TestTrue(*FString::Printf(TEXT("%s uses Punch, never generic ArmatureAction"), *Name),
			Segments[0].GetAnimReference() == Punch);
		TestTrue(*FString::Printf(TEXT("%s contains a moving source take"), *Name),
			QinglanHumanAnimation::HasChangingBoneTrack(Cast<UAnimSequence>(Segments[0].GetAnimReference())));
	}
	return true;
}
#endif
