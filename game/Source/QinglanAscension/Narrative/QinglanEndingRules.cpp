// Ending gating rules implementation. See QinglanEndingRules.h for sources.
#include "Narrative/QinglanEndingRules.h"

#include "Internationalization/Text.h"

namespace
{
	/** Single game string table (code-contracts.md "Localization keys"). */
	FText TableText(const TCHAR* Key)
	{
		return FText::FromStringTable(FName(TEXT("/Game/Qinglan/Data/ST_Game.ST_Game")), Key);
	}

	/** Requirement copy authored per missing agreement (CH06.json). */
	const TCHAR* AgreementRequirementKey(FName Flag)
	{
		const FString Name = Flag.ToString();
		if (Name == TEXT("sq_spillway_verified"))
		{
			return TEXT("story.ch06.s04.spillway_missing");
		}
		if (Name == TEXT("sq_allocation_agreed"))
		{
			return TEXT("story.ch06.s04.allocation_missing");
		}
		if (Name == TEXT("sq_handover_accepted"))
		{
			return TEXT("story.ch06.s04.handover_missing");
		}
		return nullptr;
	}
} // namespace

namespace QinglanEnding
{
	bool IsKnownEndingId(FName EndingId)
	{
		return EndingIdToEnum(EndingId) != EQinglanEnding::None;
	}

	EQinglanEnding EndingIdToEnum(FName EndingId)
	{
		// data/story/main-late.json CH06_S04 payloads use END_OPEN/END_SEAL/
		// END_SHARED; the subsystem contract (QinglanStorySubsystem.h) uses the
		// lowercase commit-flag spellings. Both resolve to the same enum.
		const FString Name = EndingId.ToString();
		if (Name == EndingOpenId || Name == TEXT("END_OPEN"))
		{
			return EQinglanEnding::Open;
		}
		if (Name == EndingSealId || Name == TEXT("END_SEAL"))
		{
			return EQinglanEnding::Seal;
		}
		if (Name == EndingSharedId || Name == TEXT("END_SHARED"))
		{
			return EQinglanEnding::Shared;
		}
		return EQinglanEnding::None;
	}

	FName EndingEnumToId(EQinglanEnding Ending)
	{
		switch (Ending)
		{
		case EQinglanEnding::Open:   return FName(EndingOpenId);
		case EQinglanEnding::Seal:   return FName(EndingSealId);
		case EQinglanEnding::Shared: return FName(EndingSharedId);
		default:                     return NAME_None;
		}
	}

	FName EndingCommitFlag(EQinglanEnding Ending)
	{
		// Exactly one flag per commit; the id and the flag are the same stable
		// string (data/story/main-late.json mutually_exclusive_flags).
		return EndingEnumToId(Ending);
	}

	bool EvaluateAvailability(EQinglanEnding Ending, const TFunctionRef<bool(FName)>& HasFlag, TArray<FName>& OutMissing)
	{
		OutMissing.Reset();
		if (Ending == EQinglanEnding::None)
		{
			return false;
		}

		// All endings require the release to have completed first
		// (docs/dev/implementation-blueprint.md section 2.3).
		if (!HasFlag(FName(ShenReleasedFlag)))
		{
			OutMissing.Add(FName(ShenReleasedFlag));
		}

		if (Ending == EQinglanEnding::Shared)
		{
			// ending_shared requires all three effective agreements; pending
			// proposals never satisfy it (docs/story/narrative-implementation.md
			// section 4; SQ02/SQ03 proposal_state.ending_reads_effective_not_pending).
			for (const TCHAR* FlagText : SharedAgreementFlags)
			{
				const FName Flag(FlagText);
				if (!HasFlag(Flag))
				{
					OutMissing.Add(Flag);
				}
			}
		}

		return OutMissing.Num() == 0;
	}

	FText RequirementTextForFlag(FName Flag)
	{
		if (const TCHAR* Key = AgreementRequirementKey(Flag))
		{
			return TableText(Key);
		}
		if (Flag == FName(ShenReleasedFlag))
		{
			// "Discharge the service bindings and release Shen Heng."
			return TableText(TEXT("story.ch06.s03.objective"));
		}
		// Generic fallback: the ending-review objective line.
		return TableText(TEXT("story.ch06.s04.objective"));
	}

	FText RequirementText(EQinglanEnding Ending, const TArray<FName>& MissingFlags)
	{
		if (MissingFlags.Num() == 0)
		{
			// Nothing missing: hand back the ending's own preview copy
			// (docs/story/narrative-implementation.md section 5: "any ongoing
			// obligation is shown explicitly in the preview").
			switch (Ending)
			{
			case EQinglanEnding::Open:   return TableText(TEXT("story.ch06.s04.open_preview"));
			case EQinglanEnding::Seal:   return TableText(TEXT("story.ch06.s04.seal_preview"));
			case EQinglanEnding::Shared: return TableText(TEXT("story.ch06.s04.shared_preview"));
			default:                     return TableText(TEXT("story.ch06.s04.objective"));
			}
		}

		TArray<FText> Lines;
		Lines.Reserve(MissingFlags.Num());
		for (const FName Flag : MissingFlags)
		{
			Lines.Add(RequirementTextForFlag(Flag));
		}
		return FText::Join(FText::FromString(TEXT("\n")), Lines);
	}
} // namespace QinglanEnding
