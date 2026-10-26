// UQinglanDataImportCommandlet: headless editor commandlet that regenerates
// the game's imported content assets from the repository data directory.
//
//   UnrealEditor-Cmd <uproject> -run=QinglanDataImport -data=<repo data dir> [-loc=<localization dir>]
//
// Inputs (docs/dev/implementation-blueprint.md section 3 "Data Contracts"):
//   <data>/story/*.json               -> UQinglanStoryRegistry      /Game/Qinglan/Data/DA_StoryRegistry
//   <data>/examples/techniques*.json  -> UQinglanTechniqueDataAsset /Game/Qinglan/Data/Techniques/DA_Tech_<id>
//   <data>/examples/realm*.json       -> UQinglanRealmDataAsset     /Game/Qinglan/Data/DA_Realm_<id>
//   <loc>/*.json + <loc>/story/*/*.json -> UStringTable             /Game/Qinglan/Data/ST_Game
//   (<loc> defaults to the sibling "localization" directory next to <data>.)
//
// Validation fails the whole import with a precise error list (code-contracts.md
// "Localization keys": the importer regenerates ST_Game; runtime only reads keys).
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "QinglanDataImportCommandlet.generated.h"

/**
 * Runs on the editor commandlet thread only; deterministic (sorted file
 * iteration, no RNG anywhere). Never runs in game builds.
 */
UCLASS()
class UQinglanDataImportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanDataImportCommandlet();

	/** Returns 0 when every file validated and all assets were saved, else 1. */
	virtual int32 Main(const FString& Params) override;
};
