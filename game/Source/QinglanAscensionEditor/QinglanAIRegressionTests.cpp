// Real GC ownership and spawned warning geometry; rendering still needs device QA.
#include "AI/QinglanAIController.h"
#include "AI/QinglanEncounterDirector.h"
#include "AI/QinglanEnemyCatalog.h"
#include "AI/QinglanEnemyCharacter.h"
#include "Components/DecalComponent.h"
#include "Components/BoxComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSphereMask.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UDynamicMesh.h"
#include "VFX/QinglanVFXCueComponent.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
	constexpr EAutomationTestFlags AIRegressionFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FAIRegressionWorld
	{
		TStrongObjectPtr<UGameInstance> Instance;
		UWorld* World = nullptr;
		FAIRegressionWorld()
		{
			if (!GEngine) { return; }
			const FString Name = TEXT("L_AIRegression_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Instance.Reset(NewObject<UGameInstance>(GEngine));
			Instance->InitializeStandalone(FName(*Name), CreatePackage(*(TEXT("/Temp/") + Name)));
			World = Instance->GetWorld();
			if (World)
			{
				World->InitializeActorsForPlay(FURL());
				World->BeginPlay();
				World->SetBegunPlay(true);
			}
		}
		~FAIRegressionWorld()
		{
			if (World) { World->EndPlay(EEndPlayReason::Quit); }
			if (Instance.IsValid()) { Instance->Shutdown(); }
			if (World)
			{
				if (GEngine) { GEngine->DestroyWorldContext(World); }
				World->DestroyWorld(false);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanQueuedEnemyDataSurvivesGC,
	"Qinglan.AI.QueuedEnemyDataSurvivesGC", AIRegressionFlags)
bool FQinglanQueuedEnemyDataSurvivesGC::RunTest(const FString& Parameters)
{
	FAIRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Standalone world"), Fixture.World)) { return false; }
	AQinglanEncounterDirector* Director = Fixture.World->SpawnActor<AQinglanEncounterDirector>();
	if (!TestNotNull(TEXT("Production encounter director"), Director)) { return false; }
	UQinglanEnemyDataAsset* CatalogData = QinglanEnemyCatalog::CreateDataAssetFromSpec(
		QinglanEnemyCatalog::GetAllArchetypes()[0], GetTransientPackage());
	UQinglanEnemyDataAsset* Data = CatalogData
		? static_cast<UQinglanEnemyDataAsset*>(StaticDuplicateObject(CatalogData, GetTransientPackage())) : nullptr;
	if (!TestNotNull(TEXT("Transient runtime archetype"), Data)) { return false; }
	TestFalse(TEXT("The queue fixture is not permanently rooted by the catalog"), Data->IsRooted());
	TWeakObjectPtr<UQinglanEnemyDataAsset> WeakData(Data);
	Director->BeginEncounter({ Data }, 1, false);
	Data = nullptr; // the director's delayed queue is now the only owner
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("Periodic GC cannot collect an archetype waiting for its portal"), WeakData.IsValid());
	AddExpectedMessage(TEXT("BeginEncounter restarted an active encounter"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	Director->BeginEncounter({}, 2, false);
	CollectGarbage(RF_NoFlags);
	TestFalse(TEXT("Replacing the wave releases unused archetypes without a permanent root"), WeakData.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGroundWarningsMatchDamageShapes,
	"Qinglan.AI.GroundWarningsMatchDamageShapes", AIRegressionFlags)
bool FQinglanGroundWarningsMatchDamageShapes::RunTest(const FString& Parameters)
{
	FAIRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Standalone world"), Fixture.World)) { return false; }
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ACharacter* Player = Fixture.World->SpawnActor<ACharacter>(ACharacter::StaticClass(),
		FVector(1000.f, 500.f, 100.f), FRotator::ZeroRotator, Params);
	APlayerController* PlayerController = Fixture.World->SpawnActor<APlayerController>();
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>(
		AQinglanEnemyCharacter::StaticClass(), FVector(0.f, 0.f, 100.f), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("Target character"), Player) || !TestNotNull(TEXT("Player controller"), PlayerController)
		|| !TestNotNull(TEXT("Enemy presenter"), Enemy)) { return false; }
	PlayerController->Possess(Player);
	AQinglanAIController* AI = Cast<AQinglanAIController>(Enemy->GetController());
	if (!AI)
	{
		AI = Fixture.World->SpawnActor<AQinglanAIController>();
		if (AI) { AI->Possess(Enemy); }
	}
	if (!TestNotNull(TEXT("Production enemy controller"), AI) || !TestNotNull(TEXT("Production VFX component"), Enemy->VFXCues.Get())) { return false; }

	const auto Prepare = [&](FName EnemyId, FName PatternId) -> const FQinglanBossPattern*
	{
		Enemy->VFXCues->ResetForPool();
		AI->ResetForPool();
		const FQinglanArchetypeSpec* Spec = QinglanEnemyCatalog::FindSpec(EnemyId);
		if (!Spec) { return nullptr; }
		UQinglanEnemyDataAsset* CatalogData = QinglanEnemyCatalog::CreateDataAssetFromSpec(*Spec, GetTransientPackage());
		UQinglanEnemyDataAsset* Data = CatalogData
			? static_cast<UQinglanEnemyDataAsset*>(StaticDuplicateObject(CatalogData, GetTransientPackage())) : nullptr;
		if (!Data) { return nullptr; }
		Data->Mesh.Reset();
		Data->IdleAnimation.Reset();
		Data->AttackMontage.Reset();
		Data->HitReactMontage.Reset();
		Enemy->InitializeFromData(Data);
		AI->Initialize(nullptr, 73);
		AI->Tick(0.2f); // actual radius/LOS sensing records the player
		const FQinglanBossPattern* Pattern = Spec->Patterns.FindByPredicate(
			[PatternId](const FQinglanBossPattern& Candidate) { return Candidate.PatternId == PatternId; });
		AI->SetCurrentBossPattern(Pattern);
		return Pattern;
	};
	const auto CheckRings = [&](const TArray<FVector>& Centers, float Radius)
	{
		TArray<UDecalComponent*> Decals;
		Enemy->GetComponents<UDecalComponent>(Decals);
		TestEqual(TEXT("One warning ring per actual damage region"), Decals.Num(), Centers.Num());
		for (const FVector& Center : Centers)
		{
			const bool bMatching = Decals.ContainsByPredicate([&](const UDecalComponent* Decal)
			{
				return Decal && Decal->GetComponentLocation().Equals(Center, 0.1)
					&& FMath::IsNearlyEqual(Decal->DecalSize.Y, double(Radius), 0.1)
					&& FMath::IsNearlyEqual(Decal->DecalSize.Z, double(Radius), 0.1);
			});
			TestTrue(TEXT("Visible ring matches frozen center and damage radius"), bMatching);
		}
	};

	const FQinglanBossPattern* Roots = Prepare(FName(TEXT("bamboo_guardian")), QinglanPatternIds::DelayedRoots());
	if (!TestNotNull(TEXT("Authored roots pattern"), Roots) || !TestTrue(TEXT("Actual sensing acquired the player"), AI->HasTarget())) { return false; }
	AI->NotifyWindupBegan(Roots->TelegraphDuration, Roots);
	const FVector FrozenRootCenter = AI->GetGroundMarkerCenter();
	Player->SetActorLocation(Player->GetActorLocation() + FVector(500.f, 0.f, 0.f));
	Enemy->SetActorLocationAndRotation(FVector(300.f, -200.f, 100.f), FRotator(0.f, 65.f, 0.f));
	Enemy->OnTelegraph.Broadcast(Enemy, Roots->TelegraphDuration);
	CheckRings({ FrozenRootCenter }, AI->GetSpec()->GroundRingRadius);
	Enemy->SetActorLocation(Enemy->GetActorLocation() + FVector(150.f, 100.f, 0.f));
	CheckRings({ FrozenRootCenter }, AI->GetSpec()->GroundRingRadius);
	TestFalse(TEXT("Root warning is not incorrectly centered on the caster"), FrozenRootCenter.Equals(Enemy->GetActorLocation()));

	const FQinglanBossPattern* Volley = Prepare(FName(TEXT("broken_guardian")), QinglanPatternIds::StormBoltVolley());
	if (!TestNotNull(TEXT("Authored volley pattern"), Volley)) { return false; }
	AI->NotifyWindupBegan(Volley->TelegraphDuration, Volley);
	TestEqual(TEXT("Volley freezes three separate strike centers"), AI->GetPendingStrikeCenters().Num(), 3);
	Enemy->OnTelegraph.Broadcast(Enemy, Volley->TelegraphDuration);
	CheckRings(AI->GetPendingStrikeCenters(), AI->GetSpec()->GroundRingRadius);

	const FQinglanBossPattern* Ring = Prepare(FName(TEXT("ashen_guardian")), QinglanPatternIds::RingShockwave());
	if (!TestNotNull(TEXT("Authored shockwave pattern"), Ring)) { return false; }
	AI->NotifyWindupBegan(Ring->TelegraphDuration, Ring);
	Enemy->OnTelegraph.Broadcast(Enemy, Ring->TelegraphDuration);
	CheckRings({ AI->GetGroundMarkerCenter() }, Ring->Range);
	const float SafeBearing = AI->GetSafeArcCenterDegrees();
	const FVector Direction = FRotator(0.f, SafeBearing, 0.f).Vector();
	TArray<UDynamicMeshComponent*> Sectors;
	Enemy->GetComponents<UDynamicMeshComponent>(Sectors);
	if (!TestEqual(TEXT("Shockwave has one exact safe-sector mesh"), Sectors.Num(), 1)) { return false; }
	UDynamicMeshComponent* Sector = Sectors[0];
	TestTrue(TEXT("Shockwave displays its actual safe bearing"),
		FMath::Abs(FMath::FindDeltaAngleDegrees(Sector->GetComponentRotation().Yaw, double(SafeBearing))) < 0.1);
	Sector->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
	{
		TestTrue(TEXT("The safe sector contains actual triangles"), Mesh.TriangleCount() > 0);
		for (int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
			const FVector A = Mesh.GetVertex(Triangle.A);
			const FVector B = Mesh.GetVertex(Triangle.B);
			const FVector C = Mesh.GetVertex(Triangle.C);
			// Pinned FDiscMeshGenerator uses clockwise XY indices with +Z normals.
			TestTrue(TEXT("Every safe triangle faces the camera above the floor"),
				FVector::CrossProduct(B - A, C - A).Z < 0.0);
		}
		for (int32 VertexId : Mesh.VertexIndicesItr())
		{
			const FVector Local = Mesh.GetVertex(VertexId);
			if (Local.IsNearlyZero()) { continue; }
			TestTrue(TEXT("Safe geometry never extends beyond the warning radius"), Local.Size2D() <= Ring->Range + 0.01);
			const FVector Point = Sector->GetComponentTransform().TransformPosition(Local);
			Player->SetActorLocation(Point);
			// Allow only numeric tolerance at the exact two boundary rays.
			TestTrue(TEXT("Every marked point lies inside the real safe angular gap"), AI->IsTargetOutsideSafeArc(30.001f));
		}
	});
	const float Tangent = FMath::Tan(FMath::DegreesToRadians(30.f));
	// This point was inside the old rectangular marker but outside the
	// mechanic's wedge: standing there must never be advertised as safe.
	const FVector UnsafeLocal(Ring->Range * 0.1f, Ring->Range * Tangent * 0.5f, 0.f);
	Player->SetActorLocation(Sector->GetComponentTransform().TransformPosition(UnsafeLocal));
	TestFalse(TEXT("Old rectangular safe-marker corner is actually dangerous"), AI->IsTargetOutsideSafeArc(30.f));
	Player->SetActorLocation(AI->GetGroundMarkerCenter() + Direction * (Ring->Range * 0.5f));
	TestTrue(TEXT("The shown lane is also exempt from shockwave damage"), AI->IsTargetOutsideSafeArc(30.f));
	Player->SetActorLocation(AI->GetGroundMarkerCenter() - Direction * (Ring->Range * 0.5f));
	TestFalse(TEXT("The opposite side is not accidentally marked safe"), AI->IsTargetOutsideSafeArc(30.f));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanWarningCreationFrame,
	"Qinglan.VFX.WarningsSurviveCreationFrame", AIRegressionFlags)
bool FQinglanWarningCreationFrame::RunTest(const FString& Parameters)
{
	FAIRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Standalone world"), Fixture.World)) { return false; }
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>();
	if (!TestNotNull(TEXT("Warning owner"), Enemy)) { return false; }
	UQinglanVFXCueComponent* Cues = Enemy->VFXCues;
	if (!TestNotNull(TEXT("Real VFX presenter"), Cues)) { return false; }
	Cues->PlayTelegraph(FVector(200.f, 300.f, 50.f), 300.f, 1.f, EQinglanElement::Fire);
	Cues->PlaySafeLane(FVector(200.f, 300.f, 50.f), 300.f, 20.f, 30.f, 1.f, EQinglanElement::Fire);
	TestEqual(TEXT("Both danger silhouettes and the safe sector are present"), Cues->GetActiveCueCount(), 3);
	Cues->TickComponent(2.f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("A hitch before creation cannot erase unseen warnings"), Cues->GetActiveCueCount(), 3);
	Fixture.World->TimeSeconds += 0.9f;
	Cues->TickComponent(0.9f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Warnings retain the full reaction interval"), Cues->GetActiveCueCount(), 3);
	Fixture.World->TimeSeconds += 0.11f;
	Cues->TickComponent(0.11f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Elapsed warning geometry is released normally"), Cues->GetActiveCueCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanGroundWarningFallback,
	"Qinglan.VFX.GroundWarningFallbackUsesSurfaceMaterialAndFloor", AIRegressionFlags)
bool FQinglanGroundWarningFallback::RunTest(const FString& Parameters)
{
	FAIRegressionWorld Fixture;
	if (!TestNotNull(TEXT("Collision world"), Fixture.World)) { return false; }
	AActor* Floor = Fixture.World->SpawnActor<AActor>();
	AQinglanEnemyCharacter* Enemy = Fixture.World->SpawnActor<AQinglanEnemyCharacter>();
	if (!TestNotNull(TEXT("Floor actor"), Floor) || !TestNotNull(TEXT("Warning owner"), Enemy)) { return false; }
	UBoxComponent* FloorBox = NewObject<UBoxComponent>(Floor);
	Floor->SetRootComponent(FloorBox);
	FloorBox->SetBoxExtent(FVector(2000.f, 2000.f, 5.f));
	FloorBox->SetCollisionObjectType(ECC_WorldStatic);
	FloorBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	FloorBox->SetCollisionResponseToAllChannels(ECR_Block);
	FloorBox->RegisterComponent();
	FloorBox->SetWorldLocation(FVector(0.f, 0.f, -5.f));
	const FVector DamageCenter(350.f, -200.f, 96.f);
	UQinglanVFXCueComponent* Cues = Enemy->VFXCues;
	Cues->PlayTelegraph(DamageCenter, 300.f, 0.9f, EQinglanElement::Fire);
	Cues->PlaySafeLane(DamageCenter, 300.f, 45.f, 30.f, 0.9f, EQinglanElement::Fire);
	TArray<UDecalComponent*> Decals;
	TArray<UStaticMeshComponent*> Discs;
	TArray<UDynamicMeshComponent*> Sectors;
	Enemy->GetComponents(Decals);
	Enemy->GetComponents(Discs);
	Enemy->GetComponents(Sectors);
	if (!TestEqual(TEXT("One decal"), Decals.Num(), 1)
		|| !TestEqual(TEXT("One independent mesh danger footprint"), Discs.Num(), 1)
		|| !TestEqual(TEXT("One safe-sector footprint"), Sectors.Num(), 1)) { return false; }
	for (const USceneComponent* Warning : { static_cast<USceneComponent*>(Decals[0]),
		static_cast<USceneComponent*>(Discs[0]), static_cast<USceneComponent*>(Sectors[0]) })
	{
		const FVector Shown = Warning->GetComponentLocation();
		TestTrue(TEXT("Warning preserves the frozen damage X/Y"),
			FMath::IsNearlyEqual(Shown.X, DamageCenter.X) && FMath::IsNearlyEqual(Shown.Y, DamageCenter.Y));
		TestTrue(TEXT("Footprints rest above the floor instead of the capsule center"), Shown.Z > 0.0 && Shown.Z <= 6.1);
	}
	UMaterialInterface* MeshMaterial = Discs[0]->GetMaterial(0);
	UMaterialInterface* DecalMaterial = Decals[0]->GetDecalMaterial();
	if (!TestNotNull(TEXT("Actual fallback material"), MeshMaterial)
		|| !TestNotNull(TEXT("Actual decal material"), DecalMaterial)) { return false; }
	TestTrue(TEXT("Low-quality mesh fallback has a renderable surface shader"), MeshMaterial->GetMaterial()->MaterialDomain == MD_Surface);
	TestTrue(TEXT("Projected warning retains its separate deferred-decal shader"), DecalMaterial->GetMaterial()->MaterialDomain == MD_DeferredDecal);
	TestTrue(TEXT("Ground fill alpha-blends instead of adding white light"), MeshMaterial->GetBlendMode() == BLEND_Translucent);
	TestEqual(TEXT("Ground fill uses its authored material"), MeshMaterial->GetMaterial()->GetFName(), FName(TEXT("M_QinglanGroundMarker")));
	TestFalse(TEXT("Ground fill respects the hero's opaque depth"), MeshMaterial->GetMaterial()->bDisableDepthTest);
	TestTrue(TEXT("Ground projection cannot reach the torso"), Decals[0]->DecalSize.X <= 10.f);
	UMaterialInterface* SafeMaterial = Sectors[0]->GetMaterial(0);
	if (!TestNotNull(TEXT("Safe-sector material"), SafeMaterial)) { return false; }
	float DangerOpacity = 0.f;
	float SafeOpacity = 0.f;
	TestTrue(TEXT("Danger fill exposes actual opacity"), MeshMaterial->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Opacity")), DangerOpacity));
	TestTrue(TEXT("Safe fill exposes actual opacity"), SafeMaterial->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Opacity")), SafeOpacity));
	TestTrue(TEXT("Terrain remains visible through the danger fill"), DangerOpacity > 0.f && DangerOpacity <= 0.35f);
	TestTrue(TEXT("The safe color dominates overlapping danger fill"), SafeOpacity >= 0.5f && SafeOpacity > DangerOpacity);
	TestTrue(TEXT("Safe sector has deterministic overlay ordering"), Sectors[0]->TranslucencySortPriority > Discs[0]->TranslucencySortPriority);
	Cues->ResetForPool();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanCueMaterialGraphs,
	"Qinglan.VFX.AuthoredCueMaterialsHaveConnectedGraphs", AIRegressionFlags)
bool FQinglanCueMaterialGraphs::RunTest(const FString& Parameters)
{
	auto ReachesParameter = [](UMaterialExpression* Root, FName Name)
	{
		TSet<UMaterialExpression*> Visited;
		TArray<UMaterialExpression*> Pending;
		if (Root) { Pending.Add(Root); }
		while (!Pending.IsEmpty())
		{
			UMaterialExpression* Expression = Pending.Pop(EAllowShrinking::No);
			if (Visited.Contains(Expression)) { continue; }
			Visited.Add(Expression);
			if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression);
				Scalar && Scalar->ParameterName == Name) { return true; }
			if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression);
				Vector && Vector->ParameterName == Name) { return true; }
			for (FExpressionInput* Input : Expression->GetInputsView())
			{
				if (Input && Input->Expression) { Pending.Add(Input->Expression); }
			}
		}
		return false;
	};
	for (const TCHAR* Name : { TEXT("M_QinglanAdditive"), TEXT("M_QinglanDecalRing"),
		TEXT("M_QinglanBarrier"), TEXT("M_QinglanGroundMarker") })
	{
		UMaterial* Material = LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("/Game/Qinglan/Materials/%s"), Name));
		if (!TestNotNull(Name, Material)) { return false; }
		const bool bGround = Material->GetFName() == FName(TEXT("M_QinglanGroundMarker"));
		const bool bDecal = Material->GetFName() == FName(TEXT("M_QinglanDecalRing"));
		// Pinned UMaterial::GetShadingModels returns DefaultLit for every
		// deferred decal. Its emissive-only property connections define this cue.
		TestTrue(FString::Printf(TEXT("%s has the domain-compatible shading model"), Name),
			Material->GetShadingModels().HasShadingModel(bDecal ? MSM_DefaultLit : MSM_Unlit));
		TestTrue(TEXT("Each cue retains its intended render domain"),
			Material->MaterialDomain == (bDecal ? MD_DeferredDecal : MD_Surface));
		TestFalse(TEXT("Authored cue respects opaque scene depth"), Material->bDisableDepthTest);
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		TestFalse(TEXT("A saved emissive constant cannot bypass the new graph"), Data->EmissiveColor.UseConstant);
		TestFalse(TEXT("A saved opacity constant cannot bypass the new graph"), Data->Opacity.UseConstant);
		TestTrue(TEXT("Color is connected to the visible graph, not just stored on a MID"),
			ReachesParameter(Data->EmissiveColor.Expression, TEXT("Color")));
		TestTrue(TEXT("Brightness is connected to the visible graph"),
			ReachesParameter(Data->EmissiveColor.Expression, TEXT("Intensity")));
		TestTrue(TEXT("Opacity is connected to its intended coverage control"),
			ReachesParameter(Data->Opacity.Expression, bGround || bDecal ? FName(TEXT("Opacity")) : FName(TEXT("Intensity"))));
		if (bDecal)
		{
			TestNull(TEXT("Ground outline does not replace the terrain base color"), Data->BaseColor.Expression);
			int32 MaskCount = 0;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Cast<UMaterialExpressionSphereMask>(Expression)) { ++MaskCount; }
			}
			TestEqual(TEXT("Projected outline has both outer and inner masks"), MaskCount, 2);
		}
	}
	return true;
}
#endif
