// Included only by QinglanGraphicsSettingsService.cpp in automation builds.
namespace
{
	struct FScopedQinglanGraphicsTestCvar
	{
		explicit FScopedQinglanGraphicsTestCvar(const TCHAR* InName)
			: Name(InName)
			, Variable(IConsoleManager::Get().RegisterConsoleVariable(
				InName, 1, TEXT("Temporary graphics-priority regression fixture"), ECVF_Default))
		{
		}

		~FScopedQinglanGraphicsTestCvar()
		{
			IConsoleManager::Get().UnregisterConsoleObject(Variable, false);
		}

		const TCHAR* Name;
		IConsoleVariable* Variable;
	};
}

bool FQinglanGraphicsRuntimeFeaturePriority::RunTest(const FString& Parameters)
{
	FScopedQinglanGraphicsTestCvar Fixture(TEXT("qinglan.Test.GraphicsFeaturePriority"));
	Fixture.Variable->Set(1, ECVF_SetByProjectSetting);
	SetFeaturePolicyCvar(Fixture.Name, 0);
	TestEqual(TEXT("Low feature policy overrides the authored project default"), Fixture.Variable->GetInt(), 0);
	TestEqual(TEXT("User policy remains at project priority"),
		Fixture.Variable->GetFlags() & ECVF_SetByMask, static_cast<uint32>(ECVF_SetByProjectSetting));
	SetFeaturePolicyCvar(Fixture.Name, 1);
	TestEqual(TEXT("Higher quality can restore the feature after Low"), Fixture.Variable->GetInt(), 1);
	return true;
}

bool FQinglanGraphicsRespectsPlatformConstraints::RunTest(const FString& Parameters)
{
	FScopedQinglanGraphicsTestCvar Fixture(TEXT("qinglan.Test.GraphicsPlatformConstraint"));
	Fixture.Variable->Set(0, ECVF_SetByDeviceProfile);
	SetFeaturePolicyCvar(Fixture.Name, 1);
	TestEqual(TEXT("A quality preset cannot enable a device-disabled feature"), Fixture.Variable->GetInt(), 0);
	TestEqual(TEXT("The device-profile priority is preserved"),
		Fixture.Variable->GetFlags() & ECVF_SetByMask, static_cast<uint32>(ECVF_SetByDeviceProfile));
	Fixture.Variable->Set(2, ECVF_SetByCommandline);
	SetFeaturePolicyCvar(Fixture.Name, 0);
	TestEqual(TEXT("Explicit command-line renderer overrides remain authoritative"), Fixture.Variable->GetInt(), 2);
	TestEqual(TEXT("The command-line priority is preserved"),
		Fixture.Variable->GetFlags() & ECVF_SetByMask, static_cast<uint32>(ECVF_SetByCommandline));
	return true;
}
