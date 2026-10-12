// Ending gating rules: pure, deterministic, headless-testable functions shared
// by UQinglanStorySubsystem preview/confirm and the automation specs
// (code-contracts.md "Narrative semantics" / "Testing hooks").
//
// Sources:
// - docs/story/narrative-implementation.md sections 4-5 (preview never commits,
//   confirm is atomic/mutually-exclusive/once, shared requires all agreements)
// - docs/dev/implementation-blueprint.md section 2.3 (END_OPEN "always",
//   END_SEAL "always", END_SHARED requires the three agreement flags,
//   all endings require mq_shen_released)
// - data/story/main-late.json CH06_S04 (ending ids, flags, requirements)
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"

/** Ending gating rules namespace; no UObject state, no randomness. */
namespace QinglanEnding
{
	// --- Stable ids (data/story/main-late.json CH06_S04 action ending_id values
	// are END_OPEN/END_SEAL/END_SHARED; the runtime/subsystem API and the
	// mutually exclusive commit flags use the lowercase flag spellings) ---
	inline constexpr const TCHAR* EndingOpenId = TEXT("ending_open");
	inline constexpr const TCHAR* EndingSealId = TEXT("ending_seal");
	inline constexpr const TCHAR* EndingSharedId = TEXT("ending_shared");

	/** Flag that must be set before any ending may be previewed or confirmed
	 * (docs/dev/implementation-blueprint.md section 2.3: "All require
	 * mq_shen_released; all release Shen"). */
	inline constexpr const TCHAR* ShenReleasedFlag = TEXT("mq_shen_released");

	/** The three effective-agreement flags END_SHARED additionally requires
	 * (docs/dev/implementation-blueprint.md section 2.3; each is set only by its
	 * quest's atomic effective-agreement commit, so reading these flags reads the
	 * effective snapshot, never a pending draft). */
	inline constexpr const TCHAR* SharedAgreementFlags[3] =
	{
		TEXT("sq_spillway_verified"),   // SQ01 completion
		TEXT("sq_allocation_agreed"),   // SQ02 effective agreement
		TEXT("sq_handover_accepted")    // SQ03 effective agreement
	};

	/** Number of shared-agreement prerequisites (kept beside the array for
	 * bounds checks in specs). */
	inline constexpr int32 SharedAgreementFlagCount = 3;

	/** True when EndingId is one of the three authored ending ids; accepts both
	 * the runtime spelling (ending_open) and the registry payload spelling
	 * (END_OPEN) from data/story/main-late.json. */
	bool IsKnownEndingId(FName EndingId);

	/** Maps an ending id (either spelling) to the enum; EQinglanEnding::None for
	 * unknown ids. */
	EQinglanEnding EndingIdToEnum(FName EndingId);

	/** Canonical runtime id for an enum value; NAME_None for None. */
	FName EndingEnumToId(EQinglanEnding Ending);

	/** The one mutually exclusive commit flag for an ending (ending_open /
	 * ending_seal / ending_shared); NAME_None for None. */
	FName EndingCommitFlag(EQinglanEnding Ending);

	/**
	 * Evaluates availability. HasFlag is queried once per prerequisite flag, so
	 * callers can route it through any flag store.
	 * Fills OutMissing with the unmet prerequisite flags in authored order
	 * (mq_shen_released first, then the three shared agreements).
	 * END_OPEN and END_SEAL are always available once Shen is released; a
	 * different valid ending remains selectable even when all three agreements
	 * are complete (docs/story/narrative-implementation.md section 4: "If the
	 * player chooses a different valid ending after completing these agreements,
	 * honor that decision").
	 */
	bool EvaluateAvailability(EQinglanEnding Ending, const TFunctionRef<bool(FName)>& HasFlag, TArray<FName>& OutMissing);

	/**
	 * Requirement text for one missing prerequisite flag, resolved from
	 * /Game/Qinglan/Data/ST_Game (keys from localization/story/en-US/CH06.json):
	 * mq_shen_released -> story.ch06.s03.objective; the three agreements ->
	 * story.ch06.s04.{spillway,allocation,handover}_missing. Unknown flags fall
	 * back to the shared ending-review objective line.
	 */
	FText RequirementTextForFlag(FName Flag);

	/** Joined requirement text for every missing flag (newline separated), or
	 * the ending's own preview text when nothing is missing
	 * (story.ch06.s04.{open,seal,shared}_preview). */
	FText RequirementText(EQinglanEnding Ending, const TArray<FName>& MissingFlags);
} // namespace QinglanEnding
