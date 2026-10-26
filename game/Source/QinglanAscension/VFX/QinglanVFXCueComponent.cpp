// VFX cue implementation: pooled mesh/decal/light cues with element colors.
// Deliberately dependency-light: engine shapes + parameterized bootstrap
// materials; every cue degrades to silhouettes at Low and replaces strobing
// with steady glows under low-flash mode (docs/06 sections 3-4).
//
// AUD-42 presentation status:
//  - Authored Niagara systems (NS_Impact/NS_Telegraph/NS_Cast) are created by
//    QinglanContentBootstrap and preferred at runtime; engine-shape decals and
//    meshes remain the fallback and the Low/low-flash silhouette.
//  - Authored SFX (swing, release, telegraph warn, hit impact) are imported by
//    QinglanAudioBootstrap; impact/strike cues play the hit SFX here.
//  - Trial storm telegraphs are presented by the trial director's own cue
//    component (the hazard delegate has no other guaranteed listener).
// Remaining: Metal-backend capture and target-hardware performance evidence.
// None of these gaps may gate gameplay: every failure here logs and returns.
#include "VFX/QinglanVFXCueComponent.h"

#include "QinglanAscension.h"

#include "Components/DecalComponent.h"
#include "Core/QinglanAudioSubsystem.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollectionInstance.h" // IWYU
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
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

	/** AUD-42: bounded Niagara budget per component. Spawns beyond the budget
	 *  keep the mesh/light placeholder so presentation stays bounded even when
	 *  many cues land in one frame (also protects automation worlds). */
	constexpr int32 MaxActiveNiagaraCues = 8;

	namespace NiagaraCueCategory
	{
		const FName Impact(TEXT("impact"));
		const FName Telegraph(TEXT("telegraph"));
		const FName Cast(TEXT("cast"));
	}

	/** Authored by QinglanContentBootstrap::BootstrapNiagaraEffects. */
	namespace NiagaraCuePath
	{
		const TCHAR* Impact = TEXT("/Game/Qinglan/VFX/NS_Impact.NS_Impact");
		const TCHAR* Telegraph = TEXT("/Game/Qinglan/VFX/NS_Telegraph.NS_Telegraph");
		const TCHAR* Cast = TEXT("/Game/Qinglan/VFX/NS_Cast.NS_Cast");
	}

	FName CategoryForCue(FName CueId)
	{
		const FString Id = CueId.ToString();
		// Strike-shaped cues (blade arcs/impacts, strikes, trails, death
		// dissolves) read as one-shot bursts; everything else is a cast/aura.
		if (Id.Contains(TEXT("impact"), ESearchCase::IgnoreCase)
			|| Id.Contains(TEXT("arc"), ESearchCase::IgnoreCase)
			|| Id.Contains(TEXT("strike"), ESearchCase::IgnoreCase)
			|| Id.Contains(TEXT("trail"), ESearchCase::IgnoreCase)
			|| Id.Contains(TEXT("dissolve"), ESearchCase::IgnoreCase))
		{
			return NiagaraCueCategory::Impact;
		}
		if (Id.Contains(TEXT("telegraph"), ESearchCase::IgnoreCase)
			|| Id.Contains(TEXT("ground_marker"), ESearchCase::IgnoreCase))
		{
			return NiagaraCueCategory::Telegraph;
		}
		return NiagaraCueCategory::Cast;
	}

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

UNiagaraSystem* UQinglanVFXCueComponent::ResolveNiagaraSystemForCue(FName CueId)
{
	const FName Category = CategoryForCue(CueId);
	if (TObjectPtr<UNiagaraSystem>* Cached = CachedNiagaraSystems.Find(Category))
	{
		return Cached->Get(); // null here is a cached miss (placeholder-only path)
	}

	const TCHAR* Path = NiagaraCuePath::Cast;
	if (Category == NiagaraCueCategory::Impact)
	{
		Path = NiagaraCuePath::Impact;
	}
	else if (Category == NiagaraCueCategory::Telegraph)
	{
		Path = NiagaraCuePath::Telegraph;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, Path);
	if (System == nullptr)
	{
		// Expected until QinglanContentBootstrap authors the systems. Keep the
		// mesh/light placeholder; never treat this as a gameplay failure.
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Niagara cue system %s missing (category %s); placeholder cue only (AUD-42)"),
			Path, *Category.ToString());
	}
	else
	{
		UE_LOG(LogQinglanCombat, Verbose, TEXT("Niagara cue system %s loaded for category %s"),
			Path, *Category.ToString());
	}
	CachedNiagaraSystems.Add(Category, System);
	return System;
}

void UQinglanVFXCueComponent::ApplyNiagaraElementParameters(UNiagaraComponent* NiagaraComponent,
	UNiagaraSystem* System, EQinglanElement Element, float Intensity) const
{
	if (!NiagaraComponent || !System)
	{
		return;
	}

	// Only drive user parameters the system actually exposes; SetVariable* on a
	// missing parameter is a no-op with a warning, which would spam per cue.
	TArray<FNiagaraVariable> Parameters;
	System->GetExposedParameters().GetParameters(Parameters);
	bool bHasColor = false;
	bool bHasSpriteSize = false;
	for (const FNiagaraVariable& Parameter : Parameters)
	{
		const FName Name = Parameter.GetName();
		bHasColor |= Name == FName(TEXT("User.Color"));
		bHasSpriteSize |= Name == FName(TEXT("User.SpriteSize"));
	}
	if (bHasColor)
	{
		NiagaraComponent->SetVariableLinearColor(FName(TEXT("User.Color")), ElementColor(Element));
	}
	if (bHasSpriteSize)
	{
		NiagaraComponent->SetVariableFloat(FName(TEXT("User.SpriteSize")),
			12.f * FMath::Clamp(Intensity, 0.1f, 4.f));
	}
}

