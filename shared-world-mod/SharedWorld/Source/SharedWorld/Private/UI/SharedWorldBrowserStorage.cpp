// Settings > Storage: active provider card, provider browser (search, grid/list), selected-provider panel.
//
// Live values (connected, account, repository, Test Connection, Disconnect) come from the existing GitHub
// backend. Everything else is display-only provider metadata and is labelled that way. No rclone yet.

#include "UI/SharedWorldBrowserWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Rclone/RcloneRuntime.h"
#include "Rclone/RcloneSelfTest.h"
#include "Misc/ConfigCacheIni.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "TimerManager.h"
#include "UI/SharedWorldStorageModel.h"
#include "UI/SharedWorldStorageProviderCard.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

namespace
{

	FString RelativeTimeText(const FDateTime& When)
	{
		const FTimespan Ago = FDateTime::UtcNow() - When;
		if (Ago.GetTotalSeconds() < 45.0) return TEXT("Just now");
		if (Ago.GetTotalMinutes() < 60.0)
		{
			const int32 M = FMath::Max(1, FMath::RoundToInt(static_cast<float>(Ago.GetTotalMinutes())));
			return FString::Printf(TEXT("%d minute%s ago"), M, M == 1 ? TEXT("") : TEXT("s"));
		}
		if (Ago.GetTotalHours() < 24.0)
		{
			const int32 H = FMath::FloorToInt(static_cast<float>(Ago.GetTotalHours()));
			return FString::Printf(TEXT("%d hour%s ago"), H, H == 1 ? TEXT("") : TEXT("s"));
		}
		const int32 D = FMath::FloorToInt(static_cast<float>(Ago.GetTotalDays()));
		return FString::Printf(TEXT("%d day%s ago"), D, D == 1 ? TEXT("") : TEXT("s"));
	}

	UBorder* SectionPanel(UWidgetTree* Tree, const FMargin& Pad = FMargin(20.f, 16.f))
	{
		return MakePanel(Tree, FLinearColor(0.045f, 0.05f, 0.06f, 0.88f), PanelEdge, Pad, RadiusL, 1.f);
	}
}

// ============================================================================ page

void USharedWorldBrowserWidget::AddStoragePage(UVerticalBox* Col)
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
	if (SelectedProviderId.IsEmpty() || !Catalog.Find(SelectedProviderId))
	{
		SelectedProviderId = Catalog.ActiveProviderId;
	}
	AddActiveStorageCard(Col, Catalog);
	AddProviderBrowser(Col);
	if (!StorageDetailsBox)
	{
		// Narrow screens: the selected-provider panel stacks under the browser.
		UBorder* Panel = MakePanel(WidgetTree, FLinearColor(0.04f, 0.045f, 0.055f, 0.86f), PanelEdge, FMargin(18.f, 16.f), RadiusL, 1.f);
		Col->AddChildToVerticalBox(Panel)->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
		StorageDetailsBox = WidgetTree->ConstructWidget<UVerticalBox>();
		Panel->SetContent(StorageDetailsBox);
		PopulateProviderDetails(StorageDetailsBox);
	}
}

void USharedWorldBrowserWidget::AddKeyValueRow(UVerticalBox* Col, const FText& Label, const FString& Value, const FLinearColor& ValueColor, bool bWrap, const FText& Tip)
{
	if (Value.IsEmpty()) return; // unavailable rows disappear cleanly
	Col->AddChildToVerticalBox(MakeDetailRow(WidgetTree, Label, Value, ValueColor, bWrap, Tip))->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
}

// ============================================================================ active provider card

