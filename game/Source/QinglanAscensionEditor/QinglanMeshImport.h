// Mesh import commandlet: bulk-imports fetched CC0 FBX assets
// (characters with embedded animation takes, monsters, nature, weapons).
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanMeshImport
#pragma once

#include "Commandlets/Commandlet.h"
#include "QinglanMeshImport.generated.h"

UCLASS()
class UQinglanMeshImport : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanMeshImport();

	virtual int32 Main(const FString& Params) override;

private:
	/** Imports one FBX as a skeletal mesh + all animation takes. */
	bool ImportSkeletalFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName);
	/** Imports one FBX as a static mesh. */
	bool ImportStaticFBX(const FString& SourceFile, const FString& DestPath, const FString& AssetName);
};
