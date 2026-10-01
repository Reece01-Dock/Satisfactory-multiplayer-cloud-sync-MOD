#include "UI/SharedWorldBrowserWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
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
		PageRoot = WidgetTree->ConstructWidget<UVerticalBox>();
		Root->SetContent(PageRoot);
	}
	return Super::RebuildWidget();
}

void USharedWorldBrowserWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (USharedWorldSubsystem* S = SW())
	{
		ChangedHandle = S->OnChanged.AddUObject(this, &USharedWorldBrowserWidget::ScheduleRebuild);
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

bool USharedWorldBrowserWidget::TrySoftRefresh()
{
	if (!ListScroll || ListScrollOwner != Page) return false;
	switch (Page)
	{
	case EPage::Main: return SoftRefreshMain();
	case EPage::Details: return SoftRefreshDetails();
	case EPage::CreatePickSave:
	case EPage::JoinFriend:
	case EPage::Advanced:
	{
		if (!ScrollContentBox) return false;
		const float Offset = ListScroll->GetScrollOffset();
		ScrollContentBox->ClearChildren();
		RowBinders.Reset();
		if (Page == EPage::CreatePickSave)
		{
			if (USharedWorldSubsystem* S = SW())
			{
				const TArray<FSharedWorldSaveInfo> Saves = S->Creation().ListLocalSaves();
				if (Saves.Num() == 0)
				{
					UTextBlock* Empty = MakeText(WidgetTree, 14, TextMuted);
					Empty->SetText(NSLOCTEXT("SharedWorld", "NoSaves", "No saves found."));
					ScrollContentBox->AddChildToVerticalBox(Empty);
				}
				for (const FSharedWorldSaveInfo& Save : Saves)
				{
					USharedWorldSavePickRow* Row = CreateWidget<USharedWorldSavePickRow>(this, USharedWorldSavePickRow::StaticClass());
					if (!Row) continue;
					ScrollContentBox->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
					FString Display = Save.SessionName.IsEmpty() ? Save.SaveName : Save.SessionName;
					if (Save.SaveName.Contains(TEXT("_autosave_")))
					{
						Display = FString::Printf(TEXT("%s — %s"), *Display, *Save.SaveName);
					}
					Row->Setup(Save.SaveName, Display, Save.LastPlayedText, this);
				}
			}
		}
		else if (Page == EPage::JoinFriend)
		{
			if (USharedWorldSubsystem* S = SW())
			{
				const FSharedWorldDiscoverySnapshot Snap = S->Discovery().BuildSnapshot();
				AddSectionHeader(ScrollContentBox, NSLOCTEXT("SharedWorld", "FriendsPlaying", "Friends Playing"));
				AddWorldRows(ScrollContentBox, Snap.FriendsPlaying, true);
				AddSectionHeader(ScrollContentBox, NSLOCTEXT("SharedWorld", "SharedWithYou", "Shared With You"));
				AddWorldRows(ScrollContentBox, Snap.SharedWithYou, false);
				AddSectionHeader(ScrollContentBox, NSLOCTEXT("SharedWorld", "Friends", "Friends"));
				if (Snap.Friends.Num() == 0)
				{
					UTextBlock* Hint = MakeText(WidgetTree, 13, TextMuted);
					Hint->SetText(NSLOCTEXT("SharedWorld", "NoFriends",
						"No online friends found yet. Ask a friend to Invite you, or use Join Using Code in Settings."));
					ScrollContentBox->AddChildToVerticalBox(Hint);
				}
				else
				{
					for (const FSharedWorldFriendInfo& F : Snap.Friends)
					{
						UTextBlock* Row = MakeText(WidgetTree, 14, TextPrimary);
						Row->SetText(FText::FromString(FString::Printf(TEXT("%s  ·  %s"),
							*F.DisplayName, F.bOnline ? TEXT("Online") : TEXT("Offline"))));
						ScrollContentBox->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
					}
				}
			}
		}
		else // Advanced — leave soft refresh to full rebuild (sign-in UI structure changes)
		{
			return false;
		}
		if (Offset > KINDA_SMALL_NUMBER)
		{
			switch (Page)
			{
			case EPage::Main: MainListScrollOffset = Offset; break;
			case EPage::Details: DetailsListScrollOffset = Offset; break;
			default: OtherListScrollOffset = Offset; break;
			}
			ListScroll->ForceLayoutPrepass();
			ListScroll->SetScrollOffset(Offset);
			RestoreListScroll(Page);
		}
		return true;
	}
	default: return false;
	}
}

bool USharedWorldBrowserWidget::SoftRefreshMain()
{
	if (!MainListBox || !MainDetailBox || !ListScroll) return false;
	const float Offset = ListScroll->GetScrollOffset();
	MainListBox->ClearChildren();
	MainDetailBox->ClearChildren();
	RowBinders.Reset();

	FSharedWorldDiscoverySnapshot Snap;
	FSharedWorldEntryView SelectedView;
	bool bHasSelected = false;
	PopulateMainList(MainListBox, Snap, SelectedView, bHasSelected);
	if (StatusText && SW())
	{
		StatusText->SetText(FText::FromString(Snap.StatusMessage));
	}
	PopulateMainDetail(MainDetailBox, SelectedView, bHasSelected);
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
	bDetailsMembersLoaded = false;
	bDetailsMembersLoading = false;
	DetailsMembersText.Reset();
	DetailsHistoryText.Reset();
	ScheduleRebuild();
	RequestDetailsMembers();
}
void USharedWorldBrowserWidget::ShowCreateWizard() { Page = EPage::CreateChoice; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowJoinFriend() { Page = EPage::JoinFriend; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowAdvanced() { Page = EPage::Advanced; ScheduleRebuild(); }
void USharedWorldBrowserWidget::ShowJoinCode() { Page = EPage::JoinCode; ScheduleRebuild(); }

void USharedWorldBrowserWidget::OnSavePicked(const FString& SaveName, const FString& DisplayName)
{
	SelectedSaveName = SaveName;
	SuggestedWorldName = DisplayName;
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
	const bool bHas = !SelectedWorldId.IsEmpty();
	if (PlayButton)
	{
		PlayButton->SetIsEnabled(bHas);
	}
	if (PlayLabel)
	{
		PlayLabel->SetColorAndOpacity(FSlateColor(bHas ? TextPrimary : TextMuted));
	}
}

void USharedWorldBrowserWidget::AddSectionHeader(UVerticalBox* Col, const FText& Title)
{
	UTextBlock* H = MakeText(WidgetTree, 14, TextMuted, true);
	H->SetText(Title);
	Col->AddChildToVerticalBox(H)->SetPadding(FMargin(0, 14, 0, 6));
}

void USharedWorldBrowserWidget::AddWorldRows(UVerticalBox* Col, const TArray<FSharedWorldEntryView>& Worlds, bool bJoinLabel, bool bShowRemove)
{
	if (Worlds.Num() == 0)
	{
		UTextBlock* Empty = MakeText(WidgetTree, 14, TextMuted);
		Empty->SetText(NSLOCTEXT("SharedWorld", "NoneYet", "None yet."));
		Col->AddChildToVerticalBox(Empty)->SetPadding(FMargin(0, 0, 0, 4));
		return;
	}
	for (const FSharedWorldEntryView& V : Worlds)
	{
		if (!FilterText.IsEmpty())
		{
			const FString Name = V.WorldName.IsEmpty() ? V.WorldId : V.WorldName;
			if (!Name.Contains(FilterText) && !V.HostName.Contains(FilterText)) continue;
		}
		const FString Display = V.WorldName.IsEmpty() ? V.WorldId : V.WorldName;

		UHorizontalBox* RowHBox = WidgetTree->ConstructWidget<UHorizontalBox>();
		Col->AddChildToVerticalBox(RowHBox)->SetPadding(FMargin(0, 0, 0, 2));

		UButton* RowBtn = nullptr;
		TObjectPtr<UTextBlock> RowLbl;
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, RowBtn, RowLbl, FText::FromString(Display), 48.f, 18);
		USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
		Binder->Browser = this;
		Binder->WorldId = V.WorldId;
		Binder->bPlayOnClick = false;
		RowBinders.Add(Binder);
		RowBtn->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
		if (UHorizontalBoxSlot* NameSlot = RowHBox->AddChildToHorizontalBox(Box))
		{
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		if (bShowRemove && V.bOwned)
		{
			UButton* RemoveBtn = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "RemoveShort", "Remove"), 13);
			USharedWorldRowBinder* RemoveBinder = NewObject<USharedWorldRowBinder>(this);
			RemoveBinder->Browser = this;
			RemoveBinder->WorldId = V.WorldId;
			RemoveBinder->bRemoveFromList = true;
			RowBinders.Add(RemoveBinder);
			RemoveBtn->OnClicked.AddDynamic(RemoveBinder, &USharedWorldRowBinder::OnClicked);
			if (UHorizontalBoxSlot* RemSlot = RowHBox->AddChildToHorizontalBox(RemoveBtn))
			{
				RemSlot->SetPadding(FMargin(8, 0, 4, 0));
				RemSlot->SetVerticalAlignment(VAlign_Center);
			}
		}
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
		PendingInviteId = I.InviteId;
		UButton* Accept = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Accept", "Accept"));
		Accept->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnAcceptInvite);
		Actions->AddChildToHorizontalBox(Accept)->SetPadding(FMargin(0, 0, 20, 0));
		UButton* Decline = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Decline", "Decline"));
		Decline->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDeclineInvite);
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
	// Prefer refreshing scroll contents in place so the scrollbar does not jump.
	if (TrySoftRefresh()) return;

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

	switch (Page)
	{
	case EPage::Welcome:
	{
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "WelcomeTitle", "Welcome to Shared Worlds"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 10));
		UTextBlock* Body = MakeText(WidgetTree, 14, TextMuted);
		Body->SetText(NSLOCTEXT("SharedWorld", "WelcomeBody",
			"Shared Worlds let you and your friends play the same factory together — even when hosts change.\n\n"
			"Connect cloud storage once (optional), or continue with a local shared folder."));
		PageRoot->AddChildToVerticalBox(Body)->SetPadding(FMargin(0, 0, 0, 20));
		TObjectPtr<UTextBlock> ConnLbl, ContLbl;
		UButton* Connect = MakePrimaryButton(WidgetTree, ConnLbl, NSLOCTEXT("SharedWorld", "Connect", "Link GitHub"));
		Connect->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnWelcomeConnect);
		PageRoot->AddChildToVerticalBox(Connect)->SetPadding(FMargin(0, 0, 0, 10));
		UButton* Cont = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "ContinueLocal", "Continue with local storage"));
		Cont->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnWelcomeContinue);
		PageRoot->AddChildToVerticalBox(Cont);
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
		break;
	}
	case EPage::Details:
		RebuildDetails();
		break;
	case EPage::CreateChoice:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "CreateTitle", "Create Shared World"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 8));
		UTextBlock* Q = MakeText(WidgetTree, 15, TextMuted);
		Q->SetText(NSLOCTEXT("SharedWorld", "CreateQ", "What would you like to do?"));
		PageRoot->AddChildToVerticalBox(Q)->SetPadding(FMargin(0, 0, 0, 16));
		TObjectPtr<UTextBlock> L1, L2;
		UButton* Existing = MakePrimaryButton(WidgetTree, L1, NSLOCTEXT("SharedWorld", "UseExisting", "Use Existing Save"));
		Existing->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnUseExistingSave);
		PageRoot->AddChildToVerticalBox(Existing)->SetPadding(FMargin(0, 0, 0, 10));
		UButton* NewGame = MakePrimaryButton(WidgetTree, L2, NSLOCTEXT("SharedWorld", "CreateNew", "Create New Game"));
		NewGame->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateNewGameHint);
		PageRoot->AddChildToVerticalBox(NewGame);
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
		break;
	}
	case EPage::CreatePickSave:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "SelectSave", "Select a Save"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 12));
		ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
		PageRoot->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
		ListScroll->AddChild(List);
		ScrollContentBox = List;
		if (USharedWorldSubsystem* S = SW())
		{
			const TArray<FSharedWorldSaveInfo> Saves = S->Creation().ListLocalSaves();
			if (Saves.Num() == 0)
			{
				UTextBlock* Empty = MakeText(WidgetTree, 14, TextMuted);
				Empty->SetText(NSLOCTEXT("SharedWorld", "NoSaves", "No saves found."));
				List->AddChildToVerticalBox(Empty);
			}
			for (const FSharedWorldSaveInfo& Save : Saves)
			{
				USharedWorldSavePickRow* Row = CreateWidget<USharedWorldSavePickRow>(this, USharedWorldSavePickRow::StaticClass());
				if (!Row) continue;
				List->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
				FString Display = Save.SessionName.IsEmpty() ? Save.SaveName : Save.SessionName;
				if (Save.SaveName.Contains(TEXT("_autosave_")))
				{
					Display = FString::Printf(TEXT("%s — %s"), *Display, *Save.SaveName);
				}
				Row->Setup(Save.SaveName, Display, Save.LastPlayedText, this);
			}
		}
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 8, 0, 0));
		ListScrollOwner = EPage::CreatePickSave;
		RestoreListScroll(EPage::CreatePickSave);
		break;
	}
	case EPage::CreateName:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "WorldNameTitle", "Shared World Name"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 12));
		NameInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		NameInput->SetText(FText::FromString(SuggestedWorldName.IsEmpty() ? SelectedSaveName : SuggestedWorldName));
		PageRoot->AddChildToVerticalBox(NameInput)->SetPadding(FMargin(0, 0, 0, 16));
		TObjectPtr<UTextBlock> CreateLbl;
		UButton* Create = MakePrimaryButton(WidgetTree, CreateLbl, NSLOCTEXT("SharedWorld", "Create", "Create"));
		Create->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateConfirm);
		PageRoot->AddChildToVerticalBox(Create);
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
		break;
	}
	case EPage::JoinFriend:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "JoinFriendTitle", "Join Friend"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 8));
		ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
		PageRoot->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
		ListScroll->AddChild(List);
		ScrollContentBox = List;
		if (USharedWorldSubsystem* S = SW())
		{
			const FSharedWorldDiscoverySnapshot Snap = S->Discovery().BuildSnapshot();
			AddSectionHeader(List, NSLOCTEXT("SharedWorld", "FriendsPlaying", "Friends Playing"));
			AddWorldRows(List, Snap.FriendsPlaying, true);
			AddSectionHeader(List, NSLOCTEXT("SharedWorld", "SharedWithYou", "Shared With You"));
			AddWorldRows(List, Snap.SharedWithYou, false);
			AddSectionHeader(List, NSLOCTEXT("SharedWorld", "Friends", "Friends"));
			if (Snap.Friends.Num() == 0)
			{
				UTextBlock* Hint = MakeText(WidgetTree, 13, TextMuted);
				Hint->SetText(NSLOCTEXT("SharedWorld", "NoFriends",
					"No online friends found yet. Ask a friend to Invite you, or use Join Using Code in Settings."));
				List->AddChildToVerticalBox(Hint);
			}
			else
			{
				for (const FSharedWorldFriendInfo& F : Snap.Friends)
				{
					UTextBlock* Row = MakeText(WidgetTree, 14, TextPrimary);
					Row->SetText(FText::FromString(FString::Printf(TEXT("%s  ·  %s"),
						*F.DisplayName, F.bOnline ? TEXT("Online") : TEXT("Offline"))));
					List->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
				}
			}
		}
		UButton* Code = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "JoinCodeLink", "Join Using Code..."));
		Code->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinCodeClicked);
		PageRoot->AddChildToVerticalBox(Code)->SetPadding(FMargin(0, 12, 0, 0));
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 8, 0, 0));
		ListScrollOwner = EPage::JoinFriend;
		RestoreListScroll(EPage::JoinFriend);
		break;
	}
	case EPage::JoinCode:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "JoinCodeTitle", "Join Using Code"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 8));
		UTextBlock* Hint = MakeText(WidgetTree, 13, TextMuted);
		Hint->SetText(NSLOCTEXT("SharedWorld", "JoinCodeHint", "Enter the share code from your friend (e.g. ABCD-EFGH)."));
		PageRoot->AddChildToVerticalBox(Hint)->SetPadding(FMargin(0, 0, 0, 12));
		CodeInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		CodeInput->SetHintText(NSLOCTEXT("SharedWorld", "CodeHint", "ABCD-EFGH"));
		PageRoot->AddChildToVerticalBox(CodeInput)->SetPadding(FMargin(0, 0, 0, 16));
		TObjectPtr<UTextBlock> JoinLbl;
		UButton* Join = MakePrimaryButton(WidgetTree, JoinLbl, NSLOCTEXT("SharedWorld", "Join", "Join"));
		Join->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinCodeSubmit);
		PageRoot->AddChildToVerticalBox(Join);
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
		break;
	}
	case EPage::Advanced:
	{
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));
		UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
		Title->SetText(NSLOCTEXT("SharedWorld", "AdvancedTitle", "Advanced Settings"));
		PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 12));
		if (USharedWorldSubsystem* S = SW())
		{
			FString Note;
			const sw::ProviderConfig P = S->ResolveDefaultStorage(Note);
			const FSharedWorldSignIn Sign = S->GetSignInStatus();
			const FString Login = S->GetGitHubLogin();

			UTextBlock* AccTitle = MakeText(WidgetTree, 16, TextPrimary, true);
			AccTitle->SetText(NSLOCTEXT("SharedWorld", "GitHubSection", "GitHub"));
			PageRoot->AddChildToVerticalBox(AccTitle)->SetPadding(FMargin(0, 0, 0, 4));

			UTextBlock* Acc = MakeText(WidgetTree, 14, TextPrimary);
			if (!Sign.bConfigured && Login.IsEmpty())
			{
				Acc->SetText(FText::FromString(TEXT("GitHub integration is not configured in this build.")));
			}
			else if (Sign.bInProgress && !Sign.UserCode.IsEmpty())
			{
				Acc->SetText(FText::FromString(FString::Printf(
					TEXT("Connect GitHub\nOpen GitHub and enter this code:\n\n%s\n\n%s"),
					*Sign.UserCode, *Sign.PlayerMessage)));
			}
			else if (Sign.bInProgress)
			{
				Acc->SetText(FText::FromString(Sign.PlayerMessage.IsEmpty() ? TEXT("Opening GitHub...") : Sign.PlayerMessage));
			}
			else if (!Login.IsEmpty())
			{
				Acc->SetText(FText::FromString(FString::Printf(TEXT("Connected as %s"), *Login)));
			}
			else
			{
				Acc->SetText(NSLOCTEXT("SharedWorld", "GitHubNotConnected", "Not connected"));
			}
			PageRoot->AddChildToVerticalBox(Acc)->SetPadding(FMargin(0, 0, 0, 8));

			if (Sign.bInProgress && !Sign.UserCode.IsEmpty())
			{
				TObjectPtr<UTextBlock> OpenLbl, CopyLbl, CancelLbl;
				UButton* Open = MakePrimaryButton(WidgetTree, OpenLbl, NSLOCTEXT("SharedWorld", "OpenGitHub", "Open GitHub"));
				Open->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnOpenGitHubVerify);
				PageRoot->AddChildToVerticalBox(Open)->SetPadding(FMargin(0, 0, 0, 6));
				UButton* Copy = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "CopyCode", "Copy Code"));
				Copy->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCopyGitHubCode);
				PageRoot->AddChildToVerticalBox(Copy)->SetPadding(FMargin(0, 0, 0, 6));
				UButton* Cancel = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "CancelLink", "Cancel"));
				Cancel->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCancelGitHubLink);
				PageRoot->AddChildToVerticalBox(Cancel)->SetPadding(FMargin(0, 0, 0, 10));
			}
			else if (Login.IsEmpty())
			{
				TObjectPtr<UTextBlock> LinkLbl;
				UButton* Link = MakePrimaryButton(WidgetTree, LinkLbl, NSLOCTEXT("SharedWorld", "LinkGitHub", "Link GitHub"));
				Link->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnLinkGitHubClicked);
				PageRoot->AddChildToVerticalBox(Link)->SetPadding(FMargin(0, 0, 0, 10));
			}
			else
			{
				UButton* Disconnect = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "DisconnectGitHub", "Disconnect"));
				Disconnect->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDisconnectGitHub);
				PageRoot->AddChildToVerticalBox(Disconnect)->SetPadding(FMargin(0, 0, 0, 6));
				UButton* Test = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "TestGitHub", "Test Connection"));
				Test->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnTestGitHubAccess);
				PageRoot->AddChildToVerticalBox(Test)->SetPadding(FMargin(0, 0, 0, 10));
			}

			if (!Sign.Error.IsEmpty() && !Sign.bInProgress)
			{
				UTextBlock* ErrMsg = MakeText(WidgetTree, 13, TextMuted);
				ErrMsg->SetText(FText::FromString(Sign.Error));
				PageRoot->AddChildToVerticalBox(ErrMsg)->SetPadding(FMargin(0, 0, 0, 10));
			}

			UTextBlock* Stor = MakeText(WidgetTree, 13, TextMuted);
			if (P.Kind == sw::ProviderKind::GitHub)
			{
				Stor->SetText(FText::FromString(FString::Printf(TEXT("Storage: GitHub %s/%s\n%s"),
					UTF8_TO_TCHAR(P.Owner.c_str()), UTF8_TO_TCHAR(P.Repo.c_str()), *Note)));
			}
			else
			{
				Stor->SetText(FText::FromString(FString::Printf(TEXT("Storage: Folder\n%s\n%s"),
					UTF8_TO_TCHAR(P.FolderPath.c_str()), *Note)));
			}
			PageRoot->AddChildToVerticalBox(Stor)->SetPadding(FMargin(0, 0, 0, 12));
			UButton* Code = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "JoinCodeLink", "Join Using Code..."));
			Code->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinCodeClicked);
			PageRoot->AddChildToVerticalBox(Code)->SetPadding(FMargin(0, 0, 0, 10));
			if (!SelectedWorldId.IsEmpty())
			{
				const FString Inv = S->EnsureInviteCode(SelectedWorldId);
				UTextBlock* CodeLbl = MakeText(WidgetTree, 14, TextPrimary);
				CodeLbl->SetText(FText::FromString(FString::Printf(TEXT("Selected world invite code: %s"), *Inv)));
				PageRoot->AddChildToVerticalBox(CodeLbl)->SetPadding(FMargin(0, 8, 0, 0));
				UTextBlock* IdLbl = MakeText(WidgetTree, 12, TextMuted);
				IdLbl->SetText(FText::FromString(FString::Printf(TEXT("World ID: %s"), *SelectedWorldId)));
				PageRoot->AddChildToVerticalBox(IdLbl);
			}
		}
		FlashText = MakeText(WidgetTree, 13, TextMuted);
		PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
		break;
	}
	case EPage::LinkGitHub:
		RebuildLinkGitHubPage();
		break;
	case EPage::Main:
	default:
		RebuildMain();
		break;
	}
}

