// Mesh import implementation: FBX via the legacy FbxFactory asset-import path
// (deterministic options, headless-safe). Characters import with all embedded
// animation takes; nature/weapon meshes import as static meshes.
#include "QinglanMeshImport.h"
#include "QinglanRawAssets.h"
#include "QinglanHumanAnimationContract.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "Engine/Texture2D.h"
#include "Engine/SkeletalMesh.h"
#include "SkeletalMeshAttributes.h"
#include "Engine/StaticMesh.h"
#include "Editor/EditorEngine.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	UAssetImportTask* MakeTask(const FString& Source, const FString& DestPath, const FString& Name, UObject* Options)
	{
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = Source;
		Task->DestinationPath = DestPath;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bReplaceExistingSettings = true;
		Task->bSave = true;
		Task->Options = Options;
		return Task;
	}

	bool ImportHumanSourceTakes(const FString& SourceFile)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
		USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
		if (!Skeleton)
		{
			UE_LOG(LogTemp, Error, TEXT("Human animation import requires the shipped mesh skeleton"));
			return false;
		}
		bool bOk = true;
		const FString SourceMD5 = LexToString(FMD5Hash::HashFile(*SourceFile));
		const bool bKnownSource = SourceMD5.Equals(TEXT("3369937780cd6d7dfe99a80179f3070f"), ESearchCase::IgnoreCase);
		for (const QinglanHumanAnimation::FSourceTake& Take : QinglanHumanAnimation::RequiredTakes)
		{
			const FString Name = QinglanHumanAnimation::AssetName(Take.Suffix);
			const FString Path = QinglanHumanAnimation::ObjectPath(Take.Suffix);
			UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Path);
			if (!Sequence)
			{
				UPackage* Package = CreatePackage(*(TEXT("/Game/Qinglan/Characters/Human/") + Name));
				Sequence = NewObject<UAnimSequence>(Package, *Name, RF_Public | RF_Standalone);
				Sequence->SetSkeleton(Skeleton);
				Sequence->GetController().SetModel(Sequence->GetDataModelInterface());
				Sequence->GetController().InitializeModel();
				FAssetRegistryModule::AssetCreated(Sequence);
			}
			Sequence->SetSkeleton(Skeleton);
			IAnimationDataController& Controller = Sequence->GetController();
			Controller.OpenBracket(FText::FromString(TEXT("Import original Human take")), false);
			// Do not accept an old procedural/previous-import pose if the FBX
			// importer reports success without populating the requested take.
			Controller.ResetModel(/*bShouldTransact=*/false);
			UFbxAnimSequenceImportData* Import = UFbxAnimSequenceImportData::GetImportDataForAnimSequence(Sequence, nullptr);
			Import->SourceAnimationName = FString(TEXT("Human Armature|")) + Take.Suffix;
			Import->AnimationLength = FBXALIT_ExportedTime;
			Import->bUseDefaultSampleRate = true;
			Import->bSnapToClosestFrameBoundary = true;
			Import->Update(SourceFile);
			UFbxImportUI* Options = NewObject<UFbxImportUI>();
			Options->Skeleton = Skeleton;
			Options->bImportMesh = false;
			Options->bImportAnimations = true;
			Options->bAutomatedImportShouldDetectType = false;
			Options->AnimSequenceImportData = Import;
			bool bImportAll = false;
			// ImportAssetTasks without Task->Factory selects Interchange. A mesh
			// reimport is not an inventory check for missing source takes. This
			// exported editor API uses the legacy FBX importer and the exact
			// SourceAnimationName, preserving the runtime paths and skeleton.
			const bool bImported = UEditorEngine::ReimportFbxAnimation(Skeleton, Sequence, Import,
				*SourceFile, bImportAll, /*bFactoryShowOptions=*/false, Options);
			// The exporter ends some takes a fraction below the last 30 Hz
			// boundary. UE's legacy importer floors the range but rounds the
			// sampled key count (ImportBoneTracks also includes an end threshold).
			// For this verified source only, restore its known final frame by
			// changing the playback range. SetNumberOfFrames does not edit bone
			// curves. GetBoneTrackTransforms cannot count stored source keys: the
			// Sequencer model evaluates only the current playback range.
			const IAnimationDataModel* Model = Sequence->GetDataModel();
			const int32 ExpectedFrames = FMath::RoundToInt(Take.Seconds * 30.f);
			if (bImported && bKnownSource && Model && Model->GetFrameRate() == FFrameRate(30, 1)
				&& Import->SourceAnimationName == FString(TEXT("Human Armature|")) + Take.Suffix
				&& ExpectedFrames - Model->GetNumberOfFrames() == 1)
			{
				UE_LOG(LogTemp, Display, TEXT("Human %s restoring verified source playback range %d -> %d frames (MD5=%s)"),
					Take.Suffix, Model->GetNumberOfFrames(), ExpectedFrames, *SourceMD5);
				Controller.SetNumberOfFrames(FFrameNumber(ExpectedFrames), false);
			}
			// Keep the reset/reimport inside a single bracket: intermediate
			// one-frame models must not be resampled at the previous asset rate.
			Controller.NotifyPopulated();
			Controller.CloseBracket(false);
			FString Reason;
			const bool bValid = QinglanHumanAnimation::Validate(Sequence, Skeleton, Take, Reason);
			if (!bImported || !bValid)
			{
				UE_LOG(LogTemp, Error, TEXT("Human source take %s failed (import=%d sourceMD5=%s): %s"),
					Take.Suffix, bImported, *SourceMD5, *Reason);
				bOk = false;
				continue;
			}
			UPackage* Package = Sequence->GetOutermost();
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			bOk &= UPackage::SavePackage(Package, Sequence,
				*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
			UE_LOG(LogTemp, Display, TEXT("Human source take %s imported at %s (%.3fs)"), Take.Suffix, *Path, Sequence->GetPlayLength());
		}
		return bOk;
	}
}

