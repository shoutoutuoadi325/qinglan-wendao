#include "Core/QinglanAudioAssets.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundWave.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanTelegraphWarningAsset,
	"Qinglan.Content.TelegraphWarningAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanTelegraphWarningAsset::RunTest(const FString& Parameters)
{
	// Exercise the reference used by both AI warning paths, not a duplicate
	// test-only filename that could pass while gameplay silently loses audio.
	USoundWave* Cue = Cast<USoundWave>(FSoftObjectPath(QinglanAudioAssets::TelegraphWarning).TryLoad());
	if (!TestNotNull(TEXT("Runtime AI warning reference loads the imported sound"), Cue)) { return false; }
	TestTrue(TEXT("Warning has audible duration"), Cue->Duration > 0.f);
	TestFalse(TEXT("Warning is a one-shot"), Cue->bLooping);
	return true;
}
#endif
