#include "QinglanCultivatorAppearance.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const FString MaterialsRoot = TEXT("/Game/Qinglan/Materials/");
	const FString HumanRoot = TEXT("/Game/Qinglan/Characters/Human/");

	bool SaveAsset(UObject* Asset)
	{
		if (!Asset) { return false; }
		UPackage* Package = Asset->GetOutermost();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Asset,
			*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	UTexture2D* ImportWeaveNormal()
	{
		UTextureFactory* Factory = NewObject<UTextureFactory>();
		Factory->bCreateMaterial = false;
		Factory->CompressionSettings = TC_Normalmap;
		Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
		Factory->LODGroup = TEXTUREGROUP_CharacterNormalMap;
		Factory->MipGenSettings = TMGS_FromTextureGroup;
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
			/ TEXT("../assets/art-source/Textures/CultivatorWeaveNormal.png"));
		Task->DestinationPath = HumanRoot.LeftChop(1);
		Task->DestinationName = TEXT("T_CultivatorWeaveNormal");
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bReplaceExistingSettings = true;
		Task->bSave = true;
		Task->Factory = Factory;
		FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr,
			*(HumanRoot + TEXT("T_CultivatorWeaveNormal.T_CultivatorWeaveNormal")));
		if (!Texture || Task->GetObjects().IsEmpty()) { return nullptr; }
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Normalmap;
		Texture->LODGroup = TEXTUREGROUP_CharacterNormalMap;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		Texture->PostEditChange();
		return SaveAsset(Texture) ? Texture : nullptr;
	}

	UMaterial* MakeParent(const FString& Name, UTexture2D* Skin, UTexture2D* Weave)
	{
		const FString Path = MaterialsRoot + Name;
		// These graphs are code-owned and rebuilt in a fresh bootstrap process.
		IFileManager::Get().Delete(*FPackageName::LongPackageNameToFilename(Path,
			FPackageName::GetAssetPackageExtension()), false, true, true);
		UPackage* Package = CreatePackage(*Path);
		Package->FullyLoad();
		UMaterial* Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone);
		Material->SetShadingModel(MSM_DefaultLit);
		Material->BlendMode = BLEND_Opaque;
        Material->TwoSided = Weave != nullptr; // thin robe hems expose both sides
		Material->SetMaterialUsage(MATUSAGE_SkeletalMesh);
		auto Scalar = [Material](const TCHAR* Parameter, float Value)
		{
			UMaterialExpressionScalarParameter* Expr = NewObject<UMaterialExpressionScalarParameter>(Material);
			Expr->ParameterName = Parameter;
			Expr->DefaultValue = Value;
			Material->GetExpressionCollection().AddExpression(Expr);
			return Expr;
		};
		UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(Material);
		Color->ParameterName = TEXT("Color");
		Color->DefaultValue = FLinearColor::White;
		Material->GetExpressionCollection().AddExpression(Color);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Color);
		Material->GetEditorOnlyData()->Roughness.Connect(0, Scalar(TEXT("Roughness"), 0.72f));
		Material->GetEditorOnlyData()->Metallic.Connect(0, Scalar(TEXT("Metallic"), 0.f));
		if (Skin)
		{
			UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
			Sample->ParameterName = TEXT("SkinPalette");
			Sample->Texture = Skin;
			Sample->SamplerType = SAMPLERTYPE_Color;
			Material->GetExpressionCollection().AddExpression(Sample);
			// Palette UVs must never use world mapping or surface repetition.
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Sample);
		}
		if (Weave)
		{
			UMaterialExpressionTextureCoordinate* UV = NewObject<UMaterialExpressionTextureCoordinate>(Material);
			Material->GetExpressionCollection().AddExpression(UV);
			UMaterialExpressionMultiply* Tiled = NewObject<UMaterialExpressionMultiply>(Material);
			Tiled->A.Connect(0, UV);
			Tiled->B.Connect(0, Scalar(TEXT("WeaveTiling"), 16.f));
			Material->GetExpressionCollection().AddExpression(Tiled);
			UMaterialExpressionTextureSampleParameter2D* Normal = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
			Normal->ParameterName = TEXT("WeaveNormal");
			Normal->Texture = Weave;
			Normal->SamplerType = SAMPLERTYPE_Normal;
			Normal->Coordinates.Connect(0, Tiled);
			Material->GetExpressionCollection().AddExpression(Normal);
			Material->GetEditorOnlyData()->Normal.Connect(0, Normal);
		}
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		return SaveAsset(Material) ? Material : nullptr;
	}
}