void USharedWorldBrowserWidget::AddActiveStorageCard(UVerticalBox* Col, const FSharedWorldStorageCatalog& Catalog)
{
	const FSharedWorldStorageProvider* P = Catalog.Active();
	USharedWorldSubsystem* S = SW();
	if (!P || !S) return;
	const FSharedWorldSignIn Sign = S->GetSignInStatus();

	FLinearColor Edge = Accent;
	Edge.A = 0.8f;
	UBorder* Outer = MakePanel(WidgetTree, FLinearColor(0.045f, 0.05f, 0.06f, 0.88f), Edge, FMargin(20.f, 16.f), RadiusL, 2.f);
	Col->AddChildToVerticalBox(Outer)->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	UVerticalBox* In = WidgetTree->ConstructWidget<UVerticalBox>();
	Outer->SetContent(In);

	UTextBlock* Title = MakeText(WidgetTree, 21, TextPrimary, true);
	Title->SetText(NSLOCTEXT("SharedWorld", "ActiveProviderTitle", "Active Storage Provider"));
	In->AddChildToVerticalBox(Title);
	UTextBlock* Sub = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Sub->SetText(NSLOCTEXT("SharedWorld", "ActiveProviderSub", "This is the storage provider used for new Shared Worlds and your current worlds."));
	In->AddChildToVerticalBox(Sub)->SetPadding(FMargin(0.f, 2.f, 0.f, 14.f));

	UBorder* Card = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(20.f, 18.f), RadiusM, 1.f);
	In->AddChildToVerticalBox(Card);
	UVerticalBox* CardCol = WidgetTree->ConstructWidget<UVerticalBox>();
	Card->SetContent(CardCol);
	UHorizontalBox* Top = WidgetTree->ConstructWidget<UHorizontalBox>();
	CardCol->AddChildToVerticalBox(Top);

	Top->AddChildToHorizontalBox(USharedWorldStorageProviderCard::BuildIcon(WidgetTree, *P, 84.f))->SetVerticalAlignment(VAlign_Center);

	// identity (left)
	UVerticalBox* Ident = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* IS = Top->AddChildToHorizontalBox(Ident))
	{
		FSlateChildSize Sz(ESlateSizeRule::Fill);
		Sz.Value = 1.f;
		IS->SetSize(Sz);
		IS->SetVerticalAlignment(VAlign_Center);
		IS->SetPadding(FMargin(20.f, 0.f, 16.f, 0.f));
	}
	UHorizontalBox* NameRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	Ident->AddChildToVerticalBox(NameRow)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	UTextBlock* Name = MakeText(WidgetTree, 28, TextPrimary, true);
	Name->SetText(FText::FromString(P->DisplayName));
	Name->SetAutoWrapText(false);
	NameRow->AddChildToHorizontalBox(Name)->SetVerticalAlignment(VAlign_Center);
	UBorder* StatusPill = nullptr;
	if (Sign.bInProgress && P->ProviderId == TEXT("github")) StatusPill = MakePill(WidgetTree, ESharedWorldTone::Working, NSLOCTEXT("SharedWorld", "Linking", "Linking..."), 14);
	else if (P->bConnected && P->ProviderId == TEXT("github")) StatusPill = MakePill(WidgetTree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "Connected", "Connected"), 14);
	else if (P->ProviderId == TEXT("local-folder")) StatusPill = MakePill(WidgetTree, ESharedWorldTone::Warning, NSLOCTEXT("SharedWorld", "LocalOnly", "Local only"), 14);
	else StatusPill = MakePill(WidgetTree, ESharedWorldTone::Inactive, NSLOCTEXT("SharedWorld", "NotLinked", "Not linked"), 14);
	NameRow->AddChildToHorizontalBox(StatusPill)->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));
	if (!P->AccountName.IsEmpty())
	{
		UTextBlock* Acc = MakeText(WidgetTree, FontBody + 1, TextPrimary);
		Acc->SetText(FText::FromString(P->AccountName));
		Ident->AddChildToVerticalBox(Acc)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	}
	const FString Place = !P->RepositoryName.IsEmpty() ? P->RepositoryName : P->Location;
	if (!Place.IsEmpty())
	{
		UTextBlock* Loc = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		Loc->SetText(FText::FromString(Place));
		Loc->SetAutoWrapText(false);
		Loc->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Ident->AddChildToVerticalBox(Loc);
	}

	// divider + key/value table (right)
	Top->AddChildToHorizontalBox(MakeRule(WidgetTree, true))->SetPadding(FMargin(0.f, 4.f, 0.f, 4.f));
	UVerticalBox* Facts = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* FS = Top->AddChildToHorizontalBox(Facts))
	{
		FSlateChildSize Sz(ESlateSizeRule::Fill);
		Sz.Value = 1.2f;
		FS->SetSize(Sz);
		FS->SetVerticalAlignment(VAlign_Center);
		FS->SetPadding(FMargin(22.f, 0.f, 0.f, 0.f));
	}
	AddKeyValueRow(Facts, NSLOCTEXT("SharedWorld", "KvAccount", "Account"), P->AccountName, TextPrimary);
	AddKeyValueRow(Facts, NSLOCTEXT("SharedWorld", "KvRepo", "Repository"), P->RepositoryName, TextPrimary);
	AddKeyValueRow(Facts, NSLOCTEXT("SharedWorld", "KvLocation", "Storage Location"), P->Location, TextPrimary);
	if (P->ProviderId == TEXT("github") && P->bConnected)
	{
		// Real: only set after the player runs Test Connection in this session.
		AddKeyValueRow(Facts, NSLOCTEXT("SharedWorld", "KvChecked", "Last Checked"),
			bLastTestRan ? RelativeTimeText(LastTestTime) : FString(TEXT("Not checked this session")), bLastTestRan ? TextPrimary : TextMuted);
	}

	// actions
	UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
	CardCol->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));
	const bool bGitConnected = P->ProviderId == TEXT("github") && P->bConnected;
	if (bGitConnected)
	{
		TObjectPtr<UTextBlock> L1, L2;
		UButton* Manage = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1, NSLOCTEXT("SharedWorld", "Manage", "Manage"), 15, FMargin(22.f, 9.f));
		Manage->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageManage);
		Actions->AddChildToHorizontalBox(Manage)->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
		UButton* Test = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L2, bBusy ? NSLOCTEXT("SharedWorld", "Testing", "Testing...") : NSLOCTEXT("SharedWorld", "TestConnection", "Test Connection"), 15, FMargin(22.f, 9.f));
		Test->SetIsEnabled(!bBusy);
		Test->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnTestGitHubAccess);
		Actions->AddChildToHorizontalBox(Test);
	}
	else if (Sign.bConfigured && !Sign.bInProgress)
	{
		TObjectPtr<UTextBlock> L1;
		UButton* Link = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L1, NSLOCTEXT("SharedWorld", "LinkGitHub", "Link GitHub"), 15, FMargin(22.f, 9.f));
		Link->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnLinkGitHubClicked);
		Actions->AddChildToHorizontalBox(Link);
	}
	USizeBox* Spacer = WidgetTree->ConstructWidget<USizeBox>();
	Actions->AddChildToHorizontalBox(Spacer)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TObjectPtr<UTextBlock> L3;
	UButton* Change = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L3, NSLOCTEXT("SharedWorld", "ChangeProvider", "Change Provider"), 15, FMargin(22.f, 9.f));
	Change->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageChangeProvider);
	Actions->AddChildToHorizontalBox(Change);

	if (Sign.bInProgress)
	{
		// A link attempt is running: show where to go, right under the card.
		UButton* Unused = nullptr;
		In->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Working,
			NSLOCTEXT("SharedWorld", "LinkRunning", "Linking GitHub"),
			NSLOCTEXT("SharedWorld", "LinkRunningBody", "Finish approving Shared Worlds on GitHub in your browser."), Unused))
			->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
	}
}

