// Blessing drafting core implementation. Pure and stateless apart from the
// caller-owned FRandomStream; safe to exercise headlessly from automation
// specs (code-contracts.md "Testing hooks": blessing offers must be
// deterministic for a given stream state).
#include "Progression/QinglanBlessingDrafting.h"

#include "Abilities/QinglanBlessingLibrary.h"
#include "Progression/QinglanDeterminism.h"

namespace
{
	/** The four blessing groups in canonical order (docs/03 section 5). */
	void GetGroupIdsInOrder(TArray<FName>& OutGroups)
	{
		OutGroups.Reset(4);
		OutGroups.Add(QinglanBlessingGroups::SwordOffense);
		OutGroups.Add(QinglanBlessingGroups::CastingEconomy);
		OutGroups.Add(QinglanBlessingGroups::Protection);
		OutGroups.Add(QinglanBlessingGroups::ElementalInteractions);
	}
} // namespace

namespace QinglanBlessingDrafting
{

bool IsGroupEligible(FName GroupId, const TArray<FName>& OwnedBlessingIds)
{
	for (const FName& Owned : OwnedBlessingIds)
	{
		FQinglanBlessingDef Def;
		if (UQinglanBlessingLibrary::FindBlessingDef(Owned, Def) && Def.GroupId == GroupId)
		{
			return false; // one-stack cap per group: capped for this expedition
		}
	}
	return true;
}

bool RollOffer(FRandomStream& BlessingStream, const TArray<FName>& OwnedBlessingIds, TArray<FName>& OutOffer)
{
	OutOffer.Reset();

	// 1) Eligible groups: exclude every group an owned blessing belongs to.
	TArray<FName> AllGroups;
	GetGroupIdsInOrder(AllGroups);
	TArray<FName> EligibleGroups;
	EligibleGroups.Reserve(AllGroups.Num());
	for (const FName& Group : AllGroups)
	{
		if (IsGroupEligible(Group, OwnedBlessingIds))
		{
			EligibleGroups.Add(Group);
		}
	}
	if (EligibleGroups.IsEmpty())
	{
		return false; // every group capped: nothing left to offer this expedition
	}

	// 2) Distinct-group selection: shuffle eligible groups, take the first
	//    OfferSize. Fewer than three groups remain -> offer what exists
	//    without inventing duplicates (docs/03 section 5).
	QinglanDeterminism::ShuffleWithStream(EligibleGroups, BlessingStream);
	const int32 GroupsToOffer = FMath::Min(OfferSize, EligibleGroups.Num());

	// 3) One candidate per offered group; owned ids are filtered even though
	//    the group cap already excludes them (belt and braces against a future
	//    per-blessing cap change).
	for (int32 GroupIndex = 0; GroupIndex < GroupsToOffer; ++GroupIndex)
	{
		TArray<FName> Candidates;
		UQinglanBlessingLibrary::GetBlessingsInGroup(EligibleGroups[GroupIndex], Candidates);
		Candidates.RemoveAll([&OwnedBlessingIds](FName Candidate)
		{
			return OwnedBlessingIds.Contains(Candidate); // owned never re-offered
		});
		if (Candidates.IsEmpty())
		{
			continue;
		}
		QinglanDeterminism::ShuffleWithStream(Candidates, BlessingStream);
		OutOffer.Add(Candidates[0]);
	}
	return !OutOffer.IsEmpty();
}

} // namespace QinglanBlessingDrafting
