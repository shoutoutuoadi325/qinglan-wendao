// Mesh import implementation: FBX via the legacy FbxFactory asset-import path
// (deterministic options, headless-safe). Characters import with all embedded
// animation takes; nature/weapon meshes import as static meshes.
#include "QinglanMeshImport.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	const FString RawRoot = TEXT("/inspire/hdd/project/qianghuaxuexi/s26044/assets-raw/quaternius/extracted");
	const FString ContentRoot = TEXT("/Game/Qinglan");

	UAssetImportTask* MakeTask(const FString& Source, const FString& DestPath, const FString& Name, UFbxImportUI* Options)
	{
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = Source;
		Task->DestinationPath = DestPath;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->Options = Options;
		return Task;
	}
}

UQinglanMeshImport::UQinglanMeshImport()
{
	LogToClipsAndLog = true;
}

int32 UQinglanMeshImport::Main(const FString& Params)
{
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
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("missing skeletal source: %s"), *Src);
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
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(RawRoot / Dir.Key / TEXT("*.fbx")), true, false);
		for (const FString& F : Files)
		{
			const FString Base = FPaths::GetBaseFilename(F);
			bOk &= ImportStaticFBX(RawRoot / Dir.Key / F, Dir.Value, Base);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("QinglanMeshImport finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}

bool UQinglanMeshImport::ImportSkeletalFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName)
{
	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bImportMesh = true;
	Options->MeshTypeToImport = FBX_IMPORT_TYPE_SKEL_MESH;
	Options->bImportAsSkeletal = true;
	Options->bImportAnimations = true;       // pulls every embedded take
	Options->bImportTextures = false;        // materials handled by our master set
	Options->bImportMaterials = false;
	Options->bConvertSceneUnit = true;
	if (Options->SkeletalMeshImportData)
	{
		Options->SkeletalMeshImportData->bTransformVertexToNormal = true; // smooth low-poly normals
		Options->SkeletalMeshImportData->ImportUniformScale = 1.0f;
	}
	UAssetImportTask* Task = MakeTask(SourceFile, DestPath, AssetName, Options);
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	const bool bOk = Task->ImportedObjects.Num() > 0;
	UE_LOG(LogTemp, Log, TEXT("skeletal import %s -> %d objects"), *AssetName, Task->ImportedObjects.Num());
	return bOk;
}

bool UQinglanMeshImport::ImportStaticFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName)
{
	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bImportMesh = true;
	Options->MeshTypeToImport = FBX_IMPORT_TYPE_STATIC_MESH;
	Options->bImportAsSkeletal = false;
	Options->bImportTextures = false;
	Options->bImportMaterials = false;
	Options->bConvertSceneUnit = true;
	if (Options->StaticMeshImportData)
	{
		Options->StaticMeshImportData->bCombineMeshes = false;
	}
	UAssetImportTask* Task = MakeTask(SourceFile, DestPath, AssetName, Options);
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
	const bool bOk = Task->ImportedObjects.Num() > 0;
	UE_LOG(LogTemp, Log, TEXT("static import %s -> %d objects"), *AssetName, Task->ImportedObjects.Num());
	return bOk;
}
