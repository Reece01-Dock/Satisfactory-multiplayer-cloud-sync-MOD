#include "UI/SharedWorldWorldCard.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldWorldCard::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		RootBorder = WidgetTree->ConstructWidget<UBorder>();
		RootBorder->SetBrushColor(RowIdle);
		RootBorder->SetPadding(FMargin(0.f));
		WidgetTree->RootWidget = RootBorder;

		SelectButton = WidgetTree->ConstructWidget<UButton>();
		SelectButton->SetBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		SelectButton->OnClicked.AddDynamic(this, &USharedWorldWorldCard::OnSelect);
		RootBorder->SetContent(SelectButton);

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		SelectButton->AddChild(Row);

		UBorder* Inner = WidgetTree->ConstructWidget<UBorder>();
		Inner->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		Inner->SetPadding(FMargin(16.f, 10.f, 8.f, 10.f));
		Row->AddChildToHorizontalBox(Inner)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Inner->SetContent(Col);
		NameText = MakeText(WidgetTree, 16, TextPrimary, true);
		Col->AddChildToVerticalBox(NameText);
		StatusText = MakeText(WidgetTree, 12, TextMuted, false);
		Col->AddChildToVerticalBox(StatusText)->SetPadding(FMargin(0, 2, 0, 0));

		ChevronText = MakeText(WidgetTree, 16, TextPrimary, true);
		ChevronText->SetText(FText::FromString(TEXT(">")));
		ChevronText->SetVisibility(ESlateVisibility::Collapsed);
		if (UHorizontalBoxSlot* ChevSlot = Row->AddChildToHorizontalBox(ChevronText))
		{
			ChevSlot->SetPadding(FMargin(4, 0, 12, 0));
			ChevSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	return Super::RebuildWidget();
}

void USharedWorldWorldCard::Setup(const FSharedWorldEntryView& View, USharedWorldBrowserWidget* Owner, bool bSelected, bool /*bJoinLabel*/)
{
	WorldId = View.WorldId;
	OwnerBrowser = Owner;
	if (!NameText) return;

	if (RootBorder)
	{
		RootBorder->SetBrushColor(bSelected ? AccentSelected : RowIdle);
	}
	NameText->SetColorAndOpacity(FSlateColor(TextPrimary));
	StatusText->SetColorAndOpacity(FSlateColor(bSelected ? FLinearColor(1.f, 1.f, 1.f, 0.85f) : TextMuted));
	NameText->SetText(FText::FromString(View.WorldName.IsEmpty() ? View.WorldId : View.WorldName));
	StatusText->SetText(FText::FromString(View.FriendlyStatusLine()));
	if (ChevronText)
	{
		ChevronText->SetVisibility(bSelected ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void USharedWorldWorldCard::OnSelect()
{
	if (USharedWorldBrowserWidget* Browser = OwnerBrowser.Get())
	{
		Browser->SelectWorld(WorldId);
	}
}
