#pragma once
#include "CoreMinimal.h"
class UWorld;
class UMaterialInterface;
class AStaticMeshActor;
namespace QinglanEnvironmentArt
{
    using FMaterialResolver = TFunctionRef<UMaterialInterface*(const TCHAR*)>;
    AStaticMeshActor* AddHall(UWorld* World, const FVector& Origin, FMaterialResolver Material);
    void Dress(UWorld* World, FMaterialResolver Material, bool bLowQuality);
}
