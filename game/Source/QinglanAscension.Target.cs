// Qinglan: Path of Ascension - game target.
using UnrealBuildTool;
using System.Collections.Generic;

public class QinglanAscensionTarget : TargetRules
{
	public QinglanAscensionTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ShadowVariableWarningLevel = WarningLevel.Warning;
		ExtraModuleNames.Add("QinglanAscension");

		// Offline single-player game: no online subsystem, no signing integration here.
	}
}
