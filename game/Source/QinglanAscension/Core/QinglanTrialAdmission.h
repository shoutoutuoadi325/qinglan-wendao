// AUD-18: shared admission policy. No rewards, travel, or narrative mutations.
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"

class UQinglanProfileSubsystem;
class UQinglanStorySubsystem;

namespace QinglanTrialAdmission
{

enum class ETrial : uint8 { Foundation, GoldenCore };
enum class ERejection : uint8
{
	None, MissingProfile, WrongStage, RanksIncomplete, BossProofMissing,
	MissingStory, InvalidScene, StoryPrerequisitesMissing, AlreadyCompleted, InvalidObjective
};

/** Snapshot boundary lets automation exercise the policy without a world. */
struct FPrerequisites
{
	bool bHasProfile = false;
	EQinglanStage Stage = EQinglanStage::QiRefining;
	int32 Rank = 0;
	bool bBambooCleared = false;
	bool bAshenCleared = false;
};

struct FResult
{
	ERejection Rejection = ERejection::MissingProfile;
	/** ST_Game key describing a rejected admission; NAME_None only on success. */
	FName ReasonKey = FName(TEXT("trial.blocked.profile_unavailable"));
	bool IsAllowed() const { return Rejection == ERejection::None; }
};

QINGLANASCENSION_API bool ResolveScene(FName SceneId, ETrial& OutTrial);
/** Golden Core profiles resolve to Golden Core, then fail the progression admission check. */
QINGLANASCENSION_API ETrial SelectForStage(EQinglanStage Stage);
QINGLANASCENSION_API FName SceneForTrial(ETrial Trial);
QINGLANASCENSION_API FResult Evaluate(ETrial Trial, const FPrerequisites& State);
QINGLANASCENSION_API FResult Evaluate(ETrial Trial, const UQinglanProfileSubsystem* Profile,
	const UQinglanStorySubsystem* Story);
/** Hub entry checks the chapter preparation scene, allowing its preparation to happen in the arena. */
QINGLANASCENSION_API FResult EvaluateEntry(ETrial Trial, const UQinglanProfileSubsystem* Profile,
	const UQinglanStorySubsystem* Story);
/** Adds imported trial-scene prerequisites; suitable after the entry choice has committed. */
QINGLANASCENSION_API FResult EvaluateForTrial(ETrial Trial, const UQinglanProfileSubsystem* Profile,
	const UQinglanStorySubsystem* Story);
/** Final authority for starting combat: validates exact scene and authored objective. */
QINGLANASCENSION_API FResult EvaluateScene(FName SceneId, FName ObjectiveId,
	const UQinglanProfileSubsystem* Profile, const UQinglanStorySubsystem* Story);

// TODO(AUD-18): define replay admission separately (Golden Core is rankless); current
// admission deliberately permits only the corresponding first campaign breakthrough.
// TODO(AUD-18): automation matrix: each boss alias, rank 0..3, mismatched stages,
// missing profile, and every entry path. This policy does not mark the audit complete.
}
