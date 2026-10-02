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
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Misc/Guid.h"
#include "Rclone/RcloneObjectStore.h"
#include "Rclone/RcloneProviders.h"
#include "Rclone/RcloneRuntime.h"
#include "SharedWorldUeConvert.h"

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
	// A world stored entirely on a provider (record, locks and saves), e.g. Dropbox.
	const bool bRcloneWorld = Entry->Provider.Kind == sw::ProviderKind::Rclone;
	// A GitHub/folder world whose save files alone live on a provider.
	const bool bRcloneSaves = !bRcloneWorld && !Entry->SaveBackend.empty();
	const FString SaveLabel = bRcloneWorld
		? SharedWorldUe::ToFString(Entry->Provider.Label.empty() ? Entry->Provider.Backend : Entry->Provider.Label)
		: (!Entry->SaveLabel.empty() ? SharedWorldUe::ToFString(Entry->SaveLabel) : SharedWorldUe::ToFString(Entry->SaveBackend));
	const bool bLinked = bRcloneWorld ? !Entry->Provider.Remote.empty() : !Entry->SaveRemote.empty();
	const FString LinkBackend = SharedWorldUe::ToFString(bRcloneWorld ? Entry->Provider.Backend : Entry->SaveBackend);
	const FString LinkedRemote = SharedWorldUe::ToFString(bRcloneWorld ? Entry->Provider.Remote : Entry->SaveRemote);

	AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StProvider", "Provider"),
		bRcloneWorld ? FText::FromString(SaveLabel) : (bGit ? NSLOCTEXT("SharedWorld", "StGit", "GitHub") : NSLOCTEXT("SharedWorld", "StFolder", "Shared folder")), TextPrimary);
	if (bRcloneWorld)
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StWhere", "Location"),
			bLinked ? FText::FromString(LinkedRemote + TEXT("/") + Item.Id()) : NSLOCTEXT("SharedWorld", "StNotLinkedHere", "Not linked on this PC"), bLinked ? TextPrimary : Warn);
	}
	else
	{
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StWhere", "Location"),
			FText::FromString(bGit ? FString::Printf(TEXT("%s/%s"), UTF8_TO_TCHAR(Entry->Provider.Owner.c_str()), UTF8_TO_TCHAR(Entry->Provider.Repo.c_str()))
				: FString(UTF8_TO_TCHAR(Entry->Provider.FolderPath.c_str()))), TextPrimary);
		AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StSaves", "Save files"),
			bRcloneSaves ? FText::FromString(SaveLabel) : NSLOCTEXT("SharedWorld", "StSavesDefault", "With the world record"), TextPrimary);
		if (bRcloneSaves)
		{
			AddDetailStatRow(P, NSLOCTEXT("SharedWorld", "StSavesHere", "On this PC"),
				bLinked ? FText::FromString(LinkedRemote + TEXT("/") + Item.Id()) : NSLOCTEXT("SharedWorld", "StSavesNotLinked", "Not linked"), bLinked ? Ok : Warn);
		}
	}
	UTextBlock* Note = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Note->SetText(bRcloneWorld
		? FText::Format(NSLOCTEXT("SharedWorld", "StNoteWorld",
			"Everything for this world (who can play, who is hosting, history and saves) is kept on {0}. Each player links their own {0} once; the owner shares the folder with them in {0}."),
			FText::FromString(SaveLabel))
		: (bRcloneSaves
			? FText::Format(NSLOCTEXT("SharedWorld", "StNoteRclone",
				"Who can play and who is hosting are kept above; the saves themselves are on {0}. Anyone can join while someone else hosts. To host, link {0} on this PC."),
				FText::FromString(SaveLabel))
			: NSLOCTEXT("SharedWorld", "StNote", "The latest save of this world is kept here so any player can pick it up. Manage the account under Shared Worlds Settings.")));
	Col->AddChildToVerticalBox(Note)->SetPadding(FMargin(2.f, 4.f, 0.f, 0.f));

	// ---- move this world (record, locks, history and saves) to the active provider
	if (Entry->Relation == sw::WorldRelation::Owned)
	{
		const FString Active = FRcloneConnections::GetDefaultSaveRemote();
		for (const FRcloneConnection& C : FRcloneConnections::Load())
		{
			if (C.RemoteName != Active) continue;
			const bool bAlreadyThere = bRcloneWorld && LinkedRemote == C.Fs();
			if (!bAlreadyThere)
			{
				TObjectPtr<UTextBlock> L;
				UButton* Move = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L,
					FText::Format(NSLOCTEXT("SharedWorld", "StMoveTo", "Move This World to {0}"), FText::FromString(C.Label)), 15, FMargin(16.f, 9.f));
				Move->SetToolTipText(FText::Format(NSLOCTEXT("SharedWorld", "StMoveToTip",
					"Copies everything (who can play, who is hosting, history and saves) to {0}. The old copy is kept but frozen. Nobody can be playing it while it moves."),
					FText::FromString(C.Label)));
				Move->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnDetailsMoveToActive);
				if (UVerticalBoxSlot* MS = Col->AddChildToVerticalBox(Move))
				{
					MS->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));
					MS->SetHorizontalAlignment(HAlign_Left);
				}
			}
			break;
		}
	}
	if (!SaveLinkNotice.IsEmpty() && !bRcloneSaves && !bRcloneWorld)
	{
		UTextBlock* N = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		N->SetText(FText::FromString(SaveLinkNotice));
		Col->AddChildToVerticalBox(N)->SetPadding(FMargin(2.f, 6.f, 0.f, 0.f));
	}
	if (!bRcloneSaves && !bRcloneWorld) return;

	// ---- link / relink this PC to the world's save storage
	UTextBlock* LinkHead = MakeText(WidgetTree, FontBody, TextPrimary, true);
	LinkHead->SetText(bLinked ? NSLOCTEXT("SharedWorld", "StRelink", "Change the link") : FText::Format(NSLOCTEXT("SharedWorld", "StLinkHead", "Link {0}"), FText::FromString(SaveLabel)));
	Col->AddChildToVerticalBox(LinkHead)->SetPadding(FMargin(2.f, 16.f, 0.f, 2.f));
	// How a friend gets read + upload access depends on what kind of storage it is (same flow for every provider).
	FText AccessHow;
	{
		static const TSet<FString> KeyBased = { TEXT("s3"), TEXT("b2"), TEXT("azureblob"), TEXT("azurefiles"), TEXT("google cloud storage"),
			TEXT("swift"), TEXT("oracleobjectstorage"), TEXT("qingstor"), TEXT("storj"), TEXT("sia") };
		static const TSet<FString> ServerBased = { TEXT("sftp"), TEXT("ftp"), TEXT("smb"), TEXT("webdav"), TEXT("hdfs"), TEXT("seafile") };
		if (KeyBased.Contains(LinkBackend))
		{
			AccessHow = FText::Format(NSLOCTEXT("SharedWorld", "StAccessKey",
				"Before you can play: the owner gives you an access key for the bucket with read and write access. Connect {0} in Settings > Storage with that key, then link here."),
				FText::FromString(SaveLabel));
		}
		else if (ServerBased.Contains(LinkBackend))
		{
			AccessHow = FText::Format(NSLOCTEXT("SharedWorld", "StAccessServer",
				"Before you can play: the owner gives you a login on the {0} server that can read and write the world's folder. Connect it in Settings > Storage, then link here."),
				FText::FromString(SaveLabel));
		}
		else
		{
			AccessHow = FText::Format(NSLOCTEXT("SharedWorld", "StAccessShare",
				"Before you can play: the owner shares the world's folder \"{1}\" with you in {0} with edit access. Connect your own {0} in Settings > Storage, then link here."),
				FText::FromString(SaveLabel), FText::FromString(Item.Id()));
		}
	}
	UTextBlock* Access = MakeText(WidgetTree, FontSmall, TextPrimary);
	Access->SetText(AccessHow);
	Col->AddChildToVerticalBox(Access)->SetPadding(FMargin(2.f, 0.f, 0.f, 6.f));
	UTextBlock* LinkHelp = MakeText(WidgetTree, FontSmall, TextMuted);
	LinkHelp->SetText(FText::Format(NSLOCTEXT("SharedWorld", "StLinkHelp",
		"Type the folder that contains the world's folder \"{0}\" (leave it empty if the world's folder was shared with you directly), then pick your connection. "
		"The mod checks you can read the world and upload to it before linking."),
		FText::FromString(Item.Id())));
	Col->AddChildToVerticalBox(LinkHelp)->SetPadding(FMargin(2.f, 0.f, 0.f, 8.f));

	TArray<FRcloneConnection> Matching;
	for (const FRcloneConnection& C : FRcloneConnections::Load())
	{
		if (C.BackendType == LinkBackend) Matching.Add(C);
	}
	if (Matching.Num() == 0)
	{
		UButton* Go = nullptr;
		Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
			FText::Format(NSLOCTEXT("SharedWorld", "StNoConn", "{0} isn't connected on this PC"), FText::FromString(SaveLabel)),
			NSLOCTEXT("SharedWorld", "StNoConnBody", "Connect it in Settings > Storage, then come back here to link this world."),
			Go, NSLOCTEXT("SharedWorld", "StOpenStorage", "Open Storage")))->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		if (Go) Go->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnSettingsClicked);
	}
	else
	{
		SaveLinkFolderInput = AddTextField(Col, NSLOCTEXT("SharedWorld", "StFolderHint", "Folder, e.g. SharedWorlds (empty = top level)"),
			bLinked ? LinkedRemote.RightChop(LinkedRemote.Find(TEXT(":")) + 1) : Matching[0].Folder);
		UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
		Grid->SetSlotPadding(FMargin(0.f, 0.f, 8.f, 8.f));
		Col->AddChildToVerticalBox(Grid);
		int32 Index = 0;
		auto Choice = [&](const FString& Remote, const FText& Label, ESharedWorldButtonRole Role)
		{
			TObjectPtr<UTextBlock> L;
			UButton* B = MakeRoleButton(WidgetTree, Role, L, Label, 15, FMargin(14.f, 9.f));
			USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
			Binder->Browser = this;
			Binder->TabKind = 6;
			Binder->ConnectValue = Remote;
			RowBinders.Add(Binder);
			B->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
			if (UUniformGridSlot* Cell = Grid->AddChildToUniformGrid(B, Index / 3, Index % 3)) Cell->SetHorizontalAlignment(HAlign_Fill);
			++Index;
		};
		for (const FRcloneConnection& C : Matching)
		{
			Choice(C.RemoteName, Matching.Num() > 1
				? FText::Format(NSLOCTEXT("SharedWorld", "StLinkWithN", "Link with {0} ({1})"), FText::FromString(C.Label), FText::FromString(C.RemoteName))
				: FText::Format(NSLOCTEXT("SharedWorld", "StLinkWith", "Link with {0}"), FText::FromString(C.Label)),
				ESharedWorldButtonRole::Config);
		}
		if (bLinked) Choice(FString(), NSLOCTEXT("SharedWorld", "StUnlink", "Unlink"), ESharedWorldButtonRole::Danger);
	}
	if (!SaveLinkNotice.IsEmpty())
	{
		UTextBlock* N = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		N->SetText(FText::FromString(SaveLinkNotice));
		Col->AddChildToVerticalBox(N)->SetPadding(FMargin(2.f, 6.f, 0.f, 0.f));
	}
}

