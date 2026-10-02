#include "UI/SharedWorldBrowserWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Misc/ConfigCacheIni.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "FGSaveSystem.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Styling/SlateTypes.h"
#include "TimerManager.h"
#include "Services/SharedWorldCreationService.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "Services/SharedWorldInviteService.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldFgWidgets.h"
#include "UI/SharedWorldModal.h"
#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldSavePickRow.h"
#include "UI/SharedWorldUiStyle.h"
#include "UI/SharedWorldWorldCard.h"
#include "UObject/UnrealType.h"
#include "Widgets/Layout/Anchors.h"

using namespace SharedWorldUi;

namespace
{
	/** Transparent content over FG submenu chrome — no heavy opaque plate. */
	const FLinearColor SubMenuBg(0.02f, 0.022f, 0.028f, 0.45f);
	const FLinearColor DetailPaneBg(0.03f, 0.035f, 0.04f, 0.35f);

	UButton* TextLink(UWidgetTree* Tree, const FText& Label, int32 FontSize = 14)
	{
		TObjectPtr<UTextBlock> L;
		return MakeTextLink(Tree, L, Label, FontSize);
	}
}

TSharedRef<SWidget> USharedWorldBrowserWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
		// Transparent — Widget_SubMenuBackground supplies the FG plate.
		Root->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		Root->SetPadding(FMargin(20.f, 16.f, 20.f, 16.f));
		WidgetTree->RootWidget = Root;
		// SizeBox with a max desired width sits between the menu and the page: content can be as wide as it
		// likes inside, but the menu container only ever sees a width that fits the screen.
		WidthClamp = WidgetTree->ConstructWidget<USizeBox>();
		PageRoot = WidgetTree->ConstructWidget<UVerticalBox>();
		WidthClamp->AddChild(PageRoot);
		Root->SetContent(WidthClamp);
		UpdateWidthClamp();
	}
	return Super::RebuildWidget();
}

void USharedWorldBrowserWidget::UpdateWidthClamp()
{
	if (!WidthClamp) return;
	float LogicalWidth = 3840.f;
	const FVector2D VS = UWidgetLayoutLibrary::GetViewportSize(this);
	const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
	if (VS.X > 1.f) LogicalWidth = VS.X / Scale;
	// Leave room for the Satisfactory navigation column and this widget's own padding.
	WidthClamp->SetMaxDesiredWidth(FMath::Max(640.f, LogicalWidth - 360.f - 40.f));
}

void USharedWorldBrowserWidget::NativeConstruct()
{
	Super::NativeConstruct();
	{
		// Remember list vs grid across sessions (per-user ini; no backend involvement).
		FString Saved;
		if (GConfig && GConfig->GetString(TEXT("SharedWorld"), TEXT("BrowserView"), Saved, GGameUserSettingsIni))
		{
			bGridView = Saved.Equals(TEXT("Grid"), ESearchCase::IgnoreCase);
		}
		if (GConfig && GConfig->GetString(TEXT("SharedWorld"), TEXT("StorageView"), Saved, GGameUserSettingsIni))
		{
			bStorageGrid = !Saved.Equals(TEXT("List"), ESearchCase::IgnoreCase);
		}
	}
	if (USharedWorldSubsystem* S = SW())
	{
		ChangedHandle = S->OnChanged.AddUObject(this, &USharedWorldBrowserWidget::OnBackendChanged);
		if (!S->IsWelcomeDone() && S->NeedsWelcomeStorageConnect())
		{
			Page = EPage::Welcome;
		}
	}
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::NativeDestruct()
{
	if (USharedWorldSubsystem* S = SW()) S->OnChanged.Remove(ChangedHandle);
	Super::NativeDestruct();
}

void USharedWorldBrowserWidget::ScheduleRebuild()
{
	// Coalesce NativeConstruct + ActivateInMainMenu + OnChanged into one rebuild next tick.
	if (bRebuildQueued) return;
	bRebuildQueued = true;
	if (UWorld* World = GetWorld())
	{
		TWeakObjectPtr<USharedWorldBrowserWidget> WeakThis(this);
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]()
		{
			if (USharedWorldBrowserWidget* Self = WeakThis.Get())
			{
				Self->bRebuildQueued = false;
				Self->RebuildPageNow();
			}
		}));
		return;
	}
	bRebuildQueued = false;
	RebuildPageNow();
}

void USharedWorldBrowserWidget::RebuildPage()
{
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::CaptureListScroll()
{
	if (!ListScroll || bPendingScrollRestore) return;
	const float Offset = ListScroll->GetScrollOffset();
	// A freshly built ScrollBox reports 0 until restore runs — never clobber a real offset with that.
	if (Offset <= KINDA_SMALL_NUMBER) return;
	switch (ListScrollOwner)
	{
	case EPage::Main: MainListScrollOffset = Offset; break;
	case EPage::Details: DetailsListScrollOffset = Offset; break;
	default: OtherListScrollOffset = Offset; break;
	}
}

void USharedWorldBrowserWidget::RestoreListScroll(EPage ForPage)
{
	if (!ListScroll) return;
	float Offset = 0.f;
	switch (ForPage)
	{
	case EPage::Main: Offset = MainListScrollOffset; break;
	case EPage::Details: Offset = DetailsListScrollOffset; break;
	default: Offset = OtherListScrollOffset; break;
	}
	if (Offset <= KINDA_SMALL_NUMBER) return;

	ListScrollOwner = ForPage;
	PendingScrollRestorePage = ForPage;
	PendingScrollRestoreOffset = Offset;
	ScrollRestoreAttempts = 0;
	bPendingScrollRestore = true;
	++ScrollRestoreGeneration;
	const int32 Gen = ScrollRestoreGeneration;
	TWeakObjectPtr<USharedWorldBrowserWidget> WeakThis(this);
	auto Queue = [WeakThis, Gen]()
	{
		if (USharedWorldBrowserWidget* Self = WeakThis.Get())
		{
			if (Self->ScrollRestoreGeneration == Gen) Self->TickScrollRestore();
		}
	};
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda(Queue));
	}
	else
	{
		TickScrollRestore();
	}
}

void USharedWorldBrowserWidget::TickScrollRestore()
{
	if (!bPendingScrollRestore || !ListScroll) { bPendingScrollRestore = false; return; }
	if (Page != PendingScrollRestorePage || ListScrollOwner != PendingScrollRestorePage)
	{
		bPendingScrollRestore = false;
		return;
	}

	const float Restore = PendingScrollRestoreOffset;
	ListScroll->ForceLayoutPrepass();
	ListScroll->SetScrollOffset(Restore);

	const float Got = ListScroll->GetScrollOffset();
	const float MaxEnd = ListScroll->GetScrollOffsetOfEnd();
	const float Target = (MaxEnd > KINDA_SMALL_NUMBER) ? FMath::Min(Restore, MaxEnd) : Restore;
	const bool bApplied = FMath::IsNearlyEqual(Got, Target, 1.f);
	const bool bLayoutReady = MaxEnd + 1.f >= Target || MaxEnd > KINDA_SMALL_NUMBER;
	if (!bApplied && ScrollRestoreAttempts < 12 && (!bLayoutReady || Got + 1.f < Target))
	{
		++ScrollRestoreAttempts;
		const int32 Gen = ScrollRestoreGeneration;
		TWeakObjectPtr<USharedWorldBrowserWidget> WeakThis(this);
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis, Gen]()
			{
				if (USharedWorldBrowserWidget* Self = WeakThis.Get())
				{
					if (Self->ScrollRestoreGeneration == Gen) Self->TickScrollRestore();
				}
			}));
			return;
		}
	}
	bPendingScrollRestore = false;
}

bool USharedWorldBrowserWidget::SoftRefreshMain()
{
	if (!MainListBox || !MainDetailBox || !ListScroll) return false;
	const float Offset = ListScroll->GetScrollOffset();
	MainListBox->ClearChildren();
	MainDetailBox->ClearChildren();
	RowBinders.Reset();

	FSharedWorldDiscoverySnapshot Snap;
	FSharedWorldBrowserItem SelectedItem;
	bool bHasSelected = false;
	PopulateMainList(MainListBox, Snap, SelectedItem, bHasSelected);
	if (StatusText && SW())
	{
		StatusText->SetText(FText::FromString(Snap.bRefreshing ? FString(TEXT("Refreshing Shared Worlds...")) : Snap.StatusMessage));
	}
	PopulateMainDetail(MainDetailBox, SelectedItem, bHasSelected);
	UpdateBottomPlay();

	if (Offset > KINDA_SMALL_NUMBER)
	{
		MainListScrollOffset = Offset;
		ListScroll->ForceLayoutPrepass();
		ListScroll->SetScrollOffset(Offset);
		RestoreListScroll(EPage::Main);
	}
	return true;
}

