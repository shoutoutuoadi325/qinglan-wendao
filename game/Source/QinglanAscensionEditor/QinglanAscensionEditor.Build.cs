// Qinglan: Path of Ascension - editor-only module (importers, validators, commandlets).
using UnrealBuildTool;

public class QinglanAscensionEditor : ModuleRules
{
	public QinglanAscensionEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"QinglanAscension"
		});

		// Allow "Folder/Header.h" includes rooted at the module directory.
		PublicIncludePaths.Add(ModuleDirectory);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"EditorStyle",
			"Json",
			"JsonUtilities",
			"AssetTools",
			"AssetRegistry", // FAssetRegistryModule::AssetCreated (not transitively public via UnrealEd/AssetTools)
			"PythonScriptPlugin",
			"UMG",
			"UMGEditor",
			"Kismet"
		});
	}
}
