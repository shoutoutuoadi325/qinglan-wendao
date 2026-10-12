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
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Niagara",
			"GameplayTags",
			"JsonUtilities",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"NavigationSystem",
			"AIModule"
		});
	}
}