bool USharedWorldBrowserWidget::SoftRefreshDetails()
{
	if (!ScrollContentBox || !ListScroll || SelectedWorldId.IsEmpty()) return false;
	FSharedWorldEntryView View;
	bool bHas = false;
	if (USharedWorldSubsystem* S = SW())
	{
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId == SelectedWorldId) { View = V; bHas = true; break; }
		}
	}
	if (!bHas) return false;

	const float Offset = ListScroll->GetScrollOffset();
	ScrollContentBox->ClearChildren();
	PopulateDetailsBody(ScrollContentBox, View);
	if (Offset > KINDA_SMALL_NUMBER)
	{
		DetailsListScrollOffset = Offset;
		ListScroll->ForceLayoutPrepass();
		ListScroll->SetScrollOffset(Offset);
		RestoreListScroll(EPage::Details);
	}
	return true;
}

USharedWorldSubsystem* USharedWorldBrowserWidget::SW() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

UWidgetSwitcher* USharedWorldBrowserWidget::FindSwitcher(UUserWidget* MainMenuRoot) const
{
	if (!MainMenuRoot) return nullptr;
	static const FName Names[] = { TEXT("mSwitcher"), TEXT("mSwitcherWidget"), TEXT("Switcher") };
	for (FName N : Names)
	{
		if (MainMenuRoot->WidgetTree)
		{
			if (UWidgetSwitcher* S = Cast<UWidgetSwitcher>(MainMenuRoot->WidgetTree->FindWidget(N))) return S;
		}
		if (UWidgetSwitcher* S = Cast<UWidgetSwitcher>(SharedWorldFg::GetObjectProp(MainMenuRoot, N))) return S;
	}
	if (MainMenuRoot->WidgetTree)
	{
		TArray<UWidget*> All;
		MainMenuRoot->WidgetTree->GetAllWidgets(All);
		for (UWidget* W : All)
		{
			if (UWidgetSwitcher* S = Cast<UWidgetSwitcher>(W)) return S;
		}
	}
	return nullptr;
}

void USharedWorldBrowserWidget::ActivateInMainMenu(UUserWidget* MainMenuRoot)
{
	// Prefer baked SharedWorldsBrowser (SubMenuBackground in mSwitcher) — same idea as
	// ModsButton_SML → ModList_SML. Fall back to wrapping into the switcher at runtime.
	UWidgetSwitcher* Switcher = FindSwitcher(MainMenuRoot);
	UWidget* SwitcherChild = this;

	UUserWidget* BakedShell = nullptr;
	if (MainMenuRoot)
	{
		if (UWidget* Named = SharedWorldFg::FindNamedWidget(MainMenuRoot, TEXT("SharedWorldsBrowser")))
		{
			BakedShell = Cast<UUserWidget>(Named);
		}
	}

	if (BakedShell)
	{
		// Hooked page may already BE the C++ browser (SML ModList pattern).
		if (Cast<USharedWorldBrowserWidget>(BakedShell))
		{
			SwitcherChild = BakedShell;
		}
		else
		{
			SharedWorldFg::FillSubMenuContent(BakedShell, this);
			SwitcherChild = BakedShell;
		}
	}
	else
	{
		UWidget* ExistingChild = this;
		for (UWidget* P = GetParent(); P; P = P->GetParent())
		{
			if (P == Switcher)
			{
				break;
			}
			ExistingChild = P;
		}
		const bool bAlreadyInSwitcher = Switcher && ExistingChild->GetParent() == Switcher;
		if (bAlreadyInSwitcher)
		{
			SwitcherChild = ExistingChild;
		}
		else
		{
			if (!SubMenuShell)
			{
				SubMenuShell = SharedWorldFg::WrapInSubMenuBackground(MainMenuRoot ? MainMenuRoot : this, this);
			}
			if (SubMenuShell)
			{
				SwitcherChild = SubMenuShell;
			}
		}
	}

	if (!Switcher)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=browser_no_switcher"));
		return;
	}

	if (SwitcherChild->GetParent() != Switcher)
	{
		SwitcherChild->RemoveFromParent();
		Switcher->AddChild(SwitcherChild);
	}
	Switcher->SetActiveWidget(SwitcherChild);

	SetVisibility(ESlateVisibility::Visible);
	if (BakedShell) BakedShell->SetVisibility(ESlateVisibility::Visible);
	if (SubMenuShell) SubMenuShell->SetVisibility(ESlateVisibility::Visible);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=browser_activated_in_switcher baked=%d"), BakedShell ? 1 : 0);
	// Paint cached list first; discovery refresh updates via OnChanged when ready.
	ScheduleRebuild();
	if (USharedWorldSubsystem* S = SW()) S->Discovery().BeginRefresh();
}

