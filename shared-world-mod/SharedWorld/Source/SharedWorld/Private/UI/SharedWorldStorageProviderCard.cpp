#include "UI/SharedWorldStorageProviderCard.h"

#include "Blueprint/WidgetTree.h"
#include "Components/ButtonSlot.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

namespace
{
	constexpr float ActionHeight = 38.f; // same for the Connect button and the Connected pill, so rows line up

	void FillButtonContent(UButton* B)
	{
		if (UButtonSlot* S = Cast<UButtonSlot>(B->GetContentSlot()))
		{
			S->SetHorizontalAlignment(HAlign_Fill);
			S->SetVerticalAlignment(VAlign_Fill);
			S->SetPadding(FMargin(0.f));
		}
	}

	/** [Easy|Advanced] and, if this is the storage in use, [ACTIVE]. Always in this order, always in this spot. */
	UHorizontalBox* BadgeRow(UWidgetTree* Tree, const FSharedWorldStorageProvider& P)
	{
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
		UBorder* Difficulty = P.Difficulty == ESharedWorldStorageDifficulty::Easy
			? MakePill(Tree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "DiffEasy", "Easy"))
			: MakePill(Tree, ESharedWorldTone::Warning, NSLOCTEXT("SharedWorld", "DiffAdvanced", "Advanced"));
		Row->AddChildToHorizontalBox(Difficulty)->SetVerticalAlignment(VAlign_Center);
		if (P.bActive)
		{
			Row->AddChildToHorizontalBox(MakeSolidBadge(Tree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "ActiveBadge", "ACTIVE")))
				->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f));
		}
		return Row;
	}

}

UWidget* USharedWorldStorageProviderCard::BuildIcon(UWidgetTree* Tree, const FSharedWorldStorageProvider& P, float Size)
{
	// A missing/empty icon never leaves a hole: fall back to the first letter on a name-derived colour.
	FLinearColor Color = P.IconColor;
	FString Mono = P.IconMonogram;
	if (Mono.IsEmpty())
	{
		Mono = P.DisplayName.IsEmpty() ? FString(TEXT("?")) : P.DisplayName.Left(1).ToUpper();
		Color = FLinearColor::MakeFromHSV8(static_cast<uint8>(GetTypeHash(P.ProviderId) % 256), 110, 95);
	}
	USizeBox* Box = Tree->ConstructWidget<USizeBox>();
	Box->SetWidthOverride(Size);
	Box->SetHeightOverride(Size);
	UBorder* Tile = Tree->ConstructWidget<UBorder>();
	Tile->SetBrush(RoundedBrush(Color, Size * 0.22f, FLinearColor(1.f, 1.f, 1.f, 0.14f), 1.f));
	Tile->SetHorizontalAlignment(HAlign_Center);
	Tile->SetVerticalAlignment(VAlign_Center);
	UTextBlock* T = MakeText(Tree, FMath::Max(10, static_cast<int32>(Size * (Mono.Len() > 2 ? 0.30f : 0.40f))), TextPrimary, true);
	T->SetAutoWrapText(false);
	T->SetText(FText::FromString(Mono));
	Tile->SetContent(T);
	Box->AddChild(Tile);
	return Box;
}

