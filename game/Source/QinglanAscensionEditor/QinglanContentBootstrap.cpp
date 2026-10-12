// Content bootstrap implementation. See header for intent.
#include "QinglanContentBootstrap.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/TargetPoint.h"
#include "EnhancedInput/Subsystem/IEnhancedInputModule.h"
#include "FileHelper.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Package.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "QinglanContentBootstrap"

namespace
{
	const FString ContentRoot = TEXT("/Game/Qinglan");
	const FString RawAssetsRoot = TEXT("/inspire/hdd/project/qianghuaxuexi/s26044/assets-raw");

	UPackage* CreateOrFetchPackage(const FString& ObjectPath)
	{
		const FString PackageName = FPackageName::GetObjectPathPackage(ObjectPath);
		UPackage* Package = CreatePackage(*PackageName);
		Package->SetPackageFlags(PKG_NewlyCreated);
		Package->FullyLoad();
		return Package;
	}
}

UQinglanContentBootstrap::UQinglanContentBootstrap()
{
	LogToClipsAndLog = true;
}

int32 UQinglanContentBootstrap::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("QinglanContentBootstrap starting"));
	bool bOk = true;
	bOk &= BootstrapInput();
	bOk &= BootstrapMaterials();
	bOk &= BootstrapMaps();
	UE_LOG(LogTemp, Log, TEXT("QinglanContentBootstrap finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}

bool UQinglanContentBootstrap::SavePackage(UPackage* Package, UObject* Asset)
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

bool UQinglanContentBootstrap::BootstrapInput()
{
	UPackage* InputPkg = CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Move"));
	UInputAction* IA_Move = MakeAction(InputPkg, TEXT("IA_Move"), EInputActionValueType::Axis3D);
	UInputAction* IA_Look = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Look")), TEXT("IA_Look"), EInputActionValueType::Axis2D);
	UInputAction* IA_Attack = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Attack")), TEXT("IA_Attack"), EInputActionValueType::Boolean);
	UInputAction* IA_Dodge = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Dodge")), TEXT("IA_Dodge"), EInputActionValueType::Boolean);
	UInputAction* IA_Lock = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Lock")), TEXT("IA_Lock"), EInputActionValueType::Boolean);
	UInputAction* IA_Cycle = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_CycleTarget")), TEXT("IA_CycleTarget"), EInputActionValueType::Boolean);
	UInputAction* IA_Interact = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Interact")), TEXT("IA_Interact"), EInputActionValueType::Boolean);
	UInputAction* IA_Pause = MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/IA_Pause")), TEXT("IA_Pause"), EInputActionValueType::Boolean);

	TArray<UInputAction*> TechniqueActions;
	for (int32 i = 0; i < 4; ++i)
	{
		const FName Name(*FString::Printf(TEXT("IA_Technique%d"), i + 1));
		TechniqueActions.Add(MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/") + Name.ToString()), Name, EInputActionValueType::Boolean));
	}
	TArray<UInputAction*> TalismanActions;
	for (int32 i = 0; i < 2; ++i)
	{
		const FName Name(*FString::Printf(TEXT("IA_Talisman%d"), i + 1));
		TalismanActions.Add(MakeAction(CreateOrFetchPackage(ContentRoot + TEXT("/Input/") + Name.ToString()), Name, EInputActionValueType::Boolean));
	}

	UPackage* ImcPkg = CreateOrFetchPackage(ContentRoot + TEXT("/Input/IMC_Default"));
	UInputMappingContext* IMC = NewObject<UInputMappingContext>(ImcPkg, TEXT("IMC_Default"), RF_Public | RF_Standalone | RF_Transactional);

	// Movement: WASD on a 2-plane (docs/02 section 1).
	Map2DAxis(IMC, IA_Move, EKeys::W, false, false, false);
	Map2DAxis(IMC, IA_Move, EKeys::S, true, false, false);
	Map2DAxis(IMC, IA_Move, EKeys::D, false, false, true);
	Map2DAxis(IMC, IA_Move, EKeys::A, false, true, true);

	// Look: mouse deltas (X yaw, Y pitch come through as Axis2D).
	{
		FEnhancedActionKeyMapping& M = IMC->MapKey(IA_Look, EKeys::Mouse2D);
		UInputModifierNegate* NegateY = NewObject<UInputModifierNegate>(IMC);
		NegateY->bY = true;
		M.Modifiers.Add(NegateY);
	}
	IMC->MapKey(IA_Attack, EKeys::LeftMouseButton).Triggers.Add(PressedTrigger(IMC));
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

UMaterial* UQinglanContentBootstrap::CreateOrLoadMasterMaterial()
{
	const FString Path = ContentRoot + TEXT("/Materials/M_QinglanSurface");
	if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *Path))
	{
		return Existing;
	}
	UPackage* Pkg = CreateOrFetchPackage(Path);
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_QinglanSurface"), RF_Public | RF_Standalone);
	Mat->BlendMode = BLEND_Opaque;
	Mat->ShadingModel = MSM_DefaultLit;

	auto AddTextureParam = [Mat](const FName& Name, int32& OutY) -> UMaterialExpressionTextureSampleParameter2D*
	{
		UMaterialExpressionTextureSampleParameter2D* Param =
			NewObject<UMaterialExpressionTextureSampleParameter2D>(Mat);
		Param->ParameterName = Name;
		Param->MaterialExpressionEditorX = -600;
		Param->MaterialExpressionEditorY = OutY;
		OutY += 220;
		Mat->GetExpressionCollection().AddExpression(Param);
		return Param;
	};

	int32 Y = -300;
	UMaterialExpressionTextureSampleParameter2D* BaseColorParam = AddTextureParam(TEXT("BaseColor"), Y);
	UMaterialExpressionTextureSampleParameter2D* NormalParam = AddTextureParam(TEXT("Normal"), Y);
	UMaterialExpressionTextureSampleParameter2D* RoughParam = AddTextureParam(TEXT("Roughness"), Y);
	UMaterialExpressionTextureSampleParameter2D* AOParam = AddTextureParam(TEXT("AmbientOcclusion"), Y);

	Mat->BaseColor.Connect(0, BaseColorParam);
	Mat->Normal.Connect(0, NormalParam);
	Mat->Roughness.Connect(1, RoughParam); // green channel of packed map when packed
	Mat->AmbientOcclusion.Connect(1, AOParam);
	Mat->UpdateMaterialExpressionCommentTexts(false);
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	SavePackage(Pkg, Mat);
	return Mat;
}

namespace
{
	UTexture2D* ImportTextureFromDisk(const FString& JpgPath, const FString& DestObjectPath, TextureGroup LODGroup)
	{
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *DestObjectPath))
		{
			return Existing;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *JpgPath) || Bytes.Num() == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("texture missing on disk: %s"), *JpgPath);
			return nullptr;
		}
		UTexture2D* Tex = FImageUtils::ImportBufferAsTexture2D(Bytes);
		if (!Tex)
		{
			return nullptr;
		}
		UPackage* Pkg = CreateOrFetchPackage(DestObjectPath);
		const FName AssetName(*FPackageName::ObjectPathToObjectName(DestObjectPath));
		Tex->Rename(*AssetName.ToString(), Pkg, REN_DontCreateRedirectors | REN_DoNotDirty);
		Tex->SetFlags(RF_Public | RF_Standalone);
		Tex->LODGroup = LODGroup;
		Tex->NeverStream = false;
		Tex->SRGB = LODGroup != TEXTUREGROUP_WorldNormalMap;
		Tex->PostEditChange();
		UQinglanContentBootstrap* NullOuter = nullptr; (void)NullOuter;
		return Tex;
	}

	bool SaveTexture(UTexture2D* Tex)
	{
		UPackage* Pkg = Cast<UPackage>(Tex->GetOutermost());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		FString FileName = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Pkg, Tex, *FileName, Args);
	}

	/** Material instance set for one Poly Haven texture asset directory. */
	struct FSurfaceSet
	{
		FString AssetId;
		FString BaseColorFile;
		FString NormalFile;
		FString RoughFile;   // may be _rough or packed _arm (G channel used)
		FString AOFile;      // optional
		FString MaterialName; // e.g. M_BambooWall
	};
}

