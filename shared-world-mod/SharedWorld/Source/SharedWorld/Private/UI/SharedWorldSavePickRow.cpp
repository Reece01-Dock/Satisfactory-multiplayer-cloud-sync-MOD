#include "UI/SharedWorldSavePickRow.h"

#include "Blueprint/WidgetTree.h"
#include "Components/ButtonSlot.h"
#include "Components/SizeBox.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldUiStyle.h"
#include "UI/SharedWorldWorldCard.h"

using namespace SharedWorldUi;

void USharedWorldSavePickRow::Setup(const FString& InSaveName, const FString& InDisplayName, const FString& InSubtitle, USharedWorldBrowserWidget* Owner, bool bSelected)
{
	if (!WidgetTree || WidgetTree->RootWidget) return;
	SaveName = InSaveName;
	DisplayName = InDisplayName;
	OwnerBrowser = Owner;
	bIsSelected = bSelected;

	RootBorder = WidgetTree->ConstructWidget<UBorder>();
	RootBorder->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = RootBorder;

	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	StyleSolidButton(Button, Clear, FLinearColor(1.f, 1.f, 1.f, 0.06f), FLinearColor(1.f, 1.f, 1.f, 0.10f), Clear, FMargin(0.f), RadiusM);
	Button->OnClicked.AddDynamic(this, &USharedWorldSavePickRow::OnClicked);
	RootBorder->SetContent(Button);

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Button->AddChild(Row);
	if (UButtonSlot* BS = Cast<UButtonSlot>(Button->GetContentSlot()))
	{
		BS->SetHorizontalAlignment(HAlign_Fill);
		BS->SetVerticalAlignment(VAlign_Fill);
		BS->SetPadding(FMargin(0.f));
	}

	// Same thumbnail treatment as the browser (no save screenshot API: tinted tile with the initial).
	USizeBox* Thumb = WidgetTree->ConstructWidget<USizeBox>();
	Thumb->SetWidthOverride(128.f);
	Thumb->SetHeightOverride(72.f);
	Thumb->SetClipping(EWidgetClipping::ClipToBounds);
	Thumb->AddChild(USharedWorldWorldCard::BuildThumbnail(WidgetTree, SaveName, DisplayName, nullptr, 34));
	Row->AddChildToHorizontalBox(Thumb)->SetPadding(FMargin(6.f, 6.f, 14.f, 6.f));

	UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* Name = MakeText(WidgetTree, 18, TextPrimary, true);
	Name->SetText(FText::FromString(DisplayName));
	Name->SetAutoWrapText(false);
	Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Text->AddChildToVerticalBox(Name);
	UTextBlock* Sub = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Sub->SetText(FText::FromString(InSubtitle));
	Sub->SetAutoWrapText(false);
	Sub->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Text->AddChildToVerticalBox(Sub)->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
	if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Text))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
		S->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f));
	}
	UpdateChrome();
}

void USharedWorldSavePickRow::UpdateChrome()
{
	if (!RootBorder) return;
	FLinearColor Edge = PanelEdge;
	float Width = 1.f;
	if (bIsSelected) { Edge = Accent; Width = 2.f; }
	if (bIsFocused) { Edge = FLinearColor(1.f, 0.82f, 0.55f, 1.f); Width = 2.f; }
	RootBorder->SetBrush(RoundedBrush(bIsSelected ? FLinearColor(0.12f, 0.085f, 0.05f, 0.88f) : RowFill, RadiusM, Edge, Width));
}

void USharedWorldSavePickRow::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	bIsFocused = true;
	UpdateChrome();
}

void USharedWorldSavePickRow::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bIsFocused = false;
	UpdateChrome();
}

void USharedWorldSavePickRow::OnClicked()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get())
	{
		B->OnSavePicked(SaveName, DisplayName);
	}
}
