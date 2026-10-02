// World Details page: Overview / Players / Sync / Storage / Advanced.
// Only shows what the backend actually exposes; there is deliberately no Activity tab until an event log exists.

#include "UI/SharedWorldBrowserWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldBrowserModel.h"
#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldUiStyle.h"
#include "UI/SharedWorldWorldCard.h"

using namespace SharedWorldUi;

namespace
{
	/** Rounded plate whose children are compact label/value rows. */
	UVerticalBox* MakeStatPlate(UWidgetTree* Tree, UVerticalBox* Parent)
	{
		UBorder* Plate = MakePanel(Tree, RowFill, PanelEdge, FMargin(16.f, 14.f, 16.f, 8.f), RadiusM, 1.f);
		Parent->AddChildToVerticalBox(Plate)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		UVerticalBox* Inner = Tree->ConstructWidget<UVerticalBox>();
		Plate->SetContent(Inner);
		return Inner;
	}
}

void USharedWorldBrowserWidget::RebuildDetails()
{
	FSharedWorldEntryView View;
	bool bHas = false;
	if (USharedWorldSubsystem* S = SW())
	{
		for (const FSharedWorldEntryView& V : S->GetWorldViews())
		{
			if (V.WorldId == SelectedWorldId) { View = V; bHas = true; break; }
		}
	}
	AddPageHeader(NSLOCTEXT("SharedWorld", "DetailsTitle", "World Details"), FText::GetEmpty());
	if (!bHas)
	{
		UButton* Unused = nullptr;
		PageBody->AddChildToVerticalBox(MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "MissingWorldTitle", "World not found"),
			NSLOCTEXT("SharedWorld", "MissingWorld", "This Shared World is not in your list on this PC."), Unused));
		return;
	}
	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(Body);
	ScrollContentBox = Body;
	PopulateDetailsBody(Body, View);
	ListScrollOwner = EPage::Details;
	RestoreListScroll(EPage::Details);
}

void USharedWorldBrowserWidget::PopulateDetailsBody(UVerticalBox* Body, const FSharedWorldEntryView& View)
{
	if (!Body || !WidgetTree) return;
	USharedWorldSubsystem* S = SW();
	const FSharedWorldBrowserItem Item = SharedWorldBrowserModel::MakeItem(View, S ? S->GetMostRecentlyPlayedWorldId() : FString());

	// ---- hero: thumbnail, name, status, primary action
	UHorizontalBox* Hero = WidgetTree->ConstructWidget<UHorizontalBox>();
	Body->AddChildToVerticalBox(Hero)->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	USizeBox* Thumb = WidgetTree->ConstructWidget<USizeBox>();
	Thumb->SetWidthOverride(208.f);
	Thumb->SetHeightOverride(117.f);
	Thumb->SetClipping(EWidgetClipping::ClipToBounds);
	Thumb->AddChild(USharedWorldWorldCard::BuildThumbnail(WidgetTree, Item.Id(), Item.DisplayName(), GetThumbnail(Item.Id()), 56));
	Hero->AddChildToHorizontalBox(Thumb)->SetPadding(FMargin(0.f, 0.f, 18.f, 0.f));

	UVerticalBox* HeroText = WidgetTree->ConstructWidget<UVerticalBox>();
	UTextBlock* Name = MakeText(WidgetTree, 30, TextPrimary, true);
	Name->SetText(FText::FromString(Item.DisplayName()));
	HeroText->AddChildToVerticalBox(Name)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	HeroText->AddChildToVerticalBox(MakeStatusBadge(WidgetTree, Item.Tone, Item.StatusLabel, 16, 14.f))->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	UTextBlock* Detail = MakeText(WidgetTree, FontBody, TextMuted);
	Detail->SetText(FText::FromString(Item.DetailStatus));
	HeroText->AddChildToVerticalBox(Detail)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	{
		TObjectPtr<UTextBlock> PlayLbl;
		UButton* PlayBtn = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Game, PlayLbl, Item.ActionLabel, 20, FMargin(40.f, 10.f));
		PlayBtn->SetIsEnabled(Item.bActionEnabled);
		PlayBtn->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnPlaySelected);
		HeroText->AddChildToVerticalBox(PlayBtn)->SetHorizontalAlignment(HAlign_Left);
		PlayButton = PlayBtn;
		PlayLabel = PlayLbl;
	}
	if (UHorizontalBoxSlot* S2 = Hero->AddChildToHorizontalBox(HeroText))
	{
		S2->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S2->SetVerticalAlignment(VAlign_Center);
	}

	// ---- tabs
	TArray<FText> Tabs;
	Tabs.Add(NSLOCTEXT("SharedWorld", "DTabOverview", "Overview"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "DTabPlayers", "Players"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "DTabSync", "Sync"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "DTabStorage", "Storage"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "DTabAdvanced", "Advanced"));
	AddTabs(Body, 3, Tabs, DetailsTab);

	switch (DetailsTab)
	{
	case 1: AddDetailsPlayers(Body, Item); break;
	case 2: AddDetailsSync(Body, Item); break;
	case 3: AddDetailsStorage(Body, Item); break;
	case 4: AddDetailsAdvanced(Body, Item); break;
	default: AddDetailsOverview(Body, Item); break;
	}
}