void USharedWorldBrowserWidget::Close()
{
	// Baked menu page — never RemoveFromParent. Return to the first switcher page.
	for (UWidget* It = this; It; It = It->GetParent())
	{
		if (UWidgetSwitcher* Sw = Cast<UWidgetSwitcher>(It->GetParent()))
		{
			if (Sw->GetChildrenCount() > 0)
			{
				Sw->SetActiveWidgetIndex(0);
			}
			return;
		}
	}
}
void USharedWorldBrowserWidget::ShowMain() { Page = EPage::Main; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowDetails()
{
	if (SelectedWorldId.IsEmpty()) return;
	Page = EPage::Details;
	bShowTechnicalDetails = false;
	DetailsTab = 0;
	bDetailsMembersLoaded = false;
	bDetailsMembersLoading = false;
	DetailsMembersText.Reset();
	DetailsHistoryText.Reset();
	ScheduleRebuild();
	RequestDetailsMembers();
}
void USharedWorldBrowserWidget::ShowCreateWizard()
{
	SelectedSaveName.Reset();
	SuggestedWorldName.Reset();
	PendingWorldName.Reset();
	CreateError.Reset();
	Page = EPage::CreatePickSave;
	ScheduleRebuild();
}
void USharedWorldBrowserWidget::ShowJoinFriend() { Page = EPage::JoinFriend; JoinTab = 0; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowAdvanced() { Page = EPage::Settings; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowJoinCode() { Page = EPage::JoinFriend; JoinTab = 2; ScheduleRebuild(); }

void USharedWorldBrowserWidget::OnSavePicked(const FString& SaveName, const FString& DisplayName)
{
	if (SelectedSaveName != SaveName)
	{
		PendingWorldName.Reset(); // new save: suggest its name again
	}
	SelectedSaveName = SaveName;
	SuggestedWorldName = DisplayName;
	CreateError.Reset();
	Page = EPage::CreateName;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::SelectWorld(const FString& WorldId)
{
	SelectedWorldId = WorldId;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::SetFlash(bool bOk, const FString& Message)
{
	if (!FlashText) return;
	FlashText->SetColorAndOpacity(FSlateColor(bOk ? Ok : Err));
	FlashText->SetText(FText::FromString(Message));
}

void USharedWorldBrowserWidget::UpdateBottomPlay()
{
	// Enabled state is decided per world by SharedWorldBrowserModel (Play is refused mid-transition);
	// only grey the label out when nothing is selected.
	if (PlayLabel && SelectedWorldId.IsEmpty())
	{
		PlayLabel->SetColorAndOpacity(FSlateColor(TextMuted));
	}
}

void USharedWorldBrowserWidget::AddInviteRows(UVerticalBox* Col, const TArray<FSharedWorldPendingInviteView>& Invites)
{
	for (const FSharedWorldPendingInviteView& I : Invites)
	{
		UBorder* Row = WidgetTree->ConstructWidget<UBorder>();
		Row->SetBrushColor(RowHover);
		Row->SetPadding(FMargin(14.f, 10.f));
		Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
		UVerticalBox* Inner = WidgetTree->ConstructWidget<UVerticalBox>();
		Row->SetContent(Inner);
		UTextBlock* Title = MakeText(WidgetTree, 15, TextPrimary, true);
		Title->SetText(FText::FromString(FString::Printf(TEXT("%s invited you to:"),
			I.FromDisplayName.IsEmpty() ? TEXT("A friend") : *I.FromDisplayName)));
		Inner->AddChildToVerticalBox(Title);
		UTextBlock* World = MakeText(WidgetTree, 16, TextPrimary, true);
		World->SetText(FText::FromString(I.WorldName));
		Inner->AddChildToVerticalBox(World)->SetPadding(FMargin(0, 2, 0, 8));
		UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
		Inner->AddChildToVerticalBox(Actions);
		// One binder per button so each invitation acts on its own id (a shared field made every row act on the last invite).
		auto Bind = [this](UButton* B, const FString& InviteId, bool bAccept)
		{
			USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
			Binder->Browser = this;
			Binder->InviteId = InviteId;
			Binder->InviteAction = bAccept ? 1 : 2;
			RowBinders.Add(Binder);
			B->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
		};
		UButton* Accept = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Accept", "Accept"));
		Bind(Accept, I.InviteId, true);
		Actions->AddChildToHorizontalBox(Accept)->SetPadding(FMargin(0, 0, 20, 0));
		UButton* Decline = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Decline", "Decline"));
		Bind(Decline, I.InviteId, false);
		Actions->AddChildToHorizontalBox(Decline);
	}
}

void USharedWorldBrowserWidget::RemoveWorldFromList(const FString& WorldId)
{
	USharedWorldSubsystem* S = SW();
	if (!S)
	{
		return;
	}
	const FString Msg = S->ForgetWorld(WorldId);
	if (Msg.IsEmpty())
	{
		if (SelectedWorldId == WorldId)
		{
			SelectedWorldId.Reset();
		}
		if (Page == EPage::Details)
		{
			Page = EPage::Main;
		}
		SetFlash(true, TEXT("Removed from this PC's list. Cloud data is unchanged."));
	}
	else
	{
		SetFlash(false, Msg);
	}
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::AddDetailStatRow(UVerticalBox* Col, const FText& Label, const FText& Value, const FLinearColor& ValueColor)
{
	if (!Col) return;
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 6));
	UTextBlock* L = MakeText(WidgetTree, 14, TextMuted, false);
	L->SetText(Label);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>();
	LabelBox->SetWidthOverride(150.f);
	LabelBox->AddChild(L);
	Row->AddChildToHorizontalBox(LabelBox)->SetPadding(FMargin(0, 0, 16, 0));
	UTextBlock* V = MakeText(WidgetTree, 14, ValueColor, false);
	V->SetText(Value);
	Row->AddChildToHorizontalBox(V)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
}

void USharedWorldBrowserWidget::RequestDetailsMembers()
{
	if (SelectedWorldId.IsEmpty() || bDetailsMembersLoading) return;
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	bDetailsMembersLoading = true;
	const FString WorldId = SelectedWorldId;
	S->FetchPlayers(WorldId, [this, WorldId](bool bOk, const FString& Message)
	{
		if (SelectedWorldId != WorldId) return;
		bDetailsMembersLoading = false;
		bDetailsMembersLoaded = true;
		DetailsMembersText = bOk ? Message : FString::Printf(TEXT("Could not load members: %s"), *Message);
		if (Page == EPage::Details) ScheduleRebuild();
	});
}


void USharedWorldBrowserWidget::RebuildMain()
{
	// Narrow screens: give the list the whole width and reach details via the "..." menu.
	float LogicalWidth = 3840.f;
	{
		const FVector2D VS = UWidgetLayoutLibrary::GetViewportSize(this);
		const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
		if (VS.X > 1.f) LogicalWidth = VS.X / Scale;
	}
	const bool bShowDetailsPanel = LogicalWidth >= 1100.f;

	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	PageRoot->AddChildToVerticalBox(Columns)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	// ================= LEFT: browser panel =================
	UBorder* LeftPanel = MakePanel(WidgetTree, PanelFill, PanelEdge, FMargin(22.f, 18.f, 22.f, 14.f), 8.f, 1.f);
	if (UHorizontalBoxSlot* LeftSlot = Columns->AddChildToHorizontalBox(LeftPanel))
	{
		FSlateChildSize Sz(ESlateSizeRule::Fill);
		Sz.Value = 2.6f;
		LeftSlot->SetSize(Sz);
		LeftSlot->SetPadding(FMargin(0.f, 0.f, bShowDetailsPanel ? 16.f : 0.f, 0.f));
	}
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	LeftPanel->SetContent(Left);

	// ---- header: globe, title, Create (primary), Join Friend (secondary)
	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
	Left->AddChildToVerticalBox(Header)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	Header->AddChildToHorizontalBox(MakeGlobeIcon(WidgetTree))->SetVerticalAlignment(VAlign_Center);
	{
		UTextBlock* Title = MakeText(WidgetTree, 32, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "BrowserTitle", "Shared Worlds"));
		Title->SetAutoWrapText(false);
		Title->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		if (UHorizontalBoxSlot* S = Header->AddChildToHorizontalBox(Title))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(14.f, 0.f, 12.f, 0.f));
		}
	}
	{
		TObjectPtr<UTextBlock> Lbl;
		UButton* Create = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, Lbl, NSLOCTEXT("SharedWorld", "CreateSharedPlus", "+  Create Shared World..."),
			17, FMargin(20.f, 12.f));
		Create->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateClicked);
		Header->AddChildToHorizontalBox(Create)->SetVerticalAlignment(VAlign_Center);
	}
	{
		TObjectPtr<UTextBlock> Lbl;
		UButton* Join = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, Lbl, NSLOCTEXT("SharedWorld", "JoinFriendEllipsis", "Join Friend..."),
			17, FMargin(20.f, 12.f));
		Join->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinFriendClicked);
		if (UHorizontalBoxSlot* S = Header->AddChildToHorizontalBox(Join))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
		}
	}

	// ---- search + list/grid toggle
	UHorizontalBox* SearchRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	Left->AddChildToVerticalBox(SearchRow)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	{
		UBorder* SearchBox = MakePanel(WidgetTree, SearchBg, PanelEdge, FMargin(14.f, 4.f), 5.f, 1.f);
		if (UHorizontalBoxSlot* S = SearchRow->AddChildToHorizontalBox(SearchBox))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}
		UHorizontalBox* In = WidgetTree->ConstructWidget<UHorizontalBox>();
		SearchBox->SetContent(In);
		In->AddChildToHorizontalBox(MakeSearchIcon(WidgetTree))->SetVerticalAlignment(VAlign_Center);

		FilterInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		FilterInput->SetHintText(NSLOCTEXT("SharedWorld", "Search", "Search for a Shared World..."));
		FilterInput->SetText(FText::FromString(FilterText));
		StyleTextField(FilterInput, 16);
		FilterInput->OnTextChanged.AddDynamic(this, &USharedWorldBrowserWidget::OnFilterChanged);
		if (UHorizontalBoxSlot* S = In->AddChildToHorizontalBox(FilterInput))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}
	}
	for (const bool bGridButton : { false, true })
	{
		const bool bActive = (bGridButton == bGridView);
		UBorder* Frame = MakePanel(WidgetTree, SearchBg, bActive ? Accent : PanelEdge, FMargin(2.f), 5.f, 2.f);
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(46.f);
		Size->SetHeightOverride(40.f);
		UButton* B = WidgetTree->ConstructWidget<USharedWorldButton>();
		StyleSolidButton(B, Clear, FLinearColor(1.f, 1.f, 1.f, 0.08f), FLinearColor(1.f, 1.f, 1.f, 0.14f), Clear, FMargin(0.f), 4.f);
		B->SetContent(bGridButton ? MakeGridIcon(WidgetTree, bActive ? Accent : TextMuted) : MakeListIcon(WidgetTree, bActive ? Accent : TextMuted));
		if (bGridButton) B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnViewGrid);
		else B->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnViewList);
		Size->AddChild(B);
		Frame->SetContent(Size);
		SearchRow->AddChildToHorizontalBox(Frame)->SetPadding(FMargin(bGridButton ? 6.f : 12.f, 0.f, 0.f, 0.f));
	}

	// ---- scrolling world list
	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	Left->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(List);
	MainListBox = List;

	FSharedWorldDiscoverySnapshot Snap;
	FSharedWorldBrowserItem SelectedItem;
	bool bHasSelected = false;
	PopulateMainList(List, Snap, SelectedItem, bHasSelected);

	// ---- footer: Refresh / Settings / status
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	Left->AddChildToVerticalBox(Footer)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
	UButton* Refresh = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Refresh", "Refresh"), 15);
	Refresh->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRefresh);
	Footer->AddChildToHorizontalBox(Refresh)->SetPadding(FMargin(0.f, 0.f, 22.f, 0.f));
	UButton* Settings = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Settings", "Settings"), 15);
	Settings->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnSettingsClicked);
	Footer->AddChildToHorizontalBox(Settings);
	StatusText = MakeText(WidgetTree, 13, TextMuted);
	StatusText->SetJustification(ETextJustify::Right);
	StatusText->SetText(FText::FromString(Snap.bRefreshing ? FString(TEXT("Refreshing Shared Worlds...")) : Snap.StatusMessage));
	if (UHorizontalBoxSlot* S = Footer->AddChildToHorizontalBox(StatusText))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
		S->SetHorizontalAlignment(HAlign_Right);
	}

	// ================= RIGHT: selected world =================
	UBorder* DetailPanel = MakePanel(WidgetTree, PanelFill, PanelEdge, FMargin(18.f, 18.f, 18.f, 14.f), 8.f, 1.f);
	if (UHorizontalBoxSlot* RightSlot = Columns->AddChildToHorizontalBox(DetailPanel))
	{
		FSlateChildSize Sz(ESlateSizeRule::Fill);
		Sz.Value = 1.1f;
		RightSlot->SetSize(Sz);
	}
	DetailPanel->SetVisibility(bShowDetailsPanel ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	UScrollBox* DetailScroll = WidgetTree->ConstructWidget<UScrollBox>();
	DetailPanel->SetContent(DetailScroll);
	UVerticalBox* Detail = WidgetTree->ConstructWidget<UVerticalBox>();
	DetailScroll->AddChild(Detail);
	MainDetailBox = Detail;
	PopulateMainDetail(Detail, SelectedItem, bHasSelected);

	FlashText = MakeText(WidgetTree, 13, TextMuted);
	PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
	UpdateBottomPlay();
	ListScrollOwner = EPage::Main;
	RestoreListScroll(EPage::Main);
}

