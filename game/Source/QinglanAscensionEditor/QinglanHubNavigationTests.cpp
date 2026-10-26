// Inspect the saved Hub collision after content regeneration, rather than
// reproducing the bootstrap's geometry in a transient test world.
#include "Combat/QinglanCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQinglanHubHallNavigation,
	"Qinglan.Content.HubHallNavigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::SmokeFilter)

bool FQinglanHubHallNavigation::RunTest(const FString& Parameters)
{
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(TEXT("/Game/Qinglan/Maps/L_Hub"));
	if (!TestNotNull(TEXT("Real regenerated Hub loads"), World)) { return false; }
	const AQinglanCharacter* PlayerDefaults = GetDefault<AQinglanCharacter>();
	const UCapsuleComponent* PlayerCapsule = PlayerDefaults ? PlayerDefaults->GetCapsuleComponent() : nullptr;
	if (!TestNotNull(TEXT("Production player capsule exists"), PlayerCapsule)) { return false; }
	const FCollisionShape WalkingCapsule = FCollisionShape::MakeCapsule(
		PlayerCapsule->GetScaledCapsuleRadius(), PlayerCapsule->GetScaledCapsuleHalfHeight());
	constexpr float WalkingSurfaceZ = 600.f;
	const float CapsuleCenterZ = WalkingSurfaceZ + PlayerCapsule->GetScaledCapsuleHalfHeight() + 2.f;
	FCollisionQueryParams Query(SCENE_QUERY_STAT(QinglanHubNavigation), false);

	// These are acceptance sampling points across the actual north entrance,
	// hall interior, and Bamboo departure, not generated test geometry. Start
	// below the roof so a ground probe cannot mistake it for a walking floor.
	struct FGroundProbe { const TCHAR* Label; FVector Location; };
	const FGroundProbe GroundProbes[] = {
		{ TEXT("Courtyard outside hall"), FVector(0.f, -2400.f, WalkingSurfaceZ) },
		{ TEXT("North doorway threshold"), FVector(0.f, -2800.f, WalkingSurfaceZ) },
		{ TEXT("Hall inside entrance"), FVector(0.f, -3200.f, WalkingSurfaceZ) },
		{ TEXT("Hall interior"), FVector(0.f, -3800.f, WalkingSurfaceZ) },
		{ TEXT("Bamboo gate approach"), FVector(0.f, -4600.f, WalkingSurfaceZ) },
	};
	for (const FGroundProbe& Probe : GroundProbes)
	{
		FHitResult Ground;
		const bool bHit = World->LineTraceSingleByChannel(Ground,
			Probe.Location + FVector(0.f, 0.f, 300.f), Probe.Location - FVector(0.f, 0.f, 200.f), ECC_Pawn, Query);
		if (TestTrue(*FString::Printf(TEXT("%s has physical ground"), Probe.Label), bHit))
		{
			TestTrue(*FString::Printf(TEXT("%s walking surface is 600cm (actual %.2f)"), Probe.Label, Ground.ImpactPoint.Z),
				FMath::IsNearlyEqual(Ground.ImpactPoint.Z, WalkingSurfaceZ, 1.f));
			TestTrue(*FString::Printf(TEXT("%s is upward-facing walkable ground"), Probe.Label), Ground.ImpactNormal.Z > 0.9f);
		}
	}

	FHitResult DoorHit;
	TestFalse(TEXT("Player-sized capsule crosses north doorway center without a blocking wall"),
		World->SweepSingleByChannel(DoorHit, FVector(0.f, -2400.f, CapsuleCenterZ),
			FVector(0.f, -3200.f, CapsuleCenterZ), FQuat::Identity, ECC_Pawn, WalkingCapsule, Query));
	for (const float SideX : { -1000.f, 1000.f })
	{
		FHitResult WallHit;
		const bool bBlocked = World->SweepSingleByChannel(WallHit, FVector(SideX, -2400.f, CapsuleCenterZ),
			FVector(SideX, -3200.f, CapsuleCenterZ), FQuat::Identity, ECC_Pawn, WalkingCapsule, Query);
		if (TestTrue(*FString::Printf(TEXT("North wall still blocks player at X=%.0f"), SideX), bBlocked))
		{
			TestTrue(TEXT("Side obstruction is the retained hall north wall"),
				WallHit.GetActor() && WallHit.GetActor()->GetName().StartsWith(TEXT("Hub_Hall_Wall_N")));
		}
	}

	// Check the actual authored interaction stations, including gate and hall
	// records: their floor-level anchors must not end up under a raised slab.
	int32 CheckedAnchors = 0;
	bool bFoundGate = false;
	for (TActorIterator<ATargetPoint> It(World); It; ++It)
	{
		const FString Name = It->GetName();
		if (!Name.StartsWith(TEXT("hub.")) && !Name.StartsWith(TEXT("record.hub."))) { continue; }
		++CheckedAnchors;
		bFoundGate |= Name == TEXT("hub.bamboo_gate") || Name.StartsWith(TEXT("hub.bamboo_gate_"));
		const FVector Anchor = It->GetActorLocation();
		FCollisionQueryParams AnchorQuery = Query;
		AnchorQuery.AddIgnoredActor(*It);
		FHitResult Ground;
		const bool bGround = World->LineTraceSingleByChannel(Ground,
			Anchor + FVector(0.f, 0.f, 300.f), Anchor - FVector(0.f, 0.f, 200.f), ECC_Pawn, AnchorQuery);
		if (TestTrue(*FString::Printf(TEXT("%s has walking ground"), *Name), bGround))
		{
			TestTrue(*FString::Printf(TEXT("%s is on its walking surface, not buried (anchor %.2f, ground %.2f)"),
				*Name, Anchor.Z, Ground.ImpactPoint.Z), FMath::IsNearlyEqual(Anchor.Z, Ground.ImpactPoint.Z, 1.f));
		}
		TestFalse(*FString::Printf(TEXT("%s has standing player clearance"), *Name),
			World->OverlapBlockingTestByChannel(Anchor + FVector(0.f, 0.f, PlayerCapsule->GetScaledCapsuleHalfHeight() + 2.f),
				FQuat::Identity, ECC_Pawn, WalkingCapsule, AnchorQuery));
	}
	TestTrue(TEXT("Actual Hub anchors were inspected"), CheckedAnchors > 0);
	TestTrue(TEXT("Actual Bamboo gate anchor was inspected"), bFoundGate);
	return true;
}
#endif