void USharedWorldBrowserWidget::AddDetailsOverview(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	const FSharedWorldEntryView& V = Item.View;
	UVerticalBox* P = MakeStatPlate(WidgetTree, Col);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvOwner", "Owner"),
		V.bOwned ? NSLOCTEXT("SharedWorld", "OwnerYou", "You") : NSLOCTEXT("SharedWorld", "OwnerShared", "Shared with you"), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvLastPlayed", "Last played"),
		FText::FromString(V.LastPlayed.IsEmpty() ? FString(TEXT("Unknown")) : V.LastPlayed), V.LastPlayed.IsEmpty() ? TextMuted : TextPrimary);
	FString Host = V.HostName;
	if (Host.IsEmpty()) Host = V.LastHostName.IsEmpty() ? FString(TEXT("Nobody hosting")) : FString::Printf(TEXT("Nobody hosting (last: %s)"), *V.LastHostName);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvHost", "Current host"), FText::FromString(Host), V.HostName.IsEmpty() ? TextMuted : TextPrimary);
	if (V.IsHostingNow())
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvPlayers", "Players online"),
			FText::FromString(FString::Printf(TEXT("%d / %d"), V.PlayerCount, V.MaxPlayers > 0 ? V.MaxPlayers : 4)), TextPrimary);
	}
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvRevision", "Revision"), FText::AsNumber(V.Revision), TextPrimary);
	if (!V.MapLabel.IsEmpty())
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvMap", "Map"), FText::FromString(V.MapLabel), TextPrimary);
	}
	if (!V.PlaytimeText.IsEmpty())
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvPlaytime", "Playtime"), FText::FromString(V.PlaytimeText), TextPrimary);
	}
	if (!V.GamePhase.IsEmpty())
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvPhase", "Game phase"), FText::FromString(V.GamePhase), TextPrimary);
	}
	if (V.RequiredModCount > 0)
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "OvMods", "Required mods"), FText::AsNumber(V.RequiredModCount), TextPrimary);
	}
	if (V.bHasError)
	{
		UButton* Unused = nullptr;
		Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Problem,
			NSLOCTEXT("SharedWorld", "OvErrTitle", "This world needs attention"), FText::FromString(V.ErrorMessage), Unused))
			->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
		AddActionRow(Col, NSLOCTEXT("SharedWorld", "DismissError", "Dismiss"), ESharedWorldButtonRole::Secondary, 40)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDismissError);
	}
}

