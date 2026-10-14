// Audio bootstrap: imports the music/SFX WAV set into /Game/Qinglan/Audio.
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