// ============================================================================ provider browser

void USharedWorldBrowserWidget::AddProviderBrowser(UVerticalBox* Col)
{
	UBorder* Outer = SectionPanel(WidgetTree);
	Col->AddChildToVerticalBox(Outer)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	ProviderBrowserAnchor = Outer;
	UVerticalBox* In = WidgetTree->ConstructWidget<UVerticalBox>();
	Outer->SetContent(In);

	// title (left) | search + view toggle (right)
	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	In->AddChildToVerticalBox(Head)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	UVerticalBox* Titles = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* TS = Head->AddChildToHorizontalBox(Titles))
	{
		TS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TS->SetVerticalAlignment(VAlign_Center);
	}
	UTextBlock* T = MakeText(WidgetTree, 21, TextPrimary, true);
	T->SetText(NSLOCTEXT("SharedWorld", "ChooseProviderTitle", "Choose a Storage Provider"));
	Titles->AddChildToVerticalBox(T);
	UTextBlock* Sub = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Sub->SetText(NSLOCTEXT("SharedWorld", "ChooseProviderSub", "Connect a new storage provider to use for your Shared Worlds."));
	Titles->AddChildToVerticalBox(Sub)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));

	// Which storage is actually in use. Selecting a card below never changes this.
	if (USharedWorldSubsystem* S = SW())
	{
		const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
		const FSharedWorldStorageProvider* ActiveProvider = nullptr;
		for (const FSharedWorldStorageProvider& Candidate : Catalog.Providers)
		{
			if (Candidate.bActive) { ActiveProvider = &Candidate; break; }
		}
		UHorizontalBox* ActiveRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		Titles->AddChildToVerticalBox(ActiveRow)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
		UTextBlock* ActiveLabel = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		ActiveLabel->SetText(NSLOCTEXT("SharedWorld", "ActiveStorageLabel", "Active storage:"));
		ActiveLabel->SetAutoWrapText(false);
		ActiveRow->AddChildToHorizontalBox(ActiveLabel)->SetVerticalAlignment(VAlign_Center);
		if (ActiveProvider)
		{
			UTextBlock* ActiveName = MakeText(WidgetTree, FontSmall + 1, TextPrimary, true);
			ActiveName->SetText(FText::FromString(ActiveProvider->DisplayName));
			ActiveName->SetAutoWrapText(false);
			ActiveRow->AddChildToHorizontalBox(ActiveName)->SetPadding(FMargin(8.f, 0.f, 10.f, 0.f));
			ActiveRow->AddChildToHorizontalBox(MakeStatusBadge(WidgetTree, ESharedWorldTone::Healthy,
				ActiveProvider->ProviderId == TEXT("local-folder") ? NSLOCTEXT("SharedWorld", "LocalOnly", "Local only") : NSLOCTEXT("SharedWorld", "Connected", "Connected"),
				FontSmall + 1, 11.f))->SetVerticalAlignment(VAlign_Center);
		}
		else
		{
			UTextBlock* None = MakeText(WidgetTree, FontSmall + 1, TextMuted, true);
			None->SetText(NSLOCTEXT("SharedWorld", "ActiveStorageNone", "None connected"));
			None->SetAutoWrapText(false);
			ActiveRow->AddChildToHorizontalBox(None)->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
		}
	}

	USizeBox* SearchSize = WidgetTree->ConstructWidget<USizeBox>();
	SearchSize->SetMinDesiredWidth(300.f);
	UBorder* SearchBox = MakePanel(WidgetTree, SearchBg, PanelEdge, FMargin(14.f, 2.f), RadiusM, 1.f);
	SearchSize->AddChild(SearchBox);
	UHorizontalBox* SearchRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	SearchBox->SetContent(SearchRow);
	SearchRow->AddChildToHorizontalBox(MakeSearchIcon(WidgetTree))->SetVerticalAlignment(VAlign_Center);
	StorageSearchInput = WidgetTree->ConstructWidget<UEditableTextBox>();
	StorageSearchInput->SetHintText(NSLOCTEXT("SharedWorld", "ProviderSearch", "Search storage providers..."));
	StorageSearchInput->SetText(FText::FromString(StorageSearch));
	StyleTextField(StorageSearchInput, 15);
	StorageSearchInput->OnTextChanged.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageSearchChanged);
	if (UHorizontalBoxSlot* S = SearchRow->AddChildToHorizontalBox(StorageSearchInput))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
	}
	if (UHorizontalBoxSlot* S = Head->AddChildToHorizontalBox(SearchSize))
	{
		S->SetVerticalAlignment(VAlign_Center);
	}
	for (const bool bGridButton : { true, false })
	{
		const bool bActive = (bGridButton == bStorageGrid);
		UBorder* Frame = MakePanel(WidgetTree, SearchBg, bActive ? Accent : PanelEdge, FMargin(2.f), RadiusM, 2.f);
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(46.f);
		Size->SetHeightOverride(40.f);
		UButton* B = WidgetTree->ConstructWidget<USharedWorldButton>();
		StyleSolidButton(B, bActive ? FLinearColor(0.16f, 0.12f, 0.08f, 0.9f) : Clear, FLinearColor(1.f, 1.f, 1.f, 0.08f), FLinearColor(1.f, 1.f, 1.f, 0.14f), Clear, FMargin(0.f), RadiusS);
		B->SetContent(bGridButton ? MakeGridIcon(WidgetTree, bActive ? Accent : TextMuted) : MakeListIcon(WidgetTree, bActive ? Accent : TextMuted));
		if (bGridButton) B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageViewGrid);
		else B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageViewList);
		Size->AddChild(B);
		Frame->SetContent(Size);
		Head->AddChildToHorizontalBox(Frame)->SetPadding(FMargin(bGridButton ? 12.f : 6.f, 0.f, 0.f, 0.f));
	}

	StorageGridBox = WidgetTree->ConstructWidget<UVerticalBox>();
	In->AddChildToVerticalBox(StorageGridBox);
	PopulateProviderGrid(StorageGridBox);

	// footer
	In->AddChildToVerticalBox(MakeRule(WidgetTree))->SetPadding(FMargin(0.f, 6.f, 0.f, 12.f));
	UHorizontalBox* Foot = WidgetTree->ConstructWidget<UHorizontalBox>();
	In->AddChildToVerticalBox(Foot);
	Foot->AddChildToHorizontalBox(MakeToneIcon(WidgetTree, ESharedWorldTone::Inactive, 14.f))->SetVerticalAlignment(VAlign_Center);
	UVerticalBox* FootText = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* Powered = MakeText(WidgetTree, FontSmall + 1, TextPrimary, true);
	Powered->SetText(NSLOCTEXT("SharedWorld", "PoweredByRclone", "Storage providers powered by rclone"));
	FootText->AddChildToVerticalBox(Powered);
	UTextBlock* Progressive = MakeText(WidgetTree, FontSmall, TextMuted);
	Progressive->SetText(NSLOCTEXT("SharedWorld", "ProgressiveNote", "Provider connections will be enabled progressively."));
	FootText->AddChildToVerticalBox(Progressive);
	if (UHorizontalBoxSlot* S = Foot->AddChildToHorizontalBox(FootText))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
		S->SetPadding(FMargin(10.f, 0.f, 12.f, 0.f));
	}
	TObjectPtr<UTextBlock> L;
	UButton* Learn = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L, NSLOCTEXT("SharedWorld", "LearnMore", "Learn More"), 14, FMargin(18.f, 7.f));
	Learn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageLearnMore);
	Foot->AddChildToHorizontalBox(Learn)->SetVerticalAlignment(VAlign_Center);
}

