// Qinglan: Path of Ascension - editor target.
using UnrealBuildTool;
using System.Collections.Generic;

public class QinglanAscensionEditorTarget : TargetRules
{
	public QinglanAscensionEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		// CI's Windows disk optimization must not invalidate every native Mac
		// engine object or change its debugging policy.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			bOmitPCDebugInfoInDevelopment = true;
		}
		ExtraModuleNames.Add("QinglanAscension");
		ExtraModuleNames.Add("QinglanAscensionEditor");
	}
}
