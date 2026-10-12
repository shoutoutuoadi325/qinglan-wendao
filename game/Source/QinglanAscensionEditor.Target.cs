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
		ExtraModuleNames.Add("QinglanAscension");
		ExtraModuleNames.Add("QinglanAscensionEditor");
	}
}
