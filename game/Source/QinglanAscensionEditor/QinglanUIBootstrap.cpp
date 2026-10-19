// UI bootstrap implementation: minimal, robust widget trees per screen.
// C++ widget bases subscribe to subsystem events; the trees below provide the
// named building blocks they manipulate at runtime.
#include "QinglanUIBootstrap.h"

#include "AssetRegistry/AssetRegistryModule.h" // FAssetRegistryModule::AssetCreated (IAssetTools::CreatedAsset is gone in UE 5.8)
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h" // complete UWidgetTree for RootWidget/ConstructWidget (UMG/Public/Blueprint)
#include "Kismet2/CompilerResultsLog.h" // FCompilerResultsLog for loud headless compiles
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h" // FFileHelper::LoadFileToArray for the disk verification pass
#include "Misc/PackageName.h"
#include "UI/QinglanUIWidgets.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h" // UE 5.8: UMGEditor/Classes root (no Factories/ subfolder)

namespace
{
	const FString ContentRoot = TEXT("/Game/Qinglan");

	UWidgetBlueprint* MakeWidgetBlueprint(const FString& ObjectPath, UClass* BaseClass)
	{
		// Callers pass the object-path form "/Game/.../WBP_X.WBP_X"; the package
		// name must NOT carry the object suffix.
		const int32 Dot = ObjectPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromStart);
		const FString PackageName = Dot > 0 ? ObjectPath.Left(Dot) : ObjectPath;
		// Force fully-loaded existing packages (asset discovery leaves them
		// half-open and SavePackage refuses those).
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package == nullptr)
		{
			Package = CreatePackage(*PackageName);
		}
		else if (!Package->IsFullyLoaded())
		{
			FlushAsyncLoading();
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		}
		Package->SetPackageFlags(PKG_NewlyCreated);
		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = BaseClass; // UE 5.8: ParentClass replaced InitialWidgetClass
		UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(
			Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), Package,
				*FPackageName::ObjectPathToObjectName(ObjectPath), RF_Public | RF_Standalone, nullptr, GWarn));
		if (!WBP)
		{
			return nullptr;
		}
		// Headless creation can leave the widget tree unset; the tree is the
		// root every ConstructWidget call parents to, so guarantee one exists.
		if (WBP->WidgetTree == nullptr)
		{
			WBP->WidgetTree = NewObject<UWidgetTree>(WBP);
		}
		FAssetRegistryModule::AssetCreated(WBP);
		return WBP;
	}

	UTextBlock* AddText(UWidgetTree* Tree, UPanelWidget* Parent, const FText& Label,
		const FVector2D& Position, const FVector2D& Size, int32 FontSize = 18, FName Name = NAME_None)
	{
		// UE 5.8: UWidget has no SetObjectName; widgets are named at construction.
		UTextBlock* Text = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
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
		const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color, FName Name = NAME_None)
	{
		// UE 5.8: UWidget has no SetObjectName; widgets are named at construction.
		UProgressBar* Bar = Tree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), Name);
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
		// Compile through a FCompilerResultsLog so headless failures are loud:
		// UE 5.8's FKismetEditorUtilities::CompileBlueprint returns void, and a
		// silently failed compile leaves no generated class - the asset would
		// then save as an uncompiled blueprint whose <Name>_C class can never
		// load at runtime (same pattern as CompileAllBlueprintsCommandlet).
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &Results);
		// Post-compile proof: the generated (and skeleton) classes must exist
		// in the package before saving, and the parent must be the requested
		// C++ base (a parent mismatch recompiles against UUserWidget).
		UE_LOG(LogTemp, Log,
			TEXT("UIBootstrap: %s compiled with %d error(s), %d warning(s); parent %s, generated class %s, skeleton class %s"),
			*WBP->GetName(), Results.NumErrors, Results.NumWarnings,
			*GetNameSafe(WBP->ParentClass), *GetNameSafe(WBP->GeneratedClass),
			*GetNameSafe(WBP->SkeletonGeneratedClass));
		if (Results.NumErrors > 0 || !WBP->GeneratedClass || !WBP->SkeletonGeneratedClass)
		{
			return false;
		}

		UPackage* Package = WBP->GetOutermost();
		// MakeWidgetBlueprint already forced a full load (or created a fresh
		// package). Re-running FullyLoad here on a package that now holds the
		// just-compiled blueprint objects re-enters async loading with dirty
		// in-memory exports and fatals in UE 5.8; the load above is the only
		// place that needs it.
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, WBP, *FileName, Args))
		{
			return false;
		}

		// Disk verification (the in-memory checks above cannot prove the
		// save): the package bytes must carry the generated class export name
		// and its UClass type - the objects StaticLoadClass resolves through
		// UQinglanHUDStatics::ResolveWidgetClass at runtime.
		TArray<uint8> PackageBytes;
		if (!FFileHelper::LoadFileToArray(PackageBytes, *FileName))
		{
			return false;
		}
		auto ContainsToken = [&PackageBytes](const char* Token)
		{
			const SIZE_T TokenLen = FPlatformString::Strlen(Token);
			for (int32 Offset = 0; Offset + static_cast<SIZE_T>(TokenLen) <= static_cast<SIZE_T>(PackageBytes.Num()); ++Offset)
			{
				if (FMemory::Memcmp(PackageBytes.GetData() + Offset, Token, TokenLen) == 0)
				{
					return true;
				}
			}
			return false;
		};
		const FTCHARToUTF8 GeneratedNameANSI(*(WBP->GetName() + TEXT("_C")));
		const FTCHARToUTF8 GeneratedTypeANSI(TEXT("WidgetBlueprintGeneratedClass"));
		const bool bClassOnDisk = ContainsToken(GeneratedNameANSI.Get()) && ContainsToken(GeneratedTypeANSI.Get());
		if (bClassOnDisk)
		{
			UE_LOG(LogTemp, Log, TEXT("UIBootstrap: %s saved to %s (%d bytes); generated class present on disk: yes"),
				*WBP->GetName(), *FileName, PackageBytes.Num());
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UIBootstrap: %s saved to %s (%d bytes); generated class present on disk: NO"),
				*WBP->GetName(), *FileName, PackageBytes.Num());
		}
		return bClassOnDisk;
	}
}

