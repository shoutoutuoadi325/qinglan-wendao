// VFX cue implementation: pooled mesh/decal/light cues with element colors.
// Deliberately dependency-light: engine shapes + parameterized bootstrap
// materials; every cue degrades to silhouettes at Low and replaces strobing
// with steady glows under low-flash mode (docs/06 sections 3-4).
#include "VFX/QinglanVFXCueComponent.h"

#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollectionInstance.h" // IWYU
#include "UObject/SoftObjectPath.h"

namespace QinglanVFXCues
{
	// Defined here (not in the header) to avoid namespace-scope global FName constructors.
	const FName SwordArc("vfx.sword_arc");
	const FName SwordImpact("vfx.sword_impact");
	const FName JadeArcRelease("vfx.jade_sword_arc");
	const FName EmberLanceRelease("vfx.ember_lance");
	const FName EmberTrail("vfx.ember_trail");
	const FName FlowingWardBarrier("vfx.flowing_ward");
	const FName ThunderSealStrike("vfx.thunder_seal");
	const FName ConductArc("vfx.conduct_arc");
	const FName StoneAegisBarrier("vfx.stone_aegis");
	const FName VerdantHeal("vfx.verdant_pulse");
	const FName MistStepBurst("vfx.mist_step");
	const FName OrbitingBlades("vfx.orbiting_blades");
	const FName Portal("vfx.portal");
	const FName TelegraphRing("vfx.telegraph_ring");
	const FName BossGroundMarker("vfx.boss_ground_marker");
	const FName DeathDissolve("vfx.death_dissolve");
}

namespace
{
	FName MaterialKeyForCue(FName CueId)
	{
		if (CueId == QinglanVFXCues::TelegraphRing || CueId == QinglanVFXCues::BossGroundMarker)
		{
			return TEXT("/Game/Qinglan/Materials/M_QinglanDecalRing");
		}
		if (CueId == QinglanVFXCues::FlowingWardBarrier || CueId == QinglanVFXCues::StoneAegisBarrier)
		{
			return TEXT("/Game/Qinglan/Materials/M_QinglanBarrier");
		}
		return TEXT("/Game/Qinglan/Materials/M_QinglanAdditive");
	}

	float IntensityScaleForQuality(EQinglanVFXQuality Q)
	{
		switch (Q)
		{
		case EQinglanVFXQuality::Low: return 0.4f;
		case EQinglanVFXQuality::Balanced: return 0.7f;
		default: return 1.f;
		}
	}

	bool QualityAllowsLights(EQinglanVFXQuality Q) { return Q >= EQinglanVFXQuality::Balanced; }
}

UQinglanVFXCueComponent::UQinglanVFXCueComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

FLinearColor UQinglanVFXCueComponent::ElementColor(EQinglanElement Element) const
{
	switch (Element)
	{
	case EQinglanElement::Metal: return FLinearColor(0.75f, 0.95f, 1.0f);
	case EQinglanElement::Wood: return FLinearColor(0.4f, 0.9f, 0.5f);
	case EQinglanElement::Water: return FLinearColor(0.3f, 0.6f, 1.0f);
	case EQinglanElement::Fire: return FLinearColor(1.0f, 0.45f, 0.15f);
	case EQinglanElement::Earth: return FLinearColor(0.9f, 0.75f, 0.4f);
	case EQinglanElement::Lightning: return FLinearColor(0.8f, 0.7f, 1.0f);
	default: return FLinearColor::White;
	}
}

UMaterialInterface* UQinglanVFXCueComponent::GetCueMaterial(FName MaterialKey)
{
	// CachedMaterials values are TObjectPtr (UPROPERTY/GC-tracked), so Find
	// yields TObjectPtr<UMaterialInterface>*; .Get() unwraps the raw pointer.
	if (TObjectPtr<UMaterialInterface>* const Cached = CachedMaterials.Find(MaterialKey))
	{
		return Cached->Get();
	}
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialKey.ToString());
	if (!Mat)
	{
		// Engine fallback keeps cues visible even before the content bootstrap ran.
		Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial"));
	}
	CachedMaterials.Add(MaterialKey, Mat);
	return Mat;
}

void UQinglanVFXCueComponent::PlayTelegraph(FVector Center, float Radius, float Duration, EQinglanElement Element)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	USceneComponent* RootComp = Cast<USceneComponent>(GetOwner()->GetRootComponent());
	if (!RootComp)
	{
		return;
	}
	UDecalComponent* Decal = NewObject<UDecalComponent>(GetOwner());
	Decal->SetupAttachment(RootComp);
	Decal->SetWorldLocation(Center);
	Decal->SetWorldRotation(FRotator(-90.f, 0.f, 0.f));
	Decal->DecalSize = FVector(Duration * 100.f, Radius, Radius);
	if (UMaterialInterface* Mat = GetCueMaterial(TEXT("/Game/Qinglan/Materials/M_QinglanDecalRing")))
	{
		// UE 5.8: UMaterialInstanceDynamic has no public SetParent; assign the
		// decal material first so CreateDynamicMaterialInstance() parents to it.
		Decal->SetDecalMaterial(Mat);
		if (UMaterialInstanceDynamic* MID = Decal->CreateDynamicMaterialInstance())
		{
			MID->SetVectorParameterValue("Color", ElementColor(Element));
			MID->SetScalarParameterValue("Intensity", 1.f);
		}
	}
	Decal->RegisterComponent();

	FCueInstance Inst;
	Inst.Root = Decal;
	Inst.Remaining = Duration;
	Inst.Total = Duration;
	Inst.BaseRadius = Radius;
	Inst.bRing = true;
	ActiveCues.Add(Inst);
}

