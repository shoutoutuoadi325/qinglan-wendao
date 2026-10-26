// Stationary practice target. Hits use the normal sword/damage pipeline.
#pragma once

#include "CoreMinimal.h"
#include "Core/QinglanCoreTypes.h"
#include "GameFramework/Actor.h"
#include "QinglanTrainingTarget.generated.h"

class UQinglanCombatComponent;
class UCapsuleComponent;
class UStaticMeshComponent;

UCLASS()
class QINGLANASCENSION_API AQinglanTrainingTarget : public AActor
{
	GENERATED_BODY()

public:
	AQinglanTrainingTarget();
	virtual void PostInitializeComponents() override;

	UQinglanCombatComponent* GetCombat() const { return Combat; }
	int32 GetAcceptedHitCount() const { return AcceptedHitCount; }
	static constexpr float HalfHeight = 90.f;

private:
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Collision;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Body;
	UPROPERTY(VisibleAnywhere) TObjectPtr<UQinglanCombatComponent> Combat;
	int32 AcceptedHitCount = 0;

	UFUNCTION() void HandleAcceptedDamage(const FQinglanDamageEvent& Event);
};