void USharedWorldBrowserWidget::AddDetailStatRow(UVerticalBox* Col, const FText& Label, const FText& Value, const FLinearColor& ValueColor)
{
	if (!Col) return;
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 6));
	UTextBlock* L = MakeText(WidgetTree, 14, TextMuted, false);
	L->SetText(Label);
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>();
	LabelBox->SetWidthOverride(110.f);
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

void USharedWorldBrowserWidget::RebuildDetails()
{
	UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
	Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
	PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 10));

	FSharedWorldEntryView View;
	bool bHas = false;
	if (USharedWorldSubsystem* S = SW())
	{
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId == SelectedWorldId) { View = V; bHas = true; break; }
		}
	}
	if (!bHas)
	{
		UTextBlock* Missing = MakeText(WidgetTree, 16, TextMuted);
		Missing->SetText(NSLOCTEXT("SharedWorld", "MissingWorld", "This Shared World is not in the local list."));
		PageRoot->AddChildToVerticalBox(Missing);
		return;
	}

	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageRoot->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(Body);
	ScrollContentBox = Body;
	PopulateDetailsBody(Body, View);

	FlashText = MakeText(WidgetTree, 13, TextMuted);
	PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 10, 0, 0));
	ListScrollOwner = EPage::Details;
	RestoreListScroll(EPage::Details);
}

