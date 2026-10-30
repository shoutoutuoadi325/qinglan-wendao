#include "UI/QinglanHUDLayout.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"

namespace QinglanHUDLayout
{
	void Apply(UWidgetTree* Tree)
	{
		if (!Tree) { return; }
		auto Place = [Tree](FName Name, float MinX, float MaxX, float Top, float Height,
			float Left = 5.f, float Right = 5.f, int32 FontSize = 0)
		{
			UWidget* Widget = Tree->FindWidget(Name);
			UCanvasPanelSlot* Slot = Widget ? Cast<UCanvasPanelSlot>(Widget->Slot) : nullptr;
			if (!Slot) { return; }
			// Horizontal stretch separates sections as the viewport width changes.
			// Vertical offsets follow the viewport bottom rather than a 1080p canvas.
			Slot->SetAnchors(FAnchors(MinX, 1.f, MaxX, 1.f));
			Slot->SetAlignment(FVector2D::ZeroVector);
			Slot->SetAutoSize(false);
			Slot->SetOffsets(FMargin(Left, Top, Right, Height));
			if (UTextBlock* Text = Cast<UTextBlock>(Widget); Text && FontSize > 0)
			{
				FSlateFontInfo Font = Text->GetFont();
				Font.Size = FontSize;
				Text->SetFont(Font);
				Text->SetRenderScale(FVector2D(1.f, 1.f));
				Text->SetAutoWrapText(true);
			}
		};

		Place(TEXT("Bar_Health"), 0.f, 0.25f, -100.f, 18.f, 40.f, 20.f);
		Place(TEXT("Bar_Energy"), 0.f, 0.25f, -74.f, 12.f, 40.f, 20.f);
		Place(TEXT("Bar_Stamina"), 0.f, 0.25f, -56.f, 10.f, 40.f, 20.f);
		Place(TEXT("Txt_Barrier"), 0.f, 0.25f, -140.f, 30.f, 40.f, 20.f, 16);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const float MinX = 0.28f + static_cast<float>(Index) * 0.12f;
			Place(*FString::Printf(TEXT("Txt_Skill%d"), Index), MinX, MinX + 0.11f,
				-160.f, 124.f, 5.f, 5.f, 18);
		}
		Place(TEXT("Txt_Talisman0"), 0.80f, 0.88f, -160.f, 124.f, 0.f, 5.f, 16);
		Place(TEXT("Txt_Talisman1"), 0.90f, 1.f, -160.f, 124.f, 0.f, 40.f, 16);
		Place(TEXT("Txt_Bark"), 0.25f, 0.75f, -290.f, 100.f, 0.f, 0.f, 18);
	}
}
