// VFX cue component: cosmetic cue selection, pooling, and quality tiers.
// Gameplay NEVER depends on these cues: removing an effect never cancels its
// gameplay effect (docs/05 section 4). Cues are element-colored, telegraphs
// keep their silhouette at Low quality and in low-flash mode (docs/06).
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/QinglanCoreTypes.h"
#include "VFX/QinglanVFXCueTypes.h"
#include "QinglanVFXCueComponent.generated.h"

class UDecalComponent;
class UPointLightComponent;
class UStaticMeshComponent;
class UNiagaraSystem;

/**
 * Plays pooled, quality-scaled cosmetic cues. One component per owning actor;
 * world cues (ground strikes, portals) spawn via the world cue pool.
 *
 * Cue anatomy per docs/04 art contract: anticipation glow -> release burst ->
 * travel trail -> impact ring -> decay. Gameplay timings drive lifetimes.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class UQinglanVFXCueComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UQinglanVFXCueComponent();

	/** Primary cue entry point used by techniques/sword/AI. */
	UFUNCTION(BlueprintCallable)
	void PlayCue(FName CueId, const FTransform& Source, FVector TargetPoint, EQinglanElement Element, float Intensity = 1.f);

	/** Telegraph ring: persistent ground outline, min 0.9 s, survives Low quality. */
	UFUNCTION(BlueprintCallable)
	void PlayTelegraph(FVector Center, float Radius, float Duration, EQinglanElement Element);

	/** Pooled reset contract: clears timers, owners, tags, lifetimes. */
	void ResetForPool();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// Plain struct (not a USTRUCT): runtime-only cue bookkeeping with weak
	// pointers, so no UPROPERTY reflection is needed or allowed for it.
	struct FCueInstance
	{
		TWeakObjectPtr<USceneComponent> Root;
		TWeakObjectPtr<UPointLightComponent> Light;
		float Remaining = 0.f;
		float Total = 0.f;
		float BaseRadius = 0.f;
		bool bRing = false;
	};
	void DecayCues(float DeltaTime);
	/** Loads a bootstrap material with engine-default fallback. */
	class UMaterialInterface* GetCueMaterial(FName MaterialKey);
	FLinearColor ElementColor(EQinglanElement Element) const;

	TArray<FCueInstance> ActiveCues;
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UMaterialInterface>> CachedMaterials;
};