void USharedWorldBrowserWidget::PopulateProviderGrid(UVerticalBox* Host)
{
	if (!Host) return;
	Host->ClearChildren();
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);

	const bool bSearching = !StorageSearch.TrimStartAndEnd().IsEmpty();
	TArray<const FSharedWorldStorageProvider*> Recommended, Other, More;
	for (const FSharedWorldStorageProvider& P : Catalog.Providers)
	{
		if (P.ProviderId == TEXT("local-folder")) continue; // shown as the active provider when in use
		if (!FSharedWorldStorageCatalog::Matches(P, StorageSearch)) continue;
		if (P.bViewAllOnly)
		{
			// Every other rclone backend: listed on "View all", when searched for, or once connected.
			if (bShowAllProviders || bSearching || P.bConnected) More.Add(&P);
			continue;
		}
		(P.bRecommended ? Recommended : Other).Add(&P);
	}
	if (Recommended.Num() == 0 && Other.Num() == 0 && More.Num() == 0)
	{
		UTextBlock* T = MakeText(WidgetTree, FontBody + 1, TextMuted);
		T->SetText(FText::Format(NSLOCTEXT("SharedWorld", "NoProviderMatch", "No storage providers match \"{0}\"."), FText::FromString(StorageSearch.TrimStartAndEnd())));
		Host->AddChildToVerticalBox(T)->SetPadding(FMargin(4.f, 8.f, 0.f, 16.f));
		return;
	}

	// Columns from the width this panel really gets (side-by-side layout gives the browser ~3/4 of the page).
	float LogicalWidth = 3840.f;
	{
		const FVector2D VS = UWidgetLayoutLibrary::GetViewportSize(this);
		const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
		if (VS.X > 1.f) LogicalWidth = VS.X / Scale;
	}
	const bool bSideBySide = LogicalWidth >= 1500.f;
	const float Chrome = bSideBySide ? (60.f + StorageDetailsWidth + 18.f + 70.f) : 100.f; // shell + details panel + gap + this panel's padding
	const int32 Cols = GridColumns(236.f, 1.f, Chrome, 4);

	const ESharedWorldCardLayout Layout = bStorageGrid ? ESharedWorldCardLayout::Tile : ESharedWorldCardLayout::Row;
	auto AddSection = [&](const FText& Title, const TArray<const FSharedWorldStorageProvider*>& Items, bool bWithViewAll)
	{
		if (Items.Num() == 0 && !bWithViewAll) return;
		UTextBlock* H = MakeText(WidgetTree, FontBody, Accent, true);
		H->SetText(Title);
		Host->AddChildToVerticalBox(H)->SetPadding(FMargin(2.f, 6.f, 0.f, 10.f));
		UUniformGridPanel* Grid = nullptr;
		if (bStorageGrid)
		{
			Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
			Grid->SetSlotPadding(FMargin(0.f, 0.f, 12.f, 12.f));
			Host->AddChildToVerticalBox(Grid)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		}
		int32 Index = 0;
		auto Place = [&](USharedWorldStorageProviderCard* Card)
		{
			if (Grid)
			{
				if (UUniformGridSlot* Cell = Grid->AddChildToUniformGrid(Card, Index / Cols, Index % Cols))
				{
					Cell->SetHorizontalAlignment(HAlign_Fill);
					Cell->SetVerticalAlignment(VAlign_Fill);
				}
				++Index;
			}
			else
			{
				Host->AddChildToVerticalBox(Card)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			}
		};
		for (const FSharedWorldStorageProvider* P : Items)
		{
			USharedWorldStorageProviderCard* Card = CreateWidget<USharedWorldStorageProviderCard>(this, USharedWorldStorageProviderCard::StaticClass());
			if (!Card) continue;
			Card->Setup(*P, this, P->ProviderId == SelectedProviderId, Layout);
			Place(Card);
		}
		if (bWithViewAll)
		{
			USharedWorldStorageProviderCard* Card = CreateWidget<USharedWorldStorageProviderCard>(this, USharedWorldStorageProviderCard::StaticClass());
			if (Card)
			{
				Card->SetupViewAll(this, Layout);
				Place(Card);
			}
		}
	};
	AddSection(NSLOCTEXT("SharedWorld", "RecommendedProviders", "Recommended Providers"), Recommended, false);
	AddSection(NSLOCTEXT("SharedWorld", "OtherProviders", "Other Providers"), Other, !bSearching && !bShowAllProviders);
	AddSection(FText::Format(NSLOCTEXT("SharedWorld", "AllProviders", "All Providers ({0})"), FText::AsNumber(More.Num())), More, false);
}