UTexture2D* USharedWorldBrowserWidget::GetThumbnail(const FString& WorldId)
{
	if (TObjectPtr<UTexture2D>* Found = ThumbCache.Find(WorldId))
	{
		return Found->Get();
	}
	// FG exposes no save screenshot API; use a sidecar image next to the world's save if one exists.
	UTexture2D* Tex = nullptr;
	const FString SaveDir = UFGSaveSystem::GetSaveDirectoryPath();
	const FString Stem = FString::Printf(TEXT("SharedWorld_%s"), *WorldId);
	for (const TCHAR* Ext : { TEXT(".png"), TEXT(".jpg"), TEXT(".jpeg") })
	{
		const FString Candidate = FPaths::Combine(SaveDir, Stem + Ext);
		if (FPaths::FileExists(Candidate))
		{
			Tex = FImageUtils::ImportFileAsTexture2D(Candidate);
			if (Tex) break;
		}
	}
	ThumbCache.Add(WorldId, Tex);
	return Tex;
}

void USharedWorldBrowserWidget::AddSectionTitle(UVerticalBox* Col, const FText& Title)
{
	UTextBlock* H = MakeText(WidgetTree, 15, TextMuted, true);
	H->SetText(Title);
	Col->AddChildToVerticalBox(H)->SetPadding(FMargin(2.f, 12.f, 0.f, 8.f));
}

void USharedWorldBrowserWidget::AddMutedLine(UVerticalBox* Col, const FText& Text)
{
	UTextBlock* T = MakeText(WidgetTree, 14, TextMuted);
	T->SetText(Text);
	Col->AddChildToVerticalBox(T)->SetPadding(FMargin(4.f, 0.f, 0.f, 10.f));
}

UButton* USharedWorldBrowserWidget::AddActionRow(UVerticalBox* Col, const FText& Label, ESharedWorldButtonRole Role, int32 Height)
{
	TObjectPtr<UTextBlock> Lbl;
	UButton* B = MakeRoleButton(WidgetTree, Role, Lbl, Label, 15, FMargin(14.f, 0.f));
	Lbl->SetJustification(ETextJustify::Left);
	if (UButtonSlot* BS = Cast<UButtonSlot>(B->GetContentSlot()))
	{
		BS->SetHorizontalAlignment(HAlign_Left);
		BS->SetVerticalAlignment(VAlign_Center);
	}
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>();
	Box->SetHeightOverride(static_cast<float>(Height));
	Box->AddChild(B);
	if (UVerticalBoxSlot* S = Col->AddChildToVerticalBox(Box))
	{
		S->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		S->SetHorizontalAlignment(HAlign_Fill);
	}
	return B;
}

void USharedWorldBrowserWidget::AddContextActions(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	UBorder* Panel = MakePanel(WidgetTree, FLinearColor(0.045f, 0.05f, 0.06f, 0.92f), PanelEdge, FMargin(10.f, 10.f, 10.f, 4.f), 5.f, 1.f);
	Col->AddChildToVerticalBox(Panel)->SetPadding(FMargin(28.f, 0.f, 0.f, 10.f));
	UVerticalBox* Inner = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->SetContent(Inner);

	AddActionRow(Inner, NSLOCTEXT("SharedWorld", "MoreDetails", "Details"), ESharedWorldButtonRole::Secondary)
		->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsSelected);
	if (Item.bCanInvite)
	{
		AddActionRow(Inner, NSLOCTEXT("SharedWorld", "MoreInvite", "Invite Players (copy code)"), ESharedWorldButtonRole::Secondary)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnMoreInvite);
	}
	const FString LocalSav = FPaths::Combine(UFGSaveSystem::GetSaveDirectoryPath(), FString::Printf(TEXT("SharedWorld_%s.sav"), *Item.Id()));
	if (FPaths::FileExists(LocalSav))
	{
		AddActionRow(Inner, NSLOCTEXT("SharedWorld", "MoreOpenFolder", "Open Local Save Location"), ESharedWorldButtonRole::Secondary)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnMoreOpenFolder);
	}
	AddActionRow(Inner, NSLOCTEXT("SharedWorld", "MoreRefresh", "Force Refresh"), ESharedWorldButtonRole::Secondary)
		->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRefresh);
	if (Item.bCanRemove)
	{
		AddActionRow(Inner, NSLOCTEXT("SharedWorld", "MoreRemove", "Remove from list"), ESharedWorldButtonRole::Danger)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRemoveSelected);
	}
}

void USharedWorldBrowserWidget::AddWorldItems(UVerticalBox* Col, const TArray<FSharedWorldBrowserItem>& Items, bool bAllowMore)
{
	UUniformGridPanel* Grid = nullptr;
	int32 Cols = 1;
	if (bGridView)
	{
		// Fixed columns sized from the screen: bounded width, equal-height tiles, no overflow.
		const float W = UWidgetLayoutLibrary::GetViewportSize(this).X / FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
		Cols = GridColumns(280.f, (W >= 1100.f && bAllowMore) ? 0.703f : 1.f, 100.f, 5);
		Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
		Grid->SetSlotPadding(FMargin(0.f, 0.f, 12.f, 12.f));
		Col->AddChildToVerticalBox(Grid)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}
	int32 Index = 0;
	for (const FSharedWorldBrowserItem& Item : Items)
	{
		USharedWorldWorldCard* Card = CreateWidget<USharedWorldWorldCard>(this, USharedWorldWorldCard::StaticClass());
		if (!Card) continue;
		Card->Setup(Item, this, Item.Id() == SelectedWorldId,
			bGridView ? ESharedWorldCardLayout::Tile : ESharedWorldCardLayout::Row, GetThumbnail(Item.Id()), bAllowMore);
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
			Col->AddChildToVerticalBox(Card)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			if (MoreMenuWorldId == Item.Id())
			{
				AddContextActions(Col, Item);
			}
		}
	}
}

