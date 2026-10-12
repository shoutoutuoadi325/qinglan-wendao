// Content bootstrap commandlet: creates engine-facing content that cannot be
// authored by hand in a headless environment - Enhanced Input assets, master
// material + instances from fetched CC0 texture sets, and the four maps with
// lighting, anchors, and blockout geometry.
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanContentBootstrap
#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "QinglanContentBootstrap.generated.h"

UCLASS()
class UQinglanContentBootstrap : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanContentBootstrap();

	virtual int32 Main(const FString& Params) override;

private:
	/** /Game/Qinglan/Input: IA_* actions + IMC_Default with full bindings. */
	bool BootstrapInput();
	/** /Game/Qinglan/Materials: master surface material + MICs per texture set. */
	bool BootstrapMaterials();
	/** /Game/Qinglan/Maps: L_Hub, L_BambooHollow, L_AshenVerge, L_TribulationArena. */
	bool BootstrapMaps();
	/** Master material with BaseColor/Normal/Roughness/AO texture parameters. */
	class UMaterial* CreateOrLoadMasterMaterial();
	/** Saves a newly created in-memory object into its package on disk. */
	bool SavePackage(UPackage* Package, UObject* Asset);
};
