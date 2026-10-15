#include "Core/QinglanTrialAdmission.h"

#include "Narrative/QinglanStorySubsystem.h"
#include "Narrative/QinglanStoryDataTypes.h"
#include "Progression/QinglanProfileSubsystem.h"
#include "Progression/QinglanProgressionTables.h"

namespace QinglanTrialAdmission
{
ETrial SelectForStage(EQinglanStage Stage)
{
	return Stage == EQinglanStage::QiRefining ? ETrial::Foundation : ETrial::GoldenCore;
}

FName SceneForTrial(ETrial Trial)
{
	return FName(Trial == ETrial::Foundation ? TEXT("CH03_S02") : TEXT("CH06_S02"));
}

bool ResolveScene(FName SceneId, ETrial& OutTrial)
{
	if (SceneId == FName(TEXT("CH03_S02")))
	{
		OutTrial = ETrial::Foundation;
		return true;
	}
	if (SceneId == FName(TEXT("CH06_S02")))
	{
		OutTrial = ETrial::GoldenCore;
		return true;
	}
	return false;
}

FResult Evaluate(ETrial Trial, const FPrerequisites& State)
{
	if (!State.bHasProfile)
	{
		return { ERejection::MissingProfile, FName(TEXT("trial.blocked.profile_unavailable")) };
	}
	const EQinglanStage RequiredStage = Trial == ETrial::Foundation
		? EQinglanStage::QiRefining : EQinglanStage::FoundationEstablishment;
	if (State.Stage != RequiredStage)
	{
		return { ERejection::WrongStage, FName(TEXT("trial.blocked.wrong_stage")) };
	}
	if (State.Rank < QinglanProgression::RanksPerStage)
	{
		return { ERejection::RanksIncomplete, FName(TEXT("cultivation.blocked.ranks_incomplete")) };
	}
	const bool bBossCleared = Trial == ETrial::Foundation ? State.bBambooCleared : State.bAshenCleared;
	if (!bBossCleared)
	{
		return { ERejection::BossProofMissing, FName(TEXT("cultivation.blocked.boss_required")) };
	}
	return { ERejection::None, NAME_None };
}

FResult Evaluate(ETrial Trial, const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
{
	FPrerequisites State;
	if (Profile)
	{
		State.bHasProfile = true;
		State.Stage = Profile->GetCultivationStage();
		State.Rank = Profile->GetCultivationRank();
	}
	const auto HasProof = [Profile, Story](const TCHAR* CampaignFlag, const TCHAR* StoryFlag)
	{
		return (Profile && Profile->HasCampaignFlag(FName(CampaignFlag)))
			|| (Story && Story->HasFlag(FName(StoryFlag)));
	};
	State.bBambooCleared = HasProof(QinglanProgression::BambooClearedCampaignFlag,
		QinglanProgression::BambooClearedStoryFlag);
	State.bAshenCleared = HasProof(QinglanProgression::AshenClearedCampaignFlag,
		QinglanProgression::AshenClearedStoryFlag);
	return Evaluate(Trial, State);
}

FResult EvaluateForTrial(ETrial Trial, const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
{
	const FResult Progression = Evaluate(Trial, Profile, Story);
	if (!Progression.IsAllowed())
	{
		return Progression;
	}
	if (!Story || !Story->GetRegistry())
	{
		return { ERejection::MissingStory, FName(TEXT("trial.unavailable")) };
	}
	const FName SceneId = SceneForTrial(Trial);
	const FQinglanRegistryScene* Scene = Story->GetRegistry()->FindMainScene(SceneId);
	if (!Scene)
	{
		return { ERejection::InvalidScene, FName(TEXT("trial.unavailable")) };
	}
	if (Story->IsSceneCompleted(SceneId))
	{
		return { ERejection::AlreadyCompleted, FName(TEXT("trial.blocked.completed")) };
	}
	for (FName Prerequisite : Scene->Prerequisites)
	{
		if (!Story->HasFlag(Prerequisite))
		{
			return { ERejection::StoryPrerequisitesMissing, FName(TEXT("trial.blocked.story_required")) };
		}
	}
	return { ERejection::None, NAME_None };
}

FResult EvaluateEntry(ETrial Trial, const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
{
	const FResult Progression = Evaluate(Trial, Profile, Story);
	if (!Progression.IsAllowed())
	{
		return Progression;
	}
	const UQinglanStoryRegistry* Registry = Story ? Story->GetRegistry() : nullptr;
	const FName PreparationScene(Trial == ETrial::Foundation ? TEXT("CH03_S01") : TEXT("CH06_S01"));
	const FQinglanRegistryScene* Scene = Registry ? Registry->FindMainScene(PreparationScene) : nullptr;
	if (!Scene)
	{
		return { ERejection::MissingStory, FName(TEXT("trial.unavailable")) };
	}
	for (FName Prerequisite : Scene->Prerequisites)
	{
		if (!Story->HasFlag(Prerequisite))
		{
			return { ERejection::StoryPrerequisitesMissing, FName(TEXT("trial.blocked.story_required")) };
		}
	}
	return { ERejection::None, NAME_None };
}

FResult EvaluateScene(FName SceneId, FName ObjectiveId,
	const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story)
{
	ETrial Trial;
	if (!ResolveScene(SceneId, Trial))
	{
		return { ERejection::InvalidScene, FName(TEXT("trial.unavailable")) };
	}
	const FResult Admission = EvaluateForTrial(Trial, Profile, Story);
	if (!Admission.IsAllowed())
	{
		return Admission;
	}
	const FQinglanRegistryScene* Scene = Story->GetRegistry()->FindMainScene(SceneId);
	for (const auto& Objective : Scene->Objectives)
	{
		if (ObjectiveId != NAME_None && Objective.ObjectiveId == ObjectiveId)
		{
			return { ERejection::None, NAME_None };
		}
	}
	return { ERejection::InvalidObjective, FName(TEXT("trial.unavailable")) };
}
}
