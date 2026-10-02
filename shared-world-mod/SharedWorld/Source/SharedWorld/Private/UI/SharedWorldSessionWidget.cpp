#include "UI/SharedWorldSessionWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "TimerManager.h"
#include "Services/SharedWorldCreationService.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "Services/SharedWorldInviteService.h"
#include "SharedWorldCore/HostMigration/Diagnostics.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldInviteRowBinder.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

namespace
{
	UButton* TextLink(UWidgetTree* Tree, const FText& Label, int32 FontSize = 14)
	{
		TObjectPtr<UTextBlock> L;
		return MakeTextLink(Tree, L, Label, FontSize);
	}
}

TSharedRef<SWidget> USharedWorldSessionWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(820.f);
		Size->SetHeightOverride(680.f);
		WidgetTree->RootWidget = Size;
		UBorder* Root = MakePanel(WidgetTree, PanelFill, PanelEdge, FMargin(24.f), RadiusL, 1.f);
		Size->AddChild(Root);
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Root->SetContent(Col);

		UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
		Col->AddChildToVerticalBox(Header)->SetPadding(FMargin(0, 0, 0, 12));
		TitleText = MakeText(WidgetTree, 24, TextPrimary, true);
		TitleText->SetText(NSLOCTEXT("SharedWorld", "SessionTitle", "SHARED WORLD"));
		Header->AddChildToHorizontalBox(TitleText)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UButton* Back = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "Back", "Back"));
		Back->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnBack);
		Header->AddChildToHorizontalBox(Back);

		UHorizontalBox* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
		Col->AddChildToVerticalBox(Tabs)->SetPadding(FMargin(0, 0, 0, 10));
		TObjectPtr<UTextBlock> T0, T1, T2, T3;
		TabButtons.Reset();
		UButton* Overview = MakeTabButton(WidgetTree, NSLOCTEXT("SharedWorld", "TabOverview", "Overview"), true);
		Overview->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnTabOverview);
		Tabs->AddChildToHorizontalBox(Overview)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* Players = MakeTabButton(WidgetTree, NSLOCTEXT("SharedWorld", "TabPlayers", "Players"), false);
		Players->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnTabPlayers);
		Tabs->AddChildToHorizontalBox(Players)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* History = MakeTabButton(WidgetTree, NSLOCTEXT("SharedWorld", "TabHistory", "History"), false);
		History->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnTabHistory);
		Tabs->AddChildToHorizontalBox(History)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* Backups = MakeTabButton(WidgetTree, NSLOCTEXT("SharedWorld", "TabBackups", "Backups"), false);
		Backups->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnTabBackups);
		Tabs->AddChildToHorizontalBox(Backups);
		TabButtons.Add(Overview);
		TabButtons.Add(Players);
		TabButtons.Add(History);
		TabButtons.Add(Backups);

		UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
		Col->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
		Scroll->AddChild(Body);

		// Overview blocks sit on the same rounded cards as the rest of Shared Worlds.
		auto AddCard = [&](UTextBlock* Text)
		{
			UBorder* Card = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(16.f, 12.f), RadiusM, 1.f);
			Card->SetContent(Text);
			Body->AddChildToVerticalBox(Card)->SetPadding(FMargin(0, 0, 0, 12));
		};
		OverviewText = MakeText(WidgetTree, 14, TextPrimary);
		AddCard(OverviewText);

		TObjectPtr<UTextBlock> MakeLbl;
		MakeSharedButton = MakePrimaryButton(WidgetTree, MakeLbl, NSLOCTEXT("SharedWorld", "MakeShared", "Make This a Shared World"), 15);
		MakeSharedButton->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnMakeSharedWorld);
		MakeSharedButton->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(MakeSharedButton)->SetPadding(FMargin(0, 0, 0, 12));

		HostText = MakeText(WidgetTree, 14, TextPrimary);
		AddCard(HostText);
		RankingText = MakeText(WidgetTree, 13, TextMuted);
		AddCard(RankingText);
		SectionText = MakeText(WidgetTree, 13, TextMuted);
		Body->AddChildToVerticalBox(SectionText)->SetPadding(FMargin(0, 0, 0, 8));

		InviteCodeText = MakeText(WidgetTree, 15, TextPrimary, true);
		InviteCodeText->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(InviteCodeText)->SetPadding(FMargin(0, 0, 0, 4));
		UButton* CopyCode = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "CopyInviteCode", "Copy invite code"));
		CopyCode->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnCopyInviteCode);
		Body->AddChildToVerticalBox(CopyCode)->SetPadding(FMargin(0, 0, 0, 10));

		UTextBlock* InGameHdr = MakeText(WidgetTree, 13, TextMuted, true);
		InGameHdr->SetText(NSLOCTEXT("SharedWorld", "InThisGame", "IN THIS GAME"));
		Body->AddChildToVerticalBox(InGameHdr)->SetPadding(FMargin(0, 4, 0, 4));
		SessionPlayersList = WidgetTree->ConstructWidget<UVerticalBox>();
		Body->AddChildToVerticalBox(SessionPlayersList)->SetPadding(FMargin(0, 0, 0, 10));

		InviteButton = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "InviteFriend", "Invite Steam Friend…"));
		InviteButton->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnInviteFriend);
		InviteButton->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(InviteButton)->SetPadding(FMargin(0, 0, 0, 6));
		InviteNameInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		InviteNameInput->SetHintText(NSLOCTEXT("SharedWorld", "FriendNameHint", "Or type a friend name / id"));
		InviteNameInput->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(InviteNameInput)->SetPadding(FMargin(0, 0, 0, 6));
		InviteFriendsList = WidgetTree->ConstructWidget<UVerticalBox>();
		InviteFriendsList->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(InviteFriendsList)->SetPadding(FMargin(0, 0, 0, 8));
		UButton* InviteConfirm = TextLink(WidgetTree, NSLOCTEXT("SharedWorld", "SendInvite", "Send Invite"));
		InviteConfirm->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnInviteConfirm);
		Body->AddChildToVerticalBox(InviteConfirm)->SetPadding(FMargin(0, 0, 0, 8));

		MembersText = MakeText(WidgetTree, 13, TextMuted);
		MembersText->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(MembersText)->SetPadding(FMargin(0, 4, 0, 8));

		UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
		Body->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0, 8, 0, 8));
		TObjectPtr<UTextBlock> L1, L2, L3, L4;
		MigrateButton = MakePrimaryButton(WidgetTree, L1, NSLOCTEXT("SharedWorld", "Migrate", "MIGRATE HOST"), 13);
		MigrateButton->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnMigrate);
		Actions->AddChildToHorizontalBox(MigrateButton)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* Reeval = MakeSecondaryButton(WidgetTree, L2, NSLOCTEXT("SharedWorld", "Reeval", "RE-EVALUATE"), 12);
		Reeval->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnReevaluate);
		Actions->AddChildToHorizontalBox(Reeval)->SetPadding(FMargin(0, 0, 8, 0));
		SaveButton = MakeSecondaryButton(WidgetTree, L3, NSLOCTEXT("SharedWorld", "SaveSync", "SAVE & SYNC"), 12);
		SaveButton->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnSaveSync);
		Actions->AddChildToHorizontalBox(SaveButton)->SetPadding(FMargin(0, 0, 8, 0));
		StopButton = MakeButton(WidgetTree, Err, L4, NSLOCTEXT("SharedWorld", "StopHost", "STOP HOSTING"), 12);
		StopButton->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnStopHosting);
		Actions->AddChildToHorizontalBox(StopButton);

		UButton* Adv = MakeSecondaryButton(WidgetTree, L2, NSLOCTEXT("SharedWorld", "Advanced", "Advanced Host Information"), 12);
		Adv->OnClicked.AddDynamic(this, &USharedWorldSessionWidget::OnToggleAdvanced);
		Body->AddChildToVerticalBox(Adv)->SetPadding(FMargin(0, 8, 0, 8));
		AdvancedText = MakeText(WidgetTree, 11, TextMuted);
		AdvancedBox = AdvancedText;
		AdvancedBox->SetVisibility(ESlateVisibility::Collapsed);
		Body->AddChildToVerticalBox(AdvancedText);

		FlashText = MakeText(WidgetTree, 13, TextMuted);
		Body->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 8, 0, 0));
	}
	return Super::RebuildWidget();
}

void USharedWorldSessionWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (USharedWorldSubsystem* S = SW())
	{
		ChangedHandle = S->OnChanged.AddUObject(this, &USharedWorldSessionWidget::HandleBackendChanged);
	}
	Refresh();
}

void USharedWorldSessionWidget::HandleBackendChanged()
{
	if (ActiveTab == 0) Refresh();
	else if (ActiveTab == 1) RefreshSessionPlayersIfChanged();
}

void USharedWorldSessionWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// Everything is event-driven (OnChanged) except the live host ranking, which the migration engine updates
	// continuously without announcing it: re-read that, only while the Overview tab is showing, every 5 s.
	if (ActiveTab != 0) return;
	RefreshAccum += InDeltaTime;
	if (RefreshAccum >= 5.f)
	{
		RefreshAccum = 0.f;
		Refresh();
	}
}

void USharedWorldSessionWidget::NativeDestruct()
{
	if (USharedWorldSubsystem* S = SW()) S->OnChanged.Remove(ChangedHandle);
	Super::NativeDestruct();
}

void USharedWorldSessionWidget::SetCardText(UTextBlock* Text, const FText& Value)
{
	if (!Text) return;
	Text->SetText(Value);
	if (UWidget* Card = Text->GetParent())
	{
		Card->SetVisibility(Value.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	}
}

USharedWorldSubsystem* USharedWorldSessionWidget::SW() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

void USharedWorldSessionWidget::Close()
{
	// Baked Manage Session page — never RemoveFromParent. Return to the first switcher page.
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
void USharedWorldSessionWidget::OnBack() { Close(); }

void USharedWorldSessionWidget::UpdateTabs()
{
	for (int32 i = 0; i < TabButtons.Num(); ++i)
	{
		StyleTabButton(TabButtons[i], i == ActiveTab);
	}
}

void USharedWorldSessionWidget::SetStatusMessage(const FText& Message)
{
	if (FlashText)
	{
		FlashText->SetColorAndOpacity(FSlateColor(TextMuted));
		FlashText->SetText(Message);
	}
}

void USharedWorldSessionWidget::Refresh()
{
	USharedWorldSubsystem* S = SW();
	if (!S || !OverviewText) return;
	const FString ActiveId = S->GetActiveWorldId();
	const bool bHasWorld = !ActiveId.IsEmpty();
	if (MakeSharedButton)
	{
		MakeSharedButton->SetVisibility(bHasWorld ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (InviteButton)
	{
		InviteButton->SetVisibility(bHasWorld && ActiveTab == 1 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}

	if (!bHasWorld)
	{
		SetCardText(OverviewText, NSLOCTEXT("SharedWorld", "NotSharedYet",
			"This save is not a Shared World yet.\n\nMake it shared so friends can Play / Join automatically."));
		SetCardText(HostText, FText::GetEmpty());
		SetCardText(RankingText, FText::GetEmpty());
		return;
	}

	SetCardText(OverviewText, FText::FromString(S->DescribeActiveSession()));
	const FString Code = S->EnsureInviteCode(ActiveId);
	FString HostBlock = FString::Printf(TEXT("Open the Players tab to add people from this game.\nShare code: %s\n"), *Code);
	FString RankBlock = TEXT("PREFERRED SUCCESSORS\n");
	FString Adv;
	const FString Diag = S->GetHostMigrationDiagnostics(ActiveId);
	Adv = Diag;
	TArray<FString> Lines;
	Diag.ParseIntoArrayLines(Lines);
	FString Preferred;
	TArray<FString> RankLines;
	bool bInCandidates = false;
	int32 RankShown = 0;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("Preferred successor:"))) Preferred = Line.RightChop(20).TrimStartAndEnd();
		else if (Line.StartsWith(TEXT("HOST CANDIDATES"))) bInCandidates = true;
		else if (bInCandidates && Line.Len() > 2 && FChar::IsDigit(Line[0]) && RankShown < 3)
		{
			RankLines.Add(Line);
			++RankShown;
		}
	}
	HostBlock += Preferred.IsEmpty() || Preferred == TEXT("(none)")
		? TEXT("Preferred Successor: (none yet)\n")
		: FString::Printf(TEXT("Preferred Successor: %s\n"), *Preferred);
	for (const FString& R : RankLines) RankBlock += R + TEXT("\n");
	if (RankLines.Num() == 0) RankBlock += TEXT("(ranking updates while connected)\n");
	SetCardText(HostText, FText::FromString(HostBlock));
	SetCardText(RankingText, FText::FromString(RankBlock.Left(1200)));
	if (AdvancedText) AdvancedText->SetText(FText::FromString(Adv.Left(4000)));
}

void USharedWorldSessionWidget::AddInviteRow(UVerticalBox* List, const FString& WorldId, const FString& PlayerId, const FString& DisplayName, const FString& SubLabel)
{
	if (!List || !WidgetTree) return;
	const FString Name = DisplayName.IsEmpty() ? PlayerId : DisplayName;
	UBorder* Row = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(12.f, 8.f), RadiusM, 1.f);
	List->AddChildToVerticalBox(Row)->SetPadding(FMargin(0, 0, 0, 6));
	UHorizontalBox* H = WidgetTree->ConstructWidget<UHorizontalBox>();
	Row->SetContent(H);
	H->AddChildToHorizontalBox(MakePlayerAvatar(WidgetTree, Name, 40.f))->SetVerticalAlignment(VAlign_Center);
	UVerticalBox* Labels = WidgetTree->ConstructWidget<UVerticalBox>();
	if (UHorizontalBoxSlot* LabelSlot = H->AddChildToHorizontalBox(Labels))
	{
		LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetPadding(FMargin(12.f, 0.f, 8.f, 0.f));
	}
	UTextBlock* NameText = MakeText(WidgetTree, 15, TextPrimary, true);
	NameText->SetText(FText::FromString(Name));
	NameText->SetAutoWrapText(false);
	NameText->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	Labels->AddChildToVerticalBox(NameText);
	if (!SubLabel.IsEmpty())
	{
		UTextBlock* Sub = MakeText(WidgetTree, 12, TextMuted);
		Sub->SetText(FText::FromString(SubLabel));
		Labels->AddChildToVerticalBox(Sub);
	}
	// Orange: inviting someone is a Shared Worlds configuration action.
	TObjectPtr<UTextBlock> AddLabel;
	UButton* AddBtn = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, AddLabel, NSLOCTEXT("SharedWorld", "AddPlayer", "Add"), 14, FMargin(20.f, 7.f));
	USharedWorldInviteRowBinder* Binder = NewObject<USharedWorldInviteRowBinder>(this);
	Binder->Session = this;
	Binder->WorldId = WorldId;
	Binder->PlayerId = PlayerId;
	Binder->DisplayName = DisplayName;
	InviteBinders.Add(Binder);
	AddBtn->OnClicked.AddDynamic(Binder, &USharedWorldInviteRowBinder::OnInviteClicked);
	if (UHorizontalBoxSlot* BtnSlot = H->AddChildToHorizontalBox(AddBtn))
	{
		BtnSlot->SetVerticalAlignment(VAlign_Center);
	}
}