// ============================================================================ selected provider panel

void USharedWorldBrowserWidget::PopulateProviderDetails(UVerticalBox* Host)
{
	if (!Host) return;
	Host->ClearChildren();
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
	const FSharedWorldStorageProvider* P = Catalog.Find(SelectedProviderId);
	if (!P) P = Catalog.Active();
	if (!P) return;
	const bool bGit = P->ProviderId == TEXT("github");
	const bool bLiveGit = bGit && P->bConnected;

	// ---- header: icon | name over [ACTIVE] [Connected / Not Connected] [Coming soon]
	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	Host->AddChildToVerticalBox(Head)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	Head->AddChildToHorizontalBox(USharedWorldStorageProviderCard::BuildIcon(WidgetTree, *P, 56.f))->SetVerticalAlignment(VAlign_Center);
	UVerticalBox* Titles = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* Name = MakeText(WidgetTree, 26, TextPrimary, true);
	Name->SetText(FText::FromString(P->DisplayName));
	Name->SetAutoWrapText(false);
	Name->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Name->SetToolTipText(FText::FromString(P->DisplayName));
	Titles->AddChildToVerticalBox(Name);
	UHorizontalBox* Pills = WidgetTree->ConstructWidget<UHorizontalBox>();
	Titles->AddChildToVerticalBox(Pills)->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	if (P->bActive)
	{
		Pills->AddChildToHorizontalBox(MakeSolidBadge(WidgetTree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "ActiveBadge", "ACTIVE"), 12))
			->SetVerticalAlignment(VAlign_Center);
	}
	if (UHorizontalBoxSlot* PS = Pills->AddChildToHorizontalBox(P->bConnected
		? MakePill(WidgetTree, ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "Connected", "Connected"), 13)
		: MakePill(WidgetTree, ESharedWorldTone::Inactive, NSLOCTEXT("SharedWorld", "NotConnected", "Not Connected"), 13)))
	{
		PS->SetVerticalAlignment(VAlign_Center);
		PS->SetPadding(FMargin(P->bActive ? 8.f : 0.f, 0.f, 0.f, 0.f));
	}
	if (P->IsComingSoon())
	{
		Pills->AddChildToHorizontalBox(MakePill(WidgetTree, ESharedWorldTone::Warning, NSLOCTEXT("SharedWorld", "ComingSoon", "Coming soon"), 13))
			->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}
	if (UHorizontalBoxSlot* TS = Head->AddChildToHorizontalBox(Titles))
	{
		TS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TS->SetVerticalAlignment(VAlign_Center);
		TS->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	}

	UTextBlock* Desc = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Desc->SetText(FText::FromString(P->Description));
	Host->AddChildToVerticalBox(Desc)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));

	// ---- rclone connection: what was really observed, plus where it stands today
	if (P->IsRcloneBacked() && P->bConnected)
	{
		Host->AddChildToVerticalBox(MakeProgressRow(WidgetTree, P->bVerified ? ESharedWorldStep::Done : ESharedWorldStep::Pending,
			P->bVerified ? NSLOCTEXT("SharedWorld", "RcVerified", "Read/write test passed")
				: NSLOCTEXT("SharedWorld", "RcNotVerified", "Last read/write test failed")))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		UTextBlock* Note = MakeText(WidgetTree, FontSmall, TextMuted);
		Note->SetText(NSLOCTEXT("SharedWorld", "RcNotYetWorlds",
			"Connected for save files. Worlds don't store their saves here yet; that switch comes in a later update."));
		Host->AddChildToVerticalBox(Note)->SetPadding(FMargin(0.f, 2.f, 0.f, 8.f));
	}

	// ---- live checks: only for a connected GitHub, only what was actually observed
	if (bLiveGit)
	{
		auto Check = [&](ESharedWorldStep Step, const FText& Text)
		{
			Host->AddChildToVerticalBox(MakeProgressRow(WidgetTree, Step, Text))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		};
		Check(ESharedWorldStep::Done, FText::Format(NSLOCTEXT("SharedWorld", "ChkLinked", "Account linked as {0}"), FText::FromString(P->AccountName)));
		if (bLastTestRan)
		{
			Check(bLastTestOk ? ESharedWorldStep::Done : ESharedWorldStep::Pending,
				bLastTestOk ? NSLOCTEXT("SharedWorld", "ChkOk", "Connection test passed") : NSLOCTEXT("SharedWorld", "ChkFail", "Connection test failed"));
		}
		else
		{
			Check(ESharedWorldStep::Pending, NSLOCTEXT("SharedWorld", "ChkNone", "Connection not tested this session"));
		}
	}
	Host->AddChildToVerticalBox(MakeRule(WidgetTree))->SetPadding(FMargin(0.f, 8.f, 0.f, 14.f));

	// ---- account information: only when there is something real to show (never blank "Account: -" rows)
	if (P->bConnected && (!P->AccountName.IsEmpty() || !P->RepositoryName.IsEmpty() || !P->Location.IsEmpty()))
	{
		UTextBlock* H = MakeText(WidgetTree, FontBody, TextPrimary, true);
		H->SetText(NSLOCTEXT("SharedWorld", "AccountInfo", "Account Information"));
		Host->AddChildToVerticalBox(H)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "KvAccount", "Account"), P->AccountName, TextPrimary, false,
			NSLOCTEXT("SharedWorld", "TipAccount", "The account Shared Worlds uses for this provider."));
		AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "KvRepo", "Repository"), P->RepositoryName, TextPrimary, false,
			NSLOCTEXT("SharedWorld", "TipRepo", "The repository that stores your worlds."));
		AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "KvLocation", "Storage Location"), P->Location, TextPrimary, false,
			NSLOCTEXT("SharedWorld", "TipLocation", "Where inside the provider your world data is kept."));
		if (P->IsRcloneBacked())
		{
			AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "KvVerified", "Last Verified"),
				P->bVerified ? RelativeTimeText(P->VerifiedUtc) : FString(TEXT("Not verified")), P->bVerified ? TextPrimary : TextMuted, false,
				NSLOCTEXT("SharedWorld", "TipVerified", "When a test file was last written, read back and deleted."));
		}
		if (bLiveGit)
		{
			AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "KvChecked", "Last Checked"),
				bLastTestRan ? RelativeTimeText(LastTestTime) : FString(TEXT("Not checked this session")), bLastTestRan ? TextPrimary : TextMuted, false,
				NSLOCTEXT("SharedWorld", "TipChecked", "Time of the last Test Connection run in this session."));
		}
		Host->AddChildToVerticalBox(MakeRule(WidgetTree))->SetPadding(FMargin(0.f, 4.f, 0.f, 14.f));
	}

	// ---- capabilities: provider-type metadata, never presented as a live check; unknown stays "Unknown"
	{
		UTextBlock* H = MakeText(WidgetTree, FontBody, TextPrimary, true);
		H->SetText(NSLOCTEXT("SharedWorld", "ProviderCaps", "Provider Capabilities"));
		Host->AddChildToVerticalBox(H)->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
		UTextBlock* Cap = MakeText(WidgetTree, FontSmall - 1, TextMuted);
		Cap->SetText(P->bHasCapabilityInfo
			? NSLOCTEXT("SharedWorld", "CapsKnown", "What this provider supports. Not a live check.")
			: NSLOCTEXT("SharedWorld", "CapsUnknown", "Confirmed once this provider is set up."));
		Host->AddChildToVerticalBox(Cap)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		auto Flag = [&](const FText& Label, bool bOn, const FText& Tip, bool bKnown = true)
		{
			const bool bHasInfo = P->bHasCapabilityInfo && bKnown;
			const FString Value = !bHasInfo ? FString(TEXT("Unknown")) : (bOn ? FString(TEXT("Supported")) : FString(TEXT("Not available")));
			AddKeyValueRow(Host, Label, Value, (bHasInfo && bOn) ? Ok : TextMuted, false, Tip);
		};
		AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "CapOps", "File Operations"),
			P->bHasCapabilityInfo ? P->FileOperationsText() : FString(TEXT("Unknown")),
			P->bHasCapabilityInfo ? TextPrimary : TextMuted, true,
			NSLOCTEXT("SharedWorld", "TipOps", "The file actions Shared Worlds can perform on this provider."));
		Flag(NSLOCTEXT("SharedWorld", "CapHash", "Hash Support"), P->bSupportsHash,
			NSLOCTEXT("SharedWorld", "TipHash", "Whether files can be verified by checksum."));
		Flag(NSLOCTEXT("SharedWorld", "CapAtomic", "Atomic Writes"), P->bSupportsAtomicWrites,
			NSLOCTEXT("SharedWorld", "TipAtomic", "Whether an upload replaces a file all at once, never half-written."), P->bExtendedCapsKnown);
		Flag(NSLOCTEXT("SharedWorld", "CapCopy", "Server Side Copy"), P->bSupportsServerSideCopy,
			NSLOCTEXT("SharedWorld", "TipCopy", "Whether copies can happen on the provider without downloading."), P->bExtendedCapsKnown);
		Flag(NSLOCTEXT("SharedWorld", "CapQuota", "Quota Information"), P->bSupportsQuota,
			NSLOCTEXT("SharedWorld", "TipQuota", "Whether free space can be read from the provider."), P->bExtendedCapsKnown);
		AddKeyValueRow(Host, NSLOCTEXT("SharedWorld", "CapTier", "Provider Tier"),
			P->ProviderTier.IsEmpty() ? FString(TEXT("Unknown")) : P->ProviderTier, P->ProviderTier.IsEmpty() ? TextMuted : TextPrimary, false,
			NSLOCTEXT("SharedWorld", "TipTier", "How mature support for this provider is."));
	}

	if (!StorageNotice.IsEmpty())
	{
		UButton* Unused = nullptr;
		Host->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
			FText::FromString(StorageNotice), FText::GetEmpty(), Unused))
			->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
	}

	// ---- actions, by state
	UVerticalBox* Buttons = WidgetTree->ConstructWidget<UVerticalBox>();
	Host->AddChildToVerticalBox(Buttons)->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
	enum class EAct : uint8 { Reconfigure, Disconnect, Connect, SetActive, RcloneTest, RcloneDisconnect };
	auto AddButton = [&](ESharedWorldButtonRole Role, const FText& Label, EAct Act)
	{
		TObjectPtr<UTextBlock> L;
		UButton* B = MakeRoleButton(WidgetTree, Role, L, Label, 16, FMargin(0.f, 11.f));
		switch (Act)
		{
		case EAct::Reconfigure: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageReconfigure); break;
		case EAct::Disconnect: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDisconnectGitHub); break;
		case EAct::SetActive: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageSetActive); break;
		case EAct::RcloneTest: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageRcloneTest); B->SetIsEnabled(!bRcloneTestRunning); break;
		case EAct::RcloneDisconnect: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageRcloneDisconnect); break;
		default: B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnStorageConnectSelected); break;
		}
		Buttons->AddChildToVerticalBox(B)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	};
	if (P->bActive && bLiveGit)
	{
		// The storage in use: nothing to "activate".
		AddButton(ESharedWorldButtonRole::Secondary, NSLOCTEXT("SharedWorld", "ReconfigureGit", "Reconfigure GitHub"), EAct::Reconfigure);
		AddButton(ESharedWorldButtonRole::Danger, NSLOCTEXT("SharedWorld", "DisconnectGit", "Disconnect"), EAct::Disconnect);
	}
	else if (P->IsRcloneBacked() && P->bConnected)
	{
		AddButton(ESharedWorldButtonRole::Secondary, bRcloneTestRunning ? NSLOCTEXT("SharedWorld", "Testing", "Testing...")
			: NSLOCTEXT("SharedWorld", "TestConnection", "Test Connection"), EAct::RcloneTest);
		AddButton(ESharedWorldButtonRole::Danger, NSLOCTEXT("SharedWorld", "DisconnectGit", "Disconnect"), EAct::RcloneDisconnect);
	}
	else if (P->bConnected && P->bAvailable && !P->bActive)
	{
		// Layout for later (several connected providers, one active). Switching storage is not implemented yet.
		AddButton(ESharedWorldButtonRole::Config, NSLOCTEXT("SharedWorld", "SetActive", "Set as Active"), EAct::SetActive);
		if (bLiveGit) AddButton(ESharedWorldButtonRole::Danger, NSLOCTEXT("SharedWorld", "DisconnectGit", "Disconnect"), EAct::Disconnect);
	}
	else if (P->ProviderId == TEXT("local-folder"))
	{
		AddButton(ESharedWorldButtonRole::Config, NSLOCTEXT("SharedWorld", "UseGitHub", "Connect GitHub"), EAct::Connect);
	}
	else
	{
		AddButton(ESharedWorldButtonRole::Config,
			FText::Format(NSLOCTEXT("SharedWorld", "ConnectNamed", "{0} {1}"), P->ConnectVerb(), FText::FromString(P->DisplayName)), EAct::Connect);
	}
}

