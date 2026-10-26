#pragma once

#include "CoreMinimal.h"

class AQinglanCharacter;
class AQinglanInteractable;
class UWorld;
class UQinglanStorySubsystem;
struct FQinglanRegistryScene;

/** Scene documents are gameplay inspections, separate from the 24 collectible lore records. */
namespace QinglanSceneInspections
{
	struct FDocument
	{
		FName Id;
		FName Alias;
		FName Scene;
		FName Anchor;
		TArray<FName> BodyKeys;
	};
	QINGLANASCENSION_API const TArray<FDocument>& Catalog();
	QINGLANASCENSION_API const FQinglanRegistryScene* FindScene(const UQinglanStorySubsystem* Story, FName SceneId);
	QINGLANASCENSION_API FText Body(const UQinglanStorySubsystem* Story, const FDocument& Document);
	QINGLANASCENSION_API bool CanRead(const UQinglanStorySubsystem* Story, const AQinglanCharacter* Player, const FDocument& Document);
	QINGLANASCENSION_API void Spawn(UWorld* World, const TMap<FName, FVector>& AnchorPositions);
	/** True for handled inspection keys, including a locked/refused inspection. */
	QINGLANASCENSION_API bool Execute(AQinglanInteractable* Actor, AQinglanCharacter* Player);
}
