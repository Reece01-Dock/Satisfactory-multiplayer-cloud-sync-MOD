#include "UI/SharedWorldWorldCard.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

namespace
{
	const FLinearColor RowSelectedFill(0.12f, 0.085f, 0.05f, 0.88f);

	/** UButton centres its content by default; rows need it stretched edge to edge. */
	void FillButtonContent(UButton* B)
	{
		if (UButtonSlot* S = Cast<UButtonSlot>(B->GetContentSlot()))
		{
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetVerticalAlignment(VAlign_Fill);
			S->SetPadding(FMargin(0.f));
		}
	}

	void Ellipsize(UTextBlock* T)
	{
		T->SetAutoWrapText(false);
		T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	}
	/** Shared status badge: tone icon (shape) + label (colour). */
	UHorizontalBox* MakeStatus(UWidgetTree* Tree, const FSharedWorldBrowserItem& Item, int32 FontSize)
	{
		return MakeStatusBadge(Tree, Item.Tone, Item.StatusLabel, FontSize);
	}

	/** The "..." button face: three small dots, no font glyph needed. */
	UHorizontalBox* MakeDots(UWidgetTree* Tree)
	{
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		for (int32 i = 0; i < 3; ++i)
		{
			H->AddChildToHorizontalBox(MakeStatusDot(Tree, TextPrimary, 4.f))->SetPadding(FMargin(2.f, 0.f));
		}
		return H;
	}
}

UWidget* USharedWorldWorldCard::BuildThumbnail(UWidgetTree* Tree, const FString& InWorldId, const FString& Name, UTexture2D* Thumbnail, int32 LetterSize)
{
	if (Thumbnail)
	{
		UScaleBox* Scale = Tree->ConstructWidget<UScaleBox>();
		Scale->SetStretch(EStretch::ScaleToFill);
		Scale->SetClipping(EWidgetClipping::ClipToBounds);
		UImage* Img = Tree->ConstructWidget<UImage>();
		Img->SetBrushFromTexture(Thumbnail, true);
		Scale->AddChild(Img);
		return Scale;
	}
	// No screenshot available: stable per-world tint with the initial, so the slot is never blank.
	const uint8 Hue = static_cast<uint8>(GetTypeHash(InWorldId) % 256);
	UBorder* Tile = Tree->ConstructWidget<UBorder>();
	Tile->SetBrush(RoundedBrush(FLinearColor::MakeFromHSV8(Hue, 110, 95), 3.f));
	Tile->SetHorizontalAlignment(HAlign_Center);
	Tile->SetVerticalAlignment(VAlign_Center);
	UTextBlock* Letter = MakeText(Tree, LetterSize, FLinearColor(1.f, 1.f, 1.f, 0.85f), true);
	Letter->SetAutoWrapText(false);
	Letter->SetText(FText::FromString(Name.IsEmpty() ? TEXT("?") : Name.Left(1).ToUpper()));
	Tile->SetContent(Letter);
	return Tile;
}

