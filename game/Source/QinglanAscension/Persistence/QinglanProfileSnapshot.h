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
	/** AUD-37: permanent technique upgrade tiers (1 = base, 2 = upgraded). */
	UPROPERTY() TMap<FName, int32> TechniqueTiers;
	/** Permanent relics bought at the sanctuary (docs/03 section 5); equip at most one. */
	UPROPERTY() TArray<FName> OwnedRelics;
	UPROPERTY() FName EquippedRelic = NAME_None;
	/** Passive manuals studied (docs/03 section 4); equip at most two. */
	UPROPERTY() TArray<FName> OwnedPassives;
	UPROPERTY() TArray<FName> EquippedPassives;
	/** Talisman id -> remaining expedition charges; refilled at the sanctuary (docs/03 section 5). */
	UPROPERTY() TMap<FName, int32> TalismanCharges;
	/** Slot order is permanent profile equipment, including exhausted slots. */
	UPROPERTY() TArray<FName> EquippedTalismans;
	/** Missing in older saves; distinguishes legacy defaults from an intentionally empty loadout. */
	UPROPERTY() bool bTalismanLoadoutInitialized = false;
	UPROPERTY() TArray<FName> CampaignFlags;
	/** Journal of committed transaction ids; idempotency key for grants/banking (§1.5). */
	UPROPERTY() TArray<FName> CommittedTransactionIds;
	UPROPERTY() FQinglanNarrativeState Narrative;
};