void USharedWorldBrowserWidget::PopulateMainList(UVerticalBox* List, FSharedWorldDiscoverySnapshot& Snap, FSharedWorldBrowserItem& OutSelected, bool& bOutHasSelected)
{
	bOutHasSelected = false;
	if (!List || !WidgetTree) return;
	USharedWorldSubsystem* S = SW();
	if (!S) return;

	Snap = S->Discovery().BuildSnapshot();
	const FSharedWorldBrowserSections Sec = SharedWorldBrowserModel::Build(Snap.AllWorlds, FilterText, S->GetMostRecentlyPlayedWorldId());

	// Selection: keep it if still visible, otherwise fall back to the first visible world.
	const FSharedWorldBrowserItem* Sel = Sec.Find(SelectedWorldId);
	if (!Sel)
	{
		Sel = Sec.First();
		if (Sel) SelectedWorldId = Sel->Id();
	}
	if (Sel)
	{
		OutSelected = *Sel;
		bOutHasSelected = true;
	}
	if (!MoreMenuWorldId.IsEmpty() && !Sec.Find(MoreMenuWorldId))
	{
		MoreMenuWorldId.Reset();
	}

	// ---- banners (human-readable; technical detail stays in the log)
	auto AddBanner = [&](const FText& Message, const FText& Sub)
	{
		UButton* Retry = nullptr;
		UBorder* Banner = MakeNoticePanel(WidgetTree, ESharedWorldTone::Problem, Message, Sub, Retry, NSLOCTEXT("SharedWorld", "Retry", "Retry"));
		List->AddChildToVerticalBox(Banner)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		if (Retry) Retry->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRefresh);
	};

	if (!Snap.ErrorMessage.IsEmpty())
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=browser_settings_problem detail=%s"), *Snap.ErrorMessage);
		AddBanner(NSLOCTEXT("SharedWorld", "ListProblem", "Your Shared World list could not be fully loaded."),
			NSLOCTEXT("SharedWorld", "ListProblemSub", "Worlds you already have are still shown."));
	}
	bool bAnyUnreachable = false;
	for (const FSharedWorldEntryView& V : Snap.AllWorlds)
	{
		if (V.CloudStatus == TEXT("UNREACHABLE"))
		{
			bAnyUnreachable = true;
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=browser_world_unreachable world=%s problem=%s"), *V.WorldId, *V.Problem);
		}
	}
	if (bAnyUnreachable && !Snap.bRefreshing)
	{
		AddBanner(NSLOCTEXT("SharedWorld", "RefreshFailed", "Unable to refresh Shared Worlds."),
			NSLOCTEXT("SharedWorld", "RefreshFailedSub", "Your local worlds are still available."));
	}
	if (Snap.bRefreshing)
	{
		UTextBlock* Loading = MakeText(WidgetTree, 14, Info);
		Loading->SetText(NSLOCTEXT("SharedWorld", "RefreshingLine", "Refreshing Shared Worlds..."));
		List->AddChildToVerticalBox(Loading)->SetPadding(FMargin(4.f, 0.f, 0.f, 8.f));
	}
	if (Snap.PendingInvites.Num() > 0)
	{
		AddSectionTitle(List, NSLOCTEXT("SharedWorld", "Invites", "INVITATIONS"));
		AddInviteRows(List, Snap.PendingInvites);
	}

	// ---- empty states
	if (Sec.TotalBeforeFilter == 0)
	{
		UButton* Create = nullptr;
		UBorder* Empty = MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "NoWorlds", "You don't have any Shared Worlds yet."),
			NSLOCTEXT("SharedWorld", "NoWorldsSub", "Create a Shared World from one of your Satisfactory saves and invite your friends."),
			Create, NSLOCTEXT("SharedWorld", "CreateSharedWorld", "Create Shared World"));
		List->AddChildToVerticalBox(Empty)->SetPadding(FMargin(0.f, 20.f, 0.f, 0.f));
		if (Create) Create->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateClicked);
		return;
	}
	const bool bFiltering = !FilterText.TrimStartAndEnd().IsEmpty();
	if (Sec.Num() == 0)
	{
		UTextBlock* T = MakeText(WidgetTree, 16, TextMuted);
		T->SetText(FText::Format(NSLOCTEXT("SharedWorld", "NoMatch", "No Shared Worlds match \"{0}\"."), FText::FromString(FilterText.TrimStartAndEnd())));
		List->AddChildToVerticalBox(T)->SetPadding(FMargin(4.f, 16.f, 0.f, 0.f));
		return;
	}

	// ---- sections (each world appears in exactly one)
	if (Sec.YourWorlds.Num() > 0 || !bFiltering)
	{
		AddSectionTitle(List, NSLOCTEXT("SharedWorld", "YourWorlds", "YOUR WORLDS"));
		if (Sec.YourWorlds.Num() > 0) AddWorldItems(List, Sec.YourWorlds);
		else AddMutedLine(List, NSLOCTEXT("SharedWorld", "NoYourWorlds", "You haven't created a Shared World yet."));
	}
	if (Sec.SharedWithYou.Num() > 0 || !bFiltering)
	{
		AddSectionTitle(List, NSLOCTEXT("SharedWorld", "SharedWithYou", "SHARED WITH YOU"));
		if (Sec.SharedWithYou.Num() > 0) AddWorldItems(List, Sec.SharedWithYou);
		else AddMutedLine(List, NSLOCTEXT("SharedWorld", "NoSharedWithYou", "No worlds have been shared with you yet."));
	}
	if (Sec.FriendsPlaying.Num() > 0 || !bFiltering)
	{
		AddSectionTitle(List, NSLOCTEXT("SharedWorld", "FriendsPlaying", "FRIENDS PLAYING"));
		if (Sec.FriendsPlaying.Num() > 0) AddWorldItems(List, Sec.FriendsPlaying);
		else AddMutedLine(List, NSLOCTEXT("SharedWorld", "NobodyPlaying", "None of your Shared Worlds are currently being hosted."));
	}
}

