// VFX cue implementation: pooled mesh/decal/light cues with element colors.
// Deliberately dependency-light: engine shapes + parameterized bootstrap
// materials; every cue degrades to silhouettes at Low and replaces strobing
// with steady glows under low-flash mode (docs/06 sections 3-4).
//
// AUD-42 REMAINING PRESENTATION GAPS (open):
//  - Authored Niagara systems per cue/stage are still absent; all ground
//    centers, safe lanes, telegraphs, releases, and impacts are engine-shape
//    placeholders (decals where available, otherwise static meshes).
//  - Audio assets for telegraph/impact/release are named SFX keys only; the
//    audio bootstrap does not ship authored sounds yet.
//  - No Metal/Low backend screenshot or GPU-capture evidence exists in code;
//    that verification must be produced by the packaging/QA pass.
// None of these gaps may gate gameplay: every failure here logs and returns.
#include "VFX/QinglanVFXCueComponent.h"

#include "QinglanAscension.h"

#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollectionInstance.h" // IWYU
#include "Platform/QinglanGraphicsSettingsService.h"
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

	/** AUD-43: bounded cosmetic-light budget per component (docs/06 VFX GPU
	 *  budget); exceeded cues keep their mesh silhouette but spawn no light. */
	constexpr int32 MaxActiveCueLights = 16;

	/** Actual player configuration, resolved from the graphics service. A
	 *  headless/test world without a GameInstance keeps the Balanced preset
	 *  with low-flash off, which is the authored default. */
	struct FQinglanResolvedCueSettings
	{
		EQinglanVFXQuality Quality = EQinglanVFXQuality::Balanced;
		bool bLowFlash = false;
	};

	EQinglanVFXQuality ToVFXQuality(EQinglanQualityPreset Preset)
	{
		switch (Preset)
		{
		case EQinglanQualityPreset::Low:      return EQinglanVFXQuality::Low;
		case EQinglanQualityPreset::Balanced: return EQinglanVFXQuality::Balanced;
		case EQinglanQualityPreset::High:     return EQinglanVFXQuality::High;
		case EQinglanQualityPreset::Ultra:    return EQinglanVFXQuality::Ultra;
		default:                              return EQinglanVFXQuality::Balanced;
		}
	}

	FQinglanResolvedCueSettings ResolveCueSettings(const UWorld* World)
	{
		FQinglanResolvedCueSettings Resolved;
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		if (const UQinglanGraphicsSettingsService* Graphics = GameInstance
			? GameInstance->GetSubsystem<UQinglanGraphicsSettingsService>() : nullptr)
		{
			const FQinglanGraphicsSettings Settings = Graphics->GetCurrentSettings();
			Resolved.Quality = ToVFXQuality(Settings.Preset);
			Resolved.bLowFlash = Settings.bLowFlashMode;
		}
		return Resolved;
	}
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
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return;
	}
	USceneComponent* RootComp = Cast<USceneComponent>(OwnerActor->GetRootComponent());
	if (!RootComp)
	{
		return;
	}
	UDecalComponent* Decal = NewObject<UDecalComponent>(OwnerActor);
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
	Decal->SetAbsolute(true, true, true);
	Decal->RegisterComponent();

	FCueInstance Inst;
	Inst.Root = Decal;
	Inst.Remaining = Duration;
	Inst.Total = Duration;
	Inst.BaseRadius = Radius;
	Inst.bRing = true;
	ActiveCues.Add(Inst);

	// AUD-42: the decal alone disappears when the bootstrap material is missing
	// or the renderer cannot draw decals (Low/Metal paths). A flat engine-shape
	// disc keeps the ground center readable at every quality tier and in
	// low-flash mode (no dynamic light is spawned here). Placeholder-only.
	UStaticMesh* DiscMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder"));
	if (!DiscMesh)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("Telegraph placeholder disc unavailable; decal-only telegraph"));
		return;
	}
	UStaticMeshComponent* Disc = NewObject<UStaticMeshComponent>(OwnerActor);
	Disc->SetStaticMesh(DiscMesh);
	Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Disc->SetGenerateOverlapEvents(false);
	Disc->SetWorldLocation(Center);
	// Cylinder primitive: radius 50 cm, height 100 cm. Flatten it into a ground
	// disc whose X/Y radius matches the authored telegraph radius.
	const float DiscScale = FMath::Max(Radius, 1.f) / 50.f;
	Disc->SetWorldScale3D(FVector(DiscScale, DiscScale, 0.02f));
	if (UMaterialInterface* Mat = GetCueMaterial(TEXT("/Game/Qinglan/Materials/M_QinglanDecalRing")))
	{
		Disc->SetMaterial(0, Mat);
		if (UMaterialInstanceDynamic* MID = Disc->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue("Color", ElementColor(Element));
			MID->SetScalarParameterValue("Intensity", 0.7f);
		}
	}
	Disc->SetAbsolute(true, true, true);
	Disc->RegisterComponent();

	FCueInstance DiscInst;
	DiscInst.Root = Disc;
	DiscInst.Remaining = Duration;
	DiscInst.Total = Duration;
	DiscInst.BaseRadius = Radius;
	DiscInst.bRing = false; // fixed ground footprint; only fades
	ActiveCues.Add(DiscInst);
}