void USharedWorldBrowserWidget::PopulateDetailsBody(UVerticalBox* Body, const FSharedWorldEntryView& View)
{
	if (!Body || !WidgetTree) return;

	const FString TitleStr = View.WorldName.IsEmpty() ? View.WorldId : View.WorldName;
	const FString Badge = StatusBadge(View.CloudStatus, View.LocalState);
	const FLinearColor BadgeColor = StatusColor(Badge);

	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	Body->AddChildToVerticalBox(TitleRow)->SetPadding(FMargin(0, 0, 0, 4));
	UTextBlock* Title = MakeText(WidgetTree, 26, TextPrimary, true);
	Title->SetText(FText::FromString(TitleStr));
	TitleRow->AddChildToHorizontalBox(Title)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UTextBlock* Status = MakeText(WidgetTree, 14, BadgeColor, true);
	FString StatusLabel = Badge;
	if (Badge == TEXT("ACTIVE") && View.IsHostingNow()) StatusLabel = TEXT("ONLINE");
	Status->SetText(FText::FromString(StatusLabel));
	TitleRow->AddChildToHorizontalBox(Status)->SetVerticalAlignment(VAlign_Center);

	UTextBlock* Subtitle = MakeText(WidgetTree, 14, TextMuted);
	FString SubtitleStr = View.MapLabel;
	if (SubtitleStr.IsEmpty()) SubtitleStr = View.OriginalSaveName;
	if (SubtitleStr.IsEmpty()) SubtitleStr = View.FriendlyStatusLine();
	Subtitle->SetText(FText::FromString(SubtitleStr));
	Body->AddChildToVerticalBox(Subtitle)->SetPadding(FMargin(0, 0, 0, 14));

	// Screenshot: try local SharedWorld save preview next to the .sav, else muted plate.
	bool bShotLoaded = false;
	{
		FString ShotPath;
		const FString SaveDir = UFGSaveSystem::GetSaveDirectoryPath();
		const FString SaveStem = FString::Printf(TEXT("SharedWorld_%s"), *SelectedWorldId);
		const TCHAR* Exts[] = { TEXT(".png"), TEXT(".jpg"), TEXT(".jpeg") };
		for (const TCHAR* Ext : Exts)
		{
			const FString Candidate = FPaths::Combine(SaveDir, SaveStem + Ext);
			if (FPaths::FileExists(Candidate)) { ShotPath = Candidate; break; }
		}
		UBorder* Shot = WidgetTree->ConstructWidget<UBorder>();
		Shot->SetBrushColor(FLinearColor(1.f, 1.f, 1.f, 0.06f));
		Shot->SetPadding(FMargin(16.f, 48.f));
		Body->AddChildToVerticalBox(Shot)->SetPadding(FMargin(0, 0, 0, 16));
		if (!ShotPath.IsEmpty())
		{
			if (UTexture2D* Tex = FImageUtils::ImportFileAsTexture2D(ShotPath))
			{
				UImage* Img = WidgetTree->ConstructWidget<UImage>();
				Img->SetBrushFromTexture(Tex, true);
				Shot->SetPadding(FMargin(0.f));
				Shot->SetContent(Img);
				bShotLoaded = true;
			}
		}
		if (!bShotLoaded)
		{
			UTextBlock* ShotLbl = MakeText(WidgetTree, 13, TextMuted);
			ShotLbl->SetText(NSLOCTEXT("SharedWorld", "NoScreenshot", "World screenshot unavailable"));
			ShotLbl->SetJustification(ETextJustify::Center);
			Shot->SetContent(ShotLbl);
		}
	}

	const int32 MaxP = View.MaxPlayers > 0 ? View.MaxPlayers : 4;
	const FString PlayersVal = View.IsHostingNow()
		? FString::Printf(TEXT("%d / %d"), View.PlayerCount, MaxP)
		: FString::Printf(TEXT("— / %d"), MaxP);
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatPlayers", "Players"),
		FText::FromString(PlayersVal), TextPrimary);
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatHost", "Host"),
		FText::FromString(View.HostName.IsEmpty()
			? (View.LastHostName.IsEmpty() ? TEXT("Nobody hosting") : View.LastHostName + TEXT(" (last)"))
			: View.HostName),
		TextPrimary);
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatPing", "Ping"),
		View.PingMs >= 0
			? FText::FromString(FString::Printf(TEXT("%d ms"), View.PingMs))
			: NSLOCTEXT("SharedWorld", "PingUnknown", "—"),
		View.PingMs >= 0 ? TextPrimary : TextMuted);
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatPlaytime", "Playtime"),
		FText::FromString(View.PlaytimeText.IsEmpty() ? TEXT("—") : View.PlaytimeText),
		View.PlaytimeText.IsEmpty() ? TextMuted : TextPrimary);
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatLastSave", "Last Save"),
		FText::FromString(View.LastPlayed.IsEmpty() ? TEXT("Unknown") : View.LastPlayed), TextPrimary);

	FText SyncText = NSLOCTEXT("SharedWorld", "SyncUnknown", "Unknown");
	FLinearColor SyncColor = TextMuted;
	if (View.bHasError)
	{
		SyncText = NSLOCTEXT("SharedWorld", "SyncError", "Error");
		SyncColor = Err;
	}
	else if (View.LocalState == TEXT("UPLOADING") || View.LocalState == TEXT("DOWNLOADING") || View.LocalState == TEXT("CHECKING"))
	{
		SyncText = NSLOCTEXT("SharedWorld", "Syncing", "Syncing…");
		SyncColor = Warn;
	}
	else if (View.IsLocalIdle() || View.LocalState == TEXT("HOSTING") || View.LocalState == TEXT("JOINED"))
	{
		SyncText = NSLOCTEXT("SharedWorld", "SyncOk", "✓ Up to date");
		SyncColor = Ok;
	}
	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatSync", "Sync"), SyncText, SyncColor);

	AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatPhase", "Game Phase"),
		FText::FromString(View.GamePhase.IsEmpty() ? TEXT("—") : View.GamePhase),
		View.GamePhase.IsEmpty() ? TextMuted : TextPrimary);

	if (View.RequiredModCount > 0)
	{
		AddDetailStatRow(Body, NSLOCTEXT("SharedWorld", "StatMods", "Mods"),
			FText::FromString(FString::Printf(TEXT("%d required"), View.RequiredModCount)), TextPrimary);
	}

	// Members
	UTextBlock* MembersHdr = MakeText(WidgetTree, 14, TextMuted, true);
	MembersHdr->SetText(NSLOCTEXT("SharedWorld", "MembersHdr", "MEMBERS"));
	Body->AddChildToVerticalBox(MembersHdr)->SetPadding(FMargin(0, 16, 0, 8));

	if (View.OnlinePlayerNames.Num() > 0)
	{
		for (const FString& Name : View.OnlinePlayerNames)
		{
			const bool bHost = !View.HostName.IsEmpty() && Name.Equals(View.HostName, ESearchCase::IgnoreCase);
			UTextBlock* Row = MakeText(WidgetTree, 14, TextPrimary);
			Row->SetText(FText::FromString(FString::Printf(TEXT("●  %s          %s"),
				*Name, bHost ? TEXT("Host") : TEXT("Online"))));
			Body->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
		}
	}

	if (bDetailsMembersLoading && !bDetailsMembersLoaded)
	{
		UTextBlock* Loading = MakeText(WidgetTree, 13, TextMuted);
		Loading->SetText(NSLOCTEXT("SharedWorld", "LoadingMembers", "Loading Shared World members…"));
		Body->AddChildToVerticalBox(Loading)->SetPadding(FMargin(0, 4, 0, 0));
	}
	else if (!DetailsMembersText.IsEmpty())
	{
		// Membership roster from cloud (may include offline players).
		TArray<FString> Lines;
		DetailsMembersText.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty()) continue;
			if (Trimmed.StartsWith(TEXT("Open:")) || Trimmed.StartsWith(TEXT("Members only")))
			{
				UTextBlock* Note = MakeText(WidgetTree, 12, TextMuted);
				Note->SetText(FText::FromString(Trimmed));
				Body->AddChildToVerticalBox(Note)->SetPadding(FMargin(0, 0, 0, 4));
				continue;
			}
			// Skip duplicates already shown as online.
			FString Display = Trimmed;
			FString RolePart;
			if (Trimmed.Split(TEXT("  ("), &Display, &RolePart))
			{
				RolePart.RemoveFromEnd(TEXT(")"));
			}
			bool bAlready = false;
			for (const FString& Online : View.OnlinePlayerNames)
			{
				if (Online.Equals(Display, ESearchCase::IgnoreCase)) { bAlready = true; break; }
			}
			if (bAlready) continue;
			UTextBlock* Row = MakeText(WidgetTree, 14, TextMuted);
			const FString Role = RolePart.IsEmpty() ? TEXT("Offline") : RolePart;
			Row->SetText(FText::FromString(FString::Printf(TEXT("○  %s          %s"), *Display, *Role)));
			Body->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 4));
		}
	}
	else if (View.OnlinePlayerNames.Num() == 0)
	{
		UTextBlock* Empty = MakeText(WidgetTree, 13, TextMuted);
		Empty->SetText(NSLOCTEXT("SharedWorld", "NoMembersYet", "No players online right now."));
		Body->AddChildToVerticalBox(Empty);
	}

	// Actions — Join Game style full-width rows.
	UVerticalBox* Actions = WidgetTree->ConstructWidget<UVerticalBox>();
	Body->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0, 20, 0, 0));
	{
		const bool bJoin = View.IsHostingNow();
		const FText PlayTxt = bJoin
			? NSLOCTEXT("SharedWorld", "JoinWorld", "Join World")
			: NSLOCTEXT("SharedWorld", "PlayWorld", "Play World");
		UButton* PlayBtn = nullptr;
		TObjectPtr<UTextBlock> PlayLbl;
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, PlayBtn, PlayLbl, PlayTxt, 52.f, 20);
		PlayBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnPlaySelected);
		AddFillRow(Actions, Box, 8.f);
		PlayButton = PlayBtn;
		PlayLabel = PlayLbl;
	}
	{
		UButton* ManageBtn = nullptr;
		TObjectPtr<UTextBlock> ManageLbl;
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, ManageBtn, ManageLbl,
			NSLOCTEXT("SharedWorld", "Manage", "Manage"), 48.f, 18);
		ManageBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnManageSelected);
		AddFillRow(Actions, Box, 8.f);
	}

	{
		UButton* TechBtn = nullptr;
		TObjectPtr<UTextBlock> TechLbl;
		const FText TechLabel = bShowTechnicalDetails
			? NSLOCTEXT("SharedWorld", "HideTech", "▾ Technical Details")
			: NSLOCTEXT("SharedWorld", "ShowTech", "▸ Technical Details");
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, TechBtn, TechLbl, TechLabel, 44.f, 16);
		TechBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnToggleTechnicalDetails);
		AddFillRow(Actions, Box, 4.f);
	}

	if (bShowTechnicalDetails)
	{
		UVerticalBox* Tech = WidgetTree->ConstructWidget<UVerticalBox>();
		Body->AddChildToVerticalBox(Tech)->SetPadding(FMargin(8, 8, 0, 0));
		AddDetailStatRow(Tech, NSLOCTEXT("SharedWorld", "TechRev", "Revision"),
			FText::AsNumber(View.Revision), TextPrimary);
		AddDetailStatRow(Tech, NSLOCTEXT("SharedWorld", "TechGen", "Generation"),
			FText::AsNumber(View.Generation), TextPrimary);
		AddDetailStatRow(Tech, NSLOCTEXT("SharedWorld", "TechId", "World ID"),
			FText::FromString(View.WorldId), TextMuted);
		if (!View.InviteCode.IsEmpty())
		{
			AddDetailStatRow(Tech, NSLOCTEXT("SharedWorld", "TechInvite", "Invite"),
				FText::FromString(View.InviteCode), TextPrimary);
		}
		if (View.bHasError)
		{
			UTextBlock* ErrT = MakeText(WidgetTree, 13, Err);
			ErrT->SetText(FText::FromString(FString::Printf(TEXT("%s\n%s"), *View.ErrorMessage, *View.ErrorDetail)));
			Tech->AddChildToVerticalBox(ErrT)->SetPadding(FMargin(0, 8, 0, 0));
		}

		UButton* HistBtn = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "SaveHistory", "Save History"));
		HistBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsHistory);
		Tech->AddChildToVerticalBox(HistBtn)->SetPadding(FMargin(0, 12, 0, 6));
		if (!DetailsHistoryText.IsEmpty())
		{
			UTextBlock* Hist = MakeText(WidgetTree, 12, TextMuted);
			Hist->SetText(FText::FromString(DetailsHistoryText));
			Tech->AddChildToVerticalBox(Hist)->SetPadding(FMargin(0, 0, 0, 8));
		}

		UHorizontalBox* RestoreRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		Tech->AddChildToVerticalBox(RestoreRow)->SetPadding(FMargin(0, 4, 0, 8));
		RestoreInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		RestoreInput->SetHintText(NSLOCTEXT("SharedWorld", "RestoreHint", "Revision number"));
		RestoreRow->AddChildToHorizontalBox(RestoreInput)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UButton* RestoreBtn = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Restore", "Restore"));
		RestoreBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsRestore);
		RestoreRow->AddChildToHorizontalBox(RestoreBtn)->SetPadding(FMargin(12, 0, 0, 0));

		if (View.bOwned)
		{
			UButton* RemoveBtn = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "RemoveFromList", "Remove from list"));
			RemoveBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRemoveSelected);
			Tech->AddChildToVerticalBox(RemoveBtn)->SetPadding(FMargin(0, 8, 0, 0));
		}
	}
}

