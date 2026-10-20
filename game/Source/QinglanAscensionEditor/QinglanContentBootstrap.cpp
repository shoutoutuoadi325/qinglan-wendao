// Content bootstrap implementation. See header for intent.
#include "QinglanContentBootstrap.h"
#include "Core/QinglanRoomLayout.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Containers/ArrayView.h"
#include "Components/StaticMeshComponent.h"
#include "EditorFramework/AssetImportData.h"
#include "Components/LightComponent.h" // complete ULightComponent for SetIntensity/SetLightColor
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Components/SkyAtmosphereComponent.h" // ASkyAtmosphere (AUD-55 rendered sky)
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Engine/World.h"
#include "Factories/TextureFactory.h"
#include "GameFramework/GameModeBase.h" // AGameModeBase for LoadClass<> (not pulled in by WorldSettings.h anymore)
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/TargetPoint.h"
#include "EnhancedInputModule.h" // UE 5.8: IEnhancedInputModule moved here from EnhancedInput/Subsystem/
#include "HAL/FileManager.h" // AUD-42: remove a just-written Niagara package if post-save verification fails
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSphereMask.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h" // UE 5.8: FSoftObjectPath replaces FPackageName::GetObjectPathPackage
#include "NiagaraEmitter.h"
#include "NiagaraSettings.h" // default effect type (mirrors UNiagaraSystemFactoryNew)
#include "NiagaraSystem.h"
#include "NiagaraEditorUtilities.h" // FNiagaraEditorUtilities::AddEmitterToSystem (AUD-42)
#include "NiagaraSystemFactoryNew.h" // UNiagaraSystemFactoryNew::InitializeSystem (AUD-42)

#define LOCTEXT_NAMESPACE "QinglanContentBootstrap"

namespace
{
	const FString ContentRoot = TEXT("/Game/Qinglan");
	const FString RawAssetsRoot = TEXT("/inspire/hdd/project/qianghuaxuexi/s26044/assets-raw");

	// AUD-55: the content bootstrap can emit a low-cost package variant
	// (-lowquality) that binds every surface to its solid-colour MIC. The maps
	// are static content, so runtime quality preset switching cannot remove
	// these actors later; the contract below keeps both variants generated.
	bool GUseLowQualitySurfaceMaterials = false;

	// Material names (M_*) that received a real BaseColor texture this run.
	// Maps use this to avoid binding a texture MIC whose BaseColor is null
	// (the previous bootstrap produced exactly that black-material failure).
	TSet<FString> GTexturedSurfaceMaterials;

	UPackage* CreateOrFetchPackage(const FString& ObjectPath)
	{
		const FString PackageName = FSoftObjectPath(ObjectPath).GetLongPackageName(); // UE 5.8: FPackageName::GetObjectPathPackage is gone
		UPackage* Package = CreatePackage(*PackageName);
		Package->SetPackageFlags(PKG_NewlyCreated);
		Package->FullyLoad();
		return Package;
	}
}

/** AUD-42: authors /Game/Qinglan/VFX/NS_{Impact,Telegraph,Cast} from engine
 *  emitter templates. A free function (not a commandlet member) so the frozen
 *  commandlet header needs no new declaration; called from Main. */
bool BootstrapNiagaraEffects();

// AUD-55 texture-set contract. Kept in a named namespace (not exported through
// the commandlet header, which is frozen) so QinglanAuditFrameworkTests can
// assert path generation, parameter mapping, and the solid-colour fallback
// without touching the content on disk.
namespace QinglanTextureSetContract
{
	FString TextureFolderPath(const FString& SetId)
	{
		return ContentRoot + TEXT("/Textures/") + SetId;
	}

	FString TexturePackagePath(const FString& SetId, const FString& Slot)
	{
		return TextureFolderPath(SetId) + TEXT("/") + Slot;
	}

	FString TextureObjectPath(const FString& SetId, const FString& Slot)
	{
		return TexturePackagePath(SetId, Slot) + TEXT(".") + Slot;
	}

	/** Poly Haven file token embedded between underscores, e.g. _diff_2k. */
	FString SourceFileToken(const FString& Slot)
	{
		if (Slot == TEXT("BaseColor")) { return TEXT("diff"); }
		if (Slot == TEXT("Roughness")) { return TEXT("rough"); }
		if (Slot == TEXT("AmbientOcclusion")) { return TEXT("ao"); }
		if (Slot == TEXT("Arm")) { return TEXT("arm"); }
		if (Slot == TEXT("Normal")) { return TEXT("nor"); }
		return FString();
	}

	/** Master-material texture parameter for a slot; ARM is channel-packed and not bound wholesale. */
	FString MaterialParameterName(const FString& Slot)
	{
		if (Slot == TEXT("Arm")) { return FString(); }
		if (Slot == TEXT("BaseColor") || Slot == TEXT("Roughness")
			|| Slot == TEXT("AmbientOcclusion") || Slot == TEXT("Normal"))
		{
			return Slot;
		}
		return FString();
	}

	/** Only colour data is sRGB; normal/roughness/AO/ARM data must stay linear. */
	bool IsSRGB(const FString& Slot)
	{
		return Slot == TEXT("BaseColor");
	}

	FString MaterialObjectPath(const FString& MaterialName)
	{
		return ContentRoot + TEXT("/Materials/") + MaterialName + TEXT(".") + MaterialName;
	}

	FString SolidMaterialAssetName(const FString& MaterialName)
	{
		return MaterialName + TEXT("_Solid");
	}

	FString SolidMaterialObjectPath(const FString& MaterialName)
	{
		return MaterialObjectPath(SolidMaterialAssetName(MaterialName));
	}

	bool ShouldUseSolidFallback(bool bLowQuality, bool bHasBaseColor)
	{
		return bLowQuality || !bHasBaseColor;
	}

	FString ResolveSurfaceMaterialObjectPath(const FString& MaterialName, bool bLowQuality, bool bHasBaseColor)
	{
		return ShouldUseSolidFallback(bLowQuality, bHasBaseColor)
			? SolidMaterialObjectPath(MaterialName)
			: MaterialObjectPath(MaterialName);
	}
}

UQinglanContentBootstrapCommandlet::UQinglanContentBootstrapCommandlet()
{
}