void USharedWorldStorageProviderCard::Setup(const FSharedWorldStorageProvider& P, USharedWorldBrowserWidget* Owner, bool bSelected, ESharedWorldCardLayout Layout)
{
	if (!WidgetTree || WidgetTree->RootWidget) return;
	ProviderId = P.ProviderId;
	OwnerBrowser = Owner;
	bIsSelected = bSelected;
	const bool bRow = Layout == ESharedWorldCardLayout::Row;

	RootBorder = WidgetTree->ConstructWidget<UBorder>();
	RootBorder->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = RootBorder;

	UButton* Select = WidgetTree->ConstructWidget<UButton>();
	StyleSolidButton(Select, Clear, FLinearColor(1.f, 1.f, 1.f, 0.05f), FLinearColor(1.f, 1.f, 1.f, 0.09f), Clear, FMargin(0.f), RadiusM);
	Select->OnClicked.AddDynamic(this, &USharedWorldStorageProviderCard::OnSelect);

	// Connect button / Connected pill (built without a lambda-bound delegate so the UFUNCTION binding stays explicit)
	USizeBox* ActionBox = WidgetTree->ConstructWidget<USizeBox>();
	ActionBox->SetHeightOverride(ActionHeight);
	if (P.bConnected)
	{
		UBorder* Pill = MakePill(WidgetTree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "ProvConnected", "Connected"), 14);
		Pill->SetHorizontalAlignment(HAlign_Center);
		Pill->SetVerticalAlignment(VAlign_Center);
		ActionBox->AddChild(Pill);
	}
	else
	{
		TObjectPtr<UTextBlock> L;
		UButton* Connect = MakeRoleButton(WidgetTree, bSelected ? ESharedWorldButtonRole::Config : ESharedWorldButtonRole::Secondary, L,
			NSLOCTEXT("SharedWorld", "ProvConnect", "Connect"), 15, FMargin(18.f, 0.f));
		Connect->OnClicked.AddDynamic(this, &USharedWorldStorageProviderCard::OnConnect);
		ActionBox->AddChild(Connect);
	}

	if (bRow)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		RootBorder->SetContent(Row);
		UHorizontalBox* Inner = WidgetTree->ConstructWidget<UHorizontalBox>();
		Select->AddChild(Inner);
		FillButtonContent(Select);
		Row->AddChildToHorizontalBox(Select)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		Inner->AddChildToHorizontalBox(BuildIcon(WidgetTree, P, 48.f))->SetPadding(FMargin(12.f, 10.f, 14.f, 10.f));
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Name = MakeText(WidgetTree, 17, TextPrimary, true);
		Name->SetText(FText::FromString(P.DisplayName));
		Name->SetAutoWrapText(false);
		Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Text->AddChildToVerticalBox(Name);
		UTextBlock* Desc = MakeText(WidgetTree, FontSmall, TextMuted);
		Desc->SetText(FText::FromString(P.Description));
		Desc->SetAutoWrapText(false);
		Desc->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Text->AddChildToVerticalBox(Desc)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		if (UHorizontalBoxSlot* S = Inner->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}
		if (UHorizontalBoxSlot* S = Inner->AddChildToHorizontalBox(BadgeRow(WidgetTree, P)))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(10.f, 0.f, 14.f, 0.f));
		}
		ActionBox->SetMinDesiredWidth(140.f);
		if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(ActionBox))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 0.f, 14.f, 0.f));
		}
	}
	else
	{
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		RootBorder->SetContent(Col);
		UVerticalBox* Inner = WidgetTree->ConstructWidget<UVerticalBox>();
		Select->AddChild(Inner);
		FillButtonContent(Select);
		// The select area takes all spare height, so the action row sits on the bottom edge on every card.
		Col->AddChildToVerticalBox(Select)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
		Inner->AddChildToVerticalBox(Head)->SetPadding(FMargin(12.f, 12.f, 12.f, 6.f));
		Head->AddChildToHorizontalBox(BuildIcon(WidgetTree, P, 44.f))->SetVerticalAlignment(VAlign_Top);
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* Name = MakeText(WidgetTree, 17, TextPrimary, true);
		Name->SetText(FText::FromString(P.DisplayName));
		Name->SetAutoWrapText(false);
		Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Text->AddChildToVerticalBox(Name);
		if (UVerticalBoxSlot* BadgeSlot = Text->AddChildToVerticalBox(BadgeRow(WidgetTree, P)))
		{
			BadgeSlot->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
			BadgeSlot->SetHorizontalAlignment(HAlign_Left);
		}
		if (UHorizontalBoxSlot* S = Head->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Top);
			S->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
		}
		USizeBox* DescBox = WidgetTree->ConstructWidget<USizeBox>();
		DescBox->SetMinDesiredHeight(40.f); // two lines, so short and long descriptions take the same room
		UTextBlock* Desc = MakeText(WidgetTree, FontSmall, TextMuted);
		Desc->SetText(FText::FromString(P.Description));
		DescBox->AddChild(Desc);
		Inner->AddChildToVerticalBox(DescBox)->SetPadding(FMargin(12.f, 0.f, 12.f, 8.f));

		if (UVerticalBoxSlot* S = Col->AddChildToVerticalBox(ActionBox))
		{
			S->SetPadding(FMargin(12.f, 0.f, 12.f, 12.f));
			S->SetHorizontalAlignment(HAlign_Fill);
		}
	}
	UpdateChrome();
}