// ============================================================================ interaction

void USharedWorldBrowserWidget::QueueStorageRefresh()
{
	// Deferred one tick: a card click must not destroy the card that is still handling it.
	if (bStorageRefreshQueued) return;
	bStorageRefreshQueued = true;
	if (UWorld* World = GetWorld())
	{
		TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([Weak]()
		{
			if (USharedWorldBrowserWidget* Self = Weak.Get())
			{
				Self->bStorageRefreshQueued = false;
				Self->RefreshStorageContent();
			}
		}));
		return;
	}
	bStorageRefreshQueued = false;
	RefreshStorageContent();
}

void USharedWorldBrowserWidget::RefreshStorageContent()
{
	if (Page != EPage::Settings || SettingsTab != 0) return;
	if (StorageGridBox) PopulateProviderGrid(StorageGridBox);
	if (StorageDetailsBox) PopulateProviderDetails(StorageDetailsBox);
}

void USharedWorldBrowserWidget::SelectProvider(const FString& ProviderId)
{
	if (ProviderId.IsEmpty()) return;
	SelectedProviderId = ProviderId;
	StorageNotice.Reset();
	QueueStorageRefresh();
}

void USharedWorldBrowserWidget::ConnectProvider(const FString& ProviderId)
{
	if (ProviderId.IsEmpty()) return;
	SelectedProviderId = ProviderId;
	if (ProviderId == TEXT("github") || ProviderId == TEXT("local-folder"))
	{
		StorageNotice.Reset();
		if (USharedWorldSubsystem* S = SW(); S && S->GetGitHubLogin().IsEmpty()) OnLinkGitHubClicked(); // real link flow
		else QueueStorageRefresh();
		return;
	}
	if (USharedWorldSubsystem* S = SW())
	{
		const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
		if (const FSharedWorldStorageProvider* P = Catalog.Find(ProviderId); P && P->IsRcloneBacked())
		{
			if (!P->bAvailable)
			{
				StorageNotice = TEXT("The storage engine isn't installed in this build, so this provider can't be set up.");
				QueueStorageRefresh();
				return;
			}
			StorageNotice.Reset();
			BeginConnect(ProviderId);
			return;
		}
	}
	StorageNotice = TEXT("This provider can't be set up yet.");
	QueueStorageRefresh();
}