int32 UQinglanContentBootstrapCommandlet::Main(const FString& Params)
{
	// -lowquality emits map bindings to the solid-colour MICs while still
	// importing the full texture set. Without the flag maps use the textured
	// MICs (missing BaseColor sets always fall back to solid).
	GUseLowQualitySurfaceMaterials = FParse::Param(*Params, TEXT("lowquality"));
	UE_LOG(LogTemp, Log, TEXT("QinglanContentBootstrap starting (lowquality=%d)"), GUseLowQualitySurfaceMaterials ? 1 : 0);
	bool bOk = true;
	bOk &= BootstrapInput();
	bOk &= BootstrapMaterials();
	bOk &= BootstrapVFXMaterials();
	bOk &= BootstrapNiagaraEffects(); // AUD-42: real Niagara cue systems (placeholder fallback kept)
	bOk &= BootstrapMaps();
	UE_LOG(LogTemp, Log, TEXT("QinglanContentBootstrap finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}

bool UQinglanContentBootstrapCommandlet::SavePackage(UPackage* Package, UObject* Asset)
{
	FString PackageName = Package->GetName();
	FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.Error = GError;
	Args.bForceByteSwapping = true;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *FileName, Args);
}

//----------------------------------------------------------------------------
// Input assets
//----------------------------------------------------------------------------

namespace
{
	UInputAction* MakeAction(UPackage* Package, const FName& Name, EInputActionValueType ValueType)
	{
		UInputAction* Action = NewObject<UInputAction>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
		Action->ValueType = ValueType;
		return Action;
	}

	UInputTriggerPressed* PressedTrigger(UObject* Outer)
	{
		UInputTriggerPressed* T = NewObject<UInputTriggerPressed>(Outer);
		return T;
	}

	void Map2DAxis(UInputMappingContext* IMC, UInputAction* Action, const FKey& Key,
		bool bNegateX, bool bNegateY, bool bSwizzle)
	{
		FEnhancedActionKeyMapping& Mapping = IMC->MapKey(Action, Key);
		if (bSwizzle)
		{
			UInputModifierSwizzleAxis* Swizzle = NewObject<UInputModifierSwizzleAxis>(IMC);
			Swizzle->Order = EInputAxisSwizzle::YXZ;
			Mapping.Modifiers.Add(Swizzle);
		}
		if (bNegateX || bNegateY)
		{
			UInputModifierNegate* Negate = NewObject<UInputModifierNegate>(IMC);
			Negate->bX = bNegateX;
			Negate->bY = bNegateY;
			Mapping.Modifiers.Add(Negate);
		}
		Mapping.Modifiers.Add(NewObject<UInputModifierDeadZone>(IMC));
	}
}

bool UQinglanContentBootstrapCommandlet::BootstrapInput()
{
	UPackage* InputPkg = CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Move"));
	// Consumer contract (AQinglanCharacter::InputMove reads FVector2D): X right,
	// Y forward. Axis3D here would mismatch the typed Get<FVector2D>() access.
	UInputAction* IA_Move = MakeAction(InputPkg, TEXT("IA_Move"), EInputActionValueType::Axis2D);
	UInputAction* IA_Look = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Look")), TEXT("IA_Look"), EInputActionValueType::Axis2D);
	UInputAction* IA_Attack = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Attack")), TEXT("IA_Attack"), EInputActionValueType::Boolean);
	UInputAction* IA_Dodge = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Dodge")), TEXT("IA_Dodge"), EInputActionValueType::Boolean);
	UInputAction* IA_Lock = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Lock")), TEXT("IA_Lock"), EInputActionValueType::Boolean);
	UInputAction* IA_Cycle = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_CycleTarget")), TEXT("IA_CycleTarget"), EInputActionValueType::Boolean);
	UInputAction* IA_Interact = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Interact")), TEXT("IA_Interact"), EInputActionValueType::Boolean);
	// Pause must keep firing while the world is paused (UInputAction::
	// bTriggerWhenPaused), otherwise Escape pauses and nothing can resume.
	UInputAction* IA_Pause = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Pause")), TEXT("IA_Pause"), EInputActionValueType::Boolean);
	IA_Pause->bTriggerWhenPaused = true;

	TArray<UInputAction*> TechniqueActions;
	for (int32 i = 0; i < 4; ++i)
	{
		const FName Name(*FString::Printf(TEXT("IA_Technique_%d"), i + 1)); // matches AQinglanCharacter CDO lookups
		TechniqueActions.Add(MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/") + Name.ToString()), Name, EInputActionValueType::Boolean));
	}
	TArray<UInputAction*> TalismanActions;
	for (int32 i = 0; i < 2; ++i)
	{
		const FName Name(*FString::Printf(TEXT("IA_Talisman_%d"), i + 1)); // matches AQinglanCharacter CDO lookups
		TalismanActions.Add(MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/") + Name.ToString()), Name, EInputActionValueType::Boolean));
	}

	UPackage* ImcPkg = CreateOrFetchPackage(ContentRoot + TEXT("/Input/IMC_Default"));
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(ImcPkg, TEXT("IMC_Default"), RF_Public | RF_Standalone | RF_Transactional);

	// Movement: WASD on a 2-plane (docs/02 section 1). Contract (matches
	// InputMove's FVector2D read: X right, Y forward):
	// W=(0,+1) S=(0,-1) A=(-1,0) D=(+1,0). A bare key feeds its 1.0 into X,
	// so the Y-axis keys swizzle first; swizzle is applied before negation.
	Map2DAxis(IMC, IA_Move, EKeys::W, false, false, true);
	Map2DAxis(IMC, IA_Move, EKeys::S, false, true, true);
	Map2DAxis(IMC, IA_Move, EKeys::D, false, false, false);
	Map2DAxis(IMC, IA_Move, EKeys::A, true, false, false);

	// Look: mouse deltas (X yaw, Y pitch come through as Axis2D).
	{
		FEnhancedActionKeyMapping& M = IMC->MapKey(IA_Look, EKeys::Mouse2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(IMC);
		NegateY->bY = true;
		M.Modifiers.Add(NegateY);
	}
	IMC->MapKey(IA_Attack, EKeys::LeftMouseButton).Triggers.Add(PressedTrigger(IMC));
	// Keyboard-only fallback for the basic attack: every other combat action
	// already has a keyboard key, so a keyboard playthrough must not require
	// the mouse for the primary swing.
	IMC->MapKey(IA_Attack, EKeys::J).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Dodge, EKeys::SpaceBar).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Lock, EKeys::MiddleMouseButton).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Cycle, EKeys::MouseWheelAxis).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Interact, EKeys::E).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Pause, EKeys::Escape).Triggers.Add(PressedTrigger(IMC));

	static const FKey* TechniqueKeys[] = { &EKeys::One, &EKeys::Two, &EKeys::Three, &EKeys::Four };
	for (int32 i = 0; i < 4; ++i)
	{
		IMC->MapKey(TechniqueActions[i], *TechniqueKeys[i]).Triggers.Add(PressedTrigger(IMC));
	}
	static const FKey* TalismanKeys[] = { &EKeys::Q, &EKeys::R };
	for (int32 i = 0; i < 2; ++i)
	{
		IMC->MapKey(TalismanActions[i], *TalismanKeys[i]).Triggers.Add(PressedTrigger(IMC));
	}

	// Gamepad: left stick move / right stick camera with the same axis
	// convention as WASD/mouse (stick 2D keys are already X right, Y up, so
	// the sticks map without swizzles).
	Map2DAxis(IMC, IA_Move, EKeys::Gamepad_Left2D, false, false, false);
	{
		FEnhancedActionKeyMapping& M = IMC->MapKey(IA_Look, EKeys::Gamepad_Right2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(IMC);
		NegateY->bY = true;
		M.Modifiers.Add(NegateY);
	}
	// Face buttons: X attack, A dodge, Y interact, B cycle target.
	IMC->MapKey(IA_Attack, EKeys::Gamepad_FaceButton_Left).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Dodge, EKeys::Gamepad_FaceButton_Bottom).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Interact, EKeys::Gamepad_FaceButton_Top).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Cycle, EKeys::Gamepad_FaceButton_Right).Triggers.Add(PressedTrigger(IMC));
	// Shoulder/trigger: left shoulder lock-on, right shoulder talisman 1,
	// left trigger talisman 2, right trigger attack (analog keys cross the
	// pressed threshold as digital inputs for Boolean actions).
	IMC->MapKey(IA_Lock, EKeys::Gamepad_LeftShoulder).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(TalismanActions[0], EKeys::Gamepad_RightShoulder).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(TalismanActions[1], EKeys::Gamepad_LeftTrigger).Triggers.Add(PressedTrigger(IMC));
	IMC->MapKey(IA_Attack, EKeys::Gamepad_RightTrigger).Triggers.Add(PressedTrigger(IMC));
	// D-pad technique slots 1-4.
	static const FKey* TechniquePadKeys[] = { &EKeys::Gamepad_DPad_Up, &EKeys::Gamepad_DPad_Right, &EKeys::Gamepad_DPad_Down, &EKeys::Gamepad_DPad_Left };
	for (int32 i = 0; i < 4; ++i)
	{
		IMC->MapKey(TechniqueActions[i], *TechniquePadKeys[i]).Triggers.Add(PressedTrigger(IMC));
	}
	// Menu/Start pauses (IA_Pause triggers while paused, so it also resumes).
	IMC->MapKey(IA_Pause, EKeys::Gamepad_Special_Right).Triggers.Add(PressedTrigger(IMC));

	bool bOk = true;
	bOk &= SavePackage(InputPkg, IA_Move);
	bOk &= SavePackage(Cast<UPackage>(IA_Look->GetOuter()), IA_Look);
	bOk &= SavePackage(Cast<UPackage>(IA_Attack->GetOuter()), IA_Attack);
	bOk &= SavePackage(Cast<UPackage>(IA_Dodge->GetOuter()), IA_Dodge);
	bOk &= SavePackage(Cast<UPackage>(IA_Lock->GetOuter()), IA_Lock);
	bOk &= SavePackage(Cast<UPackage>(IA_Cycle->GetOuter()), IA_Cycle);
	bOk &= SavePackage(Cast<UPackage>(IA_Interact->GetOuter()), IA_Interact);
	bOk &= SavePackage(Cast<UPackage>(IA_Pause->GetOuter()), IA_Pause);
	for (UInputAction* A : TechniqueActions) { bOk &= SavePackage(Cast<UPackage>(A->GetOuter()), A); }
	for (UInputAction* A : TalismanActions) { bOk &= SavePackage(Cast<UPackage>(A->GetOuter()), A); }
	bOk &= SavePackage(ImcPkg, IMC);
	return bOk;
}

//----------------------------------------------------------------------------
// Materials
//----------------------------------------------------------------------------

UMaterial* UQinglanContentBootstrapCommandlet::CreateOrLoadMasterMaterial()
{
	const FString Path = ContentRoot + TEXT("/Materials/M_QinglanSurface");

	// Rebuild deterministically: the master graph gained UV tiling and sampler
	// types, and a stale package would silently keep the old untextured graph.
	// Nothing else is loaded into this fresh commandlet process yet, so deleting
	// the package before creating it is safe.
	const FString FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*FileName, /*bRequireExists=*/false, /*bEvenReadOnly=*/true, /*bQuiet=*/true);

	UPackage* Pkg = CreateOrFetchPackage(Path);
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_QinglanSurface"), RF_Public | RF_Standalone);
	Mat->BlendMode = BLEND_Opaque;
	Mat->SetShadingModel(MSM_DefaultLit); // UE 5.8: ShadingModel is private; setter is the engine idiom

	// Shared tiling: Poly Haven sets are authored to tile, so sample them more
	// than once per blockout face instead of stretching one 2K image across a
	// 22 m terrace.
	UMaterialExpressionTextureCoordinate* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
	TexCoord->UTiling = 1.f;
	TexCoord->VTiling = 1.f;
	TexCoord->MaterialExpressionEditorX = -1100;
	TexCoord->MaterialExpressionEditorY = -300;
	Mat->GetExpressionCollection().AddExpression(TexCoord);

	UMaterialExpressionScalarParameter* Tiling = NewObject<UMaterialExpressionScalarParameter>(Mat);
	Tiling->ParameterName = TEXT("Tiling");
	Tiling->DefaultValue = 4.f;
	Tiling->MaterialExpressionEditorX = -1100;
	Tiling->MaterialExpressionEditorY = -120;
	Mat->GetExpressionCollection().AddExpression(Tiling);

	UMaterialExpressionMultiply* ScaledUV = NewObject<UMaterialExpressionMultiply>(Mat);
	ScaledUV->A.Connect(0, TexCoord);
	ScaledUV->B.Connect(0, Tiling);
	ScaledUV->MaterialExpressionEditorX = -850;
	ScaledUV->MaterialExpressionEditorY = -250;
	Mat->GetExpressionCollection().AddExpression(ScaledUV);

	auto AddTextureParam = [Mat, ScaledUV](const FName& Name, EMaterialSamplerType Sampler, int32& OutY)
		-> UMaterialExpressionTextureSampleParameter2D*
	{
		UMaterialExpressionTextureSampleParameter2D* Param =
			NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
		Param->ParameterName = Name;
		Param->SamplerType = Sampler;
		Param->Coordinates.Connect(0, ScaledUV);
		Param->MaterialExpressionEditorX = -600;
		Param->MaterialExpressionEditorY = OutY;
		OutY += 220;
		Mat->GetExpressionCollection().AddExpression(Param);
		return Param;
	};

	int32 Y = -300;
	UMaterialExpressionTextureSampleParameter2D* BaseColorParam = AddTextureParam(TEXT("BaseColor"), SAMPLERTYPE_Color, Y);
	UMaterialExpressionTextureSampleParameter2D* NormalParam = AddTextureParam(TEXT("Normal"), SAMPLERTYPE_Normal, Y);
	UMaterialExpressionTextureSampleParameter2D* RoughParam = AddTextureParam(TEXT("Roughness"), SAMPLERTYPE_LinearGrayscale, Y);
	UMaterialExpressionTextureSampleParameter2D* AOParam = AddTextureParam(TEXT("AmbientOcclusion"), SAMPLERTYPE_LinearGrayscale, Y);

	// No Poly Haven set on disk ships a normal map. Default the Normal sample to
	// the engine flat normal so the master remains valid when a MIC omits it.
	if (UTexture2D* FlatNormal = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal")))
	{
		NormalParam->Texture = FlatNormal;
	}

	// UE 5.8: material input properties (BaseColor, Normal, ...) live on UMaterialEditorOnlyData.
	Mat->GetEditorOnlyData()->BaseColor.Connect(0, BaseColorParam);
	Mat->GetEditorOnlyData()->Normal.Connect(0, NormalParam);
	Mat->GetEditorOnlyData()->Roughness.Connect(1, RoughParam); // R channel (Poly Haven rough maps are grayscale)
	Mat->GetEditorOnlyData()->AmbientOcclusion.Connect(1, AOParam);
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	SavePackage(Pkg, Mat);
	return Mat;
}