bool UQinglanContentBootstrap::BootstrapMaterials()
{
	UMaterial* Master = CreateOrLoadMasterMaterial();
	if (!Master)
	{
		return false;
	}

	const FString TexRoot = RawAssetsRoot + TEXT("/polyhaven/textures");
	TArray<FSurfaceSet> Sets;
	auto AddSet = [&Sets](const FString& Id, const FString& Name)
	{
		FSurfaceSet S;
		S.AssetId = Id;
		S.MaterialName = Name;
		Sets.Add(S);
	};
	AddSet(TEXT("bamboo_wall"), TEXT("M_BambooWall"));
	AddSet(TEXT("cobblestone_01"), TEXT("M_Cobblestone"));
	AddSet(TEXT("grey_stone_path"), TEXT("M_StonePath"));
	AddSet(TEXT("rock_path"), TEXT("M_RockPath"));
	AddSet(TEXT("dark_wooden_planks"), TEXT("M_DarkWoodPlanks"));
	AddSet(TEXT("fine_grained_wood"), TEXT("M_FineWood"));
	AddSet(TEXT("gravel_concrete"), TEXT("M_Gravel"));
	AddSet(TEXT("aerial_grass_rock"), TEXT("M_GrassRock"));
	AddSet(TEXT("brown_mud_rocks_01"), TEXT("M_MudRock"));
	AddSet(TEXT("volcanic_rock_tiles"), TEXT("M_VolcanicRock"));
	AddSet(TEXT("mossy_brick"), TEXT("M_MossyBrick"));
	AddSet(TEXT("moss_wood"), TEXT("M_MossWood"));
	AddSet(TEXT("cliff_side"), TEXT("M_Cliff"));
	AddSet(TEXT("dirt_floor"), TEXT("M_DirtFloor"));
	AddSet(TEXT("grey_roof_tiles"), TEXT("M_RoofTiles"));
	AddSet(TEXT("clay_roof_tiles"), TEXT("M_ClayRoofTiles"));
	AddSet(TEXT("thatch_roof_angled"), TEXT("M_Thatch"));
	AddSet(TEXT("concrete_rock_path"), TEXT("M_ConcretePath"));
	AddSet(TEXT("bamboo_wall_02"), TEXT("M_BambooWallAlt"));
	AddSet(TEXT("bamboo_veneer"), TEXT("M_BambooVeneer"));

	bool bAnyOk = false;
	for (const FSurfaceSet& Set : Sets)
	{
		const FString Dir = TexRoot + TEXT("/") + Set.AssetId;
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.jpg"), true, false, false);
		if (Files.Num() == 0)
		{
			continue; // textures not fetched yet; skip quietly
		}
		auto FindSuffix = [&Files](const TCHAR* Suffix) -> FString
		{
			for (const FString& F : Files)
			{
				if (F.Contains(Suffix))
				{
					return F;
				}
			}
			return FString();
		};
		FString Diff = FindSuffix(TEXT("diff"));
		FString Nor = FindSuffix(TEXT("nor"));
		FString Rough = FindSuffix(TEXT("rough"));
		FString AO = FindSuffix(TEXT("ao"));
		if (Diff.IsEmpty())
		{
			Diff = Files[0];
		}

		UPackage* Pkg = CreateOrFetchPackage(ContentRoot + TEXT("/Materials/") + Set.MaterialName);
		const FName AssetName(*Set.MaterialName);
		UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, AssetName, RF_Public | RF_Standalone);
		MIC->SetParentMasterMaterial(Master);

		auto ApplyParam = [&](const TCHAR* ParamName, const FString& File, TextureGroup Group)
		{
			if (File.IsEmpty())
			{
				return;
			}
			const FString TexPath = ContentRoot + TEXT("/Textures/") + Set.AssetId + TEXT("_") + FPaths::GetBaseFilename(File);
			UTexture2D* Tex = ImportTextureFromDisk(File, TexPath, Group);
			if (Tex)
			{
				SaveTexture(Tex);
				MIC->SetTextureParameterValue(FMaterialParameterInfo(ParamName), Tex);
			}
		};
		ApplyParam(TEXT("BaseColor"), Diff, TEXTUREGROUP_World);
		ApplyParam(TEXT("Normal"), Nor, TEXTUREGROUP_WorldNormalMap);
		ApplyParam(TEXT("Roughness"), Rough.IsEmpty() ? FindSuffix(TEXT("arm")) : Rough, TEXTUREGROUP_World);
		ApplyParam(TEXT("AmbientOcclusion"), AO.IsEmpty() ? FindSuffix(TEXT("arm")) : AO, TEXTUREGROUP_World);

		MIC->PostEditChange();
		bAnyOk |= SavePackage(Pkg, MIC);
		UE_LOG(LogTemp, Log, TEXT("material %s from %s"), *Set.MaterialName, *Set.AssetId);
	}
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
		ATargetPoint* Point = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Point)
		{
			Point->SetActorLabel(Name.ToString());
		}
		return Point;
	}

	UMaterialInterface* Mat(const TCHAR* Name)
	{
		return LoadObject<UMaterialInterface>(nullptr, *(ContentRoot + TEXT("/Materials/") + Name + TEXT(".") + Name));
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
		const FString PackagePath = ContentRoot + TEXT("/Maps/") + MapName + TEXT(".") + MapName);
		UPackage* Package = CreateOrFetchPackage(PackagePath);
		UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, *MapName, Package);
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
}