void USharedWorldBrowserWidget::PopulateMainDetail(UVerticalBox* Detail, const FSharedWorldBrowserItem& Item, bool bHasSelected)
{
	if (!Detail || !WidgetTree) return;
	PlayButton = nullptr;
	PlayLabel = nullptr;

	if (!bHasSelected)
	{
		UTextBlock* EmptyTitle = MakeText(WidgetTree, 24, TextPrimary, true);
		EmptyTitle->SetText(NSLOCTEXT("SharedWorld", "NoWorldSelected", "Shared Worlds"));
		Detail->AddChildToVerticalBox(EmptyTitle)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		UTextBlock* EmptyBody = MakeText(WidgetTree, 15, TextMuted);
		EmptyBody->SetText(NSLOCTEXT("SharedWorld", "EmptyDetail", "Select a world to see who is online, who is hosting and when it was last played."));
		Detail->AddChildToVerticalBox(EmptyBody);
		return;
	}

	const FSharedWorldEntryView& V = Item.View;

	// ---- preview
	{
		USizeBox* ThumbBox = WidgetTree->ConstructWidget<USizeBox>();
		ThumbBox->SetHeightOverride(230.f);
		ThumbBox->SetClipping(EWidgetClipping::ClipToBounds);
		ThumbBox->AddChild(USharedWorldWorldCard::BuildThumbnail(WidgetTree, Item.Id(), Item.DisplayName(), GetThumbnail(Item.Id()), 96));
		Detail->AddChildToVerticalBox(ThumbBox)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	}

	// ---- name + status
	UTextBlock* Name = MakeText(WidgetTree, 28, TextPrimary, true);
	Name->SetText(FText::FromString(Item.DisplayName()));
	Detail->AddChildToVerticalBox(Name)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	{
		UHorizontalBox* StatusRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		Detail->AddChildToVerticalBox(StatusRow)->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
		StatusRow->AddChildToHorizontalBox(MakeToneIcon(WidgetTree, Item.Tone, 14.f))->SetVerticalAlignment(VAlign_Center);
		UTextBlock* St = MakeText(WidgetTree, 16, TextPrimary);
		St->SetText(FText::FromString(Item.DetailStatus));
		if (UHorizontalBoxSlot* S = StatusRow->AddChildToHorizontalBox(St))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
			S->SetVerticalAlignment(VAlign_Center);
		}
	}

	// ---- primary + details
	{
		TObjectPtr<UTextBlock> PlayLbl;
		UButton* PlayBtn = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Game, PlayLbl, Item.ActionLabel, 23, FMargin(0.f, 10.f));
		PlayBtn->SetIsEnabled(Item.bActionEnabled);
		PlayBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnPlaySelected);
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>();
		Box->SetHeightOverride(58.f);
		Box->AddChild(PlayBtn);
		AddFillRow(Detail, Box, 8.f);
		PlayButton = PlayBtn;
		PlayLabel = PlayLbl;
	}
	AddActionRow(Detail, NSLOCTEXT("SharedWorld", "Details", "Details"), ESharedWorldButtonRole::Secondary, 48)
		->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsSelected);

	// ---- error with a way out
	if (V.bHasError)
	{
		UBorder* Box = MakePanel(WidgetTree, DangerBg, FLinearColor(1.f, 0.38f, 0.32f, 0.5f), FMargin(12.f, 10.f), 5.f, 1.f);
		Detail->AddChildToVerticalBox(Box)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Box->SetContent(Col);
		UTextBlock* T = MakeText(WidgetTree, 14, TextPrimary);
		T->SetText(FText::FromString(V.ErrorMessage));
		Col->AddChildToVerticalBox(T)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		AddActionRow(Col, NSLOCTEXT("SharedWorld", "DismissError", "Dismiss"), ESharedWorldButtonRole::Secondary, 38)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDismissError);
	}

	// ---- host / migration
	const bool bMigrating = Item.Status == ESharedWorldStatus::Migrating;
	const bool bRecovering = Item.Status == ESharedWorldStatus::Recovering;
	if ((V.IsHostingNow() && !V.HostName.IsEmpty()) || bMigrating || bRecovering)
	{
		UBorder* Box = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(14.f, 12.f), 5.f, 1.f);
		Detail->AddChildToVerticalBox(Box)->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Box->SetContent(Col);
		UTextBlock* Hdr = MakeText(WidgetTree, 13, TextMuted, true);
		Hdr->SetText(bMigrating ? NSLOCTEXT("SharedWorld", "HostMigration", "HOST MIGRATION") : NSLOCTEXT("SharedWorld", "HostHdr", "HOST"));
		Col->AddChildToVerticalBox(Hdr)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		if (bMigrating || bRecovering)
		{
			UTextBlock* T = MakeText(WidgetTree, 15, Warn);
			T->SetText(FText::FromString(V.HostName.IsEmpty() ? FString(TEXT("Finding best available host...")) : V.FriendlyStatusLine()));
			Col->AddChildToVerticalBox(T);
		}
		else
		{
			UTextBlock* Host = MakeText(WidgetTree, 19, TextPrimary, true);
			Host->SetText(FText::FromString(V.HostName));
			Col->AddChildToVerticalBox(Host)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
			if (V.PingMs >= 0)
			{
				AddDetailStatRow(Col, NSLOCTEXT("SharedWorld", "StatPing", "Ping"), FText::FromString(FString::Printf(TEXT("%d ms"), V.PingMs)), TextPrimary);
			}
			AddDetailStatRow(Col, NSLOCTEXT("SharedWorld", "StatPlayers", "Players"),
				FText::FromString(FString::Printf(TEXT("%d/%d"), V.PlayerCount, V.MaxPlayers > 0 ? V.MaxPlayers : 4)), TextPrimary);
			const bool bReady = V.CloudStatus == TEXT("ONLINE") || V.CloudStatus == TEXT("SAVING");
			AddDetailStatRow(Col, NSLOCTEXT("SharedWorld", "StatReady", "Host"),
				bReady ? NSLOCTEXT("SharedWorld", "HostReady", "Ready") : NSLOCTEXT("SharedWorld", "HostStarting", "Starting..."),
				bReady ? Ok : Warn);
		}
	}

	// ---- who is online
	if (V.OnlinePlayerNames.Num() > 0)
	{
		UTextBlock* Hdr = MakeText(WidgetTree, 13, TextMuted, true);
		Hdr->SetText(NSLOCTEXT("SharedWorld", "OnlineNow", "ONLINE NOW"));
		Detail->AddChildToVerticalBox(Hdr)->SetPadding(FMargin(0.f, 16.f, 0.f, 6.f));
		for (const FString& P : V.OnlinePlayerNames)
		{
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			Detail->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
			Row->AddChildToHorizontalBox(MakeToneIcon(WidgetTree, ESharedWorldTone::Healthy, 10.f))->SetVerticalAlignment(VAlign_Center);
			UTextBlock* T = MakeText(WidgetTree, 15, TextPrimary);
			T->SetText(FText::FromString(!V.HostName.IsEmpty() && P.Equals(V.HostName, ESearchCase::IgnoreCase)
				? FString::Printf(TEXT("%s  (host)"), *P) : P));
			if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(T))
			{
				S->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
			}
		}
	}

	// ---- "..." actions for grid mode live here (list mode shows them under the row)
	if (bGridView && MoreMenuWorldId == Item.Id())
	{
		UVerticalBox* Wrap = WidgetTree->ConstructWidget<UVerticalBox>();
		Detail->AddChildToVerticalBox(Wrap)->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
		AddContextActions(Wrap, Item);
	}

	// ---- management (wording matches ForgetWorld: this PC's list only)
	if (Item.bCanRemove)
	{
		UBorder* Sep = WidgetTree->ConstructWidget<UBorder>();
		Sep->SetBrush(RoundedBrush(PanelEdge, 0.f));
		USizeBox* SepBox = WidgetTree->ConstructWidget<USizeBox>();
		SepBox->SetHeightOverride(1.f);
		SepBox->AddChild(Sep);
		Detail->AddChildToVerticalBox(SepBox)->SetPadding(FMargin(0.f, 18.f, 0.f, 14.f));

		AddActionRow(Detail, NSLOCTEXT("SharedWorld", "RemoveFromList", "Remove from list"), ESharedWorldButtonRole::Danger, 46)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRemoveSelected);
		UTextBlock* RemoveHint = MakeText(WidgetTree, 13, TextMuted);
		RemoveHint->SetText(NSLOCTEXT("SharedWorld", "RemoveHint",
			"Remove this world from this PC only. Your cloud save and other players are not affected."));
		Detail->AddChildToVerticalBox(RemoveHint)->SetPadding(FMargin(2.f, 2.f, 0.f, 0.f));
	}

	UTextBlock* Hint = MakeText(WidgetTree, 13, TextMuted);
	Hint->SetText(Item.Action == ESharedWorldAction::Join
		? NSLOCTEXT("SharedWorld", "JoinHint", "A friend is hosting. Join connects you to their game.")
		: NSLOCTEXT("SharedWorld", "PlayHint", "Play puts you in this world. If nobody is hosting, you become host."));
	Detail->AddChildToVerticalBox(Hint)->SetPadding(FMargin(2.f, 14.f, 0.f, 0.f));
}