bool UQinglanContentBootstrapCommandlet::BootstrapVFXMaterials()
{
	// The three cue materials UQinglanVFXCueComponent resolves by convention
	// (VFX/QinglanVFXCueComponent.cpp MaterialKeyForCue): additive sprite
	// glow, deferred-decal telegraph disc, translucent fresnel barrier. Each
	// exposes the same "Color" vector + "Intensity" scalar parameter pair the
	// cue component drives through material instance dynamics.
	bool bOk = true;

	// --- Shared expression helpers -------------------------------------
	auto AddVectorParam = [](UMaterial* Mat, const TCHAR* Name, const FLinearColor& Default, int32 Y)
	{
		UMaterialExpressionVectorParameter* Param = NewObject<UMaterialExpressionVectorParameter>(Mat);
		Param->ParameterName = FName(Name);
		Param->DefaultValue = Default;
		Param->MaterialExpressionEditorX = -700;
		Param->MaterialExpressionEditorY = Y;
		Mat->GetExpressionCollection().AddExpression(Param);
		return Param;
	};
	auto AddScalarParam = [](UMaterial* Mat, const TCHAR* Name, float Default, int32 Y)
	{
		UMaterialExpressionScalarParameter* Param = NewObject<UMaterialExpressionScalarParameter>(Mat);
		Param->ParameterName = FName(Name);
		Param->DefaultValue = Default;
		Param->MaterialExpressionEditorX = -700;
		Param->MaterialExpressionEditorY = Y;
		Mat->GetExpressionCollection().AddExpression(Param);
		return Param;
	};
	auto AddMultiply = [](UMaterial* Mat, UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut, int32 Y)
	{
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(Mat);
		Mul->A.Connect(AOut, A);
		Mul->B.Connect(BOut, B);
		Mul->MaterialExpressionEditorX = -400;
		Mul->MaterialExpressionEditorY = Y;
		Mat->GetExpressionCollection().AddExpression(Mul);
		return Mul;
	};

	auto BeginMaterial = [](const FString& Path, EBlendMode Blend, EMaterialDomain Domain) -> TPair<UPackage*, UMaterial*>
	{
		if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *Path))
		{
			return { nullptr, Existing };
		}
		UPackage* Pkg = CreateOrFetchPackage(Path);
		UMaterial* Mat = NewObject<UMaterial>(Pkg, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
		Mat->BlendMode = Blend;
		Mat->SetShadingModel(MSM_Unlit); // UE 5.8: shading model is private; setter is the engine idiom
		Mat->MaterialDomain = Domain;
		return { Pkg, Mat };
	};
	auto FinishMaterial = [this](UPackage* Pkg, UMaterial* Mat)
	{
		if (Pkg == nullptr || Mat == nullptr)
		{
			return true; // already existed on disk; nothing to author
		}
		Mat->PreEditChange(nullptr);
		Mat->PostEditChange();
		const bool bSaved = SavePackage(Pkg, Mat);
		if (!bSaved)
		{
			UE_LOG(LogTemp, Error, TEXT("BootstrapVFXMaterials: save failed for %s"), *Mat->GetName());
		}
		return bSaved;
	};

	// --- M_QinglanAdditive: emissive sprite/particle glow ----------------
	{
		const FString Path = ContentRoot + TEXT("/Materials/M_QinglanAdditive");
		auto [Pkg, Mat] = BeginMaterial(Path, BLEND_Additive, MD_Surface);
		if (Pkg)
		{
			UMaterialExpressionVectorParameter* Color = AddVectorParam(Mat, TEXT("Color"), FLinearColor(0.55f, 0.85f, 1.f, 1.f), -300);
			UMaterialExpressionScalarParameter* Intensity = AddScalarParam(Mat, TEXT("Intensity"), 4.f, -100);
			UMaterialExpressionMultiply* Emissive = AddMultiply(Mat, Color, 0, Intensity, 0, -200);
			Mat->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
			// Additive translucency scales by opacity; drive it with intensity too.
			Mat->GetEditorOnlyData()->Opacity.Connect(0, Intensity);
			bOk &= FinishMaterial(Pkg, Mat);
		}
	}

	// --- M_QinglanDecalRing: telegraph disc on the ground -----------------
	{
		const FString Path = ContentRoot + TEXT("/Materials/M_QinglanDecalRing");
		auto [Pkg, Mat] = BeginMaterial(Path, BLEND_Translucent, MD_DeferredDecal);
		if (Pkg)
		{
			UMaterialExpressionTextureCoordinate* TexCoord = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
			TexCoord->MaterialExpressionEditorX = -900;
			TexCoord->MaterialExpressionEditorY = -300;
			Mat->GetExpressionCollection().AddExpression(TexCoord);

			UMaterialExpressionConstant2Vector* Center = NewObject<UMaterialExpressionConstant2Vector>(Mat);
			Center->R = 0.5f;
			Center->G = 0.5f;
			Center->MaterialExpressionEditorX = -900;
			Center->MaterialExpressionEditorY = -80;
			Mat->GetExpressionCollection().AddExpression(Center);

			UMaterialExpressionSphereMask* Disc = NewObject<UMaterialExpressionSphereMask>(Mat);
			Disc->A.Connect(0, TexCoord);
			Disc->B.Connect(0, Center);
			Disc->AttenuationRadius = 0.5f;  // UV space; covers the decal face
			Disc->HardnessPercent = 12.f;    // soft edge keeps reads readable, not harsh
			Disc->MaterialExpressionEditorX = -600;
			Disc->MaterialExpressionEditorY = -200;
			Mat->GetExpressionCollection().AddExpression(Disc);

			UMaterialExpressionVectorParameter* Color = AddVectorParam(Mat, TEXT("Color"), FLinearColor(1.f, 0.42f, 0.12f, 1.f), -420);
			UMaterialExpressionScalarParameter* Intensity = AddScalarParam(Mat, TEXT("Intensity"), 2.f, -40);

			UMaterialExpressionMultiply* DiscTimesIntensity = AddMultiply(Mat, Disc, 0, Intensity, 0, -200);
			UMaterialExpressionMultiply* Emissive = AddMultiply(Mat, Color, 0, DiscTimesIntensity, 0, -260);
			Mat->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
			Mat->GetEditorOnlyData()->Opacity.Connect(0, DiscTimesIntensity);
			bOk &= FinishMaterial(Pkg, Mat);
		}
	}

	// --- M_QinglanBarrier: fresnel edge shield -----------------------------
	{
		const FString Path = ContentRoot + TEXT("/Materials/M_QinglanBarrier");
		auto [Pkg, Mat] = BeginMaterial(Path, BLEND_Translucent, MD_Surface);
		if (Pkg)
		{
			UMaterialExpressionFresnel* Edge = NewObject<UMaterialExpressionFresnel>(Mat);
			Edge->Exponent = 2.6f;
			Edge->BaseReflectFraction = 0.04f;
			Edge->MaterialExpressionEditorX = -700;
			Edge->MaterialExpressionEditorY = -300;
			Mat->GetExpressionCollection().AddExpression(Edge);

			UMaterialExpressionVectorParameter* Color = AddVectorParam(Mat, TEXT("Color"), FLinearColor(0.4f, 0.95f, 0.75f, 1.f), -500);
			UMaterialExpressionScalarParameter* Intensity = AddScalarParam(Mat, TEXT("Intensity"), 3.f, -60);

			UMaterialExpressionMultiply* EdgeTimesIntensity = AddMultiply(Mat, Edge, 0, Intensity, 0, -200);
			UMaterialExpressionMultiply* Emissive = AddMultiply(Mat, Color, 0, EdgeTimesIntensity, 0, -320);
			Mat->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
			Mat->GetEditorOnlyData()->Opacity.Connect(0, EdgeTimesIntensity);
			bOk &= FinishMaterial(Pkg, Mat);
		}
	}
	return bOk;
}

// ---------------------------------------------------------------------------
// AUD-42: real Niagara cue systems from the engine emitter templates.
//
// Engine content is mounted at /Niagara. The three systems are deliberately
// thin wrappers around stock emitters (SimpleSpriteBurst / DirectionalBurst /
// ConfettiBurst) so the runtime has a real particle asset while the static
// mesh placeholders remain the fallback. Assets that already exist on disk are
// kept untouched. A system is only saved after its emitter handle was added;
// every asset is re-loaded after saving and required to expose >= 1 emitter.
// ---------------------------------------------------------------------------
bool BootstrapNiagaraEffects()
{
	struct FNiagaraEffectSpec
	{
		const TCHAR* AssetName;
		const TCHAR* TemplateObjectPath;
	};

	// Engine templates (verified present in this engine install).
	static const FNiagaraEffectSpec Specs[] = {
		{ TEXT("NS_Impact"),    TEXT("/Niagara/DefaultAssets/Templates/Emitters/SimpleSpriteBurst.SimpleSpriteBurst") },
		{ TEXT("NS_Telegraph"), TEXT("/Niagara/DefaultAssets/Templates/Emitters/DirectionalBurst.DirectionalBurst") },
		{ TEXT("NS_Cast"),      TEXT("/Niagara/DefaultAssets/Templates/Emitters/ConfettiBurst.ConfettiBurst") },
	};

	bool bOk = true;
	const FString Directory = ContentRoot + TEXT("/VFX");

	for (const FNiagaraEffectSpec& Spec : Specs)
	{
		const FString AssetName(Spec.AssetName);
		const FString PackageName = Directory + TEXT("/") + AssetName;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;

		// "若已存在则跳过保存，保留已有": never overwrite an authored system.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			UNiagaraSystem* Existing = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
			if (Existing == nullptr || Existing->GetEmitterHandles().Num() == 0)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("BootstrapNiagaraEffects: existing asset %s is not a usable Niagara system; keeping it untouched"),
					*ObjectPath);
				bOk = false;
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("BootstrapNiagaraEffects: %s already exists; skipped"), *ObjectPath);
			}
			continue;
		}

		UNiagaraEmitter* TemplateEmitter = LoadObject<UNiagaraEmitter>(nullptr, Spec.TemplateObjectPath);
		if (TemplateEmitter == nullptr)
		{
			// Template/API unavailable: keep the runtime placeholder path and
			// report the failure rather than authoring a broken system.
			UE_LOG(LogTemp, Error,
				TEXT("BootstrapNiagaraEffects: template emitter missing at %s; %s not created (placeholder cues remain)"),
				Spec.TemplateObjectPath, *AssetName);
			bOk = false;
			continue;
		}

		UPackage* Package = CreateOrFetchPackage(ObjectPath);
		if (Package == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("BootstrapNiagaraEffects: package creation failed for %s"), *ObjectPath);
			bOk = false;
			continue;
		}

		UNiagaraSystem* System = NewObject<UNiagaraSystem>(
			Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		if (System == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("BootstrapNiagaraEffects: system allocation failed for %s"), *ObjectPath);
			bOk = false;
			continue;
		}
		// Use the engine factory's initialization path (system scripts + graph),
		// including the factory's default effect-type assignment.
		UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes=*/true);
		if (System->GetEffectType() == nullptr)
		{
			if (UNiagaraEffectType* DefaultEffectType = GetDefault<UNiagaraSettings>()->GetDefaultEffectType())
			{
				System->SetEffectType(DefaultEffectType);
			}
		}

		const FNiagaraAssetVersion ExposedVersion = TemplateEmitter->GetExposedVersion();
		const FGuid EmitterVersion = ExposedVersion.VersionGuid.IsValid() ? ExposedVersion.VersionGuid : FGuid();
		FNiagaraEditorUtilities::AddEmitterToSystem(
			*System, *TemplateEmitter, EmitterVersion, /*bCreateCopy=*/true);

		if (System->GetEmitterHandles().Num() == 0)
		{
			// Never save an emitter-less system (would be an unusable asset).
			UE_LOG(LogTemp, Error,
				TEXT("BootstrapNiagaraEffects: %s has no emitter handle after AddEmitterToSystem; not saved"),
				*ObjectPath);
			bOk = false;
			continue;
		}

		// Compile before reading exposed parameters: the store is populated by
		// the compile pass. Script VM only (no GPU shader wait) so the editor
		// commandlet stays reasonably fast.
		System->RequestCompile(false);
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/false, /*bShowProgress=*/false);

		// Optional user-parameter defaults. The stock templates do not expose
		// User.Color/User.SpriteSize, so a miss is expected and harmless; the
		// log makes it explicit rather than silently ignoring it.
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);
		TSet<FName> ParameterNames;
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			ParameterNames.Add(Parameter.GetName());
		}
		auto SetColorDefault = [System, &ParameterNames](const TCHAR* ParameterName, const FLinearColor& Value)
		{
			const FName Name(ParameterName);
			if (!ParameterNames.Contains(Name))
			{
				UE_LOG(LogTemp, Log,
					TEXT("BootstrapNiagaraEffects: user parameter %s not exposed by template; default skipped"),
					ParameterName);
				return;
			}
			const FNiagaraVariable Variable(FNiagaraTypeDefinition::GetColorDef(), Name);
			if (System->GetExposedParameters().SetParameterValue(Value, Variable))
			{
				UE_LOG(LogTemp, Log, TEXT("BootstrapNiagaraEffects: set %s default"), ParameterName);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("BootstrapNiagaraEffects: could not set %s default"), ParameterName);
			}
		};
		auto SetFloatDefault = [System, &ParameterNames](const TCHAR* ParameterName, float Value)
		{
			const FName Name(ParameterName);
			if (!ParameterNames.Contains(Name))
			{
				UE_LOG(LogTemp, Log,
					TEXT("BootstrapNiagaraEffects: user parameter %s not exposed by template; default skipped"),
					ParameterName);
				return;
			}
			const FNiagaraVariable Variable(FNiagaraTypeDefinition::GetFloatDef(), Name);
			if (System->GetExposedParameters().SetParameterValue(Value, Variable))
			{
				UE_LOG(LogTemp, Log, TEXT("BootstrapNiagaraEffects: set %s default"), ParameterName);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("BootstrapNiagaraEffects: could not set %s default"), ParameterName);
			}
		};
		SetColorDefault(TEXT("User.Color"), FLinearColor(0.55f, 0.85f, 1.f, 1.f));
		SetFloatDefault(TEXT("User.SpriteSize"), 12.f);

		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.Error = GError;
		Args.SaveFlags = SAVE_NoError;
		const FString FileName = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, System, *FileName, Args))
		{
			UE_LOG(LogTemp, Error, TEXT("BootstrapNiagaraEffects: save failed for %s"), *ObjectPath);
			bOk = false;
			continue;
		}
		FAssetRegistryModule::AssetCreated(System);

		// Post-save contract: loadable and carrying at least one emitter.
		UNiagaraSystem* Verified = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
		if (Verified == nullptr || Verified->GetEmitterHandles().Num() == 0)
		{
			UE_LOG(LogTemp, Error,
				TEXT("BootstrapNiagaraEffects: post-save verification failed for %s (loadable=%d emitters=%d); removing the bad package"),
				*ObjectPath, Verified ? 1 : 0, Verified ? Verified->GetEmitterHandles().Num() : 0);
			// The file did not exist before this run, so remove the just-written
			// package rather than leaving a bad asset for the runtime to load.
			IFileManager::Get().Delete(*FileName);
			bOk = false;
			continue;
		}

		UE_LOG(LogTemp, Log,
			TEXT("BootstrapNiagaraEffects: created %s from %s (%d emitter handle(s))"),
			*ObjectPath, Spec.TemplateObjectPath, Verified->GetEmitterHandles().Num());
	}

	return bOk;
}