void USharedWorldBrowserWidget::ViewAllProviders()
{
	bShowAllProviders = !bShowAllProviders;
	StorageNotice.Reset();
	QueueStorageRefresh();
}

void USharedWorldBrowserWidget::OnStorageConnectSelected() { ConnectProvider(SelectedProviderId); }

void USharedWorldBrowserWidget::OnStorageManage()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	SelectProvider(FSharedWorldStorageCatalog::Build(*S).ActiveProviderId);
}

void USharedWorldBrowserWidget::OnStorageChangeProvider()
{
	// Change Provider just brings the picker into view; nothing is migrated yet.
	if (ListScroll && ProviderBrowserAnchor) ListScroll->ScrollWidgetIntoView(ProviderBrowserAnchor, true);
	if (StorageSearchInput) StorageSearchInput->SetKeyboardFocus();
}

void USharedWorldBrowserWidget::OnStorageSearchChanged(const FText& Text)
{
	StorageSearch = Text.ToString();
	if (StorageGridBox) PopulateProviderGrid(StorageGridBox); // in place: the search box keeps focus
}

static void SaveStorageView(bool bGrid)
{
	if (GConfig)
	{
		GConfig->SetString(TEXT("SharedWorld"), TEXT("StorageView"), bGrid ? TEXT("Grid") : TEXT("List"), GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

void USharedWorldBrowserWidget::OnStorageViewGrid()
{
	if (bStorageGrid) return;
	bStorageGrid = true;
	SaveStorageView(true);
	ListScroll = nullptr; // toggle highlight lives in the static header: rebuild the page once
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnStorageViewList()
{
	if (!bStorageGrid) return;
	bStorageGrid = false;
	SaveStorageView(false);
	ListScroll = nullptr;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnStorageLearnMore()
{
	FPlatformProcess::LaunchURL(TEXT("https://rclone.org/"), nullptr, nullptr);
}

void USharedWorldBrowserWidget::OnStorageReconfigure()
{
	USharedWorldSubsystem* S = SW();
	if (!S || SelectedProviderId != TEXT("github")) return;
	// Re-run the real GitHub sign-in (lets the player pick a different account). Nothing changes until it succeeds.
	S->BeginGitHubSignIn();
	Page = EPage::LinkGitHub;
	ListScroll = nullptr;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnStorageSetActive()
{
	// The panel layout supports "connected but not active" already; the switch itself is a later backend task.
	StorageNotice = TEXT("Switching the active storage is not available yet.");
	QueueStorageRefresh();
}

// ============================================================================ rclone engine (Diagnostics tab)

void USharedWorldBrowserWidget::AddRcloneEngineCard(UVerticalBox* Col)
{
	FRcloneRuntime& Rc = FRcloneRuntime::Get();
	const bool bInstalled = FPaths::FileExists(Rc.LibraryPath());
	const bool bLoaded = Rc.IsAvailable();

	UBorder* Card = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(16.f, 14.f), RadiusM, 1.f);
	Col->AddChildToVerticalBox(Card)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	UVerticalBox* In = WidgetTree->ConstructWidget<UVerticalBox>();
	Card->SetContent(In);

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	In->AddChildToVerticalBox(Head)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	UTextBlock* Title = MakeText(WidgetTree, 17, TextPrimary, true);
	Title->SetText(NSLOCTEXT("SharedWorld", "RcloneEngine", "rclone engine"));
	if (UHorizontalBoxSlot* TS = Head->AddChildToHorizontalBox(Title))
	{
		TS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TS->SetVerticalAlignment(VAlign_Center);
	}
	UBorder* Status = bLoaded
		? MakePill(WidgetTree, ESharedWorldTone::Healthy, FText::Format(NSLOCTEXT("SharedWorld", "RcloneReady", "Ready {0}"), FText::FromString(Rc.Version())), 13)
		: (bInstalled
			? MakePill(WidgetTree, ESharedWorldTone::Inactive, NSLOCTEXT("SharedWorld", "RcloneInstalled", "Installed"), 13)
			: MakePill(WidgetTree, ESharedWorldTone::Warning, NSLOCTEXT("SharedWorld", "RcloneMissing", "Not installed"), 13));
	Head->AddChildToHorizontalBox(Status)->SetVerticalAlignment(VAlign_Center);

	UTextBlock* Desc = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Desc->SetText(bInstalled
		? NSLOCTEXT("SharedWorld", "RcloneDesc", "Storage providers such as Google Drive, S3 and SFTP run through the rclone engine. The self-test round-trips a file through it on this PC; it needs no account and uses no network.")
		: NSLOCTEXT("SharedWorld", "RcloneDescMissing", "The rclone engine (librclone.dll) is not part of this install yet. It is built from the rclone source with tools/rclone/build-librclone.ps1 and placed in Binaries/ThirdParty/rclone."));
	In->AddChildToVerticalBox(Desc)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));

	TObjectPtr<UTextBlock> L;
	UButton* Run = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L,
		bRcloneSelfTestRunning ? NSLOCTEXT("SharedWorld", "RcloneTesting", "Testing...") : NSLOCTEXT("SharedWorld", "RcloneRun", "Run self-test"), 15, FMargin(20.f, 9.f));
	Run->SetIsEnabled(bInstalled && !bRcloneSelfTestRunning);
	Run->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRcloneSelfTest);
	In->AddChildToVerticalBox(Run)->SetHorizontalAlignment(HAlign_Left);

	if (!RcloneSelfTestText.IsEmpty())
	{
		UBorder* Out = MakePanel(WidgetTree, FLinearColor(0.03f, 0.035f, 0.045f, 0.9f), PanelEdge, FMargin(12.f, 10.f), RadiusS, 1.f);
		In->AddChildToVerticalBox(Out)->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
		UTextBlock* T = MakeText(WidgetTree, FontSmall, TextPrimary);
		T->SetText(FText::FromString(RcloneSelfTestText));
		Out->SetContent(T);
	}
}

void USharedWorldBrowserWidget::OnRcloneSelfTest()
{
	if (bRcloneSelfTestRunning) return;
	bRcloneSelfTestRunning = true;
	RcloneSelfTestText = TEXT("Running...");
	ListScroll = nullptr;
	ScheduleRebuild();
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FRcloneRuntime::RunDetached([Weak]()
	{
		const FRcloneSelfTestResult R = RunRcloneSelfTest(); // blocking; off the game thread
		const FString Text = R.Summary();
		FRcloneRuntime::PostToGameThread([Weak, Text]()
		{
			if (USharedWorldBrowserWidget* Self = Weak.Get())
			{
				Self->bRcloneSelfTestRunning = false;
				Self->RcloneSelfTestText = Text;
				Self->ListScroll = nullptr;
				Self->ScheduleRebuild();
			}
		});
	});
}
