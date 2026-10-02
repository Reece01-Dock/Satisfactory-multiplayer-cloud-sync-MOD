#include "UI/SharedWorldNoticeOverlay.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldNoticeOverlay::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = RootCanvas;
		// Pass clicks through empty chrome; only the banner is interactive.
		RootCanvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

		BannerSize = WidgetTree->ConstructWidget<USizeBox>();
		BannerSize->SetWidthOverride(980.f);

		// Toast: same rounded panel + tone icon as every other Shared Worlds surface.
		FLinearColor Edge = ToneColor(ESharedWorldTone::Warning);
		Edge.A = 0.75f;
		BannerBorder = WidgetTree->ConstructWidget<UBorder>();
		BannerBorder->SetBrush(RoundedBrush(FLinearColor(0.04f, 0.045f, 0.055f, 0.94f), RadiusL, Edge, 2.f));
		BannerBorder->SetPadding(FMargin(0.f));
		BannerSize->AddChild(BannerBorder);

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		BannerBorder->SetContent(Row);

		if (UHorizontalBoxSlot* IconSlot = Row->AddChildToHorizontalBox(MakeToneIcon(WidgetTree, ESharedWorldTone::Warning, 20.f)))
		{
			IconSlot->SetVerticalAlignment(VAlign_Center);
			IconSlot->SetPadding(FMargin(18.f, 0.f, 0.f, 0.f));
		}

		UVerticalBox* TextCol = WidgetTree->ConstructWidget<UVerticalBox>();
		if (UHorizontalBoxSlot* TextSlot = Row->AddChildToHorizontalBox(TextCol))
		{
			TextSlot->SetHorizontalAlignment(HAlign_Fill);
			TextSlot->SetVerticalAlignment(VAlign_Center);
			TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TextSlot->SetPadding(FMargin(14.f, 14.f, 12.f, 14.f));
		}
		BodyText = MakeText(WidgetTree, 16, TextPrimary, true);
		BodyText->SetJustification(ETextJustify::Left);
		TextCol->AddChildToVerticalBox(BodyText);

		DismissButton = MakeSecondaryButton(WidgetTree, DismissLabel,
			NSLOCTEXT("SharedWorld", "NoticeOk", "OK"), 13);
		DismissButton->OnClicked.AddDynamic(this, &USharedWorldNoticeOverlay::HandleDismissClicked);
		if (UHorizontalBoxSlot* BtnSlot = Row->AddChildToHorizontalBox(DismissButton))
		{
			BtnSlot->SetHorizontalAlignment(HAlign_Right);
			BtnSlot->SetVerticalAlignment(VAlign_Center);
			BtnSlot->SetPadding(FMargin(8.f, 10.f, 14.f, 10.f));
			BtnSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}

		if (UCanvasPanelSlot* BannerSlot = RootCanvas->AddChildToCanvas(BannerSize))
		{
			BannerSlot->SetAnchors(FAnchors(0.5f, 0.f, 0.5f, 0.f));
			BannerSlot->SetAlignment(FVector2D(0.5f, 0.f));
			BannerSlot->SetPosition(FVector2D(0.f, 36.f));
			BannerSlot->SetAutoSize(true);
			BannerSlot->SetZOrder(1);
		}
	}
	return Super::RebuildWidget();
}

void USharedWorldNoticeOverlay::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (AutoHideAt > 0.f)
	{
		AutoHideAt -= InDeltaTime;
		if (AutoHideAt <= 0.f)
		{
			HandleDismissClicked();
		}
	}
}

void USharedWorldNoticeOverlay::ShowNotice(const FText& Body, const FText& ButtonText, float AutoHideSeconds)
{
	if (BodyText) BodyText->SetText(Body);
	if (DismissLabel)
	{
		DismissLabel->SetText(ButtonText.IsEmpty()
			? NSLOCTEXT("SharedWorld", "NoticeOk", "OK")
			: ButtonText);
	}
	AutoHideAt = AutoHideSeconds;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void USharedWorldNoticeOverlay::HideNotice()
{
	AutoHideAt = 0.f;
	SetVisibility(ESlateVisibility::Collapsed);
	RemoveFromParent();
}

void USharedWorldNoticeOverlay::HandleDismissClicked()
{
	OnDismissed.Broadcast();
	HideNotice();
}
