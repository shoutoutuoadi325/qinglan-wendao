#include "QinglanRawAssets.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanRawAssetRootPrecedence,
	"Qinglan.Content.RawAssetRootPrecedence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanRawAssetRootPrecedence::RunTest(const FString& Parameters)
{
	const FString Fixture = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("RawAssetPathContract"));
	const FString ProjectDir = Fixture / TEXT("repo/game");
	const FString ExplicitRoot = Fixture / TEXT("explicit assets");
	const FString EnvironmentRoot = Fixture / TEXT("environment-assets");
	TestEqual(TEXT("quoted command-line root wins over environment"),
		QinglanRawAssets::ResolveRootFromInputs(
			FString::Printf(TEXT("-lowquality -assetsraw=\"%s\""), *ExplicitRoot), EnvironmentRoot, ProjectDir),
		ExplicitRoot);
	TestEqual(TEXT("environment root is used when flag is absent"),
		QinglanRawAssets::ResolveRootFromInputs(TEXT("-lowquality"), EnvironmentRoot, ProjectDir),
		EnvironmentRoot);
	TestEqual(TEXT("default is beside repo rather than inside game"),
		QinglanRawAssets::ResolveRootFromInputs(TEXT(""), TEXT(""), ProjectDir),
		Fixture / TEXT("assets-raw"));
	return true;
}