bool QinglanCultivatorAppearance::Bootstrap()
{
	UTexture2D* Skin = LoadObject<UTexture2D>(nullptr,
		*(HumanRoot + TEXT("T_CultivatorSkin.T_CultivatorSkin")));
	UTexture2D* Weave = ImportWeaveNormal();
	if (!Skin || !Weave)
	{
		UE_LOG(LogTemp, Error, TEXT("Cultivator appearance requires the imported hero, skin palette and weave normal"));
		return false;
	}
	UMaterial* SolidParent = MakeParent(TEXT("M_CultivatorDetail"), nullptr, nullptr);
	UMaterial* SkinParent = MakeParent(TEXT("M_CultivatorSkin"), Skin, nullptr);
	UMaterial* ClothParent = MakeParent(TEXT("M_CultivatorCloth"), nullptr, Weave);
	if (!SolidParent || !SkinParent || !ClothParent) { return false; }
	// Rebuild parents before loading last run's mesh/MIC references. That keeps
	// regeneration from loading the previous code-owned parent graphs first.
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
		*(HumanRoot + TEXT("SK_Cultivator.SK_Cultivator")));
	if (!Mesh) { return false; }

	struct FSlot
	{
		const TCHAR* Name;
		FLinearColor Color;
		float Roughness;
		float Metallic;
	};
	const FSlot Slots[] = {
		{TEXT("Cultivator_Skin"), FLinearColor(0.52f, 0.31f, 0.20f), 0.48f, 0.f},
		{TEXT("Cultivator_InkCloth"), FLinearColor(0.009f, 0.022f, 0.027f), 0.77f, 0.f},
		{TEXT("Cultivator_IvoryCloth"), FLinearColor(0.58f, 0.57f, 0.46f), 0.83f, 0.f},
		{TEXT("Cultivator_JadeCloth"), FLinearColor(0.025f, 0.105f, 0.115f), 0.72f, 0.f},
		{TEXT("Cultivator_AntiqueGold"), FLinearColor(0.45f, 0.27f, 0.075f), 0.34f, 0.78f},
		{TEXT("Cultivator_Jade"), FLinearColor(0.03f, 0.32f, 0.25f), 0.25f, 0.12f},
		{TEXT("Cultivator_Leather"), FLinearColor(0.025f, 0.019f, 0.014f), 0.58f, 0.f},
        {TEXT("Cultivator_Hair"), FLinearColor(0.011f, 0.009f, 0.013f), 0.66f, 0.f},
	};
	bool bOk = true;
	for (const FSlot& Slot : Slots)
	{
		const FString Name = FString(TEXT("MIC_")) + Slot.Name;
		const FString Path = MaterialsRoot + Name;
		UPackage* Package = CreatePackage(*Path);
		Package->FullyLoad();
		UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Path + TEXT(".") + Name));
		if (!Instance)
		{
			Instance = NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Instance);
		}
		const FString SlotName(Slot.Name);
		Instance->Parent = SlotName == TEXT("Cultivator_Skin") ? SkinParent
			: (SlotName.EndsWith(TEXT("Cloth")) ? ClothParent : SolidParent);
		Instance->ClearParameterValuesEditorOnly();
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Color"))), Slot.Color);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Roughness"))), Slot.Roughness);
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("Metallic"))), Slot.Metallic);
		Instance->PostEditChange();
		bOk &= SaveAsset(Instance);
		bool bBound = false;
		for (FSkeletalMaterial& Material : Mesh->GetMaterials())
		{
			if (Material.ImportedMaterialSlotName.ToString() == SlotName || Material.MaterialSlotName.ToString() == SlotName)
			{
				Material.MaterialInterface = Instance;
				bBound = true;
			}
		}
		if (!bBound)
		{
			UE_LOG(LogTemp, Error, TEXT("Cultivator material slot not imported: %s"), Slot.Name);
			bOk = false;
		}
	}
	Mesh->PostEditChange();
	bOk &= SaveAsset(Mesh);
	return bOk;
}