void USharedWorldSessionWidget::SetOverviewVisible(bool bVisible)
{
	const ESlateVisibility Vis = bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
	for (UTextBlock* T : { OverviewText.Get(), HostText.Get(), RankingText.Get() })
	{
		if (!T) continue;
		T->SetVisibility(Vis);
		// Cards hide with their text; Refresh() re-collapses any that are empty when the tab shows again.
		if (UWidget* Card = T->GetParent()) Card->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (MakeSharedButton && bVisible)
	{
		// Refresh() decides MakeShared visibility from world state.
	}
	else if (MakeSharedButton && !bVisible)
	{
		MakeSharedButton->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (MigrateButton) MigrateButton->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (SaveButton) SaveButton->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	if (StopButton) StopButton->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

void USharedWorldSessionWidget::RefreshSessionPlayersIfChanged()
{
	USharedWorldSubsystem* S = SW();
	if (!S || !SessionPlayersList || ActiveTab != 1) return;
	const FString LocalId = S->GetLocalPlayerId();
	FString Key;
	for (const FSharedWorldFriendInfo& P : S->GetConnectedSessionPlayers())
	{
		if (!LocalId.IsEmpty() && P.PlayerId == LocalId) continue;
		Key += P.PlayerId + TEXT("|");
	}
	if (Key == LastSessionPlayerKey) return;
	LastSessionPlayerKey = Key;
	RebuildPlayersPanel(false);
}

void USharedWorldSessionWidget::RebuildPlayersPanel(bool bForceMembersReload)
{
	USharedWorldSubsystem* S = SW();
	if (!S || !SessionPlayersList) return;
	const FString Id = S->GetActiveWorldId();
	SessionPlayersList->ClearChildren();

	if (Id.IsEmpty())
	{
		if (SectionText) SectionText->SetText(NSLOCTEXT("SharedWorld", "MakeFirst", "Make this a Shared World first."));
		if (InviteCodeText) InviteCodeText->SetVisibility(ESlateVisibility::Collapsed);
		if (MembersText) MembersText->SetVisibility(ESlateVisibility::Collapsed);
		LastSessionPlayerKey.Reset();
		return;
	}

	const FString Code = S->EnsureInviteCode(Id);
	if (InviteCodeText)
	{
		InviteCodeText->SetVisibility(ESlateVisibility::HitTestInvisible);
		InviteCodeText->SetText(FText::FromString(FString::Printf(TEXT("Invite code: %s"), *Code)));
	}
	if (SectionText)
	{
		SectionText->SetText(NSLOCTEXT("SharedWorld", "PlayersHelp",
			"Add people from this game session — they get membership and the share code.\nFriends not in-game can be invited below."));
	}
	if (InviteButton) InviteButton->SetVisibility(ESlateVisibility::Visible);

	const FString LocalId = S->GetLocalPlayerId();
	const TArray<FSharedWorldFriendInfo> InGame = S->GetConnectedSessionPlayers();
	FString Key;
	int32 Shown = 0;
	for (const FSharedWorldFriendInfo& P : InGame)
	{
		if (!LocalId.IsEmpty() && P.PlayerId == LocalId) continue;
		AddInviteRow(SessionPlayersList, Id, P.PlayerId, P.DisplayName, TEXT("Connected now · tap Add"));
		Key += P.PlayerId + TEXT("|");
		++Shown;
	}
	LastSessionPlayerKey = Key;
	if (Shown == 0)
	{
		UTextBlock* Empty = MakeText(WidgetTree, 13, TextMuted);
		Empty->SetText(NSLOCTEXT("SharedWorld", "NoSessionPlayers",
			"Nobody else is in this game yet. When friends join your session, they appear here with an Add button."));
		SessionPlayersList->AddChildToVerticalBox(Empty);
	}

	if (MembersText && (bForceMembersReload || !bMembersLoaded))
	{
		MembersText->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (!bMembersLoaded)
		{
			MembersText->SetText(NSLOCTEXT("SharedWorld", "LoadingMembers", "Loading Shared World members…"));
		}
		S->FetchPlayers(Id, [this](bool bOk, const FString& Message)
		{
			if (!MembersText || ActiveTab != 1) return;
			bMembersLoaded = true;
			MembersText->SetText(FText::FromString(bOk ? TEXT("MEMBERS\n") + Message : TEXT("Members: ") + Message));
		});
	}
}

void USharedWorldSessionWidget::InvitePlayer(const FString& WorldId, const FString& PlayerId, const FString& DisplayName)
{
	USharedWorldSubsystem* S = SW();
	if (!S || WorldId.IsEmpty()) return;
	const FString Who = PlayerId.IsEmpty() ? DisplayName : PlayerId;
	if (Who.IsEmpty())
	{
		if (FlashText) FlashText->SetText(NSLOCTEXT("SharedWorld", "NeedFriend", "Pick a player to invite."));
		return;
	}
	if (FlashText)
	{
		FlashText->SetColorAndOpacity(FSlateColor(TextMuted));
		FlashText->SetText(FText::FromString(FString::Printf(TEXT("Adding %s…"), DisplayName.IsEmpty() ? *Who : *DisplayName)));
	}
	S->Invites().InviteFriend(WorldId, Who, DisplayName.IsEmpty() ? Who : DisplayName, [this](bool bOk, const FString& Message)
	{
		if (FlashText)
		{
			FlashText->SetColorAndOpacity(FSlateColor(bOk ? Ok : Err));
			FlashText->SetText(FText::FromString(Message));
		}
		if (bOk && ActiveTab == 1) RebuildPlayersPanel(true);
	});
}

void USharedWorldSessionWidget::OnTabOverview()
{
	ActiveTab = 0;
	UpdateTabs();
	bFriendsExpanded = false;
	bMembersLoaded = false;
	LastSessionPlayerKey.Reset();
	SetOverviewVisible(true);
	if (SectionText) SectionText->SetText(FText::GetEmpty());
	if (InviteNameInput) InviteNameInput->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteFriendsList)
	{
		InviteFriendsList->ClearChildren();
		InviteFriendsList->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (InviteCodeText) InviteCodeText->SetVisibility(ESlateVisibility::Collapsed);
	if (SessionPlayersList) SessionPlayersList->ClearChildren();
	if (MembersText) MembersText->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteButton) InviteButton->SetVisibility(ESlateVisibility::Collapsed);
	InviteBinders.Reset();
	Refresh();
}

void USharedWorldSessionWidget::OnTabPlayers()
{
	ActiveTab = 1;
	UpdateTabs();
	bMembersLoaded = false;
	LastSessionPlayerKey.Reset();
	SetOverviewVisible(false);
	if (USharedWorldSubsystem* Sub = SW())
	{
		Sub->Discovery().BeginRefresh(); // kick Steam friends pull
	}
	RebuildPlayersPanel(true);
}

void USharedWorldSessionWidget::OnTabHistory()
{
	ActiveTab = 2;
	UpdateTabs();
	bFriendsExpanded = false;
	SetOverviewVisible(false);
	if (InviteNameInput) InviteNameInput->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteFriendsList)
	{
		InviteFriendsList->ClearChildren();
		InviteFriendsList->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (InviteCodeText) InviteCodeText->SetVisibility(ESlateVisibility::Collapsed);
	if (SessionPlayersList) SessionPlayersList->ClearChildren();
	if (MembersText) MembersText->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteButton) InviteButton->SetVisibility(ESlateVisibility::Collapsed);
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Id = S->GetActiveWorldId();
	if (Id.IsEmpty())
	{
		if (SectionText) SectionText->SetText(NSLOCTEXT("SharedWorld", "NoActive", "No active Shared World."));
		return;
	}
	if (SectionText) SectionText->SetText(NSLOCTEXT("SharedWorld", "LoadingHistory", "Loading save history…"));
	S->FetchHistory(Id, 25, [this](bool bOk, const FString& Message)
	{
		if (SectionText)
		{
			SectionText->SetText(FText::FromString(bOk ? TEXT("SAVE HISTORY\n\n") + Message : TEXT("Failed: ") + Message));
		}
	});
}

void USharedWorldSessionWidget::OnTabBackups()
{
	ActiveTab = 3;
	UpdateTabs();
	bFriendsExpanded = false;
	SetOverviewVisible(false);
	if (InviteNameInput) InviteNameInput->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteFriendsList)
	{
		InviteFriendsList->ClearChildren();
		InviteFriendsList->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (InviteCodeText) InviteCodeText->SetVisibility(ESlateVisibility::Collapsed);
	if (SessionPlayersList) SessionPlayersList->ClearChildren();
	if (MembersText) MembersText->SetVisibility(ESlateVisibility::Collapsed);
	if (InviteButton) InviteButton->SetVisibility(ESlateVisibility::Collapsed);
	USharedWorldSubsystem* S = SW();
	if (!S || !SectionText) return;
	const FString Id = S->GetActiveWorldId();
	FString Out = TEXT("BACKUPS / RECOVERY\n\nRecovery backups are created automatically when needed.\n");
	if (!Id.IsEmpty())
	{
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId != Id) continue;
			Out += V.BackupPath.IsEmpty() ? TEXT("\nNo recovery backup for this session.\n") : TEXT("\nA recovery backup is available.\n");
			break;
		}
	}
	SectionText->SetText(FText::FromString(Out));
}

