// Read-only export of a real save for checkpoint/restart acceptance evidence.
#pragma once

#include "Commandlets/Commandlet.h"
#include "QinglanSaveInspect.generated.h"

UCLASS()
class UQinglanSaveInspectCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanSaveInspectCommandlet();
	virtual int32 Main(const FString& Params) override;
};
