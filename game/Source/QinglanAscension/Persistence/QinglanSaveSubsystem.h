// Save subsystem: versioned saves, durable writes, backup rotation, migration.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "QinglanSaveSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanSaveCompleted, FName, ProfileId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanSaveFailed, FName, ProfileId, FText, Reason);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanProfileChanged, FName, ProfileId);

enum class EQinglanSavePhase : uint8
{
	Idle, Snapshotting, Serializing, Verifying, Replacing, Done, Failed
};

/**
 * Implements the durable write protocol:
 * snapshot+validate -> temp file -> flush/reopen/parse/verify -> preserve backup ->
 * atomic replace -> game-thread report. A failed write never deletes the last
 * valid primary. Keeps 3 profile slots x 2 validated backups.
 */
UCLASS()
class QINGLANASCENSION_API UQinglanSaveSubsystem : public UGameInstanceSubsystem // QINGLANASCENSION_API: linkage-only export for the editor automation specs (checksum roundtrip / corrupt / future-schema rejection); public API unchanged.
{
	GENERATED_BODY()

public:
	static constexpr int32 ProfileSlotCount = 3;
	static constexpr int32 BackupsPerProfile = 2;
	/** v2 adds the narrative courier clock + per-quest delivery entries. */
	static constexpr int32 CurrentSchemaVersion = 2;

#if WITH_DEV_AUTOMATION_TESTS
	/** Test-only file operations whose next invocation can be failed. */
	enum class EQinglanSaveFileOperationForTesting : uint8
	{
		Move,
		Delete,
		/**
		 * Simulates a torn temp write: the next durable write persists only a
		 * prefix and reports success, so the post-write verification (step 3)
		 * must reject it.
		 */
		Write,
		/** Reports a failed durable flush after the payload was written. */
		Flush
	};

	/**
	 * Test-only: redirects all save files to Directory. An empty string
	 * restores ProjectSavedDir/SaveGames. Tests must clear this when done.
	 */
	static void SetSaveDirectoryOverrideForTesting(const FString& Directory);

	/**
	 * Test-only one-shot fault injection: the (SkipCount + 1)-th matching
	 * operation returns failure. SkipCount 0 fails the next matching
	 * operation, 1 lets the next one through and fails the one after it, and
	 * so on. A negative SkipCount disarms. A triggered injection disarms
	 * itself, so recovery/cleanup operations run normally.
	 */
	static void SetFileOperationFailureForTesting(EQinglanSaveFileOperationForTesting Operation, int32 SkipCount);
#endif

	/** Slot 0..2. Loads the primary or, when corrupt, the latest valid backup. */
	UFUNCTION(BlueprintCallable) bool LoadProfile(int32 Slot, FText& OutNotice);
	/** Writes now (safe boundary only); see SaveGame docs for boundary rules. */
	UFUNCTION(BlueprintCallable) bool SaveNow(FText& OutError);
	/**
	 * Save-before-travel form for map-travel callers (portals, exits, gates):
	 * persists the active session's earned state before leaving the map. No
	 * active session: true (nothing earned to persist).
	 */
	UFUNCTION(BlueprintCallable) bool SaveActiveSession(FText& OutError);
	/** Saves the checkpoint before returning to profile selection. No banking or loot penalty. */
	UFUNCTION(BlueprintCallable) bool SuspendActiveSession(FText& OutError);
	/** True once a profile was loaded or created in this process session. */
	UFUNCTION(BlueprintPure) bool IsProfileSessionActive() const;
	UFUNCTION(BlueprintCallable) bool HasProfile(int32 Slot) const;
	UFUNCTION(BlueprintCallable) void CreateNewProfile(int32 Slot);
	/** Creates durably before replacing the active session; failure leaves its profile/run untouched. */
	UFUNCTION(BlueprintCallable) bool TryCreateNewProfile(int32 Slot, FText& OutError);
	/** Removes checkpoint state (used on expedition end / return to hub). */
	UFUNCTION(BlueprintCallable) void ClearCheckpoint();
	/** Run lifecycle hook: re-enable checkpoint writes after a new expedition starts successfully. */
	void BeginExpeditionCheckpoint();
	/** For UI: open the platform save folder through supported desktop APIs. */
	UFUNCTION(BlueprintCallable) bool ShowSaveFolder();

	UPROPERTY(BlueprintAssignable) FQinglanSaveCompleted OnSaveCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanSaveFailed OnSaveFailed;
	/**
	 * Broadcast after a profile load/creation fully replaced the session
	 * state; story/UI subscribers drop stale per-profile caches here.
	 */
	UPROPERTY(BlueprintAssignable) FQinglanProfileChanged OnProfileChanged;

	/** Serializes the checksummed body used by the atomic replace step.
	 * Public so the editor automation specs can roundtrip and corrupt real
	 * containers without duplicating the checksum format in tests. */
	bool SerializeVerified(const class UQinglanSaveGame* Save, TArray<uint8>& OutBytes) const;
	bool DeserializeVerified(const TArray<uint8>& Bytes, UQinglanSaveGame*& OutSave, FText& OutError) const;

protected:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

private:
	/** An explicit replacement snapshot writes a new profile without sampling or mutating the current session. */
	bool WriteProfileSave(FText& OutError, const struct FQinglanProfileSnapshot* ReplacementSnapshot,
		int32 ReplacementSlot, int32* OutSavedSequence = nullptr);
	/** Commit-boundary autosave (hub purchases, rank unlocks): saves when a profile commit lands outside an expedition. */
	UFUNCTION() void HandleProfileCommitted();

private:
	EQinglanSavePhase Phase = EQinglanSavePhase::Idle;
};