void USharedWorldWorldCard::Setup(const FSharedWorldBrowserItem& Item, USharedWorldBrowserWidget* Owner, bool bSelected,
	ESharedWorldCardLayout Layout, UTexture2D* Thumbnail, bool bShowMore)
{
	if (!WidgetTree || WidgetTree->RootWidget) return;
	WorldId = Item.Id();
	OwnerBrowser = Owner;
	bIsSelected = bSelected;
	const bool bRow = Layout == ESharedWorldCardLayout::Row;
	const FString Name = Item.DisplayName();

	RootBorder = WidgetTree->ConstructWidget<UBorder>();
	RootBorder->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = RootBorder;

	// ---- select area (thumbnail + text + status). Transparent; hover/focus tint only.
	UButton* Select = WidgetTree->ConstructWidget<UButton>();
	StyleSolidButton(Select, Clear, FLinearColor(1.f, 1.f, 1.f, 0.06f), FLinearColor(1.f, 1.f, 1.f, 0.10f), Clear, FMargin(0.f), 4.f);
	Select->OnClicked.AddDynamic(this, &USharedWorldWorldCard::OnSelect);

	// ---- primary action
	TObjectPtr<UTextBlock> PrimaryLabel;
	const bool bGreen = bSelected && Item.bActionEnabled;
	UButton* Primary = MakeRoleButton(WidgetTree, bGreen ? ESharedWorldButtonRole::Game : ESharedWorldButtonRole::Secondary, PrimaryLabel, Item.ActionLabel, 17);
	Primary->SetIsEnabled(Item.bActionEnabled);
	Primary->OnClicked.AddDynamic(this, &USharedWorldWorldCard::OnPrimary);

	// ---- more
	UButton* More = WidgetTree->ConstructWidget<UButton>();
	StyleSolidButton(More, GreyBtn, GreyBtnHover, GreyBtnPressed, GreyBtn, FMargin(0.f));
	More->SetContent(MakeDots(WidgetTree));
	More->OnClicked.AddDynamic(this, &USharedWorldWorldCard::OnMore);

	USizeBox* MoreBox = WidgetTree->ConstructWidget<USizeBox>();
	MoreBox->SetWidthOverride(46.f);
	MoreBox->SetHeightOverride(bRow ? 44.f : 40.f);
	MoreBox->AddChild(More);

	if (bRow)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		RootBorder->SetContent(Row);

		UHorizontalBox* Inner = WidgetTree->ConstructWidget<UHorizontalBox>();
		Select->AddChild(Inner);
		FillButtonContent(Select);
		Row->AddChildToHorizontalBox(Select)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		// thumbnail 16:9 with an inset
		USizeBox* ThumbBox = WidgetTree->ConstructWidget<USizeBox>();
		ThumbBox->SetWidthOverride(128.f);
		ThumbBox->SetHeightOverride(72.f);
		ThumbBox->SetClipping(EWidgetClipping::ClipToBounds);
		UOverlay* ThumbOverlay = WidgetTree->ConstructWidget<UOverlay>();
		ThumbBox->AddChild(ThumbOverlay);
		if (UOverlaySlot* S = ThumbOverlay->AddChildToOverlay(BuildThumbnail(WidgetTree, Item.Id(), Name, Thumbnail, 34)))
		{
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetVerticalAlignment(VAlign_Fill);
		}
		if (Item.bMostRecent)
		{
			UBorder* Tag = MakePanel(WidgetTree, Accent, Clear, FMargin(6.f, 1.f), 2.f, 0.f);
			UTextBlock* TagText = MakeText(WidgetTree, 10, TextOnAccent, true);
			TagText->SetAutoWrapText(false);
			TagText->SetText(NSLOCTEXT("SharedWorld", "RecentTag", "LAST PLAYED"));
			Tag->SetContent(TagText);
			if (UOverlaySlot* S = ThumbOverlay->AddChildToOverlay(Tag))
			{
				S->SetHorizontalAlignment(HAlign_Left);
				S->SetVerticalAlignment(VAlign_Top);
				S->SetPadding(FMargin(4.f));
			}
		}
		Inner->AddChildToHorizontalBox(ThumbBox)->SetPadding(FMargin(6.f, 6.f, 14.f, 6.f));

		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* NameText = MakeText(WidgetTree, 19, TextPrimary, true);
		NameText->SetText(FText::FromString(Name));
		Ellipsize(NameText);
		Text->AddChildToVerticalBox(NameText);
		UTextBlock* Sub = MakeText(WidgetTree, 14, TextMuted, false);
		Sub->SetText(FText::FromString(Item.Subtitle));
		Ellipsize(Sub);
		Text->AddChildToVerticalBox(Sub)->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
		if (UHorizontalBoxSlot* S = Inner->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}

		USizeBox* StatusBox = WidgetTree->ConstructWidget<USizeBox>();
		StatusBox->SetMinDesiredWidth(150.f);
		StatusBox->AddChild(MakeStatus(WidgetTree, Item, 14));
		if (UHorizontalBoxSlot* S = Inner->AddChildToHorizontalBox(StatusBox))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(8.f, 0.f, 14.f, 0.f));
		}

		USizeBox* PrimaryBox = WidgetTree->ConstructWidget<USizeBox>();
		PrimaryBox->SetMinDesiredWidth(104.f);
		PrimaryBox->SetHeightOverride(44.f);
		PrimaryBox->AddChild(Primary);
		if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(PrimaryBox))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		}
		if (bShowMore) if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(MoreBox))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f));
		}
	}
	else
	{
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		RootBorder->SetContent(Col);

		UVerticalBox* Inner = WidgetTree->ConstructWidget<UVerticalBox>();
		Select->AddChild(Inner);
		FillButtonContent(Select);
		Col->AddChildToVerticalBox(Select);

		USizeBox* ThumbBox = WidgetTree->ConstructWidget<USizeBox>();
		ThumbBox->SetHeightOverride(150.f);
		ThumbBox->SetClipping(EWidgetClipping::ClipToBounds);
		ThumbBox->AddChild(BuildThumbnail(WidgetTree, Item.Id(), Name, Thumbnail, 54));
		if (UVerticalBoxSlot* S = Inner->AddChildToVerticalBox(ThumbBox))
		{
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetPadding(FMargin(6.f, 6.f, 6.f, 8.f));
		}

		UTextBlock* NameText = MakeText(WidgetTree, 18, TextPrimary, true);
		NameText->SetText(FText::FromString(Name));
		Ellipsize(NameText);
		Inner->AddChildToVerticalBox(NameText)->SetPadding(FMargin(12.f, 0.f, 12.f, 0.f));
		UHorizontalBox* StatusRow = MakeStatus(WidgetTree, Item, 14);
		Inner->AddChildToVerticalBox(StatusRow)->SetPadding(FMargin(12.f, 4.f, 12.f, 0.f));
		UTextBlock* Sub = MakeText(WidgetTree, 13, TextMuted, false);
		Sub->SetText(FText::FromString(Item.Subtitle));
		Ellipsize(Sub);
		Inner->AddChildToVerticalBox(Sub)->SetPadding(FMargin(12.f, 2.f, 12.f, 8.f));

		UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
		USizeBox* PrimaryBox = WidgetTree->ConstructWidget<USizeBox>();
		PrimaryBox->SetHeightOverride(40.f);
		PrimaryBox->AddChild(Primary);
		Actions->AddChildToHorizontalBox(PrimaryBox)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		if (bShowMore) Actions->AddChildToHorizontalBox(MoreBox)->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
		Col->AddChildToVerticalBox(Actions)->SetPadding(FMargin(10.f, 0.f, 10.f, 10.f));
	}

	UpdateChrome();
}

void USharedWorldWorldCard::UpdateChrome()
{
	if (!RootBorder) return;
	FLinearColor Edge = PanelEdge;
	float Width = 1.f;
	if (bIsSelected) { Edge = Accent; Width = 2.f; }
	if (bIsFocused) { Edge = FLinearColor(1.f, 0.82f, 0.55f, 1.f); Width = 2.f; }
	RootBorder->SetBrush(RoundedBrush(bIsSelected ? RowSelectedFill : RowFill, 5.f, Edge, Width));
}

void USharedWorldWorldCard::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	bIsFocused = true;
	UpdateChrome();
}

void USharedWorldWorldCard::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bIsFocused = false;
	UpdateChrome();
}

void USharedWorldWorldCard::OnSelect()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->SelectWorld(WorldId);
}

void USharedWorldWorldCard::OnPrimary()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->PlayWorld(WorldId);
}

void USharedWorldWorldCard::OnMore()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->ToggleMoreMenu(WorldId);
}
