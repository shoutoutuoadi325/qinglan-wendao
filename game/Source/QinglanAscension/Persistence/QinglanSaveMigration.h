// Save schema migration registry and shared save-invariant validation.
// docs/05-technical-architecture.md §6: "Use migration functions v1_to_v2,
// etc., and never overwrite an unsupported newer save."
// docs/05 §7: validate nonnegative bounded currency, unique technique ids,
// referenced rewards, optional stream states, and rank limits.
#pragma once

#include "CoreMinimal.h"
#include "Persistence/QinglanSaveGame.h"

class UQinglanSaveGame;

/** Outcome of running the migration chain against a parsed save. */
enum class EQinglanMigrationResult : uint8
{
	NotNeeded,            // already at CurrentSchemaVersion
	Migrated,             // one or more registered steps were applied
	RejectedFutureSchema, // SchemaVersion > CurrentSchemaVersion: explicit reject, never silent
	Failed                // invariant failure or a missing migration step
};

/** One in-place migration step from FromVersion to FromVersion + 1. */
using FQinglanSaveMigrationStep = bool (*)(UQinglanSaveGame* Save, FText& OutError);

namespace QinglanSave
{
	/** All player-facing save text resolves from ST_Game only (code-contracts.md). */
	inline FText Loc(const TCHAR* Key)
	{
		return FText::FromStringTable(TEXT("/Game/Qinglan/Data/ST_Game"), Key);
	}

	/** Registers a step so future schema bumps pick it up automatically. */
	bool RegisterMigrationStep(int32 FromVersion, FQinglanSaveMigrationStep Step);

	/**
	 * Advances Save->SchemaVersion to UQinglanSaveSubsystem::CurrentSchemaVersion
	 * by applying registered steps one version at a time. Newer-than-current
	 * schemas are rejected explicitly; migration happens only in memory, the
	 * on-disk bytes are never rewritten during a load.
	 * QINGLANASCENSION_API: linkage-only export for the editor automation specs; public API unchanged.
	 */
	QINGLANASCENSION_API EQinglanMigrationResult MigrateToCurrentSchema(UQinglanSaveGame* Save, FText& OutError);

	/**
	 * Future v2 step, registered up front to fix the v1_to_v2 pattern
	 * (docs/05 §6). Dormant while CurrentSchemaVersion == 1.
	 */
	bool MigrateV1_ToV2(UQinglanSaveGame* Save, FText& OutError);

	/**
	 * Shared invariant validator used by durable-write step 1 (snapshot +
	 * validate) and by every load: nonnegative currency, unique technique ids,
	 * rank 0..6, bounded arrays, valid enums, equipped subset of unlocked.
	 * QINGLANASCENSION_API: linkage-only export for the editor automation specs; public API unchanged.
	 */
	QINGLANASCENSION_API bool ValidateSaveInvariants(const UQinglanSaveGame* Save, FText& OutError);

	/** FNV-1a-64 over bytes (UQinglanSaveGame::BodyChecksum contract). */
	uint64 Fnv1a64(const uint8* Data, int64 Num);
}
