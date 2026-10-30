// Qinglan: Path of Ascension - primary runtime module.
using UnrealBuildTool;

public class QinglanAscension : ModuleRules
{
	public QinglanAscension(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AIModule", // Public controller headers inherit AAIController.
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Niagara",
			"GameplayTags",
			"JsonUtilities",
			"Json"
		});

		// Allow "Folder/Header.h" includes rooted at the module directory.
		PublicIncludePaths.Add(ModuleDirectory);

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"NavigationSystem",
			// Exact safe-sector geometry; engine runtime modules available on every target.
			"GeometryCore",
			"GeometryFramework",
			// Platform/QinglanGraphicsSettingsService reads the active RHI name,
			// adapter string, and ray-tracing capability globals (docs/06 §2-§3).
			"RHI",
			// UI/QinglanHUDStatics creates widget instances (UWidgetBlueprintLibrary).
			"UMG"
		});
	}
}