void USharedWorldBrowserWidget::RebuildMain()
{
	// Mods / Load layout: left list column + right detail pane.
	UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
	PageRoot->AddChildToVerticalBox(Columns)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	// —— Left column (search + world list) ——
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* LeftSlot = Columns->AddChildToHorizontalBox(Left))
	{
		LeftSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		LeftSlot->SetPadding(FMargin(0, 0, 20, 0));
	}

	// Top actions — same full-width orange-hover rows as Join Game ("Join game directly...").
	UVerticalBox* TopLinks = WidgetTree->ConstructWidget<UVerticalBox>();
	Left->AddChildToVerticalBox(TopLinks)->SetPadding(FMargin(0, 0, 0, 8));
	{
		UButton* CreateBtn = nullptr;
		TObjectPtr<UTextBlock> Lbl;
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, CreateBtn, Lbl,
			NSLOCTEXT("SharedWorld", "CreateShared", "Create Shared World..."), 48.f, 20);
		CreateBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateClicked);
		AddFillRow(TopLinks, Box);
	}
	{
		UButton* JoinBtn = nullptr;
		TObjectPtr<UTextBlock> Lbl;
		USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, JoinBtn, Lbl,
			NSLOCTEXT("SharedWorld", "JoinFriend", "Join Friend..."), 48.f, 20);
		JoinBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinFriendClicked);
		AddFillRow(TopLinks, Box);
	}

	// Search — DescriptionText font like ModList SearchBarInputText.
	UBorder* SearchBorder = WidgetTree->ConstructWidget<UBorder>();
	SearchBorder->SetBrushColor(SearchBg);
	SearchBorder->SetPadding(FMargin(12.f, 8.f));
	Left->AddChildToVerticalBox(SearchBorder)->SetPadding(FMargin(0, 0, 0, 10));
	FilterInput = WidgetTree->ConstructWidget<UEditableTextBox>();
	FilterInput->SetHintText(NSLOCTEXT("SharedWorld", "Search", "Search for a Shared World..."));
	FilterInput->SetText(FText::FromString(FilterText));
	{
		FEditableTextBoxStyle Style = FilterInput->WidgetStyle;
		FSlateFontInfo FontInfo = Style.TextStyle.Font;
		if (UObject* FontObj = StaticLoadObject(UFont::StaticClass(), nullptr, SharedWorldFg::DescriptionFontPath))
		{
			FontInfo.FontObject = FontObj;
			FontInfo.Size = 14;
			FontInfo.TypefaceFontName = NAME_None;
			Style.TextStyle.SetFont(FontInfo);
			FilterInput->WidgetStyle = Style;
		}
	}
	FilterInput->OnTextChanged.AddDynamic(this, &USharedWorldBrowserWidget::OnFilterChanged);
	SearchBorder->SetContent(FilterInput);

	StatusText = MakeText(WidgetTree, 12, TextMuted);
	Left->AddChildToVerticalBox(StatusText)->SetPadding(FMargin(0, 0, 0, 6));

	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	Left->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(List);
	MainListBox = List;

	FSharedWorldDiscoverySnapshot Snap;
	FSharedWorldEntryView SelectedView;
	bool bHasSelected = false;
	PopulateMainList(List, Snap, SelectedView, bHasSelected);
	if (StatusText) StatusText->SetText(FText::FromString(Snap.StatusMessage));

	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
	Left->AddChildToVerticalBox(Footer)->SetPadding(FMargin(0, 12, 0, 0));
	UButton* Refresh = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Refresh", "Refresh"));
	Refresh->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRefresh);
	Footer->AddChildToHorizontalBox(Refresh)->SetPadding(FMargin(0, 0, 20, 0));
	UButton* Settings = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Settings", "Settings"));
	Settings->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnSettingsClicked);
	Footer->AddChildToHorizontalBox(Settings);

	// —— Right detail pane (content lives on SubMenuBackground plate — no extra card) ——
	UBorder* DetailBorder = WidgetTree->ConstructWidget<UBorder>();
	DetailBorder->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
	DetailBorder->SetPadding(FMargin(28.f, 16.f, 20.f, 16.f));
	if (UHorizontalBoxSlot* RightSlot = Columns->AddChildToHorizontalBox(DetailBorder))
	{
		FSlateChildSize Sz(ESlateSizeRule::Fill);
		Sz.Value = 1.15f;
		RightSlot->SetSize(Sz);
	}
	UVerticalBox* Detail = WidgetTree->ConstructWidget<UVerticalBox>();
	DetailBorder->SetContent(Detail);
	MainDetailBox = Detail;
	PopulateMainDetail(Detail, SelectedView, bHasSelected);

	FlashText = MakeText(WidgetTree, 13, TextMuted);
	PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 10, 0, 0));
	UpdateBottomPlay();
	ListScrollOwner = EPage::Main;
	RestoreListScroll(EPage::Main);
}

