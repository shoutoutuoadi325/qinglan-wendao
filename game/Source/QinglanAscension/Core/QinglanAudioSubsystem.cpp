#include "Core/QinglanAudioSubsystem.h"
#include "Core/QinglanAudioAssets.h"

#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

namespace
{
	// QinglanAudioBootstrapCommandlet imports each source file as
	// /Game/Qinglan/Audio/<Group>/SW_<file base name>.
	const TCHAR* MusicPathFor(EQinglanMusicState State)
	{
		switch (State)
		{
		case EQinglanMusicState::Hub:     return TEXT("/Game/Qinglan/Audio/Music/SW_music_hub_calm_orient_peace_valley");
		case EQinglanMusicState::Explore: return TEXT("/Game/Qinglan/Audio/Music/SW_music_combat_alt_prepare_your_swords");
		case EQinglanMusicState::Combat:  return TEXT("/Game/Qinglan/Audio/Music/SW_music_combat_fast_fight_loop");
		case EQinglanMusicState::Boss:    return TEXT("/Game/Qinglan/Audio/Music/SW_music_boss_epic_boss_battle_loop");
		default:                          return nullptr;
		}
	}

	const TCHAR* SfxPathFor(FName SfxId)
	{
		if (SfxId == FName(TEXT("sfx_sword_swing")))       { return TEXT("/Game/Qinglan/Audio/SFX/SW_sfx_sword_swing"); }
		if (SfxId == FName(TEXT("sfx_hit_impact")))        { return TEXT("/Game/Qinglan/Audio/SFX/SW_sfx_hit_impact"); }
		if (SfxId == FName(TEXT("sfx_technique_release"))) { return TEXT("/Game/Qinglan/Audio/SFX/SW_sfx_technique_release"); }
		if (SfxId == FName(TEXT("sfx_telegraph_warn")))    { return QinglanAudioAssets::TelegraphWarning; }
		return nullptr;
	}

	constexpr float MusicFadeOutSeconds = 1.0f;
	constexpr float MusicFadeInSeconds = 1.5f;
}

void UQinglanAudioSubsystem::SetMusicState(EQinglanMusicState NewState)
{
	if (NewState == MusicState)
	{
		return;
	}
	MusicState = NewState;
	StopMusic();
	if (const TCHAR* Path = MusicPathFor(NewState))
	{
		if (USoundBase* Track = LoadSound(Path))
		{
			MusicComponent = UGameplayStatics::CreateSound2D(this, Track);
			if (MusicComponent)
			{
				MusicComponent->FadeIn(MusicFadeInSeconds, 1.f);
			}
		}
	}
}

void UQinglanAudioSubsystem::PlaySfx(FName SfxId)
{
	if (const TCHAR* Path = SfxPathFor(SfxId))
	{
		if (USoundBase* Cue = LoadSound(Path))
		{
			UGameplayStatics::PlaySound2D(this, Cue);
		}
	}
}

USoundBase* UQinglanAudioSubsystem::LoadSound(const TCHAR* PackagePath) const
{
	// StaticLoadObject logs load failures; missing assets degrade to silence.
	return Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, PackagePath));
}

void UQinglanAudioSubsystem::StopMusic()
{
	if (MusicComponent)
	{
		MusicComponent->FadeOut(MusicFadeOutSeconds, 0.f);
		MusicComponent = nullptr;
	}
}
