// Interactable actor: the anchor-keyed dispatch layer of the playable loop.
//
// Content bootstrap (QinglanContentBootstrap.cpp BootstrapMaps) spawns
// ATargetPoint anchors named by interaction keys ("hub.courtyard",
// "record.hub.dry_bell", "sq.bamboo.old_bell_safe", ...); the spawner
// (Core/QinglanInteractionSpawner) instantiates one AQinglanInteractable per
// gameplay-relevant anchor, and AQinglanCharacter::InputInteract executes the
// nearest one inside the aim cone.
//
// Dispatch follows docs/dev/level-layout.md (anchor intent) and
// docs/dev/implementation-blueprint.md section 1.9 (scenes are data-driven):
// the anchor id prefix selects the behavior, and every subsystem call keeps
// the single authoritative mutation paths (UQinglanStorySubsystem for
// narrative, UQinglanProfileSubsystem for permanent state, UQinglanRunSubsystem
// for expeditions; code-contracts.md).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "QinglanInteractable.generated.h"

class AQinglanCharacter;
class UQinglanStorySubsystem;
class UShapeComponent;

/**
 * One interaction point keyed by its bootstrap anchor id. Owns no rules;
 * ExecuteInteraction routes to the subsystem that owns the behavior.
 */
UCLASS()
class QINGLANASCENSION_API AQinglanInteractable : public AActor
{
	GENERATED_BODY()

public:
	AQinglanInteractable();
	virtual void Tick(float DeltaSeconds) override;

	/** Anchor key this actor dispatches on (e.g. "hub.bamboo_gate", "door.0_1"). */
	UPROPERTY(EditInstanceOnly, Category = "Qinglan|Interaction") FName InteractionId = NAME_None;
	/** Optional string-table key for the interact hint; unauthored keys degrade to logs. */
	UPROPERTY(EditInstanceOnly, Category = "Qinglan|Interaction") FName DisplayNameKey = NAME_None;

	/** Runs the behavior keyed by InteractionId; null-safe in every world. */
	UFUNCTION(BlueprintCallable, Category = "Qinglan|Interaction")
	void ExecuteInteraction(AQinglanCharacter* Player);

	/** Proximity shape (authoring marker; aiming is a camera-cone distance check). */
	UPROPERTY(VisibleAnywhere, Category = "Qinglan|Interaction") UShapeComponent* InteractionSphere = nullptr;
};

/** Shared playable-loop helpers (map travel, seeds, anchor-keyed scene lookup). */
namespace QinglanInteraction
{
	// --- Map package paths (docs/dev/level-layout.md "Maps" table) ---
	inline constexpr const TCHAR* HubMapPath = TEXT("/Game/Qinglan/Maps/L_Hub");
	inline constexpr const TCHAR* BambooMapPath = TEXT("/Game/Qinglan/Maps/L_BambooHollow");
	inline constexpr const TCHAR* AshenMapPath = TEXT("/Game/Qinglan/Maps/L_AshenVerge");
	inline constexpr const TCHAR* ArenaMapPath = TEXT("/Game/Qinglan/Maps/L_TribulationArena");

	/** Saves then requests OpenLevel; false means no travel was requested.
	 * True does not confirm asynchronous map loading succeeded.
	 */
	bool TravelToMap(UObject* WorldContext, const TCHAR* MapPath);

	/**
	 * Deterministic expedition seed from the realm id and the fiction clock
	 * (same fiction moment -> same seed; no FMath::RandRange, code-contracts.md
	 * "Deterministic"). UQinglanRunSubsystem canonicalizes it onto the three
	 * seeded streams (Progression/QinglanDeterminism.h).
	 */
	int32 MakeExpeditionSeed(FName RealmId, const UObject* WorldContext);

	/**
	 * Begins the first authored scene whose imported Location matches the
	 * anchor id (main scenes use the exact id "hub.courtyard" style; side-quest
	 * stages use the "realm/place" form mapped from "sq.realm.place" anchors).
	 * Uncompleted scenes go first; with bAllowRevisit completed ones replay
	 * their brief revisit branch (docs/story/narrative-implementation.md 3.3),
	 * except completed protocol stages, which replay as agreement revisions
	 * with live choices. Player, when given, receives the pending-agreement
	 * status line before such a revision begins.
	 * Returns false when the registry is absent or nothing matched;
	 * prerequisites are validated by UQinglanStorySubsystem::BeginScene itself
	 * (failure is an unmet chain, logged Verbose - never an error).
	 */
	bool BeginSceneAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, bool bAllowRevisit = true, AQinglanCharacter* Player = nullptr);

	// --- Gameplay-proof dispatcher (blueprint section 1.9) ------------------
	// Choice-free scenes commit only through NotifyObjectiveCompleted ("the
	// dispatcher binds gameplay events -> mq_* flags"); these helpers find the
	// scene a gameplay proof should commit.

	/** True when the scene has authored choices (choice scenes commit from the selection itself). */
	bool SceneHasChoices(UQinglanStorySubsystem* Story, FName SceneId);

	/** Chapter id owning a main scene (NAME_None for side stages/unknown). */
	FName ChapterIdOfScene(UQinglanStorySubsystem* Story, FName SceneId);

	/**
	 * First uncompleted choice-free scene at the anchor whose prerequisites
	 * pass: the objective-proof target for an anchor re-press (the player
	 * replayed the script, then performed the interaction again).
	 */
	bool FindObjectiveProofAtAnchor(UQinglanStorySubsystem* Story, FName AnchorId, FName& OutSceneId, FName& OutObjectiveId);

	/** Same scan across chapters (boss-room clears, trial anchors). */
	bool FindObjectiveProofInChapters(UQinglanStorySubsystem* Story, const TArray<FName>& ChapterIds, FName& OutSceneId, FName& OutObjectiveId);

	/**
	 * Begins the earliest uncompleted scene of the authored chapter set whose
	 * prerequisites pass (realm entry contract: entering a realm map starts
	 * that realm's current chapter scene).
	 */
	QINGLANASCENSION_API bool BeginFirstSceneOfChapters(UQinglanStorySubsystem* Story, const TArray<FName>& ChapterIds);

	/** Chapter ids a realm expedition presents (data/story chapter ids). */
	TArray<FName> ChaptersForRealm(FName RealmId);
	/** Chapter ids a map presents when no expedition is open (arena trials, epilogue revisits). */
	TArray<FName> ChaptersForMapName(const FString& MapName);
}