void USharedWorldBrowserWidget::PopulateMainList(UVerticalBox* List, FSharedWorldDiscoverySnapshot& Snap, FSharedWorldEntryView& OutSelected, bool& bOutHasSelected)
{
	bOutHasSelected = false;
	if (!List || !WidgetTree) return;
	USharedWorldSubsystem* S = SW();
	if (!S) return;

	Snap = S->Discovery().BuildSnapshot();

	auto FindSel = [&](const TArray<FSharedWorldEntryView>& Arr) -> bool
	{
		for (const FSharedWorldEntryView& V : Arr)
		{
			if (V.WorldId == SelectedWorldId)
			{
				OutSelected = V;
				return true;
			}
		}
		return false;
	};
	bOutHasSelected = FindSel(Snap.OwnedWorlds) || FindSel(Snap.SharedWithYou) || FindSel(Snap.FriendsPlaying);
	if (!bOutHasSelected)
	{
		if (Snap.OwnedWorlds.Num() > 0) { SelectedWorldId = Snap.OwnedWorlds[0].WorldId; OutSelected = Snap.OwnedWorlds[0]; bOutHasSelected = true; }
		else if (Snap.SharedWithYou.Num() > 0) { SelectedWorldId = Snap.SharedWithYou[0].WorldId; OutSelected = Snap.SharedWithYou[0]; bOutHasSelected = true; }
		else if (Snap.FriendsPlaying.Num() > 0) { SelectedWorldId = Snap.FriendsPlaying[0].WorldId; OutSelected = Snap.FriendsPlaying[0]; bOutHasSelected = true; }
	}

	if (!Snap.ErrorMessage.IsEmpty())
	{
		UTextBlock* ErrT = MakeText(WidgetTree, 13, Err);
		ErrT->SetText(FText::FromString(Snap.ErrorMessage));
		List->AddChildToVerticalBox(ErrT)->SetPadding(FMargin(0, 0, 0, 8));
	}
	if (Snap.PendingInvites.Num() > 0)
	{
		AddSectionHeader(List, NSLOCTEXT("SharedWorld", "Invites", "INVITATIONS"));
		AddInviteRows(List, Snap.PendingInvites);
	}
	AddSectionHeader(List, NSLOCTEXT("SharedWorld", "YourWorlds", "YOUR WORLDS"));
	AddWorldRows(List, Snap.OwnedWorlds, false, true);
	AddSectionHeader(List, NSLOCTEXT("SharedWorld", "SharedWithYou", "SHARED WITH YOU"));
	AddWorldRows(List, Snap.SharedWithYou, false);
	AddSectionHeader(List, NSLOCTEXT("SharedWorld", "FriendsPlaying", "FRIENDS PLAYING"));
	if (Snap.FriendsPlaying.Num() == 0)
	{
		UTextBlock* Empty = MakeText(WidgetTree, 13, TextMuted);
		Empty->SetText(NSLOCTEXT("SharedWorld", "NobodyHosting", "Nobody currently hosting a Shared World."));
		List->AddChildToVerticalBox(Empty);
	}
	else
	{
		AddWorldRows(List, Snap.FriendsPlaying, true);
	}
}

