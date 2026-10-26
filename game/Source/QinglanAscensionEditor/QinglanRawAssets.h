#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

// All content commandlets share this precedence. ProjectDir is repo/game/;
// the default raw pack directory is beside the repository, not the engine.
namespace QinglanRawAssets
{
	inline FString ResolveRootFromInputs(const FString& Params, const FString& EnvironmentRoot,
		const FString& ProjectDir)
	{
		FString Root;
		if (!FParse::Value(*Params, TEXT("assetsraw="), Root))
		{
			Root = EnvironmentRoot.IsEmpty()
				? FPaths::Combine(ProjectDir, TEXT("../../assets-raw"))
				: EnvironmentRoot;
		}
		if (Root.IsEmpty())
		{
			return Root;
		}
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		FPaths::CollapseRelativeDirectories(Root);
		return Root;
	}

	inline FString ResolveRoot(const FString& Params)
	{
		return ResolveRootFromInputs(Params,
			FPlatformMisc::GetEnvironmentVariable(TEXT("QINGLAN_ASSETS_RAW")), FPaths::ProjectDir());
	}

	inline bool RequireDirectory(const FString& Directory)
	{
		if (!Directory.IsEmpty() && IFileManager::Get().DirectoryExists(*Directory))
		{
			return true;
		}
		UE_LOG(LogTemp, Error,
			TEXT("raw asset directory missing: %s (use -assetsraw=<root> or QINGLAN_ASSETS_RAW)"),
			*Directory);
		return false;
	}
}
