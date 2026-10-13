// Anim notifies authored onto bootstrapped combat montages. The sword
// component's phase timers stay authoritative; these only open/close the
// cosmetic-accurate blade sweep window (docs/02 section 4).
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "QinglanAnimNotifies.generated.h"

class UQinglanSwordComponent;

/**
 * Opens the blade sweep hit window on the owning player character's sword
 * component. Degrades silently when the owner has no sword component (enemy
 * previews, test worlds).
 */
UCLASS()
class UQinglanAnimNotify_SwordWindowOpen : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
};

/** Closes the sweep window early; the component's recovery timer is the backstop. */
UCLASS()
class UQinglanAnimNotify_SwordWindowClosed : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
};
