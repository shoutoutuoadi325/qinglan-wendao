#include "Combat/QinglanAnimNotifies.h"

#include "Combat/QinglanSwordComponent.h"
#include "Components/SkeletalMeshComponent.h"

namespace
{
	UQinglanSwordComponent* ResolveSword(USkeletalMeshComponent* MeshComp)
	{
		AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
		return Owner ? Owner->FindComponentByClass<UQinglanSwordComponent>() : nullptr;
	}
}

void UQinglanAnimNotify_SwordWindowOpen::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	if (UQinglanSwordComponent* Sword = ResolveSword(MeshComp))
	{
		Sword->NotifyWindowOpen();
	}
}

void UQinglanAnimNotify_SwordWindowClosed::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	if (UQinglanSwordComponent* Sword = ResolveSword(MeshComp))
	{
		Sword->NotifyWindowClosed();
	}
}