namespace
{
	struct FSurfaceSet
	{
		FString AssetId;
		FString MaterialName;         // e.g. M_BambooWall
		FLinearColor FallbackColor;   // solid MIC colour when textures are absent/low quality
	};

	struct FSlotImportSettings
	{
		bool bSRGB = false;
		TextureCompressionSettings Compression = TC_Default;
		TextureGroup Group = TEXTUREGROUP_World;
	};

	FSlotImportSettings ImportSettingsForSlot(const FString& Slot)
	{
		FSlotImportSettings Settings;
		if (Slot == TEXT("BaseColor"))
		{
			Settings.bSRGB = true;
			Settings.Compression = TC_Default;
			Settings.Group = TEXTUREGROUP_World;
		}
		else if (Slot == TEXT("Normal"))
		{
			Settings.bSRGB = false;
			Settings.Compression = TC_Normalmap;
			Settings.Group = TEXTUREGROUP_WorldNormalMap;
		}
		else if (Slot == TEXT("Arm"))
		{
			Settings.bSRGB = false;
			Settings.Compression = TC_Masks;
			Settings.Group = TEXTUREGROUP_World;
		}
		else
		{
			// Roughness / AmbientOcclusion: linear single-channel data.
			Settings.bSRGB = false;
			Settings.Compression = TC_Grayscale;
			Settings.Group = TEXTUREGROUP_World;
		}
		return Settings;
	}

	FString FindSourceFile(const TArray<FString>& Files, const FString& Slot)
	{
		const FString Token = TEXT("_") + QinglanTextureSetContract::SourceFileToken(Slot) + TEXT("_");
		for (const FString& File : Files)
		{
			if (FPaths::GetBaseFilename(File).Contains(Token, ESearchCase::IgnoreCase))
			{
				return File;
			}
		}
		return FString();
	}

	/**
	 * Import one Poly Haven JPG as a real UTexture2D package asset at
	 * /Game/Qinglan/Textures/<Set>/<Slot>.
	 *
	 * Do not use FImageUtils::ImportBufferAsTexture2D here: it creates a
	 * transient, platform-data-only UTexture2D that UPackage::SavePackage
	 * silently declines to persist (the pre-AUD-55 bootstrap did exactly that,
	 * leaving every MIC pointing at a missing texture). The editor texture
	 * factory writes source data and honours SRGB/compression/LOD settings.
	 */
	UTexture2D* ImportSlotTexture(const FString& SetId, const FString& Slot, const FString& SourceFile)
	{
		const FString ObjectPath = QinglanTextureSetContract::TextureObjectPath(SetId, Slot);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		if (!IFileManager::Get().FileExists(*SourceFile))
		{
			UE_LOG(LogTemp, Warning, TEXT("texture source missing: %s"), *SourceFile);
			return nullptr;
		}

		const FSlotImportSettings Settings = ImportSettingsForSlot(Slot);
		UTextureFactory* Options = NewObject<UTextureFactory>(GetTransientPackage());
		Options->bCreateMaterial = false;
		Options->CompressionSettings = Settings.Compression;
		// UE 5.8: UTextureFactory exposes source colour-space mode rather than
		// a plain SRGB flag. Force Linear for data maps and SRGB for BaseColor.
		Options->ColorSpaceMode = Settings.bSRGB ? ETextureSourceColorSpace::SRGB : ETextureSourceColorSpace::Linear;
		Options->LODGroup = Settings.Group;
		Options->MipGenSettings = TMGS_FromTextureGroup;

		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = SourceFile;
		Task->DestinationPath = QinglanTextureSetContract::TextureFolderPath(SetId);
		Task->DestinationName = Slot;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->Options = Options;

		FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

		UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		if (!Tex)
		{
			UE_LOG(LogTemp, Error, TEXT("texture import failed: %s -> %s"), *SourceFile, *ObjectPath);
		}
		return Tex;
	}
}

