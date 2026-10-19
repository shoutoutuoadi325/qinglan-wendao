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
class UNiagaraComponent;
class UNiagaraSystem;

/**
 * Plays pooled, quality-scaled cosmetic cues. One component per owning actor;
 * world cues (ground strikes, portals) spawn via the world cue pool.
 *
 * Cue anatomy per docs/04 art contract: anticipation glow -> release burst ->
 * travel trail -> impact ring -> decay. Gameplay timings drive lifetimes.
 */
UCLASS(ClassGroup = (Qinglan), meta = (BlueprintSpawnableComponent))
class QINGLANASCENSION_API UQinglanVFXCueComponent : public UActorComponent // linkage-only export for editor automation specs
{
	GENERATED_BODY()

public:
	UQinglanVFXCueComponent();

	/** Primary cue entry point used by techniques/sword/AI. */
	UFUNCTION(BlueprintCallable)
	void PlayCue(FName CueId, const FTransform& Source, FVector TargetPoint, EQinglanElement Element, float Intensity = 1.f);

	/** Telegraph ring: persistent ground outline, min 0.9 s, survives Low quality.
	 *  AUD-42: also spawns a quality-independent ground-center disc so the danger
	 *  area is readable even when the decal material or Niagara content is absent. */
	UFUNCTION(BlueprintCallable)
	void PlayTelegraph(FVector Center, float Radius, float Duration, EQinglanElement Element);

	/** AUD-42: readable safe-lane marker for ring/ember telegraphs (placeholder
	 *  stretched shape; no light, so Low quality and low-flash keep it visible).
	 *  SafeLaneDegrees is the world-space yaw the mechanic treats as safe. */
	UFUNCTION(BlueprintCallable)
	void PlaySafeLane(FVector Center, float Radius, float SafeLaneDegrees, float HalfWidthDegrees,
		float Duration, EQinglanElement Element);

	/** Pooled reset contract: clears timers, owners, tags, lifetimes. */
	void ResetForPool();

	/** AUD-43 automation/telemetry: live cue instance and light counts.
	 *  Both return to zero after decay or ResetForPool. */
	int32 GetActiveCueCount() const { return ActiveCues.Num(); }
	int32 GetActiveLightCount() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// Plain struct (not a USTRUCT): runtime-only cue bookkeeping with weak
	// pointers, so no UPROPERTY reflection is needed or allowed for it.
	struct FCueInstance
	{
		TWeakObjectPtr<USceneComponent> Root;
		TWeakObjectPtr<UPointLightComponent> Light;
		/** AUD-42: optional real Niagara component; weak so auto-destruction
		 *  (bAutoDestroy) can never leave a dangling pointer. */
		TWeakObjectPtr<UNiagaraComponent> Niagara;
		float Remaining = 0.f;
		float Total = 0.f;
		float BaseRadius = 0.f;
		/** Authored light intensity; decay scales from this, never compounds. */
		float LightBaseIntensity = 0.f;
		bool bRing = false;
	};
	void DecayCues(float DeltaTime);
	/** Loads a bootstrap material with engine-default fallback. */
	class UMaterialInterface* GetCueMaterial(FName MaterialKey);
	FLinearColor ElementColor(EQinglanElement Element) const;

	/** AUD-42: resolves/loads the authored Niagara system for a cue category
	 *  (impact / telegraph / cast). Caches misses too; absence is not an
	 *  error, the placeholder shapes remain authoritative for presentation. */
	UNiagaraSystem* ResolveNiagaraSystemForCue(FName CueId);
	/** Spawns the category system at Source and sets exposed user params when
	 *  present. Bounded by MaxActiveNiagaraCues; failure logs and returns. */
	void SpawnNiagaraForCue(FName CueId, const FTransform& Source, FVector TargetPoint,
		EQinglanElement Element, float Intensity, TWeakObjectPtr<UNiagaraComponent>& OutNiagara);
	void ApplyNiagaraElementParameters(UNiagaraComponent* NiagaraComponent,
		UNiagaraSystem* System, EQinglanElement Element, float Intensity) const;
	int32 GetActiveNiagaraCount() const;

	TArray<FCueInstance> ActiveCues;
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UMaterialInterface>> CachedMaterials;
	/** Category -> loaded system (null entries cache a known miss). */
	UPROPERTY(Transient) TMap<FName, TObjectPtr<UNiagaraSystem>> CachedNiagaraSystems;
};
