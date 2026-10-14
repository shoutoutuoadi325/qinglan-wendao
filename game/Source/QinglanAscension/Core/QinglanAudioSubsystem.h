// Audio subsystem: music state selection and 2D combat SFX cues.
//
// Contracts: docs/04-art-and-vfx.md (audio pass: hub/combat/boss music,
// telegraph warning, sword swing, technique release); assets are imported by
// QinglanAudioBootstrapCommandlet into /Game/Qinglan/Audio. Every entry is
// null-safe: without the imported assets or an audio device (headless runs)
// calls degrade to no-ops.
#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "QinglanAudioSubsystem.generated.h"

class USoundBase;
class UAudioComponent;

/** Music tracks keyed by gameplay state (docs/04 section 5 audio pass). */
UENUM(BlueprintType)
enum class EQinglanMusicState : uint8
{
	None,
	/** Sanctuary hub: calm valley loop. */
	Hub,
	/** Realm exploration between encounters: prepare-your-swords loop. */
	Explore,
	/** Ordinary encounter active: fast fight loop. */
	Combat,
	/** Boss encounter active: epic boss loop. */
	Boss,
};

UCLASS()
class UQinglanAudioSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Switches the music track if the state changed; crossfades the old one out. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Audio")
	void SetMusicState(EQinglanMusicState NewState);

	UFUNCTION(BlueprintPure, Category = "Qinglan|Audio")
	EQinglanMusicState GetMusicState() const { return MusicState; }

	/**
	 * Plays a 2D SFX cue by stable id (sfx_sword_swing, sfx_hit_impact,
	 * sfx_technique_release, sfx_telegraph_warn). Fire-and-forget.
	 */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Audio")
	void PlaySfx(FName SfxId);

private:
	/** Resolves a sound asset by well-known package path; null when absent. */
	USoundBase* LoadSound(const TCHAR* PackagePath) const;
	void StopMusic();

	EQinglanMusicState MusicState = EQinglanMusicState::None;
	/** Active music component kept alive so the fade-out can reach it. */
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MusicComponent;
};