UQinglanUIBootstrapCommandlet::UQinglanUIBootstrapCommandlet()
{
}

int32 UQinglanUIBootstrapCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("QinglanUIBootstrap starting"));
	bool bOk = true;

	// --- WBP_HUD: vitals bottom-left, techniques bottom-center, objective top-left.
	if (UWidgetBlueprint* Hud = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_HUD.WBP_HUD"), UQinglanHUDWidget::StaticClass()))
	{
		UWidgetTree* Tree = Hud->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddBar(Tree, Root, FVector2D(40, 980), FVector2D(420, 18), FLinearColor::Red, TEXT("Bar_Health"));
		AddBar(Tree, Root, FVector2D(40, 1006), FVector2D(420, 12), FLinearColor(0.2f, 0.5f, 1.0f), TEXT("Bar_Energy"));
		AddBar(Tree, Root, FVector2D(40, 1024), FVector2D(420, 10), FLinearColor::Green, TEXT("Bar_Stamina"));
		AddText(Tree, Root, FText::FromString(TEXT("Barrier 0")), FVector2D(40, 940), FVector2D(220, 28), 16, TEXT("Txt_Barrier"));
		for (int32 i = 0; i < 4; ++i)
		{
			// Wide enough for a full technique name at 18 px; 150 cm slots and
			// 20 px overflowed into the neighbouring label ("Jade Sword Arc"
			// rendered across slot 2 in the first packaged screenshot).
			AddText(Tree, Root, FText::FromString(FString::Printf(TEXT("[Skill %d]"), i + 1)),
				FVector2D(360 + i * 220, 980), FVector2D(210, 44), 14, *FString::Printf(TEXT("Txt_Skill%d"), i));
		}
		for (int32 i = 0; i < 2; ++i)
		{
			AddText(Tree, Root, FText::FromString(FString::Printf(TEXT("[Item %d]"), i + 1)),
				FVector2D(1340 + i * 120, 980), FVector2D(110, 36), 16, *FString::Printf(TEXT("Txt_Talisman%d"), i));
		}
		AddText(Tree, Root, FText::FromString(TEXT("Objective")), FVector2D(40, 40), FVector2D(700, 60), 20, TEXT("Txt_Objective"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(500, 830), FVector2D(900, 100), 18, TEXT("Txt_Bark"));
		bOk &= SaveWidget(Hud);
	}

	// --- WBP_Dialogue: speaker, body, dynamic choice box (filled in C++).
	if (UWidgetBlueprint* Dlg = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_Dialogue.WBP_Dialogue"), UQinglanDialogueWidget::StaticClass()))
	{
		UWidgetTree* Tree = Dlg->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddText(Tree, Root, FText::FromString(TEXT("Speaker")), FVector2D(320, 700), FVector2D(300, 40), 22, TEXT("Txt_Speaker"));
		AddText(Tree, Root, FText::FromString(TEXT("Line text")), FVector2D(320, 748), FVector2D(1240, 130), 20, TEXT("Txt_Line"));
		AddText(Tree, Root, FText::FromString(TEXT("source")), FVector2D(1320, 700), FVector2D(200, 30), 14, TEXT("Txt_SourceKind"));
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
		AddText(Tree, Root, FText::FromString(TEXT("Journal")), FVector2D(640, 60), FVector2D(640, 60), 32, TEXT("Txt_Title"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(240, 160), FVector2D(1440, 800), 18, TEXT("Txt_Body"));
		bOk &= SaveWidget(Jrn);
	}

	// --- WBP_Ending: preview body plus confirm guard (interactive flow in C++).
	if (UWidgetBlueprint* End = MakeWidgetBlueprint(ContentRoot + TEXT("/UI/WBP_Ending.WBP_Ending"), UQinglanEndingWidget::StaticClass()))
	{
		UWidgetTree* Tree = End->WidgetTree;
		UCanvasPanel* Root = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		Tree->RootWidget = Root;
		AddText(Tree, Root, FText::FromString(TEXT("Ending Preview")), FVector2D(240, 200), FVector2D(1440, 500), 24, TEXT("Txt_Preview"));
		AddText(Tree, Root, FText::FromString(TEXT("")), FVector2D(240, 760), FVector2D(1440, 200), 18, TEXT("Txt_Warning"));
		bOk &= SaveWidget(End);
	}

	UE_LOG(LogTemp, Log, TEXT("QinglanUIBootstrap finished ok=%d"), bOk);
	return bOk ? 0 : 1;
}
