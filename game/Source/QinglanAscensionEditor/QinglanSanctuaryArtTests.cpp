#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "QinglanHumanAnimationContract.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkeletalMeshAttributes.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCultivatorSavedSkinning,
    "Qinglan.Content.CultivatorSavedBindingAndCPUSkinning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FQinglanCultivatorSavedSkinning::RunTest(const FString& Parameters)
{
    USkeletalMesh* Hero = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Qinglan/Characters/Human/SK_Cultivator.SK_Cultivator"));
    USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
    if (!TestNotNull(TEXT("Saved cultivator loads"), Hero)
        || !TestNotNull(TEXT("Canonical Human loads"), Source)) { return false; }
    const FReferenceSkeleton& Reference = Source->GetRefSkeleton();
    const FReferenceSkeleton& Costume = Hero->GetRefSkeleton();
    const int32 BoneCount = Reference.GetRawBoneNum();
    if (!TestEqual(TEXT("Canonical rig contains the 48 FBX bones and mesh root"), BoneCount, 49)
        || !TestEqual(TEXT("Saved costume retains all reference bones"), Costume.GetRawBoneNum(), BoneCount)
        || !TestEqual(TEXT("Saved costume retains all inverse bind matrices"), Hero->GetRefBasesInvMatrix().Num(), BoneCount)
        || !TestEqual(TEXT("Canonical inverse bind matrices exist"), Source->GetRefBasesInvMatrix().Num(), BoneCount))
    { return false; }
    TestTrue(TEXT("Shared animation skeleton remains canonical"), Hero->GetSkeleton() == Source->GetSkeleton());
    TArray<FMatrix> ReferenceGlobal;
    ReferenceGlobal.SetNum(BoneCount);
    for (int32 Bone = 0; Bone < BoneCount; ++Bone)
    {
        const FString Label = Reference.GetBoneName(Bone).ToString();
        TestEqual(Label + TEXT(" saved name"), Costume.GetBoneName(Bone), Reference.GetBoneName(Bone));
        TestEqual(Label + TEXT(" saved parent"), Costume.GetParentIndex(Bone), Reference.GetParentIndex(Bone));
        TestTrue(Label + TEXT(" saved reference transform"),
            Costume.GetRawRefBonePose()[Bone].Equals(Reference.GetRawRefBonePose()[Bone], 1.e-3f));
        ReferenceGlobal[Bone] = Reference.GetRawRefBonePose()[Bone].ToMatrixWithScale();
        const int32 Parent = Reference.GetParentIndex(Bone);
        if (Parent != INDEX_NONE) { ReferenceGlobal[Bone] *= ReferenceGlobal[Parent]; }
        const FMatrix44f ExpectedInverse(ReferenceGlobal[Bone].Inverse());
        TestTrue(Label + TEXT(" inverse bind matches Human"),
            Hero->GetRefBasesInvMatrix()[Bone].Equals(Source->GetRefBasesInvMatrix()[Bone], 1.e-3f));
        TestTrue(Label + TEXT(" inverse bind matches recomposed reference pose"),
            Hero->GetRefBasesInvMatrix()[Bone].Equals(ExpectedInverse, 1.e-3f));
    }

    // Inspect persisted source geometry, not just the in-memory reference
    // skeleton. An uncommitted rebind can otherwise pass pose comparisons.
    for (int32 LOD = 0; LOD < Hero->GetLODNum(); ++LOD)
    {
        const FMeshDescription* Mesh = Hero->GetMeshDescription(LOD);
        if (!TestNotNull(FString::Printf(TEXT("LOD%d has saved geometry"), LOD), Mesh)) { continue; }
        FSkeletalMeshConstAttributes Attributes(*Mesh);
        if (!TestEqual(TEXT("Saved geometry carries all reference bones"), Attributes.GetNumBones(), BoneCount)) { continue; }
        bool bBonesMatch = true;
        for (int32 Bone = 0; Bone < BoneCount; ++Bone)
        {
            bBonesMatch &= Attributes.GetBoneNames()[FBoneID(Bone)] == Reference.GetBoneName(Bone);
            bBonesMatch &= Attributes.GetBoneParentIndices()[FBoneID(Bone)] == Reference.GetParentIndex(Bone);
            bBonesMatch &= Attributes.GetBonePoses()[FBoneID(Bone)].Equals(Reference.GetRawRefBonePose()[Bone], 1.e-3f);
        }
        TestTrue(TEXT("MeshDescription stores canonical names, parents and poses"), bBonesMatch);
        const auto Positions = Attributes.GetVertexPositions();
        const auto Weights = Attributes.GetVertexSkinWeights();
        if (!TestTrue(TEXT("Saved skin weight attribute exists"), Weights.IsValid())) { continue; }
        int32 InvalidVertices = 0;
        int32 MixedVertices = 0;
        TestTrue(TEXT("Costume source geometry is nonempty"), Mesh->Vertices().Num() > 0);
        for (FVertexID Vertex : Mesh->Vertices().GetElementIDs())
        {
            const auto Influences = Weights.Get(Vertex);
            float Total = 0.f;
            bool bValid = !Positions[Vertex].ContainsNaN() && Influences.Num() > 0;
            for (const auto Influence : Influences)
            {
                bValid &= Influence.GetBoneIndex() < BoneCount && FMath::IsFinite(Influence.GetWeight())
                    && Influence.GetWeight() > 0.f;
                Total += Influence.GetWeight();
            }
            bValid &= FMath::IsNearlyEqual(Total, 1.f, 0.001f);
            InvalidVertices += bValid ? 0 : 1;
            MixedVertices += Influences.Num() > 1 ? 1 : 0;
        }
        TestEqual(TEXT("Every saved vertex has finite coordinates and normalized valid weights"), InvalidVertices, 0);
        TestTrue(TEXT("Flexible clothing retains blended weights"), MixedVertices > 100);
    }

    FSkeletalMeshRenderData* Render = Hero->GetResourceForRendering();
    if (!TestNotNull(TEXT("Saved costume has built rendering geometry"), Render)
        || !TestTrue(TEXT("Built LOD0 exists"), Render->LODRenderData.Num() > 0)) { return false; }
    const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
    const FSkinWeightVertexBuffer* RenderWeights = LOD.GetSkinWeightVertexBuffer();
    if (!TestNotNull(TEXT("Built geometry retains its skin weights"), RenderWeights)
        || !TestTrue(TEXT("Editor skin weights remain readable for CPU verification"), RenderWeights->GetNeedsCPUAccess())
        || !TestNotNull(TEXT("Editor position data remains readable for CPU verification"), LOD.StaticVertexBuffers.PositionVertexBuffer.GetVertexData())
        || !TestEqual(TEXT("Built weights cover every rendering vertex"), RenderWeights->GetNumVertices(), LOD.GetNumVertices())
        || !TestTrue(TEXT("Rendering vertices are nonempty"), LOD.GetNumVertices() > 0)) { return false; }
    for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
    {
        if (!TestTrue(TEXT("Saved section vertex range is valid"), Section.BaseVertexIndex + Section.NumVertices <= LOD.GetNumVertices()))
        { return false; }
        for (const auto Bone : Section.BoneMap)
        {
            if (!TestTrue(TEXT("Built section bone maps target existing reference bones"), Bone < BoneCount)) { return false; }
        }
    }
    TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
    Component->SetSkeletalMeshAsset(Hero);
    TArray<FMatrix44f> RefToLocal;
    RefToLocal.SetNum(BoneCount);
    TArray<FMatrix> CostumeGlobal;
    CostumeGlobal.SetNum(BoneCount);
    for (int32 Bone = 0; Bone < BoneCount; ++Bone)
    {
        CostumeGlobal[Bone] = Costume.GetRawRefBonePose()[Bone].ToMatrixWithScale();
        const int32 Parent = Costume.GetParentIndex(Bone);
        if (Parent != INDEX_NONE) { CostumeGlobal[Bone] *= CostumeGlobal[Parent]; }
        RefToLocal[Bone] = Hero->GetRefBasesInvMatrix()[Bone] * FMatrix44f(CostumeGlobal[Bone]);
    }
    TArray<FVector3f> RefPositions;
    USkinnedMeshComponent::ComputeSkinnedPositions(Component.Get(), RefPositions, RefToLocal, LOD, *RenderWeights);
    float MaxReferenceError = 0.f;
    int32 InvalidReferenceVertices = 0;
    FBox ReferenceBounds(ForceInit);
    if (!TestEqual(TEXT("CPU reference skinning covers every built vertex"), RefPositions.Num(), static_cast<int32>(LOD.GetNumVertices())))
    { return false; }
    for (int32 Vertex = 0; Vertex < RefPositions.Num(); ++Vertex)
    {
        InvalidReferenceVertices += RefPositions[Vertex].ContainsNaN() ? 1 : 0;
        ReferenceBounds += FVector(RefPositions[Vertex]);
        MaxReferenceError = FMath::Max(MaxReferenceError,
            FVector3f::Distance(RefPositions[Vertex], LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex)));
    }
    TestEqual(TEXT("Reference skinning produces finite built geometry"), InvalidReferenceVertices, 0);
    TestTrue(TEXT("Reference skinning preserves a visible three-dimensional character"),
        ReferenceBounds.GetExtent().GetMax() > 0.1f);
    TestTrue(TEXT("Canonical reference pose preserves built geometry without stale inverse binds"), MaxReferenceError < 0.02f);
    AddInfo(FString::Printf(TEXT("Reference CPU skinning maximum vertex error: %.6f cm"), MaxReferenceError));

    // Evaluate read-only original animation tracks at five distinct frames,
    // then use Unreal's CPU skinning on the actual built vertex/weight data.
    // This catches empty sections, collapsed clothing and a visually static
    // costume even when its animation class and skeleton pointers are valid.
    for (const QinglanHumanAnimation::FSourceTake& Take : QinglanHumanAnimation::RequiredTakes)
    {
        const UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *QinglanHumanAnimation::ObjectPath(Take.Suffix));
        FString Reason;
        const bool bValidTake = QinglanHumanAnimation::Validate(Sequence, Source->GetSkeleton(), Take, Reason);
        if (!TestTrue(FString::Printf(TEXT("%s retains its original animation contract: %s"), Take.Suffix, *Reason),
            bValidTake)) { continue; }
        const IAnimationDataModel* Model = Sequence->GetDataModel();
        const int32 LastFrame = Model->GetNumberOfFrames();
        // The original importer restores the playback endpoint when an FBX
        // exporter stops just before the last boundary. Sample inside that
        // range so absent endpoint keys cannot return synthetic identity.
        const int32 LastSampleFrame = FMath::Max(0, LastFrame - 1);
        TArray<FName> TrackNames;
        Model->GetBoneTrackNames(TrackNames);
        TArray<FVector3f> FirstPositions;
        float MaxMotion = 0.f;
        float MaxClothMotion = 0.f;
        int32 InvalidAnimatedVertices = 0;
        for (int32 Sample = 0; Sample < 5; ++Sample)
        {
            const FFrameNumber Frame((LastSampleFrame * Sample) / 4);
            TArray<FMatrix> AnimatedGlobal;
            AnimatedGlobal.SetNum(BoneCount);
            for (int32 Bone = 0; Bone < BoneCount; ++Bone)
            {
                const FName Name = Reference.GetBoneName(Bone);
                const FTransform Local = TrackNames.Contains(Name) ? Model->GetBoneTrackTransform(Name, Frame)
                    : Reference.GetRawRefBonePose()[Bone];
                AnimatedGlobal[Bone] = Local.ToMatrixWithScale();
                const int32 Parent = Reference.GetParentIndex(Bone);
                if (Parent != INDEX_NONE) { AnimatedGlobal[Bone] *= AnimatedGlobal[Parent]; }
                RefToLocal[Bone] = Hero->GetRefBasesInvMatrix()[Bone] * FMatrix44f(AnimatedGlobal[Bone]);
            }
            TArray<FVector3f> AnimatedPositions;
            USkinnedMeshComponent::ComputeSkinnedPositions(Component.Get(), AnimatedPositions, RefToLocal, LOD, *RenderWeights);
            if (!TestEqual(TEXT("CPU animation skinning covers every built vertex"), AnimatedPositions.Num(), RefPositions.Num())) { break; }
            if (Sample == 0) { FirstPositions = AnimatedPositions; }
            FBox AnimatedBounds(ForceInit);
            for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
            {
                const bool bCloth = Hero->GetMaterials().IsValidIndex(Section.MaterialIndex)
                    && Hero->GetMaterials()[Section.MaterialIndex].ImportedMaterialSlotName.ToString().EndsWith(TEXT("Cloth"));
                for (uint32 Offset = 0; Offset < Section.NumVertices; ++Offset)
                {
                    const int32 Vertex = Section.BaseVertexIndex + Offset;
                    const FVector3f Position = AnimatedPositions[Vertex];
                    const bool bFinite = !Position.ContainsNaN() && Position.SizeSquared() < 10000.f * 10000.f;
                    InvalidAnimatedVertices += bFinite ? 0 : 1;
                    AnimatedBounds += FVector(Position);
                    const float Motion = FVector3f::Distance(Position, FirstPositions[Vertex]);
                    MaxMotion = FMath::Max(MaxMotion, Motion);
                    if (bCloth) { MaxClothMotion = FMath::Max(MaxClothMotion, Motion); }
                }
            }
            TestTrue(FString::Printf(TEXT("%s sample %d retains a visible character volume"), Take.Suffix, Sample),
                AnimatedBounds.GetExtent().GetMax() > ReferenceBounds.GetExtent().GetMax() * 0.05f);
        }
        TestEqual(FString::Printf(TEXT("%s never collapses or produces nonfinite animated geometry"), Take.Suffix), InvalidAnimatedVertices, 0);
        TestTrue(FString::Printf(TEXT("%s deforms the saved character geometry"), Take.Suffix), MaxMotion > 0.01f);
        TestTrue(FString::Printf(TEXT("%s animates the authored cloth sections"), Take.Suffix), MaxClothMotion > 0.01f);
        TestEqual(TEXT("Read-only sampling preserves the original frame count"), Model->GetNumberOfFrames(), LastFrame);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanSanctuaryArtContract,
    "Qinglan.Content.SanctuaryArtContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanSanctuaryArtContract::RunTest(const FString& Parameters)
{
    USkeletalMesh* Hero = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Qinglan/Characters/Human/SK_Cultivator.SK_Cultivator"));
    USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr,
        TEXT("/Game/Qinglan/Characters/Human/SK_Human.SK_Human"));
    if (TestNotNull(TEXT("Cultivator game asset loads"), Hero)
        && TestNotNull(TEXT("Original animation source mesh loads"), Source))
    {
        TestTrue(TEXT("Costume shares the shipped animation skeleton"), Hero->GetSkeleton() == Source->GetSkeleton());
        TestEqual(TEXT("Costume has separate skin, cloth, jade, metal, leather and hair surfaces"), Hero->GetMaterials().Num(), 8);
        for (const FSkeletalMaterial& Slot : Hero->GetMaterials())
        {
            TestNotNull(TEXT("Every costume surface is saved"), Slot.MaterialInterface.Get());
            if (Slot.MaterialInterface)
            {
                TestTrue(TEXT("Costume retains its authored material rather than the blockout palette"),
                    Slot.MaterialInterface->GetPathName().Contains(TEXT("MIC_Cultivator_")));
            }
        }
        const UAnimBlueprintGeneratedClass* Animation = Cast<UAnimBlueprintGeneratedClass>(
            LoadClass<UAnimInstance>(nullptr, TEXT("/Game/Qinglan/Characters/Human/ABP_Human.ABP_Human_C")));
        if (TestNotNull(TEXT("Player has a compiled animation graph"), Animation))
        {
            TestTrue(TEXT("Player graph can animate the new costume"), Animation->GetTargetSkeleton() == Hero->GetSkeleton());
        }
    }
    for (const TCHAR* Name : {TEXT("SM_SanctuaryRoof"), TEXT("SM_SanctuaryFrame"),
        TEXT("SM_ScholarRock"), TEXT("SM_CliffRock"), TEXT("SM_BambooCluster"), TEXT("SM_StoneLantern")})
    {
        const FString Path = FString::Printf(TEXT("/Game/Qinglan/Environments/Sanctuary/%s.%s"), Name, Name);
        UStaticMesh* Asset = LoadObject<UStaticMesh>(nullptr, *Path);
        if (!TestNotNull(Path, Asset)) { continue; }
        TestTrue(TEXT("Asset authors ordinary LODs for Metal and DX12"), Asset->GetNumSourceModels() >= 2);
        TestTrue(TEXT("Imported geometry has finite centimetre-scale bounds"),
            !Asset->GetBounds().Origin.ContainsNaN() && Asset->GetBounds().SphereRadius > 20.f
            && Asset->GetBounds().SphereRadius < 3000.f);
    }
    for (const TCHAR* Name : {TEXT("L_Hub"), TEXT("L_BambooHollow"), TEXT("L_AshenVerge"), TEXT("L_TribulationArena")})
    {
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FString(TEXT("/Game/Qinglan/Maps/")) + Name);
        if (!TestNotNull(Name, World)) { continue; }
        TArray<FBox> PlayableFloors;
        for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
        {
            const FString Label = It->GetName();
            const bool bFloor = Label.StartsWith(TEXT("Hub_Courtyard_Floor"))
                || ((Label.StartsWith(TEXT("Bamboo_Room")) || Label.StartsWith(TEXT("Ashen_Room")))
                    && Label.Contains(TEXT("_Floor"))) || Label.StartsWith(TEXT("Arena_Platform"));
            if (!bFloor) { continue; }
            FBox Floor = It->GetStaticMeshComponent()->Bounds.GetBox();
            Floor.Max.Z += 180.f; // standing player volume above the saved floor
            PlayableFloors.Add(Floor);
        }
        TestTrue(TEXT("Saved walking floors exist for skyline clearance inspection"), !PlayableFloors.IsEmpty());
        int32 ObstructingMountains = 0;
        int32 Instances = 0;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            TArray<UHierarchicalInstancedStaticMeshComponent*> Groups;
            It->GetComponents(Groups);
            for (auto* Group : Groups)
            {
                if (!Group->GetStaticMesh() || !Group->GetStaticMesh()->GetPathName().Contains(TEXT("/Sanctuary/"))) { continue; }
                Instances += Group->GetInstanceCount();
                if (Group->GetStaticMesh()->GetName() == TEXT("SM_CliffRock"))
                {
                    for (int32 Index = 0; Index < Group->GetInstanceCount(); ++Index)
                    {
                        FTransform Transform;
                        Group->GetInstanceTransform(Index, Transform, true);
                        const FBox Mountain = Group->GetStaticMesh()->GetBoundingBox().TransformBy(Transform);
                        for (const FBox& Floor : PlayableFloors)
                        {
                            if (Mountain.Intersect(Floor)) { ++ObstructingMountains; break; }
                        }
                    }
                }
                TestTrue(TEXT("Decorative art cannot block player or interaction traces"),
                    Group->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
                TestFalse(TEXT("Decoration does not alter navigation"), Group->CanEverAffectNavigation());
                for (int32 Slot = 0; Slot < Group->GetNumMaterials(); ++Slot)
                {
                    TestNotNull(TEXT("Every art material slot has a bound surface"), Group->GetMaterial(Slot));
                }
            }
        }
        TestTrue(TEXT("Saved map contains visible authored art instances"), Instances >= 20);
        TestEqual(FString::Printf(TEXT("%s distant mountains clear playable floors and standing players"), Name), ObstructingMountains, 0);
    }
    return true;
}
#endif