void USharedWorldBrowserWidget::PlayWorld(const FString& WorldId)
{
	SelectedWorldId = WorldId;
	if (USharedWorldSubsystem* S = SW()) S->Play(WorldId);
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::ToggleMoreMenu(const FString& WorldId)
{
	SelectedWorldId = WorldId;
	MoreMenuWorldId = (MoreMenuWorldId == WorldId) ? FString() : WorldId;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::SetViewMode(bool bGrid)
{
	if (bGridView == bGrid) return;
	bGridView = bGrid;
	if (GConfig)
	{
		GConfig->SetString(TEXT("SharedWorld"), TEXT("BrowserView"), bGrid ? TEXT("Grid") : TEXT("List"), GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
	// The toggle highlight lives in the static header, so force a full rebuild rather than a soft refresh.
	ListScroll = nullptr;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnViewList() { SetViewMode(false); }
void USharedWorldBrowserWidget::OnViewGrid() { SetViewMode(true); }

void USharedWorldBrowserWidget::OnMoreInvite()
{
	USharedWorldSubsystem* S = SW();
	if (!S || SelectedWorldId.IsEmpty()) return;
	const FString Code = S->EnsureInviteCode(SelectedWorldId);
	if (Code.IsEmpty())
	{
		SetFlash(false, TEXT("Could not create an invite code for this world."));
		return;
	}
	FPlatformApplicationMisc::ClipboardCopy(*Code);
	SetFlash(true, FString::Printf(TEXT("Invite code %s copied. Friends can use Join Friend > Join Using Code."), *Code));
}

void USharedWorldBrowserWidget::OnMoreOpenFolder()
{
	FPlatformProcess::ExploreFolder(*UFGSaveSystem::GetSaveDirectoryPath());
}

void USharedWorldBrowserWidget::OnDismissError()
{
	if (USharedWorldSubsystem* S = SW(); S && !SelectedWorldId.IsEmpty())
	{
		S->Dismiss(SelectedWorldId);
	}
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnCreateClicked() { ShowCreateWizard(); }
void USharedWorldBrowserWidget::OnJoinFriendClicked() { ShowJoinFriend(); }
void USharedWorldBrowserWidget::OnSettingsClicked() { ShowAdvanced(); }
void USharedWorldBrowserWidget::OnJoinCodeClicked() { ShowJoinCode(); }
void USharedWorldBrowserWidget::OnRefresh()
{
	ThumbCache.Reset(); // re-check for screenshots too
	if (USharedWorldSubsystem* S = SW()) S->Discovery().BeginRefresh();
	// Progress is shown by the list's own "Refreshing Shared Worlds..." line; a flash here would never clear.
	// Soft-refresh in place if the list is already up; discovery OnChanged will refresh data.
	ScheduleRebuild();
}
void USharedWorldBrowserWidget::OnPlaySelected()
{
	if (SelectedWorldId.IsEmpty()) return;
	if (USharedWorldSubsystem* S = SW()) S->Play(SelectedWorldId);
}
void USharedWorldBrowserWidget::OnDetailsSelected()
{
	ShowDetails();
}
void USharedWorldBrowserWidget::OnToggleTechnicalDetails()
{
	bShowTechnicalDetails = !bShowTechnicalDetails;
	ScheduleRebuild();
}
void USharedWorldBrowserWidget::OnDetailsHistory()
{
	USharedWorldSubsystem* S = SW();
	if (!S || SelectedWorldId.IsEmpty() || bBusy) return;
	bBusy = true;
	DetailsHistoryText = TEXT("Loading save history...");
	DetailsTab = 2;
	ScheduleRebuild();
	const FString WorldId = SelectedWorldId;
	S->FetchHistory(WorldId, 20, [this, WorldId](bool bOk, const FString& Message)
	{
		bBusy = false;
		if (SelectedWorldId != WorldId) return;
		DetailsHistoryText = bOk ? Message : FString::Printf(TEXT("Failed: %s"), *Message);
		DetailsTab = 2;
		ScheduleRebuild();
	});
}
void USharedWorldBrowserWidget::OnDetailsRestore()
{
	USharedWorldSubsystem* S = SW();
	if (!S || !RestoreInput || SelectedWorldId.IsEmpty()) return;
	const int64 Rev = FCString::Atoi64(*RestoreInput->GetText().ToString().TrimStartAndEnd());
	if (Rev <= 0)
	{
		SetFlash(false, TEXT("Enter a valid revision number."));
		return;
	}
	const FString WorldId = SelectedWorldId;
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FSharedWorldModalSpec Spec;
	Spec.Tone = ESharedWorldTone::Warning;
	Spec.Title = FText::Format(NSLOCTEXT("SharedWorld", "RestoreTitle", "Restore revision {0}?"), FText::AsNumber(Rev));
	Spec.Body = NSLOCTEXT("SharedWorld", "RestoreBody",
		"This makes the chosen revision the newest one. Progress made since then stays in the history, but everyone will continue from the restored save.");
	Spec.ConfirmLabel = NSLOCTEXT("SharedWorld", "RestoreConfirm", "Restore");
	Spec.CancelLabel = NSLOCTEXT("SharedWorld", "Cancel", "Cancel");
	Spec.ConfirmRole = ESharedWorldButtonRole::Danger;
	Spec.OnConfirm = [Weak, WorldId, Rev]()
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		if (!Self) return;
		if (USharedWorldSubsystem* Sub = Self->SW()) Sub->Restore(WorldId, Rev);
		Self->SetFlash(true, FString::Printf(TEXT("Restoring revision %lld as a new revision..."), Rev));
	};
	USharedWorldModal::Show(GetOwningPlayer(), Spec);
}
void USharedWorldBrowserWidget::OnRemoveSelected()
{
	if (SelectedWorldId.IsEmpty()) return;
	FString Name = SelectedWorldId;
	if (USharedWorldSubsystem* S = SW())
	{
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId == SelectedWorldId) { Name = V.WorldName.IsEmpty() ? V.WorldId : V.WorldName; break; }
		}
	}
	const FString WorldId = SelectedWorldId;
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FSharedWorldModalSpec Spec;
	Spec.Tone = ESharedWorldTone::Warning;
	Spec.Title = NSLOCTEXT("SharedWorld", "RemoveTitle", "Remove Shared World?");
	Spec.Body = FText::Format(NSLOCTEXT("SharedWorld", "RemoveBody",
		"{0} will be removed from this PC.\n\nThe shared cloud save and other players will not be affected."), FText::FromString(Name));
	Spec.ConfirmLabel = NSLOCTEXT("SharedWorld", "RemoveConfirm", "Remove");
	Spec.CancelLabel = NSLOCTEXT("SharedWorld", "Cancel", "Cancel");
	Spec.ConfirmRole = ESharedWorldButtonRole::Danger;
	Spec.OnConfirm = [Weak, WorldId]()
	{
		if (USharedWorldBrowserWidget* Self = Weak.Get()) Self->RemoveWorldFromList(WorldId);
	};
	if (!USharedWorldModal::Show(GetOwningPlayer(), Spec))
	{
		// No player controller to host the dialog: do not remove silently.
		SetFlash(false, TEXT("Could not open the confirmation dialog."));
	}
}
void USharedWorldBrowserWidget::OnLinkGitHubClicked()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	if (!S->GetGitHubLogin().IsEmpty())
	{
		ShowAdvanced();
		return;
	}
	Page = EPage::LinkGitHub;
	S->BeginGitHubSignIn();
	RebuildPage();
}
void USharedWorldBrowserWidget::OnOpenGitHubVerify()
{
	if (USharedWorldSubsystem* S = SW())
	{
		const FSharedWorldSignIn Sign = S->GetSignInStatus();
		if (!Sign.VerificationUri.IsEmpty())
		{
			FPlatformProcess::LaunchURL(*Sign.VerificationUri, nullptr, nullptr);
		}
	}
}
void USharedWorldBrowserWidget::OnCopyGitHubCode()
{
	if (USharedWorldSubsystem* S = SW())
	{
		const FSharedWorldSignIn Sign = S->GetSignInStatus();
		if (!Sign.UserCode.IsEmpty())
		{
			FPlatformApplicationMisc::ClipboardCopy(*Sign.UserCode);
			SetFlash(true, TEXT("Code copied."));
		}
	}
}
void USharedWorldBrowserWidget::OnCancelGitHubLink()
{
	if (USharedWorldSubsystem* S = SW()) S->CancelGitHubSignIn();
	ShowAdvanced();
}
void USharedWorldBrowserWidget::OnDisconnectGitHub()
{
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FSharedWorldModalSpec Spec;
	Spec.Tone = ESharedWorldTone::Warning;
	Spec.Title = NSLOCTEXT("SharedWorld", "DisconnectTitle", "Disconnect GitHub?");
	Spec.Body = NSLOCTEXT("SharedWorld", "DisconnectBody",
		"Shared Worlds won't be able to upload or download saves until you link storage again.\n\nYour worlds and saves in the cloud are not deleted.");
	Spec.ConfirmLabel = NSLOCTEXT("SharedWorld", "DisconnectConfirm", "Disconnect");
	Spec.CancelLabel = NSLOCTEXT("SharedWorld", "Cancel", "Cancel");
	Spec.ConfirmRole = ESharedWorldButtonRole::Danger;
	Spec.OnConfirm = [Weak]()
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		if (!Self) return;
		if (USharedWorldSubsystem* S = Self->SW()) S->SignOutOfGitHub();
		Self->bLastTestRan = false;
		Self->SelectedProviderId = TEXT("local-folder");
		Self->PendingFlash = TEXT("GitHub disconnected.");
		Self->bPendingFlashOk = true;
		Self->Page = EPage::Settings;
		Self->ListScroll = nullptr;
		Self->ScheduleRebuild();
	};
	USharedWorldModal::Show(GetOwningPlayer(), Spec);
}
void USharedWorldBrowserWidget::OnTestGitHubAccess()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	bBusy = true;
	SetFlash(true, TEXT("Testing GitHub access..."));
	ListScroll = nullptr;
	ScheduleRebuild(); // shows "Testing..." on the button
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	S->TestGitHubAccess([Weak](bool bOk, const FString& Message)
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		if (!Self) return;
		Self->bBusy = false;
		// Real result of the real call: this is what "Last Checked" and the connection check row show.
		Self->bLastTestRan = true;
		Self->bLastTestOk = bOk;
		Self->LastTestTime = FDateTime::UtcNow();
		Self->PendingFlash = Message;
		Self->bPendingFlashOk = bOk;
		Self->ListScroll = nullptr;
		Self->ScheduleRebuild();
	});
}
void USharedWorldBrowserWidget::OnFilterChanged(const FText& Text)
{
	FilterText = Text.ToString().TrimStartAndEnd();
	if (Page == EPage::Main) RebuildPage();
}
void USharedWorldBrowserWidget::HandleInvite(const FString& InviteId, bool bAccept)
{
	PendingInviteId = InviteId;
	if (bAccept) OnAcceptInvite();
	else OnDeclineInvite();
}

void USharedWorldBrowserWidget::OnAcceptInvite()
{
	if (PendingInviteId.IsEmpty()) return;
	if (USharedWorldSubsystem* S = SW())
	{
		S->Invites().AcceptInvite(PendingInviteId, [this](bool bOk, const FString& Message)
		{
			SetFlash(bOk, Message);
			RebuildPage();
		});
	}
}
void USharedWorldBrowserWidget::OnDeclineInvite()
{
	if (PendingInviteId.IsEmpty()) return;
	if (USharedWorldSubsystem* S = SW())
	{
		S->Invites().DeclineInvite(PendingInviteId, [this](bool bOk, const FString& Message)
		{
			SetFlash(bOk, Message);
			RebuildPage();
		});
	}
}

// ============================================================================ page dispatch / navigation

bool USharedWorldBrowserWidget::TrySoftRefresh()
{
	if (!ListScroll || ListScrollOwner != Page) return false;
	switch (Page)
	{
	case EPage::Main: return SoftRefreshMain();
	case EPage::Details: return SoftRefreshDetails();
	default: return false; // other pages hold typed input; they are rebuilt only when needed
	}
}