void USharedWorldBrowserWidget::LinkWorldSaves(const FString& RemoteName)
{
	USharedWorldSubsystem* S = SW();
	if (!S || SelectedWorldId.IsEmpty()) return;
	const FString WorldId = SelectedWorldId;
	if (RemoteName.IsEmpty())
	{
		const FString LinkErr = S->SetWorldSaveRemote(WorldId, FString());
		SaveLinkNotice = LinkErr.IsEmpty() ? FString(TEXT("Unlinked. Link it again before playing this world.")) : LinkErr;
		ScheduleRebuild();
		return;
	}
	FString Folder = SaveLinkFolderInput ? SaveLinkFolderInput->GetText().ToString().TrimStartAndEnd() : FString();
	Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Folder.StartsWith(TEXT("/"))) Folder.RightChopInline(1);
	while (Folder.EndsWith(TEXT("/"))) Folder.LeftChopInline(1);
	if (Folder.Contains(TEXT("..")))
	{
		SaveLinkNotice = TEXT("That folder name isn't allowed.");
		ScheduleRebuild();
		return;
	}
	const FString Remote = RemoteName + TEXT(":") + Folder;
	const sw::WorldEntry* Entry = S->FindWorldEntry(WorldId);
	const bool bWholeWorld = Entry && Entry->Provider.Kind == sw::ProviderKind::Rclone;
	const FString Label = Entry ? SharedWorldUe::ToFString(bWholeWorld
		? (Entry->Provider.Label.empty() ? Entry->Provider.Backend : Entry->Provider.Label)
		: (Entry->SaveLabel.empty() ? Entry->SaveBackend : Entry->SaveLabel)) : FString(TEXT("the storage"));
	SaveLinkNotice = TEXT("Checking that you can read and upload to this world's folder...");
	ScheduleRebuild();

	// Every player must be able to read the world and upload to it (anyone may take over as host), so the link is only
	// made after proving both, whatever the provider: find the world, then write and remove a small test file.
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FRcloneRuntime::RunDetached([Weak, WorldId, Remote, bWholeWorld, Label]()
	{
		FString Fs = Remote;
		if (!Fs.EndsWith(TEXT(":"))) Fs += TEXT("/");
		Fs += WorldId;
		FString Problem;
		int32 Found = 0;
		if (bWholeWorld)
		{
			FRcloneLogStore Record(Fs + TEXT("/record"));
			auto Entries = Record.List("log");
			if (!Entries) Problem = FString::Printf(TEXT("Couldn't read the world in %s: %s"), *Fs, UTF8_TO_TCHAR(Entries.Err().Message.c_str()));
			else if (Entries->empty()) Problem = FString::Printf(TEXT("No world found in %s. Check the folder, and that the owner shared the world's folder with you."), *Fs);
			else Found = static_cast<int32>(Entries->size());
		}
		else
		{
			FRcloneObjectStore Saves(Fs);
			auto Listed = Saves.List();
			if (!Listed) Problem = FString::Printf(TEXT("Couldn't read %s: %s"), *Fs, UTF8_TO_TCHAR(Listed.Err().Message.c_str()));
			else if (Listed->empty()) Problem = FString::Printf(TEXT("No save files found in %s. Check the folder, and that the owner shared the world's folder with you."), *Fs);
			else Found = static_cast<int32>(Listed->size());
		}
		if (Problem.IsEmpty())
		{
			// Upload test: view-only access is not enough to host.
			FRcloneLogStore Probe(Fs);
			const std::string Name = "link-check-" + std::string(TCHAR_TO_UTF8(*FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
			auto Wrote = Probe.Create("linkcheck", Name, "ok");
			if (!Wrote)
			{
				Problem = FString::Printf(TEXT("You can see this world but can't upload to it. Ask the owner to give you edit access to the world's folder in %s, then link again."), *Label);
			}
			else
			{
				(void)Probe.Delete("linkcheck", *Wrote);
			}
		}
		FRcloneRuntime::PostToGameThread([Weak, WorldId, Remote, Fs, Found, Problem, Label]()
		{
			USharedWorldBrowserWidget* Self = Weak.Get();
			USharedWorldSubsystem* Sub = Self ? Self->SW() : nullptr;
			if (!Sub) return;
			if (!Problem.IsEmpty())
			{
				Self->SaveLinkNotice = Problem.Left(400);
			}
			else
			{
				const FString LinkErr = Sub->SetWorldSaveRemote(WorldId, Remote);
				Self->SaveLinkNotice = !LinkErr.IsEmpty() ? LinkErr
					: FString::Printf(TEXT("Linked to %s. You can read and upload to this world, so you can join and take over hosting. Press Play when ready."), *Label);
			}
			Self->ScheduleRebuild();
		});
	});
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

void USharedWorldBrowserWidget::OnDetailsMoveToActive()
{
	USharedWorldSubsystem* S = SW();
	const FString Active = FRcloneConnections::GetDefaultSaveRemote();
	if (!S || SelectedWorldId.IsEmpty() || Active.IsEmpty()) return;
	SaveLinkNotice = TEXT("Moving the world... keep the game open. This can take a few minutes for large saves.");
	ScheduleRebuild();
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	S->MoveWorldTo(SelectedWorldId, Active, [Weak](bool bOk, const FString& Message)
	{
		if (USharedWorldBrowserWidget* Self = Weak.Get())
		{
			Self->SaveLinkNotice = Message;
			Self->ScheduleRebuild();
		}
	});
}
