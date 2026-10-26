#pragma once

#include "CoreMinimal.h"

class AQinglanCharacter;
class AActor;

/** Presentation only: never grants objective proof or advances a room. */
struct FQinglanObjectiveGuide
{
	FName SceneId = NAME_None;
	FName HintKey = NAME_None;
	AActor* Target = nullptr;
};

namespace QinglanObjectiveGuide
{
	QINGLANASCENSION_API FQinglanObjectiveGuide Resolve(AQinglanCharacter* Player);
}
