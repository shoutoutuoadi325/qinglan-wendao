// UI bootstrap commandlet: creates the Widget Blueprints (WBP_*) with their
// static layout trees. Dynamic content (choices, technique icons, journal
// entries) is built at runtime by the C++ widget bases.
//
// Run: UnrealEditor-Cmd QinglanAscension.uproject -run=QinglanUIBootstrap
#pragma once

#include "Commandlets/Commandlet.h"
#include "QinglanUIBootstrap.generated.h"

class UWidgetBlueprint;
class UWidgetTree;

UCLASS()
class UQinglanUIBootstrapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UQinglanUIBootstrapCommandlet();
	virtual int32 Main(const FString& Params) override;

private:
	UWidgetBlueprint* CreateWidgetBP(const FString& Path, UClass* WidgetClass);
};