void USharedWorldBrowserWidget::PopulateMainDetail(UVerticalBox* Detail, const FSharedWorldEntryView& SelectedView, bool bHasSelected)
{
	if (!Detail || !WidgetTree) return;
	PlayButton = nullptr;
	PlayLabel = nullptr;

	if (bHasSelected)
	{
		UTextBlock* DetailTitle = MakeText(WidgetTree, 26, TextPrimary, true);
		DetailTitle->SetText(FText::FromString(SelectedView.WorldName.IsEmpty() ? SelectedView.WorldId : SelectedView.WorldName));
		Detail->AddChildToVerticalBox(DetailTitle)->SetPadding(FMargin(0, 0, 0, 10));

		UTextBlock* Meta = MakeText(WidgetTree, 14, TextMuted);
		FString MetaLine = SelectedView.FriendlyStatusLine();
		if (!SelectedView.LastPlayed.IsEmpty())
		{
			MetaLine += FString::Printf(TEXT("\nLast played: %s"), *SelectedView.LastPlayed);
		}
		Meta->SetText(FText::FromString(MetaLine));
		Detail->AddChildToVerticalBox(Meta)->SetPadding(FMargin(0, 0, 0, 20));

		const bool bJoin = SelectedView.IsHostingNow();
		const FText PlayTxt = bJoin ? NSLOCTEXT("SharedWorld", "Join", "Join") : NSLOCTEXT("SharedWorld", "Play", "Play");
		{
			UButton* PlayBtn = nullptr;
			TObjectPtr<UTextBlock> PlayLbl;
			USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, PlayBtn, PlayLbl, PlayTxt, 52.f, 20);
			PlayBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnPlaySelected);
			AddFillRow(Detail, Box, 8.f);
			PlayButton = PlayBtn;
			PlayLabel = PlayLbl;
			PlayStandardButton = nullptr;
		}
		{
			UButton* DetBtn = nullptr;
			TObjectPtr<UTextBlock> DetLbl;
			USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, DetBtn, DetLbl,
				NSLOCTEXT("SharedWorld", "Details", "Details"), 48.f, 18);
			DetBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsSelected);
			AddFillRow(Detail, Box, 8.f);
		}
		if (SelectedView.bOwned)
		{
			UBorder* RemoveBox = WidgetTree->ConstructWidget<UBorder>();
			RemoveBox->SetBrushColor(FLinearColor(0.08f, 0.02f, 0.02f, 0.35f));
			RemoveBox->SetPadding(FMargin(14.f, 12.f));
			Detail->AddChildToVerticalBox(RemoveBox)->SetPadding(FMargin(0, 16, 0, 0));
			UVerticalBox* RemoveCol = WidgetTree->ConstructWidget<UVerticalBox>();
			RemoveBox->SetContent(RemoveCol);
			UTextBlock* RemoveHint = MakeText(WidgetTree, 12, TextMuted);
			RemoveHint->SetText(NSLOCTEXT("SharedWorld", "RemoveHint",
				"Remove this world from this PC only. Your cloud save and other players are not affected."));
			RemoveCol->AddChildToVerticalBox(RemoveHint)->SetPadding(FMargin(0, 0, 0, 8));
			UButton* RemoveBtn = nullptr;
			TObjectPtr<UTextBlock> RemoveLbl;
			USizeBox* RemoveRow = MakeJoinMenuRowBox(WidgetTree, RemoveBtn, RemoveLbl,
				NSLOCTEXT("SharedWorld", "RemoveFromList", "Remove from list"), 44.f, 16);
			RemoveBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnRemoveSelected);
			RemoveCol->AddChildToVerticalBox(RemoveRow);
		}

		UTextBlock* Hint = MakeText(WidgetTree, 13, TextMuted);
		Hint->SetText(bJoin
			? NSLOCTEXT("SharedWorld", "JoinHint", "A friend is hosting — Join connects you to their game.")
			: NSLOCTEXT("SharedWorld", "PlayHint", "Play puts you in this world. If nobody is hosting, you become host."));
		Detail->AddChildToVerticalBox(Hint);
	}
	else
	{
		UTextBlock* EmptyTitle = MakeText(WidgetTree, 22, TextPrimary, true);
		EmptyTitle->SetText(NSLOCTEXT("SharedWorld", "NoWorldSelected", "Shared Worlds"));
		Detail->AddChildToVerticalBox(EmptyTitle)->SetPadding(FMargin(0, 0, 0, 10));
		UTextBlock* EmptyBody = MakeText(WidgetTree, 14, TextMuted);
		EmptyBody->SetText(NSLOCTEXT("SharedWorld", "EmptyDetail",
			"Create a Shared World from one of your saves, or join a friend.\n\nSelect a world on the left to see details."));
		Detail->AddChildToVerticalBox(EmptyBody)->SetPadding(FMargin(0, 0, 0, 20));
		{
			UButton* CreateBtn = nullptr;
			TObjectPtr<UTextBlock> CLbl;
			USizeBox* Box = MakeJoinMenuRowBox(WidgetTree, CreateBtn, CLbl,
				NSLOCTEXT("SharedWorld", "CreateShared", "Create Shared World"), 52.f, 20);
			CreateBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateClicked);
			AddFillRow(Detail, Box);
			CreateStandardButton = nullptr;
		}
	}
}

