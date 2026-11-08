#include "QinglanEnvironmentArt.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Core/QinglanRoomLayout.h"

namespace QinglanEnvironmentArt
{
namespace
{
UStaticMesh* Mesh(const TCHAR* Name)
{
    return LoadObject<UStaticMesh>(nullptr, *FString::Printf(
        TEXT("/Game/Qinglan/Environments/Sanctuary/%s.%s"), Name, Name));
}
void Bind(UStaticMeshComponent* Component, FMaterialResolver Material)
{
    const TArray<FStaticMaterial>& Slots = Component->GetStaticMesh()->GetStaticMaterials();
    for (int32 Index = 0; Index < Slots.Num(); ++Index)
    {
        const FString Slot = Slots[Index].ImportedMaterialSlotName.ToString();
        const TCHAR* Surface = Slot.StartsWith(TEXT("RoofTile")) ? TEXT("M_RoofTiles")
            : Slot.StartsWith(TEXT("Wood")) ? TEXT("M_DarkWoodPlanks")
            : Slot.StartsWith(TEXT("Leaf")) ? TEXT("M_SanctuaryLeaf")
            : Slot.StartsWith(TEXT("Bamboo")) ? TEXT("M_BambooVeneer")
            : Slot.StartsWith(TEXT("Bronze")) ? TEXT("M_SanctuaryBronze")
            : Slot.StartsWith(TEXT("Glow")) ? TEXT("M_SanctuaryGlow") : TEXT("M_Cliff");
        if (UMaterialInterface* Bound = Material(Surface)) { Component->SetMaterial(Index, Bound); }
    }
}
UHierarchicalInstancedStaticMeshComponent* Group(UWorld* World, const TCHAR* Name,
    const TCHAR* Label, FMaterialResolver Material, bool bFar = false)
{
    UStaticMesh* Asset = Mesh(Name);
    if (!Asset) { UE_LOG(LogTemp, Error, TEXT("Sanctuary mesh missing: %s"), Name); return nullptr; }
    AActor* Owner = World->SpawnActor<AActor>();
    Owner->SetActorLabel(Label);
    auto* Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner);
    Owner->AddInstanceComponent(Component);
    Owner->SetRootComponent(Component);
    Component->SetStaticMesh(Asset);
    Component->SetMobility(EComponentMobility::Static);
    Component->SetCollisionProfileName(TEXT("NoCollision"));
    Component->SetCanEverAffectNavigation(false);
    Component->SetGenerateOverlapEvents(false);
    Component->SetCastShadow(!bFar);
    Component->InstanceStartCullDistance = bFar ? 55000 : 14000;
    Component->InstanceEndCullDistance = bFar ? 65000 : 20000;
    Bind(Component, Material);
    if (bFar)
    {
        if (auto* Distant = Material(TEXT("M_SanctuaryDistantRock")))
        {
            for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot) { Component->SetMaterial(Slot, Distant); }
        }
    }
    Component->RegisterComponent();
    return Component;
}
void Instance(UHierarchicalInstancedStaticMeshComponent* Group, const FVector& Location,
    const FVector& Scale, float Yaw)
{
    if (Group) { Group->AddInstance(FTransform(FRotator(0, Yaw, 0), Location, Scale)); }
}
}
AStaticMeshActor* AddHall(UWorld* World, const FVector& Origin, FMaterialResolver Material)
{
    UStaticMesh* Roof = Mesh(TEXT("SM_SanctuaryRoof"));
    UStaticMesh* Frame = Mesh(TEXT("SM_SanctuaryFrame"));
    if (!Roof || !Frame) { return nullptr; }
    AStaticMeshActor* Result = nullptr;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        auto* Actor = World->SpawnActor<AStaticMeshActor>();
        Actor->SetActorLabel(Index ? TEXT("Hub_Hall_CraftedFrame") : TEXT("Hub_Hall_SweptRoof"));
        auto* Component = Actor->GetStaticMeshComponent();
        Component->SetMobility(EComponentMobility::Static);
        Component->SetStaticMesh(Index ? Frame : Roof);
        Component->SetCollisionProfileName(TEXT("NoCollision"));
        Component->SetCanEverAffectNavigation(false);
        Bind(Component, Material);
        Actor->SetActorLocation(Origin);
        Actor->SetActorScale3D(FVector(2, 2, 1));
        if (!Index) { Result = Actor; }
    }
    return Result;
}
void Dress(UWorld* World, FMaterialResolver Material, bool bLowQuality)
{
    const FString Name = World->GetName();
    const bool Hub = Name.Contains(TEXT("Hub"));
    const bool Bamboo = Name.Contains(TEXT("Bamboo"));
    const bool Ashen = Name.Contains(TEXT("Ashen"));
    const bool Arena = Name.Contains(TEXT("Arena"));
    auto* Far = Group(World, TEXT("SM_CliffRock"), TEXT("DistantMountainSilhouettes"), Material, true);
    const float WorldCenter = Bamboo || Ashen ? 16500.f : 0.f;
    FRandomStream Random(818);
    const int32 Peaks = bLowQuality ? 14 : 28;
    for (int32 Index = 0; Index < Peaks; ++Index)
    {
        const float Angle = Index * 2.f * PI / Peaks;
        const float Radius = (Bamboo || Ashen ? 45000.f : 26000.f)
            + Random.FRandRange(-1500.f, 2500.f);
        Instance(Far, FVector(WorldCenter + FMath::Cos(Angle)*Radius,
            FMath::Sin(Angle)*Radius, -4700.f), FVector(Random.FRandRange(12, 20),
            Random.FRandRange(10, 16), Random.FRandRange(10, 16)), Random.FRandRange(0, 360));
    }
    auto* Rocks = Group(World, TEXT("SM_ScholarRock"), TEXT("AuthoredRockBorders"), Material);
    auto* Plants = Hub || Bamboo ? Group(World, TEXT("SM_BambooCluster"), TEXT("LayeredBambooGroves"), Material) : nullptr;
    auto* Lamps = Hub || Arena ? Group(World, TEXT("SM_StoneLantern"), TEXT("SanctuaryLanterns"), Material) : nullptr;
    if (Hub)
    {
        // Side pavilions frame the hall approach and give the large courtyard
        // a readable architectural scale without occupying interaction stations.
        auto* PavilionRoofs = Group(World, TEXT("SM_SanctuaryRoof"), TEXT("CourtyardPavilionRoofs"), Material);
        auto* PavilionFrames = Group(World, TEXT("SM_SanctuaryFrame"), TEXT("CourtyardPavilionFrames"), Material);
        for (float X : {-4300.f, 4300.f})
        {
            const FVector Position(X, -3900.f, 600.f);
            Instance(PavilionRoofs, Position, FVector(.55f), 0.f);
            Instance(PavilionFrames, Position, FVector(.55f), 0.f);
        }
        // Garden islands bring authored vegetation into the normal camera's
        // view while keeping the shrine, training yard and central routes open.
        for (float X : {-4300.f, 4300.f})
        {
            for (float Y : {-1200.f, 900.f, 3000.f})
            {
                Instance(Plants, FVector(X, Y, 600), FVector(.65f, .65f, .75f), Y * .03f);
                Instance(Plants, FVector(X + 320.f, Y + 240.f, 600), FVector(.5f), X * .02f);
                Instance(Rocks, FVector(X - 160.f, Y - 150.f, 550), FVector(.45f, .5f, .35f), Y * .02f);
            }
        }
        // Only dress the outside of the authored 120 m courtyard; anchors and
        // its cross-shaped approach routes keep their original clearances.
        for (int32 Side = 0; Side < 4; ++Side)
        {
            for (int32 Index = 0; Index < (bLowQuality ? 6 : 12); ++Index)
            {
                const float Along = -5700.f + Index * (bLowQuality ? 2100.f : 1050.f);
                const FVector Position = Side < 2 ? FVector(Along, Side ? 6300 : -6300, -650)
                    : FVector(Side == 2 ? -6300 : 6300, Along, -650);
                Instance(Rocks, Position, FVector(2.8, 2.6, 2.4), Index * 31.f);
                if (Index % 2 == 0) { Instance(Plants, Position + FVector(0,0,1250), FVector(1.1), Index*47.f); }
            }
        }
        for (float X : {-4200.f, 4200.f})
        {
            for (float Y : {-4200.f, -1200.f, 1800.f, 4500.f})
            {
                Instance(Lamps, FVector(X,Y,600), FVector(.75), 0);
            }
        }
    }
    else if (Bamboo || Ashen)
    {
        for (int32 Room = 0; Room < QinglanRoomLayout::RoomCount; ++Room)
        {
            const float X = QinglanRoomLayout::RoomCenter(Room).X;
            const int32 Count = bLowQuality ? 8 : 16;
            for (int32 Index = 0; Index < Count; ++Index)
            {
                // Two parallel borders leave combat space and corridor mouths open.
                const float Side = Index % 2 ? 1.f : -1.f;
                const float Along = -2100.f + (Index / 2) * (bLowQuality ? 1400.f : 600.f);
                const FVector Position(X+Along, Side * Random.FRandRange(2250.f,2550.f), -80);
                Instance(Rocks, Position + FVector(Random.FRandRange(-120.f,120.f),0,-120.f),
                    FVector(Random.FRandRange(.7f,1.15f), Random.FRandRange(.7f,1.2f),
                        Random.FRandRange(.35f,.65f)), Random.FRandRange(0,360));
                // Put foliage on the room-facing side of low, irregular rocks.
                // Tall uniform rock borders previously hid the entire grove.
                Instance(Plants, Position + FVector(0,-Side*180.f,80),
                    FVector(Random.FRandRange(.9f,1.25f)), Random.FRandRange(0,360));
            }
        }
    }
    else if (Arena)
    {
        for (int32 Index = 0; Index < 12; ++Index)
        {
            const float Angle = Index * 2.f * PI / 12.f;
            // South remains clear for the decision bridge.
            if (FMath::Sin(Angle) < -.7f) { continue; }
            Instance(Lamps, FVector(FMath::Cos(Angle)*3200,FMath::Sin(Angle)*3200,0), FVector(.85), 0);
            Instance(Rocks, FVector(FMath::Cos(Angle)*3900,FMath::Sin(Angle)*3900,-250), FVector(1.5), Angle*180.f/PI);
        }
    }
}
}