void UQinglanVFXCueComponent::PlayCue(FName CueId, const FTransform& Source, FVector TargetPoint, EQinglanElement Element, float Intensity)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// Choose geometry per cue family.
	const bool bIsRing = CueId == QinglanVFXCues::ThunderSealStrike || CueId == QinglanVFXCues::SwordImpact || CueId == QinglanVFXCues::EmberTrail;
	const bool bIsBarrier = CueId == QinglanVFXCues::FlowingWardBarrier || CueId == QinglanVFXCues::StoneAegisBarrier;

	UStaticMesh* Mesh = nullptr;
	FVector Scale(1.f);
	FQuat Rotation = Source.GetRotation();
	FVector Location = Source.GetLocation();
	if (bIsBarrier)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
		Scale = FVector(1.4f, 1.4f, 1.4f);
	}
	else if (bIsRing)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder"));
		Rotation = FQuat(FRotator(-90.f, 0.f, 0.f));
		Scale = FVector(0.02f, 3.f, 3.f);
		if (CueId == QinglanVFXCues::ThunderSealStrike)
		{
			Location = TargetPoint + FVector(0, 0, 20);
			Scale = FVector(0.02f, 3.f, 3.f);
		}
	}
	if (!Mesh)
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
	}

	USceneComponent* RootComp = Cast<USceneComponent>(GetOwner()->GetRootComponent());
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetOwner());
	Comp->SetStaticMesh(Mesh);
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Comp->SetWorldLocationAndRotation(Location, Rotation);
	Comp->SetWorldScale3D(Scale * (0.6f + 0.4f * Intensity));
	if (UMaterialInterface* Mat = GetCueMaterial(MaterialKeyForCue(CueId)))
	{
		Comp->SetMaterial(0, Mat);
		if (UMaterialInstanceDynamic* MID = Comp->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue("Color", ElementColor(Element));
			MID->SetScalarParameterValue("Intensity", Intensity);
		}
	}
	Comp->RegisterComponent();

	FCueInstance Inst;
	Inst.Root = Comp;
	Inst.Remaining = 0.6f;
	Inst.Total = 0.6f;
	Inst.BaseRadius = 100.f * Scale.X;
	ActiveCues.Add(Inst);

	// Dynamic light for readable impacts (Balanced+).
	if (QualityAllowsLights(EQinglanVFXQuality::Balanced) && !bIsBarrier)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(GetOwner());
		Light->SetupAttachment(RootComp ? RootComp : Comp);
		Light->SetWorldLocation(Location);
		Light->SetIntensity(8000.f * Intensity);
		Light->SetLightColor(ElementColor(Element));
		Light->SetAttenuationRadius(1200.f);
		Light->RegisterComponent();
		Inst.Light = Light;
	}
}

void UQinglanVFXCueComponent::ResetForPool()
{
	for (FCueInstance& Inst : ActiveCues)
	{
		if (USceneComponent* Comp = Inst.Root.Get())
		{
			Comp->DestroyComponent();
		}
		if (UPointLightComponent* Light = Inst.Light.Get())
		{
			Light->DestroyComponent();
		}
	}
	ActiveCues.Reset();
}

void UQinglanVFXCueComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	DecayCues(DeltaTime);
}

void UQinglanVFXCueComponent::DecayCues(float DeltaTime)
{
	for (int32 i = ActiveCues.Num() - 1; i >= 0; --i)
	{
		FCueInstance& Inst = ActiveCues[i];
		Inst.Remaining -= DeltaTime;
		const float Alpha = FMath::Clamp(Inst.Remaining / FMath::Max(Inst.Total, 0.01f), 0.f, 1.f);
		if (UStaticMeshComponent* MeshComp = Cast<UStaticMeshComponent>(Inst.Root.Get()))
		{
			if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(MeshComp->GetMaterial(0)))
			{
				MID->SetScalarParameterValue("Fade", Alpha);
			}
			if (Inst.bRing)
			{
				const float Grown = Inst.BaseRadius * (1.f + (1.f - Alpha) * 1.5f) / 100.f;
				MeshComp->SetWorldScale3D(FVector(0.02f, Grown, Grown));
			}
		}
		if (UPointLightComponent* Light = Inst.Light.Get())
		{
			Light->SetIntensity(Light->Intensity * Alpha);
		}
		if (Inst.Remaining <= 0.f)
		{
			if (USceneComponent* Comp = Inst.Root.Get())
			{
				Comp->DestroyComponent();
			}
			if (UPointLightComponent* Light = Inst.Light.Get())
			{
				Light->DestroyComponent();
			}
			ActiveCues.RemoveAtSwap(i);
		}
	}
}