int32 UQinglanVFXCueComponent::GetActiveNiagaraCount() const
{
	int32 Count = 0;
	for (const FCueInstance& Inst : ActiveCues)
	{
		if (Inst.Niagara.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

void UQinglanVFXCueComponent::SpawnNiagaraForCue(FName CueId, const FTransform& Source,
	FVector TargetPoint, EQinglanElement Element, float Intensity,
	TWeakObjectPtr<UNiagaraComponent>& OutNiagara)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return; // headless contexts keep the logical cue only
	}
	UNiagaraSystem* System = ResolveNiagaraSystemForCue(CueId);
	if (!System)
	{
		return; // placeholder geometry already covers the cue
	}
	// Quality/low-flash policy: Niagara systems are additive decorative bursts,
	// so Low quality and low-flash mode keep the static-mesh silhouette only
	// (telegraphs stay readable because PlayTelegraph/PlaySafeLane always draw
	// their quality-independent ground markers).
	const FQinglanResolvedCueSettings CueSettings = ResolveCueSettings(World);
	if (CueSettings.bLowFlash || CueSettings.Quality == EQinglanVFXQuality::Low)
	{
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Niagara cue %s suppressed by Low quality / low-flash; placeholder remains"),
			*CueId.ToString());
		return;
	}
	if (GetActiveNiagaraCount() >= MaxActiveNiagaraCues)
	{
		UE_LOG(LogQinglanCombat, Verbose,
			TEXT("Niagara cue budget reached (%d); placeholder only for %s"),
			MaxActiveNiagaraCues, *CueId.ToString());
		return;
	}

	// bAutoDestroy: the system ends and destroys itself; we only keep a weak
	// reference so ResetForPool/decay cannot dangle (and can cut it short when
	// the cue budget needs the slot back). No dynamic light is created here,
	// preserving the low-flash policy.
	const FVector SpawnLocation = Source.GetLocation().IsNearlyZero() && !TargetPoint.IsNearlyZero()
		? TargetPoint
		: Source.GetLocation();
	UNiagaraComponent* NiagaraComponent = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		World, System, SpawnLocation, Source.GetRotation().Rotator(), FVector::OneVector,
		/*bAutoDestroy=*/true, /*bAutoActivate=*/true, ENCPoolMethod::None, /*bPreCullCheck=*/true);
	if (!NiagaraComponent)
	{
		UE_LOG(LogQinglanCombat, Warning,
			TEXT("Niagara spawn failed for cue %s (%s); placeholder cue remains"),
			*CueId.ToString(), *System->GetName());
		return;
	}
	ApplyNiagaraElementParameters(NiagaraComponent, System, Element,
		Intensity * IntensityScaleForQuality(CueSettings.Quality));
	OutNiagara = NiagaraComponent;
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

	// AUD-42: real Niagara telegraph when the authored system exists; the decal
	// and disc above remain the fallback and the quality-independent silhouette.
	FTransform NiagaraSource;
	NiagaraSource.SetLocation(Center);
	SpawnNiagaraForCue(QinglanVFXCues::TelegraphRing, NiagaraSource, Center, Element, 1.f, Inst.Niagara);
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
	FTransform NiagaraSource;
	NiagaraSource.SetLocation(Center);
	NiagaraSource.SetRotation(FQuat(FRotator(0.f, SafeLaneDegrees, 0.f)));
	SpawnNiagaraForCue(QinglanVFXCues::TelegraphRing, NiagaraSource, Center, Element, 0.8f, Inst.Niagara);
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

	// AUD-42: layer the authored Niagara system over the placeholder geometry.
	// Missing systems (pre-bootstrap) leave the static mesh exactly as before.
	SpawnNiagaraForCue(CueId, Source, TargetPoint, Element, Intensity, Inst.Niagara);

	// AUD-42: impact cues carry the authored hit SFX. Swing/release/telegraph
	// sounds are owned by the sword, technique and enemy paths, so only
	// strike-shaped cues play here (no double audio on a sword swing).
	const FString CueName = CueId.ToString();
	if (CueName.Contains(TEXT("impact"), ESearchCase::IgnoreCase)
		|| CueName.Contains(TEXT("strike"), ESearchCase::IgnoreCase))
	{
		UGameInstance* Instance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
		if (UQinglanAudioSubsystem* Audio = Instance ? Instance->GetSubsystem<UQinglanAudioSubsystem>() : nullptr)
		{
			Audio->PlaySfx(FName(TEXT("sfx_hit_impact")));
		}
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
		// AUD-42: release real Niagara instances with the pool reset. The weak
		// pointer is safe if the system already auto-destroyed (null).
		if (UNiagaraComponent* Niagara = Inst.Niagara.Get())
		{
			Niagara->Deactivate();
			Niagara->DestroyComponent();
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
			// AUD-42: end the real emitter with the cue. bAutoDestroy would
			// also handle this; destroying here frees the per-component budget
			// deterministically. The weak reference is never dereferenced
			// unguarded.
			if (UNiagaraComponent* Niagara = Inst.Niagara.Get())
			{
				Niagara->Deactivate();
				Niagara->DestroyComponent();
			}
			ActiveCues.RemoveAtSwap(i);
		}
	}
}