void UQinglanVFXCueComponent::PlaySafeLane(FVector Center, float Radius, float SafeLaneDegrees,
	float HalfWidthDegrees, float Duration, EQinglanElement Element)
{
	// AUD-42 placeholder: a wedge is approximated by a stretched box laid along
	// the safe bearing. It deliberately carries no light and no Niagara, so it
	// reads identically at Low quality and in low-flash mode; the authoritative
	// safe-lane value stays in the AI math (presentation never gates gameplay).
	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return;
	}
	UStaticMesh* BoxMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube"));
	if (!BoxMesh)
	{
		UE_LOG(LogQinglanCombat, Warning, TEXT("Safe-lane placeholder mesh unavailable; telegraph remains center-only"));
		return;
	}

	const float YawRad = FMath::DegreesToRadians(SafeLaneDegrees);
	const FVector Direction(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.f);
	const float HalfWidthCm = Radius * FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(HalfWidthDegrees, 1.f, 80.f)));
	const FVector LaneCenter = Center + Direction * (Radius * 0.5f) + FVector(0.f, 0.f, 4.f);

	UStaticMeshComponent* Lane = NewObject<UStaticMeshComponent>(OwnerActor);
	Lane->SetStaticMesh(BoxMesh); // 100 cm cube: X = length, Y = width, Z = thickness
	Lane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Lane->SetGenerateOverlapEvents(false);
	Lane->SetAbsolute(true, true, true);
	Lane->SetWorldLocationAndRotation(LaneCenter, FRotator(0.f, SafeLaneDegrees, 0.f));
	Lane->SetWorldScale3D(FVector(
		FMath::Max(Radius, 1.f) / 100.f,
		FMath::Max(HalfWidthCm * 2.f, 1.f) / 100.f,
		0.03f));
	if (UMaterialInterface* Mat = GetCueMaterial(TEXT("/Game/Qinglan/Materials/M_QinglanDecalRing")))
	{
		Lane->SetMaterial(0, Mat);
		if (UMaterialInstanceDynamic* MID = Lane->CreateAndSetMaterialInstanceDynamic(0))
		{
			MID->SetVectorParameterValue("Color", ElementColor(Element));
			MID->SetScalarParameterValue("Intensity", 0.65f);
		}
	}
	Lane->RegisterComponent();

	FCueInstance Inst;
	Inst.Root = Lane;
	Inst.Remaining = Duration;
	Inst.Total = Duration;
	Inst.BaseRadius = Radius;
	Inst.bRing = false;
	ActiveCues.Add(Inst);
}

void UQinglanVFXCueComponent::PlayCue(FName CueId, const FTransform& Source, FVector TargetPoint, EQinglanElement Element, float Intensity)
{
	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor)
	{
		return; // headless contexts cannot host render components
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

	USceneComponent* RootComp = Cast<USceneComponent>(OwnerActor->GetRootComponent());
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(OwnerActor);
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

	// AUD-43: build the complete instance (including the light) BEFORE putting
	// it into ActiveCues, so DecayCues/ResetForPool can always destroy the real
	// light. Quality and low-flash come from the player's actual settings, not
	// a hardcoded Balanced check. Low/low-flash keep the mesh silhouette and
	// suppress only decorative dynamic lights.
	const FQinglanResolvedCueSettings CueSettings = ResolveCueSettings(World);
	const bool bLightAllowed = !bIsBarrier
		&& !CueSettings.bLowFlash
		&& QualityAllowsLights(CueSettings.Quality)
		&& GetActiveLightCount() < MaxActiveCueLights;

	FCueInstance Inst;
	Inst.Root = Comp;
	Inst.Remaining = 0.6f;
	Inst.Total = 0.6f;
	Inst.BaseRadius = 100.f * Scale.X;
	if (bLightAllowed)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(OwnerActor);
		Light->SetupAttachment(RootComp ? RootComp : Comp);
		Light->SetWorldLocation(Location);
		Light->SetIntensity(8000.f * Intensity);
		Light->SetLightColor(ElementColor(Element));
		Light->SetAttenuationRadius(1200.f);
		Light->RegisterComponent();
		Inst.Light = Light;
		Inst.LightBaseIntensity = 8000.f * Intensity;
	}
	ActiveCues.Add(Inst);
}

int32 UQinglanVFXCueComponent::GetActiveLightCount() const
{
	int32 Count = 0;
	for (const FCueInstance& Inst : ActiveCues)
	{
		if (Inst.Light.IsValid())
		{
			++Count;
		}
	}
	return Count;
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
			// Scale from the authored base, never compound frame-to-frame.
			Light->SetIntensity(Inst.LightBaseIntensity * Alpha);
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