void USharedWorldStorageProviderCard::SetupViewAll(USharedWorldBrowserWidget* Owner, ESharedWorldCardLayout Layout)
{
	if (!WidgetTree || WidgetTree->RootWidget) return;
	OwnerBrowser = Owner;
	ProviderId.Reset();
	RootBorder = WidgetTree->ConstructWidget<UBorder>();
	RootBorder->SetPadding(FMargin(0.f));
	WidgetTree->RootWidget = RootBorder;

	UButton* Btn = WidgetTree->ConstructWidget<UButton>();
	StyleSolidButton(Btn, Clear, FLinearColor(1.f, 1.f, 1.f, 0.05f), FLinearColor(1.f, 1.f, 1.f, 0.09f), Clear, FMargin(0.f), RadiusM);
	Btn->OnClicked.AddDynamic(this, &USharedWorldStorageProviderCard::OnViewAll);
	RootBorder->SetContent(Btn);

	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	Btn->AddChild(Col);
	FillButtonContent(Btn);

	// "+" in a ring, drawn from primitives
	USizeBox* Ring = WidgetTree->ConstructWidget<USizeBox>();
	Ring->SetWidthOverride(36.f);
	Ring->SetHeightOverride(36.f);
	UOverlay* O = WidgetTree->ConstructWidget<UOverlay>();
	Ring->AddChild(O);
	auto Add = [&](float W, float H, const FSlateBrush& Brush)
	{
		USizeBox* S = WidgetTree->ConstructWidget<USizeBox>();
		S->SetWidthOverride(W);
		S->SetHeightOverride(H);
		UBorder* B = WidgetTree->ConstructWidget<UBorder>();
		B->SetBrush(Brush);
		S->AddChild(B);
		if (UOverlaySlot* Slot = O->AddChildToOverlay(S))
		{
			Slot->SetHorizontalAlignment(HAlign_Center);
			Slot->SetVerticalAlignment(VAlign_Center);
		}
	};
	Add(36.f, 36.f, RoundedBrush(Clear, 18.f, TextPrimary, 2.f));
	Add(16.f, 2.f, RoundedBrush(TextPrimary, 1.f));
	Add(2.f, 16.f, RoundedBrush(TextPrimary, 1.f));
	if (UVerticalBoxSlot* RingSlot = Col->AddChildToVerticalBox(Ring))
	{
		RingSlot->SetPadding(FMargin(0.f, 22.f, 0.f, 8.f));
		RingSlot->SetHorizontalAlignment(HAlign_Center);
	}

	UTextBlock* T = MakeText(WidgetTree, 16, TextPrimary, true);
	T->SetJustification(ETextJustify::Center);
	T->SetText(NSLOCTEXT("SharedWorld", "ViewAllProviders", "View All Providers"));
	Col->AddChildToVerticalBox(T)->SetPadding(FMargin(10.f, 0.f, 10.f, 4.f));
	UTextBlock* D = MakeText(WidgetTree, FontSmall, TextMuted);
	D->SetJustification(ETextJustify::Center);
	D->SetText(NSLOCTEXT("SharedWorld", "ViewAllDesc", "See all supported storage providers."));
	Col->AddChildToVerticalBox(D)->SetPadding(FMargin(10.f, 0.f, 10.f, 16.f));
	UpdateChrome();
}

void USharedWorldStorageProviderCard::UpdateChrome()
{
	if (!RootBorder) return;
	// Orange = the provider you are looking at (selected). Green ACTIVE/Connected badges mark state, never the border.
	FLinearColor Edge = PanelEdge;
	float Width = 1.f;
	if (bIsSelected) { Edge = Accent; Width = 2.f; }
	if (bIsFocused) { Edge = FLinearColor(1.f, 0.82f, 0.55f, 1.f); Width = 2.f; }
	RootBorder->SetBrush(RoundedBrush(bIsSelected ? FLinearColor(0.12f, 0.085f, 0.05f, 0.88f) : RowFill, RadiusM, Edge, Width));
}

void USharedWorldStorageProviderCard::NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnAddedToFocusPath(InFocusEvent);
	bIsFocused = true;
	UpdateChrome();
}

void USharedWorldStorageProviderCard::NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnRemovedFromFocusPath(InFocusEvent);
	bIsFocused = false;
	UpdateChrome();
}

void USharedWorldStorageProviderCard::OnSelect()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->SelectProvider(ProviderId);
}

void USharedWorldStorageProviderCard::OnConnect()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->ConnectProvider(ProviderId);
}

void USharedWorldStorageProviderCard::OnViewAll()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get()) B->ViewAllProviders();
}