void USharedWorldBrowserWidget::AddDetailsPlayers(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	const FSharedWorldEntryView& V = Item.View;
	bool bAny = false;
	if (V.OnlinePlayerNames.Num() > 0)
	{
		AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "OnlineNow", "ONLINE NOW"));
		for (const FString& Name : V.OnlinePlayerNames)
		{
			const bool bHost = !V.HostName.IsEmpty() && Name.Equals(V.HostName, ESearchCase::IgnoreCase);
			const FString Detail = (bHost && V.PingMs >= 0) ? FString::Printf(TEXT("Ping %d ms"), V.PingMs) : FString();
			Col->AddChildToVerticalBox(MakePlayerRow(WidgetTree, Name, bHost ? FString(TEXT("Host")) : FString(TEXT("Player")),
				ESharedWorldTone::Healthy, NSLOCTEXT("SharedWorld", "OnlineLbl", "Online"), Detail))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		}
		bAny = true;
	}
	if (bDetailsMembersLoading && !bDetailsMembersLoaded)
	{
		AddMutedLine(Col, NSLOCTEXT("SharedWorld", "LoadingMembers", "Loading members..."));
		return;
	}
	bool bOfflineHeader = false;
	if (!DetailsMembersText.IsEmpty())
	{
		TArray<FString> Lines;
		DetailsMembersText.ParseIntoArrayLines(Lines, false);
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.IsEmpty()) continue;
			if (Line.StartsWith(TEXT("Open:")) || Line.StartsWith(TEXT("Members only")))
			{
				AddMutedLine(Col, FText::FromString(Line));
				continue;
			}
			FString Display = Line, Role;
			if (Line.Split(TEXT("  ("), &Display, &Role)) Role.RemoveFromEnd(TEXT(")"));
			bool bOnline = false;
			for (const FString& O : V.OnlinePlayerNames)
			{
				if (O.Equals(Display, ESearchCase::IgnoreCase)) { bOnline = true; break; }
			}
			if (bOnline) continue;
			if (!bOfflineHeader)
			{
				AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "Members", "MEMBERS"));
				bOfflineHeader = true;
			}
			Col->AddChildToVerticalBox(MakePlayerRow(WidgetTree, Display, Role, ESharedWorldTone::Inactive,
				NSLOCTEXT("SharedWorld", "OfflineLbl", "Offline")))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
			bAny = true;
		}
	}
	if (!bAny)
	{
		UButton* Unused = nullptr;
		Col->AddChildToVerticalBox(MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "NoMembersTitle", "No members online"),
			NSLOCTEXT("SharedWorld", "NoMembersBody", "Players show up here when they join this Shared World."), Unused));
	}
	if (Item.bCanInvite)
	{
		AddActionRow(Col, NSLOCTEXT("SharedWorld", "InviteCopy", "Invite Players (copy code)"), ESharedWorldButtonRole::Config, 44)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnMoreInvite);
	}
}

void USharedWorldBrowserWidget::AddDetailsSync(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	const FSharedWorldEntryView& V = Item.View;
	UVerticalBox* P = MakeStatPlate(WidgetTree, Col);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "SyRev", "Cloud revision"), FText::AsNumber(V.Revision), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "SyLast", "Last played"),
		FText::FromString(V.LastPlayed.IsEmpty() ? FString(TEXT("Unknown")) : V.LastPlayed), V.LastPlayed.IsEmpty() ? TextMuted : TextPrimary);
	{
		UHorizontalBox* Badge = nullptr;
		if (Item.Status == ESharedWorldStatus::Error)
			Badge = MakeStatusBadge(WidgetTree, ESharedWorldTone::Problem, NSLOCTEXT("SharedWorld", "SyErr", "Needs attention"), FontBody);
		else if (Item.Status == ESharedWorldStatus::Syncing)
			Badge = MakeStatusBadge(WidgetTree, ESharedWorldTone::Working, NSLOCTEXT("SharedWorld", "SySyncing", "Syncing"), FontBody);
		else if (Item.Status == ESharedWorldStatus::Unreachable)
			Badge = MakeStatusBadge(WidgetTree, ESharedWorldTone::Problem, NSLOCTEXT("SharedWorld", "SyUnreach", "Can't reach cloud"), FontBody);
		else
			Badge = MakeStatusBadge(WidgetTree, ESharedWorldTone::Inactive, NSLOCTEXT("SharedWorld", "SyIdle", "No sync in progress"), FontBody);
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		P->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		UTextBlock* L = MakeText(WidgetTree, FontBody - 1, TextMuted);
		L->SetText(NSLOCTEXT("SharedWorld", "SyStatus", "Sync status"));
		USizeBox* LB = WidgetTree->ConstructWidget<USizeBox>();
		LB->SetWidthOverride(150.f);
		LB->AddChild(L);
		Row->AddChildToHorizontalBox(LB)->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
		Row->AddChildToHorizontalBox(Badge);
	}

	AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "SaveHistory", "SAVE HISTORY"));
	AddActionRow(Col, NSLOCTEXT("SharedWorld", "LoadHistory", "Load save history"), ESharedWorldButtonRole::Secondary, 42)
		->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsHistory);
	if (!DetailsHistoryText.IsEmpty())
	{
		UBorder* Box = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(14.f, 10.f), RadiusM, 1.f);
		UTextBlock* T = MakeText(WidgetTree, FontSmall, TextMuted);
		T->SetText(FText::FromString(DetailsHistoryText));
		Box->SetContent(T);
		Col->AddChildToVerticalBox(Box)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	}
	if (V.bOwned)
	{
		AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "RestoreSection", "RESTORE AN OLDER SAVE"));
		UTextBlock* Why = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		Why->SetText(NSLOCTEXT("SharedWorld", "RestoreWhy", "Enter a revision number from the history. It becomes the newest revision for everyone."));
		Col->AddChildToVerticalBox(Why)->SetPadding(FMargin(2.f, 0.f, 0.f, 8.f));
		RestoreInput = AddTextField(Col, NSLOCTEXT("SharedWorld", "RestoreHint", "Revision number"), FString());
		AddActionRow(Col, NSLOCTEXT("SharedWorld", "RestoreBtn", "Restore revision"), ESharedWorldButtonRole::Danger, 42)
			->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsRestore);
	}
}