UQinglanMeshImportCommandlet::UQinglanMeshImportCommandlet()
{
}

int32 UQinglanMeshImportCommandlet::Main(const FString& Params)
{
	const FString AssetsRoot = QinglanRawAssets::ResolveRoot(Params);
	if (!QinglanRawAssets::RequireDirectory(AssetsRoot))
	{
		return 1;
	}
	const FString RawRoot = AssetsRoot / TEXT("quaternius/extracted");
	if (!QinglanRawAssets::RequireDirectory(RawRoot))
	{
		return 1;
	}
	UE_LOG(LogTemp, Log, TEXT("QinglanMeshImport starting; raw root=%s"), *RawRoot);
	bool bOk = true;

	// --- Skeletal: characters and monsters with embedded takes.
	struct FSkelEntry { const TCHAR* Src; const TCHAR* Dest; const TCHAR* Name; };
	const FSkelEntry Skeletal[] = {
		{ TEXT("Animated Human by @Quaternius/FBX/Animated Human.fbx"), TEXT("/Game/Qinglan/Characters/Human"), TEXT("SK_Human") },
		{ TEXT("Animated Monster Pack by @Quaternius/FBX/Slime.fbx"), TEXT("/Game/Qinglan/Characters/Monsters"), TEXT("SK_Slime") },
		{ TEXT("Animated Monster Pack by @Quaternius/FBX/Skeleton.fbx"), TEXT("/Game/Qinglan/Characters/Monsters"), TEXT("SK_Skeleton") },
		{ TEXT("Animated Monster Pack by @Quaternius/FBX/Dragon.fbx"), TEXT("/Game/Qinglan/Characters/Monsters"), TEXT("SK_Dragon") },
		{ TEXT("Animated Monster Pack by @Quaternius/FBX/Bat.fbx"), TEXT("/Game/Qinglan/Characters/Monsters"), TEXT("SK_Bat") },
		{ TEXT("Knight Character by @Quaternius/FBX/KnightCharacter.fbx"), TEXT("/Game/Qinglan/Characters/Knight"), TEXT("SK_Knight") },
	};
	for (const FSkelEntry& E : Skeletal)
	{
		const FString Src = RawRoot + TEXT("/") + E.Src;
		if (IFileManager::Get().FileExists(*Src))
		{
			bOk &= ImportSkeletalFBX(Src, E.Dest, E.Name);
			if (FString(E.Name) == TEXT("SK_Human"))
			{
				bOk &= ImportHumanSourceTakes(Src);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("missing skeletal source: %s"), *Src);
			bOk = false;
		}
	}

	// --- Hero skin texture (AUD-55): the Quaternius Human pack ships its
	// clothed skin as a PNG next to the blend file; import it so the character
	// material instance is not left with the engine grey default.
	{
		const FString SkinSource = RawRoot + TEXT("/Animated Human by @Quaternius/Blend/Textures/ClothedLightSkin.png");
		if (IFileManager::Get().FileExists(*SkinSource))
		{
			bOk &= ImportTexturePNG(SkinSource, TEXT("/Game/Qinglan/Characters/Human"), TEXT("T_Human_Clothed"));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("missing human skin texture: %s"), *SkinSource);
			bOk = false;
		}
	}

	// --- Static: nature and weapons.
	TArray<FString> StaticFiles;
	const TArray<TPair<FString, FString>> StaticDirs = {
		{ TEXT("Nature pack vol.3/FBX"), TEXT("/Game/Qinglan/Environments/Nature") },
		{ TEXT("Medieval Weapons Pack - Sept 2018/FBX"), TEXT("/Game/Qinglan/Weapons") },
	};
	for (const auto& Dir : StaticDirs)
	{
		if (!QinglanRawAssets::RequireDirectory(RawRoot / Dir.Key))
		{
			bOk = false;
			continue;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(RawRoot / Dir.Key / TEXT("*.fbx")), true, false);
		if (Files.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("no static FBX sources under %s"), *(RawRoot / Dir.Key));
			bOk = false;
		}
		for (const FString& F : Files)
		{
			const FString Base = FPaths::GetBaseFilename(F);
			// Tree3.fbx contains a mesh ('Icosphere_008') the FBX SDK cannot
			// triangulate, so the whole file yields zero importable objects
			// and would fail the strict run. Nothing references it (the nature
			// set is decorative; AUD-55); skip it rather than loosen the
			// mandatory-output contract for every other file.
			if (Base == TEXT("Tree3"))
			{
				UE_LOG(LogTemp, Warning, TEXT("skipping known-bad source (FBX SDK cannot triangulate Icosphere_008): %s"), *Base);
				continue;
			}
			bOk &= ImportStaticFBX(RawRoot / Dir.Key / F, Dir.Value, Base);
		}
	}

	// Project-original kit is tracked with its generator; it needs no external acquisition.
	const FString OriginalRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/original/environment"));
	for (const TCHAR* Name : {TEXT("SM_SanctuaryRoof"), TEXT("SM_SanctuaryFrame"),
		TEXT("SM_ScholarRock"), TEXT("SM_CliffRock"), TEXT("SM_BambooCluster"), TEXT("SM_StoneLantern")})
	{
		bOk &= ImportStaticFBX(OriginalRoot / (FString(Name) + TEXT(".fbx")),
			TEXT("/Game/Qinglan/Environments/Sanctuary"), Name);
	}

	// Costume geometry is rebound to the native Human pose and original takes.
	// This import must never create another animation/skeleton family.
	const FString HeroRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/art-source"));
	USkeletalMesh* Human = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
	if (!Human || !Human->GetSkeleton() || !IFileManager::Get().FileExists(*(HeroRoot / TEXT("SK_Cultivator.fbx"))))
	{
		UE_LOG(LogTemp, Error, TEXT("Cultivator import requires its tracked FBX and the original Human skeleton"));
		bOk = false;
	}
	else
	{
		UFbxImportUI* Options = NewObject<UFbxImportUI>();
		Options->bAutomatedImportShouldDetectType = false;
		Options->MeshTypeToImport = FBXIT_SkeletalMesh;
		Options->bImportAsSkeletal = true;
		Options->bImportMesh = true;
		Options->bImportAnimations = false;
		Options->bImportTextures = false;
		Options->bImportMaterials = false;
		Options->bCreatePhysicsAsset = false;
		Options->Skeleton = Human->GetSkeleton();
		Options->SkeletalMeshImportData->bUpdateSkeletonReferencePose = false;
        // The FBX preserves the evaluated Idle77 defaults and rest bind matrices;
        // Unreal skins geometry to those T0 defaults, matching native Human.
        Options->SkeletalMeshImportData->bUseT0AsRefPose = true;
		// Blender sources carry smooth normals; Unreal builds MikkTSpace tangents.
		Options->SkeletalMeshImportData->NormalImportMethod = FBXNIM_ImportNormals;
		UAssetImportTask* Task = MakeTask(HeroRoot / TEXT("SK_Cultivator.fbx"),
			TEXT("/Game/Qinglan/Characters/Human"), TEXT("SK_Cultivator"), Options);
		// Explicit legacy factory honors the supplied skeleton on unattended imports.
		Task->Factory = NewObject<UFbxFactory>();
		FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
		USkeletalMesh* Cultivator = LoadObject<USkeletalMesh>(nullptr,
			TEXT("/Game/Qinglan/Characters/Human/SK_Cultivator.SK_Cultivator"));
		bool bCompatible = Cultivator && !Task->GetObjects().IsEmpty()
			&& Cultivator->GetSkeleton() == Human->GetSkeleton();
        UE_LOG(LogTemp, Display, TEXT("Cultivator import objects=%d skeleton=%s source=%s"),
            Task->GetObjects().Num(), Cultivator && Cultivator->GetSkeleton() ? *Cultivator->GetSkeleton()->GetPathName() : TEXT("none"),
            *Human->GetSkeleton()->GetPathName());
		if (bCompatible)
		{
            // Legacy T0 import updates the mesh reference skeleton and skins
            // vertices, but its MeshDescription can retain rest-pose bone
            // attributes. Persist the evaluated pose there too, so subsequent
            // LOD builds use the same binding as the saved rendering mesh.
            for (int32 LOD = 0; LOD < Cultivator->GetLODNum(); ++LOD)
            {
                FMeshDescription* Description = Cultivator->GetMeshDescription(LOD);
                if (!Description) { bCompatible = false; break; }
                FSkeletalMeshAttributes Attributes(*Description);
                const FReferenceSkeleton& Rig = Cultivator->GetRefSkeleton();
                if (Attributes.GetNumBones() != Rig.GetRawBoneNum()) { bCompatible = false; break; }
                for (int32 Bone = 0; Bone < Rig.GetRawBoneNum(); ++Bone)
                {
                    Attributes.GetBoneNames().Set(FBoneID(Bone), Rig.GetBoneName(Bone));
                    Attributes.GetBoneParentIndices().Set(FBoneID(Bone), Rig.GetParentIndex(Bone));
                    Attributes.GetBonePoses().Set(FBoneID(Bone), Rig.GetRawRefBonePose()[Bone]);
                }
                bCompatible &= Cultivator->CommitMeshDescription(LOD);
            }
            Cultivator->PostEditChange();
            UPackage* Package = Cultivator->GetOutermost();
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            bCompatible &= UPackage::SavePackage(Package, Cultivator,
                *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), SaveArgs);
			const FReferenceSkeleton& Original = Human->GetRefSkeleton();
			const FReferenceSkeleton& Costume = Cultivator->GetRefSkeleton();
			bCompatible &= Original.GetNum() == Costume.GetNum();
            UE_LOG(LogTemp, Display, TEXT("Cultivator rig bones source=%d costume=%d"), Original.GetNum(), Costume.GetNum());
			for (int32 Bone = 0; bCompatible && Bone < Original.GetNum(); ++Bone)
			{
				bCompatible = Original.GetBoneName(Bone) == Costume.GetBoneName(Bone)
					&& Original.GetParentIndex(Bone) == Costume.GetParentIndex(Bone)
					&& Original.GetRefBonePose()[Bone].Equals(Costume.GetRefBonePose()[Bone], 1.e-3f);
                if (!bCompatible)
                {
                    UE_LOG(LogTemp, Error, TEXT("Cultivator bone %d source=%s parent=%d pose=%s costume=%s parent=%d pose=%s"),
                        Bone, *Original.GetBoneName(Bone).ToString(), Original.GetParentIndex(Bone), *Original.GetRefBonePose()[Bone].ToString(),
                        *Costume.GetBoneName(Bone).ToString(), Costume.GetParentIndex(Bone), *Costume.GetRefBonePose()[Bone].ToString());
                }
			}
			// Check every required take after import; no reimport/retarget is permitted.
			for (const QinglanHumanAnimation::FSourceTake& Take : QinglanHumanAnimation::RequiredTakes)
			{
				const UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *QinglanHumanAnimation::ObjectPath(Take.Suffix));
				FString Reason;
				if (!QinglanHumanAnimation::Validate(Sequence, Human->GetSkeleton(), Take, Reason))
				{
					UE_LOG(LogTemp, Error, TEXT("Cultivator animation contract %s: %s"), Take.Suffix, *Reason);
					bCompatible = false;
				}
			}
		}
		if (!bCompatible)
		{
			UE_LOG(LogTemp, Error, TEXT("Cultivator reference pose or Human animation contract does not match"));
		}
		bOk &= bCompatible;
	}
	bOk &= ImportTexturePNG(HeroRoot / TEXT("Textures/NakedLightSkin.png"),
		TEXT("/Game/Qinglan/Characters/Human"), TEXT("T_CultivatorSkin"));
	UE_LOG(LogTemp, Log, TEXT("QinglanMeshImport finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}

bool UQinglanMeshImportCommandlet::ImportSkeletalFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName)
{
	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bImportMesh = true;
	Options->MeshTypeToImport = FBXIT_SkeletalMesh; // UE 5.8 EFBXImportType (FBX_IMPORT_TYPE_* macros are gone)
	Options->bImportAsSkeletal = true;
	Options->bImportAnimations = true;       // pulls every embedded take
	Options->bImportTextures = false;        // materials handled by our master set
	Options->bImportMaterials = false;
	if (AssetName == TEXT("SK_Human") && Options->AnimSequenceImportData)
	{
		// The Human FBX declares 24 FPS but its playable takes end on 30 Hz
		// boundaries (with floating-point export noise). Bake consistently at
		// 30 Hz and align the last frame instead of rejecting six takes.
		Options->AnimSequenceImportData->bUseDefaultSampleRate = true;
		Options->AnimSequenceImportData->bSnapToClosestFrameBoundary = true;
	}
	// UE 5.8: UFbxImportUI no longer exposes bConvertSceneUnit (unit conversion is the importer default).
	if (Options->SkeletalMeshImportData)
	{
		Options->SkeletalMeshImportData->bComputeWeightedNormals = true; // smooth low-poly normals (bTransformVertexToNormal is gone)
	}
	UAssetImportTask* Task = MakeTask(SourceFile, DestPath, AssetName, Options);
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	const bool bOk = Task->GetObjects().Num() > 0; // UE 5.8: ImportedObjects replaced by GetObjects()
	UE_LOG(LogTemp, Log, TEXT("skeletal import %s -> %d objects"), *AssetName, Task->GetObjects().Num());
	return bOk;
}

bool UQinglanMeshImportCommandlet::ImportStaticFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName)
{
	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bImportMesh = true;
	Options->MeshTypeToImport = FBXIT_StaticMesh; // UE 5.8 EFBXImportType (FBX_IMPORT_TYPE_* macros are gone)
	Options->bImportAsSkeletal = false;
	Options->bImportTextures = false;
	Options->bImportMaterials = false;
	// UE 5.8: UFbxImportUI no longer exposes bConvertSceneUnit (unit conversion is the importer default).
	if (Options->StaticMeshImportData)
	{
		Options->StaticMeshImportData->bCombineMeshes = false;
		if (DestPath.EndsWith(TEXT("/Sanctuary")))
		{
			Options->StaticMeshImportData->bAutoGenerateCollision = false;
			Options->StaticMeshImportData->NormalImportMethod = FBXNIM_ImportNormals;
		}
	}
	UAssetImportTask* Task = MakeTask(SourceFile, DestPath, AssetName, Options);
	if (DestPath.EndsWith(TEXT("/Sanctuary")))
	{
		Options->bAutomatedImportShouldDetectType = false;
		Task->Factory = NewObject<UFbxFactory>();
	}
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	const bool bOk = Task->GetObjects().Num() > 0; // UE 5.8: ImportedObjects replaced by GetObjects()
	if (bOk && DestPath.EndsWith(TEXT("/Sanctuary")))
	{
		for (UObject* Imported : Task->GetObjects())
		{
			if (UStaticMesh* Static = Cast<UStaticMesh>(Imported))
			{
				// Ordinary reduction LODs work on Metal as well as DX12. Nanite is optional.
				Static->SetLODGroup(TEXT("SmallProp"));
				UPackage* Package = Static->GetOutermost();
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				if (!UPackage::SavePackage(Package, Static,
					*FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args))
				{ return false; }
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("static import %s -> %d objects"), *AssetName, Task->GetObjects().Num());
	return bOk;
}

bool UQinglanMeshImportCommandlet::ImportTexturePNG(const FString& SourceFile, const FString& DestPath, const FString& AssetName)
{
	UTextureFactory* Options = NewObject<UTextureFactory>(GetTransientPackage());
	Options->NoCompression = true;
	Options->ColorSpaceMode = ETextureSourceColorSpace::SRGB;
	Options->MipGenSettings = TMGS_NoMipmaps;
	UAssetImportTask* Task = MakeTask(SourceFile, DestPath, AssetName, Options);
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	const FString ObjectPath = DestPath / AssetName + TEXT(".") + AssetName;
	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
	if (!Texture || Task->GetObjects().IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("palette texture import failed: %s -> %s"), *SourceFile, *ObjectPath);
		return false;
	}
	// This 32x32 image is a five-band colour lookup, not a continuous surface.
	// Both hand UV islands sit within the top skin band. Mip averaging mixes
	// that band with hair/clothing, changing the hands at different distances.
	// Apply after import too: Interchange can bypass texture-factory options.
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->CompressionNone = true;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->Filter = TF_Nearest;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->NeverStream = true;
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	UPackage* Package = Texture->GetOutermost();
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bOk = UPackage::SavePackage(Package, Texture, *Filename, SaveArgs);
	UE_LOG(LogTemp, Log, TEXT("texture import %s -> %d objects"), *AssetName, Task->GetObjects().Num());
	return bOk;
}
