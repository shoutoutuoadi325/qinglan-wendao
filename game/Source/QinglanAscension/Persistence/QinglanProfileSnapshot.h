// Shared profile snapshot exchanged between UQinglanProfileSubsystem and
// UQinglanSaveSubsystem. Mirrors the permanent (non-checkpoint) fields of
// UQinglanSaveGame, i.e. the v1 schema of docs/dev/save.json
// (data/examples/save.json) described in docs/05-technical-architecture.md §6.
//
// Single module-wide definition: the scaffold Progression/QinglanProfileSubsystem.h
// forward-declares this type; Progression includes this header
// (Progression/QinglanProfileSubsystem.cpp). ExportStateForSave has merge
// semantics - it overwrites only the progression-owned fields and passes
// ProfileId/ContentVersion and the story-owned Narrative members through.
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"
#include "Persistence/QinglanSaveGame.h"
#include "QinglanProfileSnapshot.generated.h"

/** Portable snapshot of permanent profile state; no asset pointers, no text, no paths (docs/05 §7). */
USTRUCT()
struct FQinglanProfileSnapshot
{
	GENERATED_BODY()

	/** Content/data revision this progress was played on, e.g. "design-1". */
	UPROPERTY() FName ContentVersion = NAME_None;
	UPROPERTY() FName ProfileId = NAME_None;
	UPROPERTY() EQinglanStage CultivationStage = EQinglanStage::QiRefining;
	/** 0..6; six ranks per stage (docs/dev/implementation-blueprint.md §1.5). */
	UPROPERTY() int32 CultivationRank = 0;
	UPROPERTY() FQinglanInventory Inventory;
	UPROPERTY() TArray<FName> UnlockedTechniques;
	UPROPERTY() TArray<FName> EquippedTechniques;
	UPROPERTY() TArray<FName> CampaignFlags;
	/** Journal of committed transaction ids; idempotency key for grants/banking (§1.5). */
	UPROPERTY() TArray<FName> CommittedTransactionIds;
	UPROPERTY() FQinglanNarrativeState Narrative;
};
