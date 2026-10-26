// Audio bootstrap: imports the project-original music/SFX WAV set from
// assets-raw/audio/original into /Game/Qinglan/Audio (AUD-56).
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject
//      -run=QinglanAscensionEditor.QinglanAudioBootstrapCommandlet
#pragma once

#include "Commandlets/Commandlet.h"
#include "QinglanAudioBootstrap.generated.h"

class USoundWave;

UCLASS()
class UQinglanAudioBootstrapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanAudioBootstrapCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	/** One WAV -> /Game/Qinglan/Audio/<Group>/SW_<BaseName>; looping for music. */
	USoundWave* ImportSoundWave(const FString& SourceFile, const TCHAR* Group, bool bLooping);
};