bool UQinglanContentBootstrap::BootstrapMaps()
{
	bool bOk = true;

	// --- L_Hub: mountain sanctuary courtyard on terraces.
	if (UWorld* Hub = CreateNewMapWorld(TEXT("L_Hub")))
	{
		UMaterialInterface* Cliff = Mat(TEXT("M_Cliff"));
		UMaterialInterface* Stone = Mat(TEXT("M_StonePath"));
		UMaterialInterface* Wood = Mat(TEXT("M_FineWood"));
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

		// Interaction anchors (docs/dev/level-layout.md).
		const TArray<TPair<FName, FVector>> HubAnchors = {
			{TEXT("hub.courtyard"), FVector(0, 0, 500)},
			{TEXT("hub.training_yard"), FVector(3000, 2500, 500)},
			{TEXT("hub.cultivation_shrine"), FVector(-3200, 2500, 500)},
			{TEXT("hub.shrine"), FVector(0, 3200, 500)},
			{TEXT("hub.workbench"), FVector(2500, -2000, 500)},
			{TEXT("hub.lamp_workbench"), FVector(2500, -2600, 500)},
			{TEXT("hub.bamboo_gate"), FVector(0, -4800, 500)},
			{TEXT("hub.handover_table"), FVector(-1200, -3400, 500)},
			{TEXT("hub.archive_shelf"), FVector(-2400, -3000, 500)},
			{TEXT("hub.archive_table"), FVector(-2600, -2200, 500)},
			{TEXT("hub.courier_board"), FVector(1600, 3000, 500)},
			{TEXT("hub.inspection_board"), FVector(-1600, 3000, 500)},
			{TEXT("hub.water_table"), FVector(3200, -400, 500)},
			{TEXT("hub.stonewright_crate"), FVector(3600, 800, 500)},
			{TEXT("hub.old_table"), FVector(-3800, 800, 500)},
			{TEXT("hub.training_mat"), FVector(3000, 3300, 500)},
			{TEXT("record.hub.apprentice_quarters"), FVector(2200, 1600, 500)},
			{TEXT("record.hub.courier_shelf"), FVector(1400, 3400, 500)},
			{TEXT("record.hub.dry_bell"), FVector(0, 600, 500)},
			{TEXT("record.hub.governor_reader"), FVector(-3000, 1400, 500)},
			{TEXT("record.hub.post_ending_noticeboard"), FVector(600, 3600, 500)},
			{TEXT("record.hub.post_ending_workshop"), FVector(4200, 200, 500)},
			{TEXT("record.hub.practice_rack"), FVector(3800, 3000, 500)},
			{TEXT("record.hub.sealed_dispatch_packet"), FVector(800, -3600, 500)},
			{TEXT("record.hub.workshop"), FVector(2800, -1200, 500)},
		};
		for (const auto& A : HubAnchors)
		{
			SpawnAnchor(Hub, A.Value, A.Key);
		}
		APlayerStart* Start = Hub->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FVector(0, 1000, 500), FRotator(0, 180, 0), FActorSpawnParameters());
		if (Start) { Start->SetActorLabel(TEXT("PlayerStart")); }

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
		for (int32 i = 0; i < 7; ++i)
		{
			const float X = i * 5500.f;
			SpawnShape(Bamboo, EngineCube(), FVector(X, 0, -50), FVector(4500, 4500, 100), i % 2 ? Grass : Dirt, *FString::Printf(TEXT("Bamboo_Room%d_Floor"), i));
			// Corridor to next room.
			if (i < 6)
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

		const TArray<TPair<FName, FVector>> BambooAnchors = {
			{TEXT("bamboo.room_0"), FVector(0, 0, 100)},
			{TEXT("bamboo.rooms_1_to_5"), FVector(11000, 0, 100)},
			{TEXT("bamboo.room_1_2_guided"), FVector(5500, 0, 100)},
			{TEXT("bamboo.room_5_safe"), FVector(27500, 0, 100)},
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
		APlayerStart* PS = Bamboo->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, FActorSpawnParameters());
		if (PS) { PS->SetActorLabel(TEXT("PlayerStart")); }
		bOk &= SaveWorldMap(Bamboo);
		UE_LOG(LogTemp, Log, TEXT("L_BambooHollow created"));
	}

	// --- L_AshenVerge: ashen volcanic terrain, 7 rooms.
	if (UWorld* Ashen = CreateNewMapWorld(TEXT("L_AshenVerge")))
	{
		SetMapGameMode(Ashen, LoadClass<AGameModeBase>(nullptr, TEXT("/Script/QinglanAscension.QinglanRealmGameMode")));
		UMaterialInterface* Volcanic = Mat(TEXT("M_VolcanicRock"));
		UMaterialInterface* Dirt = Mat(TEXT("M_DirtFloor"));

		for (int32 i = 0; i < 7; ++i)
		{
			const float X = i * 5500.f;
			SpawnShape(Ashen, EngineCube(), FVector(X, 0, -50), FVector(4500, 4500, 100), Volcanic, *FString::Printf(TEXT("Ashen_Room%d_Floor"), i));
			if (i < 6)
			{
				SpawnShape(Ashen, EngineCube(), FVector(X + 2750.f, 0, -50), FVector(1000, 800, 100), Volcanic, *FString::Printf(TEXT("Ashen_Corr%d"), i));
			}
		}
		StandardLighting(Ashen, FLinearColor(1.0f, 0.75f, 0.6f), 5.0f, -40.f, -60.f);

		const TArray<TPair<FName, FVector>> AshenAnchors = {
			{TEXT("ashen.room_0"), FVector(0, 0, 100)},
			{TEXT("ashen.approach_1_2"), FVector(8250, 0, 100)},
			{TEXT("ashen.archive_3"), FVector(16500, 0, 100)},
			{TEXT("ashen.vent_4"), FVector(22000, 0, 100)},
			{TEXT("ashen.boss_5"), FVector(27500, 0, 100)},
			{TEXT("ashen.release_antechamber"), FVector(27500, 2000, 100)},
			{TEXT("sq.ashen.archive_safe"), FVector(16500, 1600, 100)},
			{TEXT("sq.ashen.orchard_safe"), FVector(21500, 1800, 100)},
			{TEXT("sq.ashen.vent_room_safe"), FVector(22000, -1600, 100)},
			{TEXT("record.ashen.archive_drawer"), FVector(16200, 800, 100)},
			{TEXT("record.ashen.archive_table"), FVector(16800, 800, 100)},
			{TEXT("record.ashen.cooled_forge_station"), FVector(13000, -1500, 100)},
			{TEXT("record.ashen.governor_reader"), FVector(16600, -800, 100)},
			{TEXT("record.ashen.outer_archive"), FVector(16000, -1600, 100)},
			{TEXT("record.ashen.shelter_wall"), FVector(19500, 1700, 100)},
		};
		for (const auto& A : AshenAnchors)
		{
			SpawnAnchor(Ashen, A.Value, A.Key);
		}
		APlayerStart* PS2 = Ashen->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FVector(0, 0, 100), FRotator::ZeroRotator, FActorSpawnParameters());
		if (PS2) { PS2->SetActorLabel(TEXT("PlayerStart")); }
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
		SpawnShape(Arena, EngineCube(), FVector(2600, 2600, 0), FVector(400, 400, 200), Stone, TEXT("Governor_Platform"));
		StandardLighting(Arena, FLinearColor(0.7f, 0.75f, 1.0f), 3.0f, -70.f, 90.f);

		const TArray<TPair<FName, FVector>> ArenaAnchors = {
			{TEXT("trial.foundation_arena"), FVector(0, 0, 100)},
			{TEXT("trial.golden_core_arena"), FVector(0, 0, 100)},
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
		bOk &= SaveWorldMap(Arena);
		UE_LOG(LogTemp, Log, TEXT("L_TribulationArena created"));
	}

	return bOk;
}

#undef LOCTEXT_NAMESPACE
