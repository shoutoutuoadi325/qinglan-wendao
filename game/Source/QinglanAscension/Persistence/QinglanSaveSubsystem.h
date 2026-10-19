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
	/** True once a profile was loaded or created in this process session. */
	UFUNCTION(BlueprintPure) bool IsProfileSessionActive() const;
	UFUNCTION(BlueprintCallable) bool HasProfile(int32 Slot) const;
	UFUNCTION(BlueprintCallable) void CreateNewProfile(int32 Slot);
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
	/** Commit-boundary autosave (hub purchases, rank unlocks): saves when a profile commit lands outside an expedition. */
	UFUNCTION() void HandleProfileCommitted();

private:
	EQinglanSavePhase Phase = EQinglanSavePhase::Idle;
};