bool UQinglanContentBootstrapCommandlet::BootstrapMaterials()
{
	UMaterial* Master = CreateOrLoadMasterMaterial();
	if (!Master)
	{
		return false;
	}

	// Solid-colour parent used when a texture set has no BaseColor (e.g.
	// fine_grained_wood on disk) and for the -lowquality package variant.
	// Keeping this as a material rather than leaving BaseColor unbound avoids
	// the black/checker default material appearing in the world.
	UMaterial* Solid = nullptr;
	{
		const FString Path = ContentRoot + TEXT("/Materials/M_QinglanSolid");
		const FString FileName = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*FileName, false, true, true);
		UPackage* Pkg = CreateOrFetchPackage(Path);
		Solid = NewObject<UMaterial>(Pkg, TEXT("M_QinglanSolid"), RF_Public | RF_Standalone);
		Solid->BlendMode = BLEND_Opaque;
		Solid->SetShadingModel(MSM_DefaultLit);

		UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(Solid);
		Color->ParameterName = TEXT("Color");
		Color->DefaultValue = FLinearColor(0.34f, 0.32f, 0.29f, 1.f);
		Color->MaterialExpressionEditorX = -500;
		Color->MaterialExpressionEditorY = -200;
		Solid->GetExpressionCollection().AddExpression(Color);

		UMaterialExpressionScalarParameter* Roughness = NewObject<UMaterialExpressionScalarParameter>(Solid);
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.85f;
		Roughness->MaterialExpressionEditorX = -500;
		Roughness->MaterialExpressionEditorY = 0;
		Solid->GetExpressionCollection().AddExpression(Roughness);

		Solid->GetEditorOnlyData()->BaseColor.Connect(0, Color);
		Solid->GetEditorOnlyData()->Roughness.Connect(0, Roughness);
		Solid->PreEditChange(nullptr);
		Solid->PostEditChange();
		SavePackage(Pkg, Solid);
	}

	const FString TexRoot = RawAssetsRoot + TEXT("/polyhaven/textures");
	TArray<FSurfaceSet> Sets;
	auto AddSet = [&Sets](const FString& Id, const FString& Name, const FLinearColor& Color)
	{
		FSurfaceSet S;
		S.AssetId = Id;
		S.MaterialName = Name;
		S.FallbackColor = Color;
		Sets.Add(S);
	};
	AddSet(TEXT("bamboo_wall"), TEXT("M_BambooWall"), FLinearColor(0.24f, 0.28f, 0.14f, 1.f));
	AddSet(TEXT("cobblestone_01"), TEXT("M_Cobblestone"), FLinearColor(0.33f, 0.32f, 0.30f, 1.f));
	AddSet(TEXT("cobblestone_04"), TEXT("M_CobblestoneAlt"), FLinearColor(0.34f, 0.33f, 0.31f, 1.f));
	AddSet(TEXT("grey_stone_path"), TEXT("M_StonePath"), FLinearColor(0.38f, 0.38f, 0.37f, 1.f));
	AddSet(TEXT("rock_path"), TEXT("M_RockPath"), FLinearColor(0.36f, 0.34f, 0.31f, 1.f));
	AddSet(TEXT("dark_wooden_planks"), TEXT("M_DarkWoodPlanks"), FLinearColor(0.13f, 0.09f, 0.06f, 1.f));
	AddSet(TEXT("fine_grained_wood"), TEXT("M_FineWood"), FLinearColor(0.28f, 0.19f, 0.11f, 1.f));
	AddSet(TEXT("gravel_concrete"), TEXT("M_Gravel"), FLinearColor(0.43f, 0.42f, 0.40f, 1.f));
	AddSet(TEXT("aerial_grass_rock"), TEXT("M_GrassRock"), FLinearColor(0.20f, 0.29f, 0.11f, 1.f));
	AddSet(TEXT("brown_mud_rocks_01"), TEXT("M_MudRock"), FLinearColor(0.28f, 0.20f, 0.13f, 1.f));
	AddSet(TEXT("volcanic_rock_tiles"), TEXT("M_VolcanicRock"), FLinearColor(0.14f, 0.13f, 0.13f, 1.f));
	AddSet(TEXT("mossy_brick"), TEXT("M_MossyBrick"), FLinearColor(0.26f, 0.30f, 0.20f, 1.f));
	AddSet(TEXT("moss_wood"), TEXT("M_MossWood"), FLinearColor(0.22f, 0.25f, 0.13f, 1.f));
	AddSet(TEXT("cliff_side"), TEXT("M_Cliff"), FLinearColor(0.36f, 0.35f, 0.32f, 1.f));
	AddSet(TEXT("dirt_floor"), TEXT("M_DirtFloor"), FLinearColor(0.30f, 0.23f, 0.15f, 1.f));
	AddSet(TEXT("grey_roof_tiles"), TEXT("M_RoofTiles"), FLinearColor(0.24f, 0.25f, 0.26f, 1.f));
	AddSet(TEXT("clay_roof_tiles"), TEXT("M_ClayRoofTiles"), FLinearColor(0.40f, 0.19f, 0.12f, 1.f));
	AddSet(TEXT("thatch_roof_angled"), TEXT("M_Thatch"), FLinearColor(0.39f, 0.31f, 0.17f, 1.f));
	AddSet(TEXT("concrete_rock_path"), TEXT("M_ConcretePath"), FLinearColor(0.44f, 0.43f, 0.41f, 1.f));
	AddSet(TEXT("bamboo_wall_02"), TEXT("M_BambooWallAlt"), FLinearColor(0.22f, 0.26f, 0.13f, 1.f));
	AddSet(TEXT("bamboo_veneer"), TEXT("M_BambooVeneer"), FLinearColor(0.30f, 0.25f, 0.12f, 1.f));

	static const TCHAR* SlotNames[] = {
		TEXT("BaseColor"), TEXT("Roughness"), TEXT("AmbientOcclusion"), TEXT("Arm"), TEXT("Normal")
	};

	auto AcquireMIC = [](const FString& Name) -> UMaterialInstanceConstant*
	{
		// Fully-load the package before editing/saving it. LoadObject alone can
		// leave a package partially loaded, which UE 5.8 refuses to save.
		UPackage* Pkg = CreateOrFetchPackage(ContentRoot + TEXT("/Materials/") + Name);
		const FName AssetName(*Name);
		if (UMaterialInstanceConstant* Existing = FindObject<UMaterialInstanceConstant>(Pkg, *AssetName.ToString()))
		{
			return Existing;
		}
		return NewObject<UMaterialInstanceConstant>(Pkg, AssetName, RF_Public | RF_Standalone);
	};

	int32 ImportedTextures = 0;
	int32 TexturedSets = 0;
	bool bAnyOk = false;
	for (const FSurfaceSet& Set : Sets)
	{
		const FString Dir = TexRoot + TEXT("/") + Set.AssetId;
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jpg"), true, false, false);
		Files.Sort(); // deterministic suffix choice across filesystems

		TMap<FString, UTexture2D*> SlotTextures;
		for (const TCHAR* SlotName : SlotNames)
		{
			const FString Slot(SlotName);
			const FString Source = FindSourceFile(Files, Slot);
			if (Source.IsEmpty())
			{
				continue; // this surface set genuinely has no such map
			}
			if (UTexture2D* Tex = ImportSlotTexture(Set.AssetId, Slot, Source))
			{
				SlotTextures.Add(Slot, Tex);
				++ImportedTextures;
			}
		}

		const bool bHasBaseColor = SlotTextures.Contains(TEXT("BaseColor"));
		if (bHasBaseColor)
		{
			GTexturedSurfaceMaterials.Add(Set.MaterialName);
			++TexturedSets;
		}

		// Textured instance. Sets without a BaseColor use the solid parent so a
		// missing source never yields a black textured material.
		UMaterialInstanceConstant* MIC = AcquireMIC(Set.MaterialName);
		MIC->Parent = bHasBaseColor ? Master : Solid; // UE 5.8: SetParentMasterMaterial removed; assign Parent directly
		MIC->ClearParameterValuesEditorOnly();
		if (bHasBaseColor)
		{
			for (const TCHAR* SlotName : SlotNames)
			{
				const FString Slot(SlotName);
				const FString ParamName = QinglanTextureSetContract::MaterialParameterName(Slot);
				if (ParamName.IsEmpty())
				{
					continue; // ARM is channel-packed; the AO fallback below uses its R channel
				}
				UTexture2D** Found = SlotTextures.Find(Slot);
				// cobblestone_01 / gravel_concrete have no _ao_ map; the packed ARM
				// map stores AO in R, which is the channel the master samples.
				if (!Found && Slot == TEXT("AmbientOcclusion"))
				{
					Found = SlotTextures.Find(TEXT("Arm"));
				}
				if (Found && *Found)
				{
					MIC->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*ParamName)), *Found); // UE 5.8: MIC editor-only setter
				}
			}
		}
		else
		{
			MIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Color"))), Set.FallbackColor);
			MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Roughness"))), 0.85f);
		}
		MIC->PostEditChange();
		bAnyOk |= SavePackage(Cast<UPackage>(MIC->GetOutermost()), MIC);

		// Companion solid instance for Low quality and for map resolution when
		// the texture set has no BaseColor. Always generated, even in a
		// fully textured run, so the material contract stays stable.
		UMaterialInstanceConstant* SolidMIC = AcquireMIC(QinglanTextureSetContract::SolidMaterialAssetName(Set.MaterialName));
		SolidMIC->Parent = Solid;
		SolidMIC->ClearParameterValuesEditorOnly();
		SolidMIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Color"))), Set.FallbackColor);
		SolidMIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Roughness"))), 0.85f);
		SolidMIC->PostEditChange();
		bAnyOk |= SavePackage(Cast<UPackage>(SolidMIC->GetOutermost()), SolidMIC);

		UE_LOG(LogTemp, Log, TEXT("material %s from %s (%d texture map(s)%s)"),
			*Set.MaterialName, *Set.AssetId, SlotTextures.Num(),
			bHasBaseColor ? TEXT("") : TEXT(", solid fallback: no BaseColor"));
	}

	// AUD-55: hero character material. The Quaternius Human pack ships its
	// clothed skin texture; MeshImport imports it before this commandlet runs.
	// The mesh is otherwise rendered with the engine grey default, which read
	// as an untextured blockout in the packaged screenshots.
	if (UTexture2D* Skin = LoadObject<UTexture2D>(nullptr,
			TEXT("/Game/Qinglan/Characters/Human/T_Human_Clothed.T_Human_Clothed")))
	{
		UMaterialInstanceConstant* HumanMIC = AcquireMIC(TEXT("MIC_HumanCharacter"));
		HumanMIC->Parent = Master;
		HumanMIC->ClearParameterValuesEditorOnly();
		HumanMIC->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo(FName(TEXT("BaseColor"))), Skin);
		HumanMIC->SetScalarParameterValueEditorOnly(
			FMaterialParameterInfo(FName(TEXT("Tiling"))), 1.f);
		HumanMIC->SetScalarParameterValueEditorOnly(
			FMaterialParameterInfo(FName(TEXT("Roughness"))), 0.75f);
		HumanMIC->PostEditChange();
		bAnyOk |= SavePackage(Cast<UPackage>(HumanMIC->GetOutermost()), HumanMIC);
		UE_LOG(LogTemp, Log, TEXT("material MIC_HumanCharacter from Quaternius clothed skin"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("MIC_HumanCharacter skipped: T_Human_Clothed not imported"));
	}

	UE_LOG(LogTemp, Log, TEXT("BootstrapMaterials: %d surface sets, %d textured, %d Texture2D assets"),
		Sets.Num(), TexturedSets, ImportedTextures);
	return bAnyOk;
}
//----------------------------------------------------------------------------
// Maps
//----------------------------------------------------------------------------

namespace
{
	UStaticMesh* EngineCube()
	{
		static UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		return Cube;
	}
	UStaticMesh* EngineCylinder()
	{
		static UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		return Cyl;
	}

