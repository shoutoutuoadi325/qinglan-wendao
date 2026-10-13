// Animation bootstrap: builds combat montages from imported FBX takes.
// Discovers anim sequences under /Game/Qinglan/Characters, cuts combo montages
// with notify windows, and pairs them to techniques/enemies by naming rules.
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanAnimBootstrap
#pragma once

#include "Commandlets/Commandlet.h"
#include "QinglanAnimBootstrap.generated.h"

class UAnimSequence;
class UAnimMontage;

UCLASS()
class UQinglanAnimBootstrapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanAnimBootstrapCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	/** One combo cut montage with begin/end window notifies. */
	UAnimMontage* BuildComboMontage(const FString& OutPath, const FString& MontageName,
		UAnimSequence* Sequence, float WindupFraction, float ActiveFraction);
};