void USharedWorldSessionWidget::OnMakeSharedWorld()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	if (FlashText) FlashText->SetText(NSLOCTEXT("SharedWorld", "Creating", "Creating Shared World..."));
	S->Creation().CreateFromCurrentWorld(FString(), [this](bool bOk, const FString& Message)
	{
		if (FlashText)
		{
			FlashText->SetColorAndOpacity(FSlateColor(bOk ? Ok : Err));
			FlashText->SetText(FText::FromString(Message));
		}
		Refresh();
	});
}

void USharedWorldSessionWidget::OnInviteFriend()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Id = S->GetActiveWorldId();
	bFriendsExpanded = true;
	S->Discovery().BeginRefresh();
	if (InviteNameInput) InviteNameInput->SetVisibility(ESlateVisibility::Visible);
	if (InviteFriendsList)
	{
		InviteFriendsList->ClearChildren();
		InviteFriendsList->SetVisibility(ESlateVisibility::Visible);
		UTextBlock* Hdr = MakeText(WidgetTree, 13, TextMuted, true);
		Hdr->SetText(NSLOCTEXT("SharedWorld", "SteamFriendsHdr", "STEAM FRIENDS"));
		InviteFriendsList->AddChildToVerticalBox(Hdr)->SetPadding(FMargin(0, 0, 0, 6));
		const TArray<FSharedWorldFriendInfo> Friends = S->Discovery().ListFriends();
		int32 Added = 0;
		for (const FSharedWorldFriendInfo& F : Friends)
		{
			AddInviteRow(InviteFriendsList, Id, F.PlayerId, F.DisplayName,
				F.bOnline ? TEXT("Online · tap Add") : TEXT("Offline · tap Add"));
			++Added;
			if (InviteNameInput && InviteNameInput->GetText().IsEmpty() && F.bOnline)
			{
				InviteNameInput->SetText(FText::FromString(F.DisplayName));
			}
		}
		if (Added == 0)
		{
			UTextBlock* Empty = MakeText(WidgetTree, 13, TextMuted);
			Empty->SetText(NSLOCTEXT("SharedWorld", "NoSteamFriends",
				"Steam friends are still loading (or unavailable). Use the invite code, or open this again in a moment."));
			InviteFriendsList->AddChildToVerticalBox(Empty);
			// Retry once shortly after ReadFriendsList has a chance to complete.
			if (!bFriendsRetryPending)
			{
				if (UWorld* World = GetWorld())
				{
					bFriendsRetryPending = true;
					FTimerHandle Ignore;
					World->GetTimerManager().SetTimer(Ignore, FTimerDelegate::CreateWeakLambda(this, [this]()
					{
						bFriendsRetryPending = false;
						if (ActiveTab == 1 && bFriendsExpanded) OnInviteFriend();
					}), 2.0f, false);
				}
			}
		}
		else
		{
			bFriendsRetryPending = false;
		}
	}
}