void USharedWorldBrowserWidget::AddDetailsStorage(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	USharedWorldSubsystem* S = SW();
	const sw::WorldEntry* Entry = S ? S->FindWorldEntry(Item.Id()) : nullptr;
	UVerticalBox* P = MakeStatPlate(WidgetTree, Col);
	if (!Entry)
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StNone", "Storage"), NSLOCTEXT("SharedWorld", "StUnknown", "Unknown"), TextMuted);
		return;
	}
	const bool bGit = Entry->Provider.Kind == sw::ProviderKind::GitHub;
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StProvider", "Provider"), bGit ? NSLOCTEXT("SharedWorld", "StGit", "GitHub") : NSLOCTEXT("SharedWorld", "StFolder", "Shared folder"), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StWhere", "Location"),
		FText::FromString(bGit ? FString::Printf(TEXT("%s/%s"), UTF8_TO_TCHAR(Entry->Provider.Owner.c_str()), UTF8_TO_TCHAR(Entry->Provider.Repo.c_str()))
			: FString(UTF8_TO_TCHAR(Entry->Provider.FolderPath.c_str()))), TextPrimary);
	UTextBlock* Note = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Note->SetText(NSLOCTEXT("SharedWorld", "StNote", "The latest save of this world is kept here so any player can pick it up. Manage the account under Shared Worlds Settings."));
	Col->AddChildToVerticalBox(Note)->SetPadding(FMargin(2.f, 4.f, 0.f, 0.f));
}

void USharedWorldBrowserWidget::AddDetailsAdvanced(UVerticalBox* Col, const FSharedWorldBrowserItem& Item)
{
	const FSharedWorldEntryView& V = Item.View;
	UTextBlock* AdvNote = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	AdvNote->SetText(NSLOCTEXT("SharedWorld", "AdvNote", "Technical details for troubleshooting. You don't need these to play."));
	Col->AddChildToVerticalBox(AdvNote)->SetPadding(FMargin(2.f, 0.f, 0.f, 10.f));
	UVerticalBox* P = MakeStatPlate(WidgetTree, Col);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdWorldId", "World ID"), FText::FromString(V.WorldId), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdRev", "Revision"), FText::AsNumber(V.Revision), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdGen", "Generation"), FText::AsNumber(V.Generation), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdCloud", "Cloud state"), FText::FromString(V.CloudStatus), TextPrimary);
	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdLocal", "Local state"), FText::FromString(V.LocalState.IsEmpty() ? FString(TEXT("IDLE")) : V.LocalState), TextPrimary);
	if (!V.InviteCode.IsEmpty())
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdInvite", "Invite code"), FText::FromString(V.InviteCode), TextPrimary);
	}
	if (V.bHasError)
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "AdErrCode", "Error code"), FText::FromString(V.ErrorCode), Err);
		if (!V.ErrorDetail.IsEmpty())
		{
			UTextBlock* D = MakeText(WidgetTree, FontSmall, TextMuted);
			D->SetText(FText::FromString(V.ErrorDetail));
			P->AddChildToVerticalBox(D)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		}
	}
}