	AStaticMeshActor* SpawnShape(UWorld* World, UStaticMesh* Mesh, const FVector& Center, const FVector& Size,
		UMaterialInterface* Material, const FName& Label)
	{
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(World, AStaticMeshActor::StaticClass(), Label);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested; // re-runnable bootstrap
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label.ToString());
		UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
		Comp->SetStaticMesh(Mesh);
		Comp->SetMobility(EComponentMobility::Static);
		Comp->SetCollisionProfileName(TEXT("BlockAll"));
		Actor->SetActorLocation(Center);
		Actor->SetActorScale3D(Size / FVector(100.f));
		if (Material)
		{
			Comp->SetMaterial(0, Material);
		}
		return Actor;
	}

	ATargetPoint* SpawnAnchor(UWorld* World, const FVector& Location, const FName& Name)
	{
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(World, ATargetPoint::StaticClass(), Name);
		// UE 5.8 defaults a named spawn to Required_Fatal; a repeated bootstrap
		// pass would trip the unique-name fatal, so let the engine re-uniquify.
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		ATargetPoint* Point = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Point)
		{
			Point->SetActorLabel(Name.ToString());
		}
		return Point;
	}

	/**
	 * Resolve a surface material through the AUD-55 contract. Low quality or a
	 * set with no imported BaseColor binds the solid-colour companion; the last
	 * resort is the solid master itself, never an unbound textured MIC.
	 */
	UMaterialInterface* Mat(const TCHAR* Name)
	{
		const FString SetName(Name);
		const FString ObjectPath = QinglanTextureSetContract::ResolveSurfaceMaterialObjectPath(
			SetName, GUseLowQualitySurfaceMaterials, GTexturedSurfaceMaterials.Contains(SetName));
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *ObjectPath))
		{
			return Material;
		}
		UE_LOG(LogTemp, Warning, TEXT("surface material missing: %s; using solid master"), *ObjectPath);
		return LoadObject<UMaterialInterface>(nullptr, *(ContentRoot + TEXT("/Materials/M_QinglanSolid.M_QinglanSolid")));
	}

	// -----------------------------------------------------------------------
	// AUD-55 nature dressing.
	//
	// Deterministic, collision-free scatter over the imported Quaternius
	// nature meshes (Bush/Grass/Rock/Tree; Tree3 is skipped by MeshImport
	// because the FBX SDK cannot triangulate it). Decoration uses NoCollision
	// and CanEverAffectNavigation=false so it never blocks the authored
	// walkable floors or navigation. Placement rejects points near every
	// interaction anchor / PlayerStart and inside an optional exclusion box
	// (the hub hall), which also keeps corridor mouths clear because the
	// scatter radius stays inside the room perimeter.
	//
	// Remaining art work (cannot be authored by code alone):
	// - Niagara hero VFX for techniques/impacts/boss tells; current cue
	//   materials are placeholder unlit/additive meshes.
	// - Hand-composed scene composition (architecture silhouettes, landmark
	//   framing, prop clusters) replacing the deterministic scatter.
	// - Final lighting/material render screenshots and target-hardware
	//   performance validation; none of that is asserted by this commandlet.
	// -----------------------------------------------------------------------
	UStaticMesh* NatureMesh(const TCHAR* NameRaw)
	{
		static TMap<FString, UStaticMesh*> Cache;
		const FString Name(NameRaw);
		if (UStaticMesh** Found = Cache.Find(Name))
		{
			return *Found;
		}
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr,
			*(ContentRoot + TEXT("/Environments/Nature/") + Name + TEXT(".") + Name));
		Cache.Add(Name, Mesh);
		if (!Mesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("nature mesh not imported (run MeshImport first): %s"), *Name);
		}
		return Mesh;
	}

	UMaterialInterface* NatureMaterialFor(const FString& MeshName)
	{
		if (MeshName.StartsWith(TEXT("Rock")))
		{
			return Mat(TEXT("M_RockPath"));
		}
		if (MeshName.StartsWith(TEXT("Tree")))
		{
			return Mat(TEXT("M_MossWood"));
		}
		return Mat(TEXT("M_GrassRock"));
	}

	AStaticMeshActor* SpawnDecoMesh(UWorld* World, UStaticMesh* Mesh, const FString& MeshName,
		const FVector& Location, float YawDeg, float UniformScale, const FString& Label)
	{
		if (!Mesh)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.Name = MakeUniqueObjectName(World, AStaticMeshActor::StaticClass(), FName(*Label));
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0.f, YawDeg, 0.f), Params);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent();
		Comp->SetMobility(EComponentMobility::Static);
		Comp->SetStaticMesh(Mesh);
		// Decoration must not block navigation or gameplay traces.
		Comp->SetCollisionProfileName(TEXT("NoCollision"));
		Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Comp->SetCanEverAffectNavigation(false);
		Comp->SetGenerateOverlapEvents(false);
		Actor->SetActorScale3D(FVector(UniformScale));
		if (UMaterialInterface* Material = NatureMaterialFor(MeshName))
		{
			const int32 NumMaterials = Comp->GetNumMaterials();
			for (int32 Index = 0; Index < NumMaterials; ++Index)
			{
				Comp->SetMaterial(Index, Material);
			}
		}
		return Actor;
	}

	bool DressingSpotAllowed(const FVector& Location, const TArray<FVector>& AvoidPoints,
		const TArray<FVector>& Placed, const FVector2D& AvoidMin, const FVector2D& AvoidMax, bool bUseAvoidBox)
	{
		constexpr float MinAnchorDistanceSq = 450.f * 450.f;
		constexpr float MinPropSpacingSq = 180.f * 180.f;
		for (const FVector& Avoid : AvoidPoints)
		{
			if (FVector::DistSquared2D(Location, Avoid) < MinAnchorDistanceSq)
			{
				return false;
			}
		}
		for (const FVector& Other : Placed)
		{
			if (FVector::DistSquared2D(Location, Other) < MinPropSpacingSq)
			{
				return false;
			}
		}
		if (bUseAvoidBox
			&& Location.X >= AvoidMin.X && Location.X <= AvoidMax.X
			&& Location.Y >= AvoidMin.Y && Location.Y <= AvoidMax.Y)
		{
			return false;
		}
		return true;
	}

	/** Fixed-seed ring scatter; Count is a hard cap per call. */
	void ScatterNature(UWorld* World, const FString& LabelPrefix, int32 Seed,
		const FVector& Center, float FloorZ, float MinRadius, float MaxRadius, int32 Count,
		TArrayView<const TCHAR*> MeshNames, const TArray<FVector>& AvoidPoints,
		TArray<FVector>& Placed, const FVector2D& AvoidMin, const FVector2D& AvoidMax, bool bUseAvoidBox)
	{
		if (Count <= 0 || MeshNames.Num() == 0)
		{
			return;
		}
		// The -lowquality package variant halves the fixed dressing budget.
		// Runtime quality presets cannot add/remove static actors, so the
		// standard build keeps the full deterministic set.
		const int32 EffectiveCount = GUseLowQualitySurfaceMaterials ? FMath::Max(0, Count / 2) : Count;
		FRandomStream Rand(Seed);
		int32 Spawned = 0;
		const int32 MaxAttempts = EffectiveCount * 12;
		for (int32 Attempt = 0; Attempt < MaxAttempts && Spawned < EffectiveCount; ++Attempt)
		{
			const float Angle = Rand.FRandRange(0.f, 2.f * PI);
			const float Radius = Rand.FRandRange(MinRadius, MaxRadius);
			const FVector Location(Center.X + FMath::Cos(Angle) * Radius,
				Center.Y + FMath::Sin(Angle) * Radius, FloorZ);
			if (!DressingSpotAllowed(Location, AvoidPoints, Placed, AvoidMin, AvoidMax, bUseAvoidBox))
			{
				continue;
			}
			const TCHAR* MeshName = MeshNames[Rand.RandRange(0, MeshNames.Num() - 1)];
			UStaticMesh* Mesh = NatureMesh(MeshName);
			if (!Mesh)
			{
				continue;
			}
			const float Scale = Rand.FRandRange(0.8f, 1.35f);
			const float Yaw = Rand.FRandRange(0.f, 360.f);
			const FString Label = FString::Printf(TEXT("%s_%s_%d"), *LabelPrefix, MeshName, Placed.Num());
			if (SpawnDecoMesh(World, Mesh, MeshName, Location, Yaw, Scale, Label))
			{
				Placed.Add(Location);
				++Spawned;
			}
		}
	}

	void StandardLighting(UWorld* World, const FLinearColor& SunTint, float SunIntensity, float SunPitchDeg, float SunYawDeg)
	{
		FActorSpawnParameters p;
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FVector(0, 0, 3000), FRotator(SunPitchDeg, SunYawDeg, 0), p);
		if (Sun)
		{
			Sun->SetActorLabel(TEXT("Sun"));
			Sun->GetLightComponent()->SetIntensity(SunIntensity);
			Sun->GetLightComponent()->SetLightColor(SunTint);
			Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
			// AUD-55: drive the sky atmosphere from the sun so the horizon is
			// lit instead of rendering as a black void in the packaged build.
			if (UDirectionalLightComponent* SunComp = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
			{
				SunComp->SetAtmosphereSunLight(true);
			}
		}
		ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>(ASkyAtmosphere::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, p);
		if (Atmosphere)
		{
			Atmosphere->SetActorLabel(TEXT("SkyAtmosphere"));
		}
		ASkyLight* Sky = World->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FVector(0, 0, 500), FRotator::ZeroRotator, p);
		if (Sky)
		{
			Sky->SetActorLabel(TEXT("SkyLight"));
			Sky->GetLightComponent()->SetIntensity(1.0f);
			Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		}
		AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, p);
		if (Fog)
		{
			Fog->SetActorLabel(TEXT("HeightFog"));
			if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
			{
				FogComp->FogDensity = 0.02f;
			}
		}
	}

	UWorld* CreateNewMapWorld(const FString& MapName)
	{
		// Re-runs must rebuild the map from scratch: a saved world already owns
		// its PersistentLevel/WorldSettings names, and spawning into it trips
		// the unique-name fatal. Delete the previous file; the bootstrap is the
		// single author of this layout (docs/dev/level-layout.md).
		const FString PackageName = ContentRoot + TEXT("/Maps/") + MapName;
		const FString MapFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
		IFileManager::Get().Delete(*MapFileName, /*bRequireExists=*/false, /*bEvenReadOnly=*/true, /*bQuiet=*/true);

		const FString PackagePath = PackageName + TEXT(".") + MapName;
		UPackage* Package = CreateOrFetchPackage(PackagePath);
		UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
		if (World)
		{
			// SavePackage refuses worlds without the public-asset flags
			// ("saving would cause data loss"); stamp them at creation.
			World->SetFlags(RF_Public | RF_Standalone);
		}
		return World;
	}


	void SetMapGameMode(UWorld* World, UClass* GameModeClass)
	{
		if (World && World->GetWorldSettings())
		{
			World->GetWorldSettings()->DefaultGameMode = GameModeClass;
		}
	}

	bool SaveWorldMap(UWorld* World)
	{
		UPackage* Package = World->GetOutermost();
		FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetMapPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, World, *FileName, Args);
	}

	void ConfigureKillZ(UWorld* World, float KillZ)
	{
		// Falls off any exposed edge must cross KillZ so the engine kills the
		// pawn (routed through the health-defeat flow) instead of falling
		// forever below the walkable surface.
		if (AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr)
		{
			Settings->KillZ = KillZ;
		}
	}
}

