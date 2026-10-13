// Save subsystem: versioned saves, durable writes, backup rotation, migration.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "QinglanSaveSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FQinglanSaveCompleted, FName, ProfileId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FQinglanSaveFailed, FName, ProfileId, FText, Reason);

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
	static constexpr int32 CurrentSchemaVersion = 1;

	/** Slot 0..2. Loads the primary or, when corrupt, the latest valid backup. */
	UFUNCTION(BlueprintCallable) bool LoadProfile(int32 Slot, FText& OutNotice);
	/** Writes now (safe boundary only); see SaveGame docs for boundary rules. */
	UFUNCTION(BlueprintCallable) bool SaveNow(FText& OutError);
	UFUNCTION(BlueprintCallable) bool HasProfile(int32 Slot) const;
	UFUNCTION(BlueprintCallable) void CreateNewProfile(int32 Slot);
	/** Removes checkpoint state (used on expedition end / return to hub). */
	UFUNCTION(BlueprintCallable) void ClearCheckpoint();
	/** For UI: open the platform save folder through supported desktop APIs. */
	UFUNCTION(BlueprintCallable) bool ShowSaveFolder();

	UPROPERTY(BlueprintAssignable) FQinglanSaveCompleted OnSaveCompleted;
	UPROPERTY(BlueprintAssignable) FQinglanSaveFailed OnSaveFailed;

	/** Serializes the checksummed body used by the atomic replace step. */
	bool SerializeVerified(const class UQinglanSaveGame* Save, TArray<uint8>& OutBytes) const;
	bool DeserializeVerified(const TArray<uint8>& Bytes, UQinglanSaveGame*& OutSave, FText& OutError) const;

private:
	EQinglanSavePhase Phase = EQinglanSavePhase::Idle;
};
