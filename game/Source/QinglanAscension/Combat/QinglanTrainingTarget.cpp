#include "Combat/QinglanTrainingTarget.h"

#include "Combat/QinglanCombatComponent.h"
#include "Combat/QinglanDamageMath.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

AQinglanTrainingTarget::AQinglanTrainingTarget()
{
	PrimaryActorTick.bCanEverTick = false;
	Tags.AddUnique(FName(QinglanCombatTunables::QinglanTrainingTargetTag));
	Collision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("TrainingCollision"));
	SetRootComponent(Collision);
	Collision->InitCapsuleSize(45.f, HalfHeight);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Collision->SetCollisionObjectType(ECC_Pawn); // production sword overlap/sweep object filter
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("TrainingPost"));
	Body->SetupAttachment(Collision);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PostMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (PostMesh.Succeeded()) { Body->SetStaticMesh(PostMesh.Object); }
	Body->SetRelativeScale3D(FVector(0.8f, 0.8f, 1.8f));
	Combat = CreateDefaultSubobject<UQinglanCombatComponent>(TEXT("Combat"));
}

void AQinglanTrainingTarget::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	Combat->OnDamageResolved.AddUniqueDynamic(this, &AQinglanTrainingTarget::HandleAcceptedDamage);
}

void AQinglanTrainingTarget::HandleAcceptedDamage(const FQinglanDamageEvent& Event)
{
	++AcceptedHitCount;
	// This actor owns no AI/death/reward lifecycle. Restore even a lethal hit
	// before the resolved-result notification; subsequent real swings remain
	// possible. Do not clear the damage dedupe ledger or synthesize story proof.
	FQinglanAttributes Restored = Combat->GetAttributes();
	Restored.Health = Restored.MaxHealth;
	Combat->SetDerivedAttributes(Restored); // Heal intentionally refuses a zero-health victim
}