void USharedWorldSessionWidget::OnInviteConfirm()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Id = S->GetActiveWorldId();
	const FString Who = InviteNameInput ? InviteNameInput->GetText().ToString().TrimStartAndEnd() : FString();
	InvitePlayer(Id, Who, Who);
}

void USharedWorldSessionWidget::OnCopyInviteCode()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Id = S->GetActiveWorldId();
	if (Id.IsEmpty()) return;
	const FString Code = S->EnsureInviteCode(Id);
	FPlatformApplicationMisc::ClipboardCopy(*Code);
	if (FlashText)
	{
		FlashText->SetColorAndOpacity(FSlateColor(Ok));
		FlashText->SetText(FText::FromString(FString::Printf(TEXT("Copied invite code %s"), *Code)));
	}
}

void USharedWorldSessionWidget::OnMigrate()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Id = S->GetActiveWorldId();
	if (Id.IsEmpty()) return;
	const FString Result = S->RequestMigrationTo(Id, TEXT("auto"));
	if (SectionText) SectionText->SetText(FText::FromString(Result));
}

void USharedWorldSessionWidget::OnReevaluate()
{
	Refresh();
	if (SectionText) SectionText->SetText(NSLOCTEXT("SharedWorld", "ReevalDone", "Host ranking refreshed."));
}

void USharedWorldSessionWidget::OnSaveSync()
{
	if (USharedWorldSubsystem* S = SW())
	{
		const FString Result = S->RequestCheckpoint();
		if (SectionText) SectionText->SetText(FText::FromString(Result));
	}
}

void USharedWorldSessionWidget::OnStopHosting()
{
	if (USharedWorldSubsystem* S = SW())
	{
		const FString Result = S->RequestStop();
		if (SectionText) SectionText->SetText(FText::FromString(Result));
	}
}

void USharedWorldSessionWidget::OnToggleAdvanced()
{
	bShowAdvanced = !bShowAdvanced;
	if (AdvancedBox) AdvancedBox->SetVisibility(bShowAdvanced ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}
