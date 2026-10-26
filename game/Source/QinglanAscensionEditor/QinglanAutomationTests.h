// Headless automation specs for QinglanAscension (editor module, -nullrhi safe).
//
// Everything in this folder exercises pure C++ paths only: no rendering, no
// input, no RNG. Where a world is unavoidable (combat component semantics),
// the spec builds a private UWorld and advances paused game time explicitly,
// so results are identical under `Automation RunTests Qinglan` in a
// `-unattended -nullrhi` editor commandlet.
//
// Authoritative expectations: docs/dev/code-contracts.md ("Combat semantics",
// "Narrative semantics", "Testing hooks"), docs/02-combat-and-camera.md
// sections 3-7, docs/03-progression-and-economy.md sections 1-6,
// docs/05-technical-architecture.md section 6.
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"
#include "Persistence/QinglanSaveGame.h"

/**
 * Pure mirror of the ending gate contract used by the ending-gating spec.
 *
 * The authoritative implementation is UQinglanStorySubsystem (world-bound and
 * seeded only through scene playback, so it cannot be driven headlessly from a
 * transient object); these helpers encode the same documented rules over the
 * portable FQinglanNarrativeState payload so the spec can verify the gating
 * matrix deterministically:
 *   - all endings require mq_shen_released (docs/dev/implementation-blueprint.md section 2.3)
 *   - ending_shared additionally requires the three effective agreements
 *     sq_spillway_verified / sq_allocation_agreed / sq_handover_accepted
 *     (code-contracts.md "Narrative semantics"; SQ02/SQ03 keep pending vs
 *     effective version slots and the preview reads effective only)
 *   - once an ending is confirmed no ending is available again (mutually
 *     exclusive, commit once per profile)
 */
namespace QinglanEndingGateMirror
{
	/** Ending enum -> committed ending flag id (ending_open / ending_seal / ending_shared). */
	inline FName ToEndingFlagId(EQinglanEnding Ending)
	{
		switch (Ending)
		{
		case EQinglanEnding::Open:   return FName(TEXT("ending_open"));
		case EQinglanEnding::Seal:   return FName(TEXT("ending_seal"));
		case EQinglanEnding::Shared: return FName(TEXT("ending_shared"));
		default:                     return NAME_None;
		}
	}

	/** mq_shen_released is set only after the release completion sequence dissolves (blueprint sections 1.9 / 2.3). */
	inline FName ShenReleasedFlag() { return FName(TEXT("mq_shen_released")); }

	/** True when the narrative state carries the flag at all (pending/effective split is per quest below). */
	inline bool HasFlag(const FQinglanNarrativeState& Narrative, FName Flag)
	{
		return Narrative.Flags.Contains(Flag);
	}

	/**
	 * An agreement is effective only when its quest flag is set AND the quest no
	 * longer sits in PendingAgreements (SQ02/SQ03 pending vs effective version
	 * slots; the shared-ending preview reads effective only).
	 */
	inline bool IsAgreementEffective(const FQinglanNarrativeState& Narrative, FName QuestId, FName QuestFlag)
	{
		if (Narrative.PendingAgreements.Contains(QuestId))
		{
			return false;
		}
		return HasFlag(Narrative, QuestFlag);
	}

	/** All three effective agreements: SQ01 verification flag plus SQ02/SQ03 effective versions. */
	inline bool SharedEndingUnlocked(const FQinglanNarrativeState& Narrative)
	{
		// SQ01 has no proposal versioning; its completion flag is the agreement.
		if (!HasFlag(Narrative, FName(TEXT("sq_spillway_verified")))) { return false; }
		if (!IsAgreementEffective(Narrative, FName(TEXT("SQ02")), FName(TEXT("sq_allocation_agreed")))) { return false; }
		if (!IsAgreementEffective(Narrative, FName(TEXT("SQ03")), FName(TEXT("sq_handover_accepted")))) { return false; }
		return true;
	}

	/** Availability gate mirror; see the contract notes above. */
	inline bool IsEndingAvailable(const FQinglanNarrativeState& Narrative, EQinglanEnding Ending)
	{
		if (Ending == EQinglanEnding::None) { return false; }
		if (Narrative.ConfirmedEnding != EQinglanEnding::None) { return false; } // committed once, no re-offer
		if (!HasFlag(Narrative, ShenReleasedFlag())) { return false; }           // all endings release Shen
		if (Ending == EQinglanEnding::Shared) { return SharedEndingUnlocked(Narrative); }
		return true; // open / seal are always available once Shen is released
	}
}
