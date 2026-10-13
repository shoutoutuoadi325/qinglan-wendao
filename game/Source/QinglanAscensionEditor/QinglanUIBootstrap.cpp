// UI bootstrap implementation: minimal, robust widget trees per screen.
// C++ widget bases subscribe to subsystem events; the trees below provide the
// named building blocks they manipulate at runtime.
#include "QinglanUIBootstrap.h"

#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Factories/WidgetBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

namespace
{
	const FString ContentRoot = TEXT("/Game/Qinglan");

	UWidgetBlueprint* MakeWidgetBlueprint(const FString& ObjectPath, UClass* BaseClass)
	{
		const FString PackagePath = ObjectPath;
		UPackage* Package = CreatePackage(*PackagePath);
		Package->SetPackageFlags(PKG_NewlyCreated);
		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->InitialWidgetClass = BaseClass;
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(
			Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), Package,
				*FPackageName::ObjectPathToObjectName(ObjectPath), RF_Public | RF_Standalone, nullptr, GWarn));
		if (!WBP)
		{
			return nullptr;
		}
		FAssetToolsModule::GetModule().Get().CreatedAsset(WBP);
		return WBP;
	}

	UTextBlock* AddText(UWidgetTree* Tree, UPanelWidget* Parent, const FText& Label,
		const FVector2D& Position, const FVector2D& Size, int32 FontSize = 18)
	{
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Text->Text = Label;
		// TODO pass font size when the default font asset chain is wired; scale via render transform.
		Text->SetRenderScale(FVector2D(FontSize / 18.f, FontSize / 18.f));
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Parent->AddChild(Text)))
		{
			Slot->SetPosition(Position);
			Slot->SetSize(Size);
		}
		return Text;
	}

	UProgressBar* AddBar(UWidgetTree* Tree, UPanelWidget* Parent,
		const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color)
	{
		UProgressBar* Bar = Tree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
		Bar->SetFillColorAndOpacity(Color);
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Parent->AddChild(Bar)))
		{
			Slot->SetPosition(Position);
			Slot->SetSize(Size);
		}
		return Bar;
	}

	bool SaveWidget(UWidgetBlueprint* WBP)
	{
		FKismetEditorUtilities::CompileBlueprint(WBP);
		UPackage* Package = WBP->GetOutermost();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, WBP, *FileName, Args);
	}
}

UQinglanUIBootstrap::UQinglanUIBootstrap()
{
	LogToClipsAndLog = true;
}

int32 UQinglanUIBootstrap::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("QinglanUIBootstrap starting"));
	bool bOk = true;

	// --- WBP_HUD: vitals bottom-left, techniques bottom-center, objective top-left.
	if (UWidgetBlueprint* Hud = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_HUD.WBP_HUD"), UQinglanHUDWidget::StaticClass()))
	{
		UWidgetTree* Tree = Hud->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddBar(Tree, Root, FVector2D(40, 980), FVector2D(420, 18), FLinearColor::Red)->SetObjectName(TEXT("Bar_Health"));
		AddBar(Tree, Root, FVector2D(40, 1006), FVector2D(420, 12), FLinearColor(0.2f, 0.5f, 1.0f))->SetObjectName(TEXT("Bar_Energy"));
		AddBar(Tree, Root, FVector2D(40, 1024), FVector2D(420, 10), FLinearColor::Green)->SetObjectName(TEXT("Bar_Stamina"));
		AddText(Tree, Root, FText::FromString(TEXT("Barrier 0")), FVector2D(40, 940), FVector2D(220, 28), 16)->SetObjectName(TEXT("Txt_Barrier"));
		for (int32 i = 0; i < 4; ++i)
		{
			AddText(Tree, Root, FText::FromString(FString::Printf(TEXT("[Skill %d]"), i + 1)),
				FVector2D(700 + i * 150, 980), FVector2D(140, 44), 20)->SetObjectName(*FString::Printf(TEXT("Txt_Skill%d"), i));
		}
		for (int32 i = 0; i < 2; ++i)
		{
			AddText(Tree, Root, FText::FromString(FString::Printf(TEXT("[Item %d]"), i + 1)),
				FVector2D(1340 + i * 120, 980), FVector2D(110, 36), 16)->SetObjectName(*FString::Printf(TEXT("Txt_Talisman%d"), i));
		}
		AddText(Tree, Root, FText::FromString(TEXT("Objective")), FVector2D(40, 40), FVector2D(700, 60), 20)->SetObjectName(TEXT("Txt_Objective"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(500, 830), FVector2D(900, 100), 18)->SetObjectName(TEXT("Txt_Bark"));
		bOk &= SaveWidget(Hud);
	}

	// --- WBP_Dialogue: speaker, body, dynamic choice box (filled in C++).
	if (UWidgetBlueprint* Dlg = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_Dialogue.WBP_Dialogue"), UQinglanDialogueWidget::StaticClass()))
	{
		UWidgetTree* Tree = Dlg->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddText(Tree, Root, FText::FromString(TEXT("Speaker")), FVector2D(320, 700), FVector2D(300, 40), 22)->SetObjectName(TEXT("Txt_Speaker"));
		AddText(Tree, Root, FText::FromString(TEXT("Line text")), FVector2D(320, 748), FVector2D(1240, 130), 20)->SetObjectName(TEXT("Txt_Line"));
		AddText(Tree, Root, FText::FromString(TEXT("source")), FVector2D(1320, 700), FVector2D(200, 30), 14)->SetObjectName(TEXT("Txt_SourceKind"));
		UVerticalBox* Choices = Tree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Box_Choices"));
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Root->AddChild(Choices)))
		{
			Slot->SetPosition(FVector2D(320, 890));
			Slot->SetSize(FVector2D(1240, 160));
		}
		bOk &= SaveWidget(Dlg);
	}

	// --- WBP_Journal: single scrolling body (entries composed at runtime).
	if (UWidgetBlueprint* Jrn = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_Journal.WBP_Journal"), UQinglanJournalWidget::StaticClass()))
	{
		UWidgetTree* Tree = Jrn->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddText(Tree, Root, FText::FromString(TEXT("Journal")), FVector2D(640, 60), FVector2D(640, 60), 32)->SetObjectName(TEXT("Txt_Title"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(240, 160), FVector2D(1440, 800), 18)->SetObjectName(TEXT("Txt_Body"));
		bOk &= SaveWidget(Jrn);
	}

	// --- WBP_Ending: preview body plus confirm guard (interactive flow in C++).
	if (UWidgetBlueprint* End = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_Ending.WBP_Ending"), UQinglanEndingWidget::StaticClass()))
	{
		UWidgetTree* Tree = End->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddText(Tree, Root, FText::FromString(TEXT("Ending Preview")), FVector2D(240, 200), FVector2D(1440, 500), 24)->SetObjectName(TEXT("Txt_Preview"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(240, 760), FVector2D(1440, 200), 18)->SetObjectName(TEXT("Txt_Warning"));
		bOk &= SaveWidget(End);
	}

	UE_LOG(LogTemp, Log, TEXT("QinglanUIBootstrap finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}