void USharedWorldBrowserWidget::OnBackendChanged()
{
	// The create wizard and invite-code form hold text the player is typing: a background cloud
	// refresh must never rebuild them. Everything else follows the backend through this event.
	switch (Page)
	{
	case EPage::CreatePickSave:
	case EPage::CreateName:
	case EPage::CreateReview:
		return;
	case EPage::JoinFriend:
		if (JoinTab == 2) return;
		break;
	default:
		break;
	}
	// Rebuilding recreates every widget (hover, focus and scroll are lost, the page flickers). Backend events fire
	// every few seconds whether or not anything this page shows changed, so only rebuild on a real change.
	const FString Sig = ComputePageSignature();
	if (!Sig.IsEmpty() && Sig == PageSignature) return;
	PageSignature = Sig;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::RebuildPageNow()
{
	if (!PageRoot || !WidgetTree) return;
	if (USharedWorldSubsystem* S = SW())
	{
		if (!SelectedWorldId.IsEmpty() && !S->FindWorldEntry(SelectedWorldId))
		{
			SelectedWorldId.Reset();
		}
	}
	UpdateWidthClamp();
	// Prefer refreshing scroll contents in place so the scrollbar does not jump.
	if (TrySoftRefresh())
	{
		PageSignature = ComputePageSignature();
		return;
	}

	CaptureListScroll();
	++ScrollRestoreGeneration;
	bPendingScrollRestore = false;
	PageRoot->ClearChildren();
	RowBinders.Reset();
	FlashText = nullptr;
	StatusText = nullptr;
	FilterInput = nullptr;
	NameInput = nullptr;
	CodeInput = nullptr;
	RestoreInput = nullptr;
	PlayButton = nullptr;
	PlayLabel = nullptr;
	ListScroll = nullptr;
	MainListBox = nullptr;
	MainDetailBox = nullptr;
	ScrollContentBox = nullptr;
	PageBody = nullptr;

	if (Page == EPage::Main)
	{
		PageBody = PageRoot;
		RebuildMain();
	}
	else
	{
		// Every sub-page shares one panel and one header, so the whole mod reads as one system.
		UBorder* Shell = MakePanel(WidgetTree, PanelFill, PanelEdge, FMargin(30.f, 22.f, 30.f, 18.f), RadiusL, 1.f);
		PageRoot->AddChildToVerticalBox(Shell)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PageBody = WidgetTree->ConstructWidget<UVerticalBox>();
		Shell->SetContent(PageBody);
		switch (Page)
		{
		case EPage::Welcome: RebuildWelcomePage(); break;
		case EPage::Details: RebuildDetails(); break;
		case EPage::CreatePickSave: RebuildCreatePickSavePage(); break;
		case EPage::CreateName: RebuildCreateNamePage(); break;
		case EPage::CreateReview: RebuildCreateReviewPage(); break;
		case EPage::Creating: RebuildCreatingPage(); break;
		case EPage::JoinFriend: RebuildJoinPage(); break;
		case EPage::Settings: RebuildSettingsPage(); break;
		case EPage::LinkGitHub: RebuildLinkGitHubPage(); break;
		case EPage::Main:
		default: break;
		}
		if (!FlashText)
		{
			FlashText = MakeText(WidgetTree, FontSmall + 1, TextMuted);
			PageBody->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
		}
	}
	PageSignature = ComputePageSignature();
	if (!PendingFlash.IsEmpty() && FlashText)
	{
		SetFlash(bPendingFlashOk, PendingFlash);
		PendingFlash.Reset();
	}
}

/** Back, one level at a time: "..." menu -> sub-page (wizard step by step) -> browser. */
bool USharedWorldBrowserWidget::HandleBack()
{
	if (!MoreMenuWorldId.IsEmpty())
	{
		MoreMenuWorldId.Reset();
		ScheduleRebuild();
		return true;
	}
	switch (Page)
	{
	case EPage::Main:
		return false; // let the Satisfactory menu handle Back (leaves Shared Worlds)
	case EPage::CreateName:
		Page = EPage::CreatePickSave;
		break;
	case EPage::CreateReview:
		Page = EPage::CreateName;
		break;
	case EPage::Creating:
		return true; // an upload is running: Back is ignored rather than abandoning it
	case EPage::LinkGitHub:
		if (USharedWorldSubsystem* S = SW())
		{
			if (S->GetSignInStatus().bInProgress) S->CancelGitHubSignIn();
		}
		Page = EPage::Settings;
		break;
	default:
		Page = EPage::Main;
		break;
	}
	ScheduleRebuild();
	return true;
}

FReply USharedWorldBrowserWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right)
	{
		if (HandleBack()) return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void USharedWorldBrowserWidget::OnBack() { HandleBack(); }

void USharedWorldBrowserWidget::SetTab(int32 Kind, int32 Index)
{
	switch (Kind)
	{
	case 1: JoinTab = Index; break;
	case 2: SettingsTab = Index; break;
	case 3: DetailsTab = Index; break;
	default: return;
	}
	// Tab bars live inside rebuilt content: page 1/2 rebuild fully, details refreshes in place.
	if (Kind != 3) ListScroll = nullptr;
	ScheduleRebuild();
}

int32 USharedWorldBrowserWidget::GridColumns(float MinCardWidth, float ColumnFraction, float Chrome, int32 MaxCols)
{
	float W = 3840.f;
	const FVector2D VS = UWidgetLayoutLibrary::GetViewportSize(this);
	const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
	if (VS.X > 1.f) W = VS.X / Scale;
	const float Avail = W * ColumnFraction - Chrome;
	return FMath::Clamp(FMath::FloorToInt(Avail / FMath::Max(MinCardWidth, 1.f)), 1, FMath::Max(MaxCols, 1));
}

namespace
{
	/** Everything the browser shows about one world, as a string (for "did anything visible change?"). */
	FString WorldSig(const FSharedWorldEntryView& V)
	{
		return FString::Printf(TEXT("%s|%s|%s|%s|%d|%lld|%s|%s|%d|%s|%d|%s|%d|%s;"),
			*V.WorldId, *V.WorldName, *V.CloudStatus, *V.HostName, V.PlayerCount, V.Revision, *V.LastPlayed,
			*V.LocalState, V.bHasError ? 1 : 0, *V.ErrorMessage, V.PingMs, *FString::Join(V.OnlinePlayerNames, TEXT(",")),
			V.bCreating ? 1 : 0, *V.LocalMessage);
	}

	FString SignInSig(USharedWorldSubsystem& S)
	{
		const FSharedWorldSignIn Sign = S.GetSignInStatus();
		return FString::Printf(TEXT("%d|%d|%s|%s|%s|%s"), static_cast<int32>(Sign.State), Sign.bInProgress ? 1 : 0,
			*Sign.UserCode, *Sign.Error, *S.GetGitHubLogin(), *Sign.PlayerMessage);
	}
}

FString USharedWorldBrowserWidget::ComputePageSignature() const
{
	USharedWorldSubsystem* S = SW();
	if (!S) return FString();
	switch (Page)
	{
	case EPage::Main:
	{
		FString Sig = S->IsDiscoveryRefreshing() ? TEXT("R;") : TEXT("I;");
		Sig += S->GetSettingsProblem();
		for (const FSharedWorldPendingInviteView& I : S->GetPendingInviteViews()) Sig += I.InviteId + TEXT(";");
		for (const FSharedWorldEntryView& V : S->GetWorldViews()) Sig += WorldSig(V);
		return Sig;
	}
	case EPage::Details:
	{
		FString Sig = FString::Printf(TEXT("%d%d%d|"), bDetailsMembersLoading ? 1 : 0, bDetailsMembersLoaded ? 1 : 0, DetailsMembersText.Len());
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId == SelectedWorldId) Sig += WorldSig(V);
		}
		return Sig;
	}
	case EPage::Settings:
	case EPage::LinkGitHub:
		return SignInSig(*S); // the only backend state these pages display
	case EPage::JoinFriend:
	{
		FString Sig;
		for (const FSharedWorldPendingInviteView& I : S->GetPendingInviteViews()) Sig += I.InviteId + TEXT(";");
		for (const FSharedWorldEntryView& V : S->GetWorldViews()) Sig += WorldSig(V);
		for (const FSharedWorldFriendInfo& F : S->Discovery().ListFriends()) Sig += F.DisplayName + (F.bOnline ? TEXT("+") : TEXT("-"));
		return Sig;
	}
	default:
		return FString(); // wizard / welcome / creating pages always rebuild when asked
	}
}
