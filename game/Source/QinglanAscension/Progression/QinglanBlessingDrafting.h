// Blessing drafting core: the pure, deterministic 1-of-3 offer roll used by
// UQinglanRunSubsystem after draft rooms (blueprint section 1.4: rooms 2 and
// 4; docs/03-progression-and-economy.md section 5: "Offer three distinct
// eligible blessings after rooms 2 and 4").
//
// Rules implemented here (docs/03 section 5 + blueprint section 2.6):
//   - 12 blessings = 4 groups x 3 (sword offense / casting economy /
//     protection / elemental interactions; definitions live in
//     UQinglanBlessingLibrary, never duplicated here).
//   - Exactly 3 offers, each from a DISTINCT group (mutually exclusive pick).
//   - One-stack cap per group: a group with an owned blessing is capped for
//     the rest of the expedition and never offered again.
//   - Owned blessings are never re-offered.
//   - If fewer than three options remain, return the available count without
//     inventing duplicates ("show the available count", docs/03 section 5).
//
// Determinism: the ONLY randomness is the caller-owned blessings
// FRandomStream (docs/01 section 6); this file consumes it in a fixed order
// (group shuffle first, then one candidate shuffle per offered group).
//
// FIXME-CONTRACT: the expedition module assignment names the blessing id
// families "blessing_sword_* / blessing_cast_* / blessing_prot_* /
// blessing_elem_*", while the shipped catalog (Abilities/QinglanBlessingLibrary,
// authored because blueprint section 5 lists the 12 names as a genuine
// content gap) uses ids of the form "b_*" inside the same four groups. The
// draft is id-agnostic (group-driven), so renaming the catalog ids to the
// assignment's families requires no change here.
#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"

namespace QinglanBlessingDrafting
{
	/** Offers per draft: exactly 3 when eligible supply allows (docs/03 section 5). */
	inline constexpr int32 OfferSize = 3;
	/** Blessings per group (12 total = 4 groups x 3; docs/03 section 5). */
	inline constexpr int32 BlessingsPerGroup = 3;

	/**
	 * True when the group is eligible: no owned blessing belongs to it
	 * ("one-stack-cap per group" — blueprint section 2.6 "one-stack-cap never
	 * re-offered" applied at group granularity).
	 */
	bool IsGroupEligible(FName GroupId, const TArray<FName>& OwnedBlessingIds);

	/**
	 * Rolls one offer set from the run's blessings stream.
	 * Consumes the stream deterministically: one Fisher-Yates shuffle of the
	 * eligible groups, then one Fisher-Yates shuffle of each offered group's
	 * candidates. OutOffer holds between 0 and OfferSize ids (fewer only when
	 * the expedition already exhausted groups); returns !OutOffer.IsEmpty().
	 */
	bool RollOffer(FRandomStream& BlessingStream, const TArray<FName>& OwnedBlessingIds, TArray<FName>& OutOffer);
} // namespace QinglanBlessingDrafting