void USharedWorldBrowserWidget::OnCreateClicked() { ShowCreateWizard(); }
void USharedWorldBrowserWidget::OnJoinFriendClicked() { ShowJoinFriend(); }
void USharedWorldBrowserWidget::OnSettingsClicked() { ShowAdvanced(); }
void USharedWorldBrowserWidget::OnJoinCodeClicked() { ShowJoinCode(); }
void USharedWorldBrowserWidget::OnRefresh()
{
	if (USharedWorldSubsystem* S = SW()) S->Discovery().BeginRefresh();
	SetFlash(true, TEXT("Refreshing..."));
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
void USharedWorldBrowserWidget::OnManageSelected()
{
	bShowTechnicalDetails = true;
	ScheduleRebuild();
}
void USharedWorldBrowserWidget::OnDetailsHistory()
{
	USharedWorldSubsystem* S = SW();
	if (!S || SelectedWorldId.IsEmpty() || bBusy) return;
	bBusy = true;
	DetailsHistoryText = TEXT("Loading save history…");
	bShowTechnicalDetails = true;
	ScheduleRebuild();
	const FString WorldId = SelectedWorldId;
	S->FetchHistory(WorldId, 20, [this, WorldId](bool bOk, const FString& Message)
	{
		bBusy = false;
		if (SelectedWorldId != WorldId) return;
		DetailsHistoryText = bOk ? Message : FString::Printf(TEXT("Failed: %s"), *Message);
		bShowTechnicalDetails = true;
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
	S->Restore(SelectedWorldId, Rev);
	SetFlash(true, FString::Printf(TEXT("Restoring revision %lld as a new revision…"), Rev));
}
void USharedWorldBrowserWidget::OnRemoveSelected()
{
	if (!SelectedWorldId.IsEmpty())
	{
		RemoveWorldFromList(SelectedWorldId);
	}
}
void USharedWorldBrowserWidget::OnBack()
{
	if (Page == EPage::Details) { ShowMain(); return; }
	if (Page == EPage::CreatePickSave) { Page = EPage::CreateChoice; ScheduleRebuild(); return; }
	if (Page == EPage::CreateName) { Page = EPage::CreatePickSave; ScheduleRebuild(); return; }
	if (Page == EPage::LinkGitHub)
	{
		if (USharedWorldSubsystem* S = SW())
		{
			const FSharedWorldSignIn Sign = S->GetSignInStatus();
			if (Sign.bInProgress) S->CancelGitHubSignIn();
		}
		ShowAdvanced();
		return;
	}
	ShowMain();
}
void USharedWorldBrowserWidget::OnUseExistingSave()
{
	Page = EPage::CreatePickSave;
	RebuildPage();
}
void USharedWorldBrowserWidget::OnCreateNewGameHint()
{
	SetFlash(true, TEXT("Start a New Game from the main menu, save once, then return here and choose Use Existing Save."));
}
void USharedWorldBrowserWidget::OnCreateConfirm()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	const FString Name = NameInput ? NameInput->GetText().ToString().TrimStartAndEnd() : FString();
	if (Name.IsEmpty() || SelectedSaveName.IsEmpty())
	{
		SetFlash(false, TEXT("Pick a save and enter a name."));
		return;
	}
	bBusy = true;
	SetFlash(true, TEXT("Creating Shared World..."));
	S->Creation().CreateFromExistingSave(Name, SelectedSaveName, [this](bool bOk, const FString& Message)
	{
		bBusy = false;
		SetFlash(bOk, Message);
		if (bOk) ShowMain();
	});
}
void USharedWorldBrowserWidget::OnJoinCodeSubmit()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	const FString Code = CodeInput ? CodeInput->GetText().ToString() : FString();
	bBusy = true;
	S->Invites().JoinUsingCode(Code, [this](bool bOk, const FString& Message)
	{
		bBusy = false;
		SetFlash(bOk, Message);
		if (bOk) ShowMain();
	});
}
void USharedWorldBrowserWidget::OnWelcomeContinue()
{
	if (USharedWorldSubsystem* S = SW()) S->MarkWelcomeDone();
	ShowMain();
}
void USharedWorldBrowserWidget::OnWelcomeConnect()
{
	OnLinkGitHubClicked();
	if (USharedWorldSubsystem* S = SW()) S->MarkWelcomeDone();
}
void USharedWorldBrowserWidget::RebuildLinkGitHubPage()
{
	UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "← Back"));
	Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
	PageRoot->AddChildToVerticalBox(Back)->SetPadding(FMargin(0, 0, 0, 12));

	UTextBlock* Title = MakeText(WidgetTree, 22, TextPrimary, true);
	Title->SetText(NSLOCTEXT("SharedWorld", "ConnectGitHubTitle", "Connect GitHub"));
	PageRoot->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 12));

	USharedWorldSubsystem* S = SW();
	if (!S)
	{
		return;
	}
	const FSharedWorldSignIn Sign = S->GetSignInStatus();
	if (!Sign.bConfigured)
	{
		UTextBlock* Msg = MakeText(WidgetTree, 14, TextPrimary);
		Msg->SetText(NSLOCTEXT("SharedWorld", "GitHubNotConfigured", "GitHub integration is not configured in this build."));
		PageRoot->AddChildToVerticalBox(Msg);
		return;
	}
	if (Sign.State == ESharedWorldGitHubAuthState::Connected || !S->GetGitHubLogin().IsEmpty())
	{
		UTextBlock* Msg = MakeText(WidgetTree, 14, TextPrimary);
		Msg->SetText(FText::FromString(FString::Printf(TEXT("GitHub connected successfully\nConnected as %s"), *S->GetGitHubLogin())));
		PageRoot->AddChildToVerticalBox(Msg)->SetPadding(FMargin(0, 0, 0, 12));
		UButton* Disconnect = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "DisconnectGitHub", "Disconnect"));
		Disconnect->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDisconnectGitHub);
		PageRoot->AddChildToVerticalBox(Disconnect);
		return;
	}

	UTextBlock* Body = MakeText(WidgetTree, 14, TextMuted);
	if (!Sign.UserCode.IsEmpty())
	{
		Body->SetText(FText::FromString(FString::Printf(
			TEXT("Open GitHub and enter this code:\n\n%s\n\n%s"),
			*Sign.UserCode,
			Sign.VerificationUri.IsEmpty() ? TEXT("") : *Sign.VerificationUri)));
	}
	else
	{
		Body->SetText(FText::FromString(Sign.PlayerMessage.IsEmpty() ? TEXT("Opening GitHub...") : Sign.PlayerMessage));
	}
	PageRoot->AddChildToVerticalBox(Body)->SetPadding(FMargin(0, 0, 0, 16));

	if (!Sign.UserCode.IsEmpty())
	{
		TObjectPtr<UTextBlock> OpenLbl;
		UButton* Open = MakePrimaryButton(WidgetTree, OpenLbl, NSLOCTEXT("SharedWorld", "OpenGitHub", "Open GitHub"));
		Open->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnOpenGitHubVerify);
		PageRoot->AddChildToVerticalBox(Open)->SetPadding(FMargin(0, 0, 0, 8));
		UButton* Copy = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "CopyCode", "Copy Code"));
		Copy->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCopyGitHubCode);
		PageRoot->AddChildToVerticalBox(Copy)->SetPadding(FMargin(0, 0, 0, 8));
	}
	UButton* Cancel = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "CancelLink", "Cancel"));
	Cancel->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCancelGitHubLink);
	PageRoot->AddChildToVerticalBox(Cancel);

	if (!Sign.Error.IsEmpty())
	{
		UTextBlock* ErrMsg = MakeText(WidgetTree, 13, TextMuted);
		ErrMsg->SetText(FText::FromString(Sign.Error));
		PageRoot->AddChildToVerticalBox(ErrMsg)->SetPadding(FMargin(0, 12, 0, 0));
	}
	FlashText = MakeText(WidgetTree, 13, TextMuted);
	PageRoot->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 12, 0, 0));
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
	if (USharedWorldSubsystem* S = SW())
	{
		S->SignOutOfGitHub();
		SetFlash(true, TEXT("GitHub disconnected."));
	}
	if (Page == EPage::LinkGitHub) ShowAdvanced();
	else RebuildPage();
}
void USharedWorldBrowserWidget::OnTestGitHubAccess()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	bBusy = true;
	SetFlash(true, TEXT("Testing GitHub access..."));
	S->TestGitHubAccess([this](bool bOk, const FString& Message)
	{
		bBusy = false;
		SetFlash(bOk, Message);
		RebuildPage();
	});
}
void USharedWorldBrowserWidget::OnFilterChanged(const FText& Text)
{
	FilterText = Text.ToString().TrimStartAndEnd();
	if (Page == EPage::Main) RebuildPage();
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