bool UQinglanContentBootstrapCommandlet::BootstrapMaps()
{
	bool bOk = true;

	// --- L_Hub: mountain sanctuary courtyard on terraces.
	if (UWorld* Hub = CreateNewMapWorld(TEXT("L_Hub")))
	{
		// AUD-55: ground/walls bind textured surface sets (cobblestone and dark
		// planks both have imported BaseColor maps). Collision stays BlockAll.
		UMaterialInterface* Cliff = Mat(TEXT("M_Cliff"));
		UMaterialInterface* Stone = Mat(TEXT("M_Cobblestone"));
		UMaterialInterface* Wood = Mat(TEXT("M_DarkWoodPlanks"));
		UMaterialInterface* Roof = Mat(TEXT("M_RoofTiles"));

		// Terrace mountain: stacked slabs leading to a 60 m courtyard plateau.
		SpawnShape(Hub, EngineCube(), FVector(0, 0, -1600), FVector(22000, 22000, 800), Cliff, TEXT("Hub_Terrace_0"));
		SpawnShape(Hub, EngineCube(), FVector(0, 0, -800), FVector(18000, 18000, 800), Cliff, TEXT("Hub_Terrace_1"));
		SpawnShape(Hub, EngineCube(), FVector(0, 0, -200), FVector(15000, 15000, 800), Cliff, TEXT("Hub_Terrace_2"));
		SpawnShape(Hub, EngineCube(), FVector(0, 0, 400), FVector(12000, 12000, 400), Stone, TEXT("Hub_Courtyard_Floor"));

		// Hall shell: floor, four walls, roof (dressing pass replaces boxes).
		const FVector HallCenter(0, -3800, 700);
		SpawnShape(Hub, EngineCube(), HallCenter + FVector(0, 0, 0), FVector(3000, 2000, 40), Wood, TEXT("Hub_Hall_Floor"));
		SpawnShape(Hub, EngineCube(), HallCenter + FVector(0, 1000, 300), FVector(3000, 100, 600), Wood, TEXT("Hub_Hall_Wall_N"));
		SpawnShape(Hub, EngineCube(), HallCenter + FVector(-1500, 0, 300), FVector(100, 2000, 600), Wood, TEXT("Hub_Hall_Wall_W"));
		SpawnShape(Hub, EngineCube(), HallCenter + FVector(1500, 0, 300), FVector(100, 2000, 600), Wood, TEXT("Hub_Hall_Wall_E"));
		SpawnShape(Hub, EngineCube(), HallCenter + FVector(0, 0, 640), FVector(3400, 2400, 160), Roof, TEXT("Hub_Hall_Roof"));

		StandardLighting(Hub, FLinearColor(1.0f, 0.96f, 0.88f), 6.0f, -50.f, 35.f);
		ConfigureKillZ(Hub, -5000.f); // terrace stack reaches -2000; falls route to defeat

		// Interaction anchors (docs/dev/level-layout.md). Z=600 is the courtyard
		// floor surface (slab center 400, thickness 400); anchors below it would
		// spawn/focus inside the floor.
		const TArray<TPair<FName, FVector>> HubAnchors = {
			{TEXT("hub.courtyard"), FVector(0, 0, 600)},
			{TEXT("hub.training_yard"), FVector(3000, 2500, 600)},
			{TEXT("hub.cultivation_shrine"), FVector(-3200, 2500, 600)},
			{TEXT("hub.shrine"), FVector(0, 3200, 600)},
			{TEXT("hub.workbench"), FVector(2500, -2000, 600)},
			{TEXT("hub.lamp_workbench"), FVector(2500, -2600, 600)},
			{TEXT("hub.bamboo_gate"), FVector(0, -4800, 600)},
			{TEXT("hub.handover_table"), FVector(-1200, -3400, 600)},
			{TEXT("hub.archive_shelf"), FVector(-2400, -3000, 600)},
			{TEXT("hub.archive_table"), FVector(-2600, -2200, 600)},
			{TEXT("hub.courier_board"), FVector(1600, 3000, 600)},
			{TEXT("hub.inspection_board"), FVector(-1600, 3000, 600)},
			{TEXT("hub.water_table"), FVector(3200, -400, 600)},
			{TEXT("hub.stonewright_crate"), FVector(3600, 800, 600)},
			{TEXT("hub.old_table"), FVector(-3800, 800, 600)},
			{TEXT("hub.training_mat"), FVector(3000, 3300, 600)},
			// AUD-10: CH05_S03 / CH07_S01-S03 scene anchors (ledger + correspondence).
			{TEXT("hub.ledger_table"), FVector(-600, 1800, 600)},
			{TEXT("hub.correspondence_table"), FVector(-1000, -1400, 600)},
			{TEXT("record.hub.apprentice_quarters"), FVector(2200, 1600, 600)},
			{TEXT("record.hub.courier_shelf"), FVector(1400, 3400, 600)},
			{TEXT("record.hub.dry_bell"), FVector(0, 600, 600)},
			{TEXT("record.hub.governor_reader"), FVector(-3000, 1400, 600)},
			{TEXT("record.hub.post_ending_noticeboard"), FVector(600, 3600, 600)},
			{TEXT("record.hub.post_ending_workshop"), FVector(4200, 200, 600)},
			{TEXT("record.hub.practice_rack"), FVector(3800, 3000, 600)},
			{TEXT("record.hub.sealed_dispatch_packet"), FVector(800, -3600, 600)},
			{TEXT("record.hub.workshop"), FVector(2800, -1200, 600)},
		};
		for (const auto& A : HubAnchors)
		{
			SpawnAnchor(Hub, A.Value, A.Key);
		}
		APlayerStart* Start = Hub->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FVector(0, 1000, 600), FRotator(0, 180, 0), FActorSpawnParameters());
		if (Start) { Start->SetActorLabel(TEXT("PlayerStart")); }

		// Courtyard dressing ring, clear of every anchor/PlayerStart and the
		// hall interior. Fixed seeds keep regeneration byte-identical.
		TArray<FVector> HubAvoid;
		for (const auto& A : HubAnchors)
		{
			HubAvoid.Add(A.Value);
		}
		HubAvoid.Add(FVector(0, 1000, 600));
		TArray<FVector> HubPlaced;
		static const TCHAR* HubGrass[] = { TEXT("Grass1"), TEXT("Grass2"), TEXT("Grass3") };
		static const TCHAR* HubBush[] = { TEXT("Bush1"), TEXT("Bush2"), TEXT("Bush3") };
		static const TCHAR* HubRock[] = { TEXT("Rock1"), TEXT("Rock2"), TEXT("Rock3") };
		static const TCHAR* HubTree[] = { TEXT("Tree1"), TEXT("Tree2"), TEXT("Tree4") }; // Tree3 is unimportable
		const FVector2D HallMin(-1900.f, -5100.f);
		const FVector2D HallMax(1900.f, -2700.f);
		ScatterNature(Hub, TEXT("Hub_Grass"), 101, FVector::ZeroVector, 600.f, 4300.f, 5500.f, 6,
			MakeArrayView(HubGrass), HubAvoid, HubPlaced, HallMin, HallMax, true);
		ScatterNature(Hub, TEXT("Hub_Bush"), 102, FVector::ZeroVector, 600.f, 4300.f, 5500.f, 4,
			MakeArrayView(HubBush), HubAvoid, HubPlaced, HallMin, HallMax, true);
		ScatterNature(Hub, TEXT("Hub_Rock"), 103, FVector::ZeroVector, 600.f, 4300.f, 5500.f, 4,
			MakeArrayView(HubRock), HubAvoid, HubPlaced, HallMin, HallMax, true);
		ScatterNature(Hub, TEXT("Hub_Tree"), 104, FVector::ZeroVector, 600.f, 4300.f, 5500.f, 2,
			MakeArrayView(HubTree), HubAvoid, HubPlaced, HallMin, HallMax, true);

		bOk &= SaveWorldMap(Hub);
		UE_LOG(LogTemp, Log, TEXT("L_Hub created"));
	}

	// --- L_BambooHollow: 7 rooms along a winding corridor, 45 m per side.
	if (UWorld* Bamboo = CreateNewMapWorld(TEXT("L_BambooHollow")))
	{
		SetMapGameMode(Bamboo, LoadClass<AGameModeBase>(nullptr, TEXT("/Script/QinglanAscension.QinglanRealmGameMode")));
		UMaterialInterface* Grass = Mat(TEXT("M_GrassRock"));
		UMaterialInterface* BambooMat = Mat(TEXT("M_BambooVeneer"));
		UMaterialInterface* Dirt = Mat(TEXT("M_DirtFloor"));

		// Rooms 0..6 spaced 55 m apart along +X; each 45 m across (docs/01 rooms).
		for (int32 i = 0; i < QinglanRoomLayout::RoomCount; ++i)
		{
			const float X = QinglanRoomLayout::RoomCenter(i).X;
			SpawnShape(Bamboo, EngineCube(), FVector(X, 0, -50), FVector(4500, 4500, 100), i % 2 ? Grass : Dirt, *FString::Printf(TEXT("Bamboo_Room%d_Floor"), i));
			// Corridor to next room.
			if (i + 1 < QinglanRoomLayout::RoomCount)
			{
				SpawnShape(Bamboo, EngineCube(), FVector(X + 2750.f, 0, -50), FVector(1000, 800, 100), Grass, *FString::Printf(TEXT("Bamboo_Corr%d"), i));
			}
			// Bamboo grove dressing: cylinders along room edges.
			for (int32 b = 0; b < 8; ++b)
			{
				const float Ang = b * PI / 4.f;
				const FVector C(X + FMath::Cos(Ang) * 1800.f, FMath::Sin(Ang) * 1800.f, 400.f);
				SpawnShape(Bamboo, EngineCylinder(), C, FVector(60, 60, 900), BambooMat, *FString::Printf(TEXT("Bamboo_Room%d_Stalk%d"), i, b));
			}
		}
		StandardLighting(Bamboo, FLinearColor(0.85f, 0.95f, 0.85f), 4.5f, -65.f, 20.f);
		ConfigureKillZ(Bamboo, -3000.f); // room/corridor floors top at Z=0

		const TArray<TPair<FName, FVector>> BambooAnchors = {
			{TEXT("bamboo.room_0"), FVector(0, 0, 100)},
			{TEXT("bamboo.rooms_1_to_5"), FVector(11000, 0, 100)},
			{TEXT("bamboo.room_1_2_guided"), FVector(5500, 0, 100)},
			// AUD-22: the exit station lives in the final room (index 6); the
			// victory authority requires standing there after the boss room.
			{TEXT("bamboo.room_5_safe"), QinglanRoomLayout::RoomCenter(6) + FVector(0.f, 0.f, 100.f)},
			{TEXT("sq.bamboo.old_bell_safe"), FVector(11000, 1500, 100)},
			{TEXT("sq.bamboo.picnic_shelf_safe"), FVector(16500, -1500, 100)},
			{TEXT("sq.bamboo.spillway_safe"), FVector(11000, -1500, 100)},
			{TEXT("sq.bamboo.wheel_safe"), FVector(16500, 1500, 100)},
			{TEXT("record.bamboo.room_2_alcove"), FVector(11000, 1800, 100)},
			{TEXT("record.bamboo.lower_stair"), FVector(22000, 1200, 100)},
			{TEXT("record.bamboo.reader_archive"), FVector(22000, -1200, 100)},
			{TEXT("record.bamboo.spillway_map_board"), FVector(9500, 1900, 100)},
			{TEXT("record.bamboo.anchor_maintenance_panel"), FVector(15000, -1900, 100)},
			{TEXT("record.bamboo.abandoned_supply_chest"), FVector(17500, 900, 100)},
			{TEXT("record.bamboo.disused_spillway_shelter"), FVector(9500, -1900, 100)},
		};
		for (const auto& A : BambooAnchors)
		{
			SpawnAnchor(Bamboo, A.Value, A.Key);
		}
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			APlayerStart* PS = Bamboo->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), QinglanRoomLayout::RoomEntrance(Room), FRotator::ZeroRotator, FActorSpawnParameters());
			if (PS)
			{
				PS->SetActorLabel(FString::Printf(TEXT("PlayerStart_Room%d"), Room));
				PS->PlayerStartTag = QinglanRoomLayout::EntranceTag(Room);
				PS->Tags.AddUnique(PS->PlayerStartTag);
			}
		}

		// AUD-55 dressing: fixed-seed perimeter scatter per room plus two
		// corridor props outside the 800 cm walkable strip. The ring radius
		// stays inside the 2250 cm room half-extent, so corridor mouths and
		// PlayerStarts remain clear; every anchor is also distance-checked.
		TArray<FVector> BambooAvoid;
		for (const auto& A : BambooAnchors)
		{
			BambooAvoid.Add(A.Value);
		}
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			BambooAvoid.Add(QinglanRoomLayout::RoomEntrance(Room));
		}
		static const TCHAR* BambooGrass[] = { TEXT("Grass1"), TEXT("Grass2"), TEXT("Grass3") };
		static const TCHAR* BambooBush[] = { TEXT("Bush1"), TEXT("Bush2"), TEXT("Bush3") };
		static const TCHAR* BambooRock[] = { TEXT("Rock1"), TEXT("Rock2") };
		static const TCHAR* BambooTree[] = { TEXT("Tree1"), TEXT("Tree2"), TEXT("Tree4") }; // Tree3 is unimportable
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			const FVector Center = QinglanRoomLayout::RoomCenter(Room);
			TArray<FVector> RoomPlaced;
			ScatterNature(Bamboo, FString::Printf(TEXT("Bamboo_Room%d_Grass"), Room), 2000 + Room * 11,
				Center, 0.f, 1400.f, 1950.f, 4, MakeArrayView(BambooGrass), BambooAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Bamboo, FString::Printf(TEXT("Bamboo_Room%d_Bush"), Room), 2100 + Room * 11,
				Center, 0.f, 1500.f, 1950.f, 2, MakeArrayView(BambooBush), BambooAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Bamboo, FString::Printf(TEXT("Bamboo_Room%d_Rock"), Room), 2200 + Room * 11,
				Center, 0.f, 1600.f, 2000.f, 1, MakeArrayView(BambooRock), BambooAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Bamboo, FString::Printf(TEXT("Bamboo_Room%d_Tree"), Room), 2300 + Room * 11,
				Center, 0.f, 1700.f, 1950.f, 1, MakeArrayView(BambooTree), BambooAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		}
		TArray<FVector> BambooCorridorPlaced;
		for (int32 Room = 0; Room + 1 < QinglanRoomLayout::RoomCount; ++Room)
		{
			const FVector CorridorCenter = QinglanRoomLayout::RoomCenter(Room) + FVector(2750.f, 0.f, 0.f);
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				if (GUseLowQualitySurfaceMaterials && Side < 0)
				{
					continue; // low-quality variant keeps one prop per corridor
				}
				// Corridor floor is 800 cm wide (Y +/-400); sit props just inside
				// the edge, leaving the centre strip walkable.
				const FVector Location(CorridorCenter.X, Side * 320.f, 0.f);
				if (!DressingSpotAllowed(Location, BambooAvoid, BambooCorridorPlaced,
					FVector2D::ZeroVector, FVector2D::ZeroVector, false))
				{
					continue;
				}
				FRandomStream Rand(7000 + Room * 13 + (Side > 0 ? 1 : 2));
				const TCHAR* MeshName = (Side > 0) ? TEXT("Rock1") : TEXT("Grass2");
				if (SpawnDecoMesh(Bamboo, NatureMesh(MeshName), MeshName, Location,
					Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.7f, 0.95f),
					FString::Printf(TEXT("Bamboo_Corr%d_Dressing%d"), Room, Side > 0 ? 1 : 0)))
				{
					BambooCorridorPlaced.Add(Location);
				}
			}
		}

		bOk &= SaveWorldMap(Bamboo);
		UE_LOG(LogTemp, Log, TEXT("L_BambooHollow created"));
	}

	// --- L_AshenVerge: ashen volcanic terrain, 7 rooms.
	if (UWorld* Ashen = CreateNewMapWorld(TEXT("L_AshenVerge")))
	{
		SetMapGameMode(Ashen, LoadClass<AGameModeBase>(nullptr, TEXT("/Script/QinglanAscension.QinglanRealmGameMode")));
		UMaterialInterface* Volcanic = Mat(TEXT("M_VolcanicRock"));
		UMaterialInterface* Dirt = Mat(TEXT("M_DirtFloor"));

		for (int32 i = 0; i < QinglanRoomLayout::RoomCount; ++i)
		{
			const float X = QinglanRoomLayout::RoomCenter(i).X;
			SpawnShape(Ashen, EngineCube(), FVector(X, 0, -50), FVector(4500, 4500, 100), Volcanic, *FString::Printf(TEXT("Ashen_Room%d_Floor"), i));
			if (i + 1 < QinglanRoomLayout::RoomCount)
			{
				SpawnShape(Ashen, EngineCube(), FVector(X + 2750.f, 0, -50), FVector(1000, 800, 100), Volcanic, *FString::Printf(TEXT("Ashen_Corr%d"), i));
			}
		}
		StandardLighting(Ashen, FLinearColor(1.0f, 0.75f, 0.6f), 5.0f, -40.f, -60.f);
		ConfigureKillZ(Ashen, -3000.f);

		const TArray<TPair<FName, FVector>> AshenAnchors = {
			{TEXT("ashen.room_0"), FVector(0, 0, 100)},
			{TEXT("ashen.approach_1_2"), FVector(8250, 0, 100)},
			{TEXT("ashen.archive_3"), FVector(16500, 0, 100)},
			{TEXT("ashen.vent_4"), FVector(22000, 0, 100)},
			// AUD-22: ashen's exit station shares the final-room placement.
			{TEXT("ashen.boss_5"), QinglanRoomLayout::RoomCenter(6) + FVector(0.f, 0.f, 100.f)},
			{TEXT("ashen.release_antechamber"), FVector(27500, 2000, 100)},
			{TEXT("sq.ashen.archive_safe"), FVector(16500, 1600, 100)},
			{TEXT("sq.ashen.orchard_safe"), FVector(21500, 1800, 100)},
			{TEXT("sq.ashen.vent_room_safe"), FVector(22000, -1600, 100)},
			{TEXT("record.ashen.archive_drawer"), FVector(16200, 800, 100)},
			{TEXT("record.ashen.archive_table"), FVector(16800, 800, 100)},
			{TEXT("record.ashen.cooled_forge_station"), FVector(13000, -1500, 100)},
			{TEXT("record.ashen.governor_reader"), FVector(16600, -800, 100)},
			{TEXT("record.ashen.outer_archive"), FVector(16000, -1600, 100)},
			// On room 3's floor near its north edge; the old (19500,1700) sat
			// in the void between rooms, outside the corridor's 400 cm half-width.
			{TEXT("record.ashen.shelter_wall"), FVector(19000, 1800, 100)},
		};
		for (const auto& A : AshenAnchors)
		{
			SpawnAnchor(Ashen, A.Value, A.Key);
		}
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			APlayerStart* PS = Ashen->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), QinglanRoomLayout::RoomEntrance(Room), FRotator::ZeroRotator, FActorSpawnParameters());
			if (PS)
			{
				PS->SetActorLabel(FString::Printf(TEXT("PlayerStart_Room%d"), Room));
				PS->PlayerStartTag = QinglanRoomLayout::EntranceTag(Room);
				PS->Tags.AddUnique(PS->PlayerStartTag);
			}
		}

		// AUD-55 dressing: volcanic rocks dominate, with sparse grass/bushes and
		// an occasional dead tree. Same deterministic ring + anchor distance
		// policy as the bamboo hollow.
		TArray<FVector> AshenAvoid;
		for (const auto& A : AshenAnchors)
		{
			AshenAvoid.Add(A.Value);
		}
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			AshenAvoid.Add(QinglanRoomLayout::RoomEntrance(Room));
		}
		static const TCHAR* AshenRock[] = { TEXT("Rock1"), TEXT("Rock2"), TEXT("Rock3") };
		static const TCHAR* AshenGrass[] = { TEXT("Grass1"), TEXT("Grass3") };
		static const TCHAR* AshenBush[] = { TEXT("Bush1"), TEXT("Bush3") };
		static const TCHAR* AshenTree[] = { TEXT("Tree4") };
		for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
		{
			const FVector Center = QinglanRoomLayout::RoomCenter(Room);
			TArray<FVector> RoomPlaced;
			ScatterNature(Ashen, FString::Printf(TEXT("Ashen_Room%d_Rock"), Room), 3000 + Room * 11,
				Center, 0.f, 1450.f, 2000.f, 3, MakeArrayView(AshenRock), AshenAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Ashen, FString::Printf(TEXT("Ashen_Room%d_Grass"), Room), 3100 + Room * 11,
				Center, 0.f, 1550.f, 1950.f, 2, MakeArrayView(AshenGrass), AshenAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Ashen, FString::Printf(TEXT("Ashen_Room%d_Bush"), Room), 3200 + Room * 11,
				Center, 0.f, 1600.f, 1950.f, 1, MakeArrayView(AshenBush), AshenAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
			ScatterNature(Ashen, FString::Printf(TEXT("Ashen_Room%d_Tree"), Room), 3300 + Room * 11,
				Center, 0.f, 1700.f, 1950.f, 1, MakeArrayView(AshenTree), AshenAvoid, RoomPlaced,
				FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		}
		TArray<FVector> AshenCorridorPlaced;
		for (int32 Room = 0; Room + 1 < QinglanRoomLayout::RoomCount; ++Room)
		{
			const FVector CorridorCenter = QinglanRoomLayout::RoomCenter(Room) + FVector(2750.f, 0.f, 0.f);
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				if (GUseLowQualitySurfaceMaterials && Side < 0)
				{
					continue; // low-quality variant keeps one prop per corridor
				}
				// Corridor floor is 800 cm wide (Y +/-400); keep the centre clear.
				const FVector Location(CorridorCenter.X, Side * 320.f, 0.f);
				if (!DressingSpotAllowed(Location, AshenAvoid, AshenCorridorPlaced,
					FVector2D::ZeroVector, FVector2D::ZeroVector, false))
				{
					continue;
				}
				FRandomStream Rand(8000 + Room * 13 + (Side > 0 ? 1 : 2));
				const TCHAR* MeshName = (Side > 0) ? TEXT("Rock2") : TEXT("Rock1");
				if (SpawnDecoMesh(Ashen, NatureMesh(MeshName), MeshName, Location,
					Rand.FRandRange(0.f, 360.f), Rand.FRandRange(0.7f, 1.0f),
					FString::Printf(TEXT("Ashen_Corr%d_Dressing%d"), Room, Side > 0 ? 1 : 0)))
				{
					AshenCorridorPlaced.Add(Location);
				}
			}
		}

		bOk &= SaveWorldMap(Ashen);
		UE_LOG(LogTemp, Log, TEXT("L_AshenVerge created"));
	}

	// --- L_TribulationArena: storm platform 60 m across + decision platform.
	if (UWorld* Arena = CreateNewMapWorld(TEXT("L_TribulationArena")))
	{
		SetMapGameMode(Arena, LoadClass<AGameModeBase>(nullptr, TEXT("/Script/QinglanAscension.QinglanRealmGameMode")));
		UMaterialInterface* Stone = Mat(TEXT("M_StonePath"));
		SpawnShape(Arena, EngineCube(), FVector(0, 0, -50), FVector(6000, 6000, 100), Stone, TEXT("Arena_Platform"));
		SpawnShape(Arena, EngineCube(), FVector(0, -4500, -50), FVector(1400, 1400, 100), Stone, TEXT("Decision_Platform"));
		// Walkable bridge across the 800 cm gap between the arena edge (Y=-3000)
		// and the decision platform edge (Y=-3800); there is no jump input.
		SpawnShape(Arena, EngineCube(), FVector(0, -3400, -50), FVector(800, 800, 100), Stone, TEXT("Arena_Decision_Bridge"));
		SpawnShape(Arena, EngineCube(), FVector(2600, 2600, 0), FVector(400, 400, 200), Stone, TEXT("Governor_Platform"));
		StandardLighting(Arena, FLinearColor(0.7f, 0.75f, 1.0f), 3.0f, -70.f, 90.f);
		ConfigureKillZ(Arena, -3000.f);

		const TArray<TPair<FName, FVector>> ArenaAnchors = {
			{TEXT("trial.foundation_arena"), FVector(0, 0, 100)},
			// Distinct from the foundation arena: aim selection keeps only the
			// strictly-better dot score, so equal positions never disambiguate.
			{TEXT("trial.golden_core_arena"), FVector(0, -1500, 100)},
			{TEXT("trial.decision_platform"), FVector(0, -4500, 100)},
			{TEXT("trial.quiet_governor_platform"), FVector(2600, 2600, 200)},
			{TEXT("trial.preparation_shrine"), FVector(-2600, 2600, 100)},
		};
		for (const auto& A : ArenaAnchors)
		{
			SpawnAnchor(Arena, A.Value, A.Key);
		}
		APlayerStart* PS3 = Arena->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FVector(0, 2000, 100), FRotator(0, -90, 0), FActorSpawnParameters());
		if (PS3) { PS3->SetActorLabel(TEXT("PlayerStart")); }

		// AUD-55 dressing: keep the 60 m arena's centre fight space empty and
		// ring rocks/grass/bushes just inside the platform edge. The bridge and
		// decision platform (Y <= -3000) are outside the scatter ring.
		TArray<FVector> ArenaAvoid;
		for (const auto& A : ArenaAnchors)
		{
			ArenaAvoid.Add(A.Value);
		}
		ArenaAvoid.Add(FVector(0, 2000, 100));
		TArray<FVector> ArenaPlaced;
		static const TCHAR* ArenaRock[] = { TEXT("Rock1"), TEXT("Rock2"), TEXT("Rock3") };
		static const TCHAR* ArenaGrass[] = { TEXT("Grass1"), TEXT("Grass2") };
		static const TCHAR* ArenaBush[] = { TEXT("Bush1"), TEXT("Bush2") };
		ScatterNature(Arena, TEXT("Arena_Rock"), 4100, FVector::ZeroVector, 0.f, 2250.f, 2800.f, 5,
			MakeArrayView(ArenaRock), ArenaAvoid, ArenaPlaced, FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		ScatterNature(Arena, TEXT("Arena_Grass"), 4101, FVector::ZeroVector, 0.f, 2250.f, 2800.f, 4,
			MakeArrayView(ArenaGrass), ArenaAvoid, ArenaPlaced, FVector2D::ZeroVector, FVector2D::ZeroVector, false);
		ScatterNature(Arena, TEXT("Arena_Bush"), 4102, FVector::ZeroVector, 0.f, 2300.f, 2800.f, 2,
			MakeArrayView(ArenaBush), ArenaAvoid, ArenaPlaced, FVector2D::ZeroVector, FVector2D::ZeroVector, false);

		bOk &= SaveWorldMap(Arena);
		UE_LOG(LogTemp, Log, TEXT("L_TribulationArena created"));
	}

	return bOk;
}

#undef LOCTEXT_NAMESPACE
