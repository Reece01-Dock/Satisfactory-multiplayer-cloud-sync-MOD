// Shared Worlds browser sub-pages. Everything here is built from SharedWorldUiStyle.h components
// and bound to real backend data; nothing is hard-coded sample content.

#include "UI/SharedWorldBrowserWidget.h"

#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ButtonSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "FGSaveSystem.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Services/SharedWorldCreationService.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "Services/SharedWorldInviteService.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldBrowserModel.h"
#include "UI/SharedWorldModal.h"
#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldSavePickRow.h"
#include "UI/SharedWorldUiStyle.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Rclone/RcloneProviders.h"
#include "SharedWorldUeConvert.h"

using namespace SharedWorldUi;

namespace
{
	/** Save file size on disk, or -1. */
	int64 SaveFileSize(const FString& SaveName)
	{
		const FString Path = FPaths::Combine(UFGSaveSystem::GetSaveDirectoryPath(), SaveName + TEXT(".sav"));
		return IFileManager::Get().FileSize(*Path);
	}

	FString SizeText(int64 Bytes)
	{
		return Bytes >= 0 ? FText::AsMemory(static_cast<uint64>(Bytes)).ToString() : FString();
	}

	UHorizontalBox* ButtonRow(UWidgetTree* Tree, UVerticalBox* Col, float TopPad = 18.f)
	{
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
		if (UVerticalBoxSlot* S = Col->AddChildToVerticalBox(Row))
		{
			S->SetPadding(FMargin(0.f, TopPad, 0.f, 0.f));
			S->SetHorizontalAlignment(HAlign_Right);
		}
		return Row;
	}
}

// ============================================================================ shared page chrome

void USharedWorldBrowserWidget::AddPageHeader(const FText& Title, const FText& Subtitle, bool bShowBack, bool bShowGlobe)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	PageBody->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, Subtitle.IsEmpty() ? 16.f : 4.f));
	if (bShowGlobe)
	{
		Row->AddChildToHorizontalBox(MakeGlobeIcon(WidgetTree))->SetVerticalAlignment(VAlign_Center);
	}
	UTextBlock* T = MakeText(WidgetTree, FontTitle - 4, TextPrimary, true);
	T->SetText(Title);
	T->SetAutoWrapText(false);
	T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(T))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
		if (bShowGlobe) S->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));
	}
	if (bShowBack)
	{
		TObjectPtr<UTextBlock> L;
		UButton* Back = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L, NSLOCTEXT("SharedWorld", "Back", "Back"), 16, FMargin(22.f, 9.f));
		Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		Row->AddChildToHorizontalBox(Back)->SetVerticalAlignment(VAlign_Center);
	}
	if (!Subtitle.IsEmpty())
	{
		UTextBlock* Sub = MakeText(WidgetTree, FontBody, TextMuted);
		Sub->SetText(Subtitle);
		PageBody->AddChildToVerticalBox(Sub)->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
	}
}

UButton* USharedWorldBrowserWidget::AddTabs(UVerticalBox* Col, int32 Kind, const TArray<FText>& Labels, int32 Active)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	UButton* First = nullptr;
	for (int32 i = 0; i < Labels.Num(); ++i)
	{
		UButton* B = MakeTabButton(WidgetTree, Labels[i], i == Active);
		USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
		Binder->Browser = this;
		Binder->TabKind = Kind;
		Binder->TabIndex = i;
		RowBinders.Add(Binder);
		B->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
		Row->AddChildToHorizontalBox(B)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		if (!First) First = B;
	}
	return First;
}

UEditableTextBox* USharedWorldBrowserWidget::AddTextField(UVerticalBox* Col, const FText& Hint, const FString& Initial)
{
	UBorder* Frame = MakePanel(WidgetTree, SearchBg, PanelEdge, FMargin(12.f, 2.f), RadiusM, 1.f);
	Col->AddChildToVerticalBox(Frame)->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	UEditableTextBox* Box = WidgetTree->ConstructWidget<UEditableTextBox>();
	Box->SetHintText(Hint);
	Box->SetText(FText::FromString(Initial));
	StyleTextField(Box, 17);
	Frame->SetContent(Box);
	return Box;
}

// ============================================================================ first-time setup

void USharedWorldBrowserWidget::RebuildWelcomePage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "WelcomeTitle", "Welcome to Shared Worlds"),
		NSLOCTEXT("SharedWorld", "WelcomeSub", "Play one factory with friends, even when the host changes."), false);
	TArray<FText> Steps;
	Steps.Add(NSLOCTEXT("SharedWorld", "WStep1", "Connect storage"));
	Steps.Add(NSLOCTEXT("SharedWorld", "WStep2", "Choose a save"));
	Steps.Add(NSLOCTEXT("SharedWorld", "WStep3", "Create your first world"));
	PageBody->AddChildToVerticalBox(MakeStepper(WidgetTree, Steps, 0))->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));

	UTextBlock* Body = MakeText(WidgetTree, FontBody + 1, TextMuted);
	Body->SetText(NSLOCTEXT("SharedWorld", "WelcomeBody",
		"Shared Worlds keeps the latest save of your factory in cloud storage, so any of you can pick it up and host.\n\n"
		"Link a GitHub account to set that up, or continue with a local shared folder."));
	PageBody->AddChildToVerticalBox(Body)->SetPadding(FMargin(0.f, 0.f, 0.f, 22.f));

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	PageBody->AddChildToVerticalBox(Row)->SetHorizontalAlignment(HAlign_Left);
	if (USharedWorldSubsystem* S = SW(); S && S->IsGitHubAuthConfigured())
	{
		TObjectPtr<UTextBlock> L;
		UButton* Link = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L, NSLOCTEXT("SharedWorld", "Connect", "Link GitHub"), 17, FMargin(22.f, 12.f));
		Link->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnWelcomeConnect);
		Row->AddChildToHorizontalBox(Link)->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	}
	TObjectPtr<UTextBlock> L2;
	UButton* Cont = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L2, NSLOCTEXT("SharedWorld", "ContinueLocal", "Continue with local storage"), 17, FMargin(22.f, 12.f));
	Cont->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnWelcomeContinue);
	Row->AddChildToHorizontalBox(Cont);
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

// ============================================================================ create wizard

namespace
{
	TArray<FText> CreateSteps()
	{
		TArray<FText> Steps;
		Steps.Add(NSLOCTEXT("SharedWorld", "CStep1", "Choose save"));
		Steps.Add(NSLOCTEXT("SharedWorld", "CStep2", "World details"));
		Steps.Add(NSLOCTEXT("SharedWorld", "CStep3", "Review"));
		return Steps;
	}
}

void USharedWorldBrowserWidget::RebuildCreatePickSavePage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "CreateTitle", "Create Shared World"),
		NSLOCTEXT("SharedWorld", "CreatePickSub", "Choose a save to turn into a Shared World. Your original save is not modified."));
	PageBody->AddChildToVerticalBox(MakeStepper(WidgetTree, CreateSteps(), 0))->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));

	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(List);
	ScrollContentBox = List;

	if (USharedWorldSubsystem* S = SW())
	{
		const TArray<FSharedWorldSaveInfo> Saves = S->Creation().ListLocalSaves();
		if (Saves.Num() == 0)
		{
			UButton* Unused = nullptr;
			List->AddChildToVerticalBox(MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "NoSavesTitle", "No saves found"),
				NSLOCTEXT("SharedWorld", "NoSavesBody", "Start a New Game from the main menu, save once, then come back here to share it."), Unused));
		}
		for (const FSharedWorldSaveInfo& Save : Saves)
		{
			USharedWorldSavePickRow* Row = CreateWidget<USharedWorldSavePickRow>(this, USharedWorldSavePickRow::StaticClass());
			if (!Row) continue;
			List->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
			FString Display = Save.SessionName.IsEmpty() ? Save.SaveName : Save.SessionName;
			TArray<FString> Parts;
			Parts.Add(Save.SaveName);
			if (!Save.LastPlayedText.IsEmpty()) Parts.Add(FString::Printf(TEXT("Last played %s"), *Save.LastPlayedText));
			const FString Size = SizeText(SaveFileSize(Save.SaveName));
			if (!Size.IsEmpty()) Parts.Add(Size);
			Row->Setup(Save.SaveName, Display, FString::Join(Parts, TEXT("  ·  ")), this, Save.SaveName == SelectedSaveName);
		}
	}
	ListScrollOwner = EPage::CreatePickSave;
	RestoreListScroll(EPage::CreatePickSave);
}

void USharedWorldBrowserWidget::RebuildCreateNamePage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "CreateTitle", "Create Shared World"),
		NSLOCTEXT("SharedWorld", "CreateNameSub", "Name your Shared World. This is what you and your friends will see."));
	PageBody->AddChildToVerticalBox(MakeStepper(WidgetTree, CreateSteps(), 1))->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));

	UTextBlock* Label = MakeSectionHeader(WidgetTree, NSLOCTEXT("SharedWorld", "WorldNameLabel", "SHARED WORLD NAME"));
	PageBody->AddChildToVerticalBox(Label)->SetPadding(FMargin(2.f, 0.f, 0.f, 6.f));
	NameInput = AddTextField(PageBody, NSLOCTEXT("SharedWorld", "WorldNameHint", "Name your Shared World"),
		PendingWorldName.IsEmpty() ? (SuggestedWorldName.IsEmpty() ? SelectedSaveName : SuggestedWorldName) : PendingWorldName);
	NameInput->OnTextChanged.AddDynamic(this, &USharedWorldBrowserWidget::OnNameChanged);

	UTextBlock* FromSave = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	FromSave->SetText(FText::Format(NSLOCTEXT("SharedWorld", "FromSave", "From save: {0}"), FText::FromString(SelectedSaveName)));
	PageBody->AddChildToVerticalBox(FromSave)->SetPadding(FMargin(2.f, 0.f, 0.f, 16.f));

	if (USharedWorldSubsystem* S = SW())
	{
		FString Note;
		const sw::ProviderConfig P = S->Creation().ResolveDefaultProvider(Note);
		const bool bGit = P.Kind == sw::ProviderKind::GitHub;
		UHorizontalBox* Storage = MakeStatusBadge(WidgetTree, bGit ? ESharedWorldTone::Healthy : ESharedWorldTone::Warning,
			bGit ? NSLOCTEXT("SharedWorld", "StorageGit", "Connected") : NSLOCTEXT("SharedWorld", "StorageLocal", "Local folder"));
		PageBody->AddChildToVerticalBox(MakeSettingsRow(WidgetTree, NSLOCTEXT("SharedWorld", "StorageRow", "Storage"),
			bGit ? NSLOCTEXT("SharedWorld", "StorageGitDesc", "The latest save is kept safely in your linked cloud storage.")
				: NSLOCTEXT("SharedWorld", "StorageLocalDesc", "Saved to a local shared folder. Link GitHub in Settings to play online with friends."), Storage))
			->SetPadding(FMargin(0.f));
	}
	UTextBlock* Access = MakeText(WidgetTree, FontBody, TextPrimary, true);
	Access->SetText(NSLOCTEXT("SharedWorld", "AccessVal", "Invited players"));
	Access->SetAutoWrapText(false);
	if (UVerticalBoxSlot* S = PageBody->AddChildToVerticalBox(MakeSettingsRow(WidgetTree, NSLOCTEXT("SharedWorld", "AccessRow", "Who can play"),
		NSLOCTEXT("SharedWorld", "AccessDesc", "Only players you invite can join this Shared World."), Access)))
	{
		S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}

	UHorizontalBox* Buttons = ButtonRow(WidgetTree, PageBody);
	TObjectPtr<UTextBlock> L1, L2;
	UButton* Back = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1, NSLOCTEXT("SharedWorld", "Back", "Back"), 17, FMargin(24.f, 11.f));
	Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
	Buttons->AddChildToHorizontalBox(Back)->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	UButton* Next = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L2, NSLOCTEXT("SharedWorld", "Continue", "Continue"), 17, FMargin(24.f, 11.f));
	Next->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateNext);
	Buttons->AddChildToHorizontalBox(Next);
}

void USharedWorldBrowserWidget::OnNameChanged(const FText& Text)
{
	PendingWorldName = Text.ToString();
}

void USharedWorldBrowserWidget::OnCreateNext()
{
	const FString Name = (NameInput ? NameInput->GetText().ToString() : PendingWorldName).TrimStartAndEnd();
	if (Name.IsEmpty() || SelectedSaveName.IsEmpty())
	{
		SetFlash(false, TEXT("Enter a name for your Shared World."));
		return;
	}
	PendingWorldName = Name;
	Page = EPage::CreateReview;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::RebuildCreateReviewPage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "CreateTitle", "Create Shared World"),
		NSLOCTEXT("SharedWorld", "CreateReviewSub", "Check the details, then create your Shared World."));
	PageBody->AddChildToVerticalBox(MakeStepper(WidgetTree, CreateSteps(), 2))->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));

	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(Col);
	ScrollContentBox = Col;

	auto AddValueRow = [&](const FText& Label, const FString& Value, const FText& Desc = FText::GetEmpty())
	{
		UTextBlock* V = MakeText(WidgetTree, FontBody, TextPrimary, true);
		V->SetAutoWrapText(false);
		V->SetText(FText::FromString(Value));
		Col->AddChildToVerticalBox(MakeSettingsRow(WidgetTree, Label, Desc, V))->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	};
	AddValueRow(NSLOCTEXT("SharedWorld", "RevName", "Shared World name"), PendingWorldName);
	AddValueRow(NSLOCTEXT("SharedWorld", "RevSave", "Source save"), SelectedSaveName,
		NSLOCTEXT("SharedWorld", "RevSaveDesc", "Copied to the cloud. The original save is not changed."));
	const int64 Bytes = SaveFileSize(SelectedSaveName);
	if (Bytes >= 0) AddValueRow(NSLOCTEXT("SharedWorld", "RevSize", "Save size"), SizeText(Bytes));
	AddValueRow(NSLOCTEXT("SharedWorld", "RevAccess", "Who can play"), TEXT("Invited players"));
	if (USharedWorldSubsystem* S = SW())
	{
		FString Note;
		const sw::ProviderConfig P = S->Creation().ResolveDefaultProvider(Note);
		if (P.Kind == sw::ProviderKind::Rclone)
		{
			// The active provider holds the whole world; there is nothing separate to choose.
			AddValueRow(NSLOCTEXT("SharedWorld", "RevStorageAll", "Storage"), SharedWorldUe::ToFString(P.Label.empty() ? P.Backend : P.Label),
				NSLOCTEXT("SharedWorld", "RevStorageAllDesc", "The world, who is hosting, its history and saves. Change the active storage in Settings > Storage."));
		}
		else
		{
		const FString DefaultName = P.Kind == sw::ProviderKind::GitHub ? FString(TEXT("GitHub")) : FString(TEXT("Local folder"));
		AddValueRow(NSLOCTEXT("SharedWorld", "RevStorage", "World record"), DefaultName,
			NSLOCTEXT("SharedWorld", "RevStorageDesc", "Who can play, who is hosting, and the save history."));

		// Save files: the default store, or any connected provider that passed its read/write test.
		TArray<FRcloneConnection> Usable;
		for (const FRcloneConnection& C : FRcloneConnections::Load())
		{
			if (C.bVerified) Usable.Add(C);
		}
		if (!PendingSaveConnection.IsEmpty() && !Usable.ContainsByPredicate([&](const FRcloneConnection& C) { return C.RemoteName == PendingSaveConnection; }))
		{
			PendingSaveConnection.Reset(); // disconnected since it was picked
		}
		UTextBlock* H = MakeText(WidgetTree, FontBody, TextPrimary, true);
		H->SetText(NSLOCTEXT("SharedWorld", "RevSaves", "Save files"));
		Col->AddChildToVerticalBox(H)->SetPadding(FMargin(0.f, 8.f, 0.f, 2.f));
		UTextBlock* HD = MakeText(WidgetTree, FontSmall, TextMuted);
		HD->SetText(Usable.Num() > 0
			? NSLOCTEXT("SharedWorld", "RevSavesDesc", "Where the world's saves are uploaded. Friends who want to host link the same storage; anyone can still join while someone else hosts.")
			: NSLOCTEXT("SharedWorld", "RevSavesNone", "Saves go with the world record. Connect Google Drive, Dropbox and others in Settings > Storage to keep them there instead."));
		Col->AddChildToVerticalBox(HD)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>();
		Grid->SetSlotPadding(FMargin(0.f, 0.f, 8.f, 8.f));
		Col->AddChildToVerticalBox(Grid)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		int32 Index = 0;
		auto Choice = [&](const FString& Remote, const FString& Label, const FString& Tip)
		{
			UButton* B = MakeTabButton(WidgetTree, FText::FromString(Label), PendingSaveConnection == Remote);
			B->SetToolTipText(FText::FromString(Tip));
			USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
			Binder->Browser = this;
			Binder->TabKind = 5;
			Binder->ConnectValue = Remote;
			RowBinders.Add(Binder);
			B->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
			if (UUniformGridSlot* Cell = Grid->AddChildToUniformGrid(B, Index / 3, Index % 3)) Cell->SetHorizontalAlignment(HAlign_Fill);
			++Index;
		};
		Choice(FString(), DefaultName, TEXT("Keep the saves with the world record."));
		for (const FRcloneConnection& C : Usable)
		{
			Choice(C.RemoteName, C.Label, FString::Printf(TEXT("Upload saves to %s, folder \"%s\"."), *C.Label, *C.Folder));
		}
		}
	}
	if (!CreateError.IsEmpty())
	{
		UButton* Unused = nullptr;
		Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Problem,
			NSLOCTEXT("SharedWorld", "CreateFailed", "Couldn't create the Shared World"), FText::FromString(CreateError), Unused))
			->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
	ListScrollOwner = EPage::CreateReview;
	RestoreListScroll(EPage::CreateReview);

	UHorizontalBox* Buttons = ButtonRow(WidgetTree, PageBody);
	TObjectPtr<UTextBlock> L1, L2;
	UButton* Back = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1, NSLOCTEXT("SharedWorld", "Back", "Back"), 17, FMargin(24.f, 11.f));
	Back->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
	Buttons->AddChildToHorizontalBox(Back)->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	UButton* Create = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L2, NSLOCTEXT("SharedWorld", "CreateSharedWorld", "Create Shared World"), 17, FMargin(24.f, 11.f));
	Create->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCreateConfirm);
	Buttons->AddChildToHorizontalBox(Create);
}

void USharedWorldBrowserWidget::OnCreateConfirm()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	if (PendingWorldName.IsEmpty() || SelectedSaveName.IsEmpty()) return;
	bBusy = true;
	CreateError.Reset();
	Page = EPage::Creating;
	ListScroll = nullptr;
	ScheduleRebuild();
	FSharedWorldSaveTarget Target;
	for (const FRcloneConnection& C : FRcloneConnections::Load())
	{
		if (!PendingSaveConnection.IsEmpty() && C.RemoteName == PendingSaveConnection)
		{
			Target.Remote = C.Fs();
			Target.Backend = C.BackendType;
			Target.Label = C.Label;
		}
	}
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	S->Creation().CreateFromExistingSave(PendingWorldName, SelectedSaveName, [Weak](bool bOk, const FString& Message)
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		if (!Self) return;
		Self->bBusy = false;
		if (bOk)
		{
			Self->PendingFlash = Message.IsEmpty() ? FString(TEXT("Shared World created.")) : Message;
			Self->bPendingFlashOk = true;
			Self->PendingWorldName.Reset();
			Self->SelectedSaveName.Reset();
			Self->PendingSaveConnection.Reset();
			Self->Page = EPage::Main;
		}
		else
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=create_failed detail=%s"), *Message);
			Self->CreateError = Message;
			Self->Page = EPage::CreateReview;
		}
		Self->ListScroll = nullptr;
		Self->ScheduleRebuild();
	}, Target);
}

void USharedWorldBrowserWidget::RebuildCreatingPage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "CreatingTitle", "Creating Shared World"), FText::FromString(PendingWorldName), false);
	UButton* Unused = nullptr;
	PageBody->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Working,
		NSLOCTEXT("SharedWorld", "CreatingNotice", "Uploading your save"),
		NSLOCTEXT("SharedWorld", "CreatingBody", "This can take a few minutes for large saves. Keep the game open until it finishes."), Unused))
		->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));
	// The backend reports no percentage for creation, so the bar is an honest marquee, not a guess.
	PageBody->AddChildToVerticalBox(MakeProgressBar(WidgetTree, 0.f, true))->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));
	PageBody->AddChildToVerticalBox(MakeProgressRow(WidgetTree, ESharedWorldStep::Active,
		NSLOCTEXT("SharedWorld", "CreatingStep", "Preparing and uploading the world")));
}

// ============================================================================ join

void USharedWorldBrowserWidget::RebuildJoinPage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "JoinTitle", "Join a Shared World"),
		NSLOCTEXT("SharedWorld", "JoinSub", "Accept an invite, join a friend who is playing, or enter an invite code."));

	FSharedWorldDiscoverySnapshot Snap;
	if (USharedWorldSubsystem* S = SW()) Snap = S->Discovery().BuildSnapshot();

	TArray<FText> Tabs;
	Tabs.Add(Snap.PendingInvites.Num() > 0
		? FText::Format(NSLOCTEXT("SharedWorld", "TabInvitesN", "Invites ({0})"), FText::AsNumber(Snap.PendingInvites.Num()))
		: NSLOCTEXT("SharedWorld", "TabInvites", "Invites"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "TabFriendWorlds", "Friend Worlds"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "TabInviteCode", "Invite Code"));
	AddTabs(PageBody, 1, Tabs, JoinTab);

	if (JoinTab == 2)
	{
		AddJoinCodeForm(PageBody);
		return;
	}
	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* List = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(List);
	ScrollContentBox = List;
	if (JoinTab == 0) AddJoinInvites(List, Snap);
	else AddJoinFriendWorlds(List, Snap);
	ListScrollOwner = EPage::JoinFriend;
	RestoreListScroll(EPage::JoinFriend);
}

void USharedWorldBrowserWidget::AddJoinInvites(UVerticalBox* Col, const FSharedWorldDiscoverySnapshot& Snap)
{
	if (Snap.PendingInvites.Num() == 0)
	{
		UButton* Unused = nullptr;
		Col->AddChildToVerticalBox(MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "NoInvitesTitle", "No invites"),
			NSLOCTEXT("SharedWorld", "NoInvitesBody", "When a friend invites you to a Shared World, it will appear here."), Unused));
		return;
	}
	for (const FSharedWorldPendingInviteView& I : Snap.PendingInvites)
	{
		const FString From = I.FromDisplayName.IsEmpty() ? FString(TEXT("A friend")) : I.FromDisplayName;
		UBorder* Row = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(14.f, 10.f), RadiusM, 1.f);
		Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		UHorizontalBox* H = WidgetTree->ConstructWidget<UHorizontalBox>();
		Row->SetContent(H);
		H->AddChildToHorizontalBox(MakePlayerAvatar(WidgetTree, From, 44.f))->SetVerticalAlignment(VAlign_Center);
		UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
		UTextBlock* W = MakeText(WidgetTree, 18, TextPrimary, true);
		W->SetText(FText::FromString(I.WorldName));
		W->SetAutoWrapText(false);
		W->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Text->AddChildToVerticalBox(W);
		UTextBlock* F = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		F->SetText(FText::Format(NSLOCTEXT("SharedWorld", "InvitedBy", "{0} invited you"), FText::FromString(From)));
		Text->AddChildToVerticalBox(F);
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(14.f, 0.f, 10.f, 0.f));
		}
		auto Bind = [this](UButton* B, const FString& InviteId, bool bAccept)
		{
			USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
			Binder->Browser = this;
			Binder->InviteId = InviteId;
			Binder->InviteAction = bAccept ? 1 : 2;
			RowBinders.Add(Binder);
			B->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
		};
		TObjectPtr<UTextBlock> L1, L2;
		UButton* Decline = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1, NSLOCTEXT("SharedWorld", "Decline", "Decline"), 16, FMargin(20.f, 9.f));
		Bind(Decline, I.InviteId, false);
		H->AddChildToHorizontalBox(Decline)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		UButton* Accept = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L2, NSLOCTEXT("SharedWorld", "Accept", "Accept"), 16, FMargin(20.f, 9.f));
		Bind(Accept, I.InviteId, true);
		H->AddChildToHorizontalBox(Accept);
	}
}

void USharedWorldBrowserWidget::AddJoinFriendWorlds(UVerticalBox* Col, const FSharedWorldDiscoverySnapshot& Snap)
{
	USharedWorldSubsystem* S = SW();
	const FSharedWorldBrowserSections Sec = SharedWorldBrowserModel::Build(Snap.AllWorlds, FString(), S ? S->GetMostRecentlyPlayedWorldId() : FString());
	AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "PlayingNow", "PLAYING NOW"));
	if (Sec.FriendsPlaying.Num() > 0)
	{
		AddWorldItems(Col, Sec.FriendsPlaying, /*bAllowMore=*/false);
	}
	else
	{
		UButton* Unused = nullptr;
		Col->AddChildToVerticalBox(MakeEmptyState(WidgetTree, NSLOCTEXT("SharedWorld", "NobodyTitle", "Nobody is playing"),
			NSLOCTEXT("SharedWorld", "NobodyBody", "None of your Shared Worlds are currently being hosted."), Unused));
	}
	TArray<FSharedWorldFriendInfo> Online;
	for (const FSharedWorldFriendInfo& F : Snap.Friends)
	{
		if (F.bOnline) Online.Add(F);
	}
	if (Online.Num() > 0)
	{
		AddSectionTitle(Col, NSLOCTEXT("SharedWorld", "FriendsOnline", "FRIENDS ONLINE"));
		for (const FSharedWorldFriendInfo& F : Online)
		{
			Col->AddChildToVerticalBox(MakePlayerRow(WidgetTree, F.DisplayName, FString(), ESharedWorldTone::Healthy,
				NSLOCTEXT("SharedWorld", "OnlineLbl", "Online")))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		}
	}
}

void USharedWorldBrowserWidget::AddJoinCodeForm(UVerticalBox* Col)
{
	UTextBlock* Hint = MakeText(WidgetTree, FontBody, TextMuted);
	Hint->SetText(NSLOCTEXT("SharedWorld", "JoinCodeHint", "Enter the invite code your friend shared with you (for example ABCD-EFGH)."));
	Col->AddChildToVerticalBox(Hint)->SetPadding(FMargin(2.f, 0.f, 0.f, 12.f));
	CodeInput = AddTextField(Col, NSLOCTEXT("SharedWorld", "CodeHint", "ABCD-EFGH"), FString());
	UHorizontalBox* Buttons = ButtonRow(WidgetTree, Col, 4.f);
	TObjectPtr<UTextBlock> L;
	UButton* Join = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L, NSLOCTEXT("SharedWorld", "JoinWithCode", "Join Shared World"), 17, FMargin(24.f, 11.f));
	Join->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnJoinCodeSubmit);
	Buttons->AddChildToHorizontalBox(Join);
}

void USharedWorldBrowserWidget::OnJoinCodeSubmit()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	const FString Code = CodeInput ? CodeInput->GetText().ToString() : FString();
	bBusy = true;
	SetFlash(true, TEXT("Joining..."));
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	S->Invites().JoinUsingCode(Code, [Weak](bool bOk, const FString& Message)
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		if (!Self) return;
		Self->bBusy = false;
		if (bOk)
		{
			Self->PendingFlash = Message;
			Self->bPendingFlashOk = true;
			Self->Page = EPage::Main;
			Self->ListScroll = nullptr;
			Self->ScheduleRebuild();
		}
		else
		{
			Self->SetFlash(false, Message);
		}
	});
}

// ============================================================================ settings

void USharedWorldBrowserWidget::AddSettingsSync(UVerticalBox* Col)
{
	USharedWorldSubsystem* S = SW();
	auto Row = [&](const FText& Label, const FText& Desc, const FString& Value)
	{
		UTextBlock* V = MakeText(WidgetTree, FontBody, TextPrimary, true);
		V->SetAutoWrapText(false);
		V->SetText(FText::FromString(Value));
		Col->AddChildToVerticalBox(MakeSettingsRow(WidgetTree, Label, Desc, V))->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
	};
	const int32 Minutes = S ? FMath::Max(1, FMath::RoundToInt(S->GetCheckpointIntervalSeconds() / 60.f)) : 0;
	if (S)
	{
		Row(NSLOCTEXT("SharedWorld", "CheckpointRow", "Checkpoint interval"),
			NSLOCTEXT("SharedWorld", "CheckpointDesc", "How often the host saves and uploads the world while it is running."),
			FString::Printf(TEXT("Every %d min"), Minutes));
	}
	Row(NSLOCTEXT("SharedWorld", "LatestRow", "Latest save before hosting"),
		NSLOCTEXT("SharedWorld", "LatestDesc", "Playing downloads the newest revision first, so everyone continues from the same factory."),
		TEXT("Always"));
	UTextBlock* Note = MakeText(WidgetTree, FontSmall + 1, TextMuted);
	Note->SetText(NSLOCTEXT("SharedWorld", "SyncManaged", "These are managed by Shared Worlds and can't be changed here yet."));
	Col->AddChildToVerticalBox(Note)->SetPadding(FMargin(2.f, 6.f, 0.f, 0.f));
}

void USharedWorldBrowserWidget::AddSettingsDiagnostics(UVerticalBox* Col)
{
	AddRcloneEngineCard(Col);
	USharedWorldSubsystem* S = SW();
	UButton* Unused = nullptr;
	Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
		NSLOCTEXT("SharedWorld", "DiagTitle", "For troubleshooting"),
		NSLOCTEXT("SharedWorld", "DiagBody", "Only share this log with someone helping you fix a problem."), Unused))->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	const FString Log = S ? S->RecentLog(80) : FString();
	UBorder* Box = MakePanel(WidgetTree, RowFill, PanelEdge, FMargin(14.f, 10.f), RadiusM, 1.f);
	Col->AddChildToVerticalBox(Box)->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));
	UTextBlock* T = MakeText(WidgetTree, FontSmall - 1, TextMuted);
	T->SetText(FText::FromString(Log.IsEmpty() ? FString(TEXT("No log lines yet.")) : Log));
	Box->SetContent(T);
	TObjectPtr<UTextBlock> L;
	UButton* Copy = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L, NSLOCTEXT("SharedWorld", "CopyLog", "Copy log"), 15, FMargin(20.f, 9.f));
	Copy->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCopyDiagnostics);
	Col->AddChildToVerticalBox(Copy)->SetHorizontalAlignment(HAlign_Left);
}

void USharedWorldBrowserWidget::OnCopyDiagnostics()
{
	if (USharedWorldSubsystem* S = SW())
	{
		FPlatformApplicationMisc::ClipboardCopy(*S->RecentLog(200));
		SetFlash(true, TEXT("Log copied."));
	}
}

// ============================================================================ GitHub link page

void USharedWorldBrowserWidget::RebuildLinkGitHubPage()
{
	AddPageHeader(NSLOCTEXT("SharedWorld", "LinkTitle", "Link GitHub"),
		NSLOCTEXT("SharedWorld", "LinkSub", "Approve Shared Worlds on GitHub so it can store your worlds."));
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldSignIn Sign = S->GetSignInStatus();
	UButton* Unused = nullptr;
	if (!Sign.bConfigured)
	{
		PageBody->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
			NSLOCTEXT("SharedWorld", "NoGitHubTitle", "GitHub linking isn't available"),
			NSLOCTEXT("SharedWorld", "NoGitHubBody", "This build doesn't include a GitHub connection."), Unused));
		return;
	}
	if (Sign.State == ESharedWorldGitHubAuthState::Connected || !S->GetGitHubLogin().IsEmpty())
	{
		PageBody->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Healthy,
			NSLOCTEXT("SharedWorld", "GitHubDone", "GitHub connected"),
			FText::Format(NSLOCTEXT("SharedWorld", "GitHubAs", "Connected as {0}."), FText::FromString(S->GetGitHubLogin())), Unused))
			->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
		UHorizontalBox* Row = ButtonRow(WidgetTree, PageBody, 4.f);
		TObjectPtr<UTextBlock> L;
		UButton* Done = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L, NSLOCTEXT("SharedWorld", "Done", "Done"), 17, FMargin(24.f, 11.f));
		Done->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
		Row->AddChildToHorizontalBox(Done);
		return;
	}
	if (!Sign.UserCode.IsEmpty())
	{
		UBorder* Code = MakePanel(WidgetTree, RowFill, Accent, FMargin(24.f), RadiusM, 2.f);
		PageBody->AddChildToVerticalBox(Code)->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
		UVerticalBox* CC = WidgetTree->ConstructWidget<UVerticalBox>();
		Code->SetContent(CC);
		UTextBlock* L = MakeText(WidgetTree, FontBody + 1, TextMuted);
		L->SetText(NSLOCTEXT("SharedWorld", "EnterCode", "Open GitHub and enter this code:"));
		CC->AddChildToVerticalBox(L)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		UTextBlock* C = MakeText(WidgetTree, 44, TextPrimary, true);
		C->SetAutoWrapText(false);
		C->SetText(FText::FromString(Sign.UserCode));
		CC->AddChildToVerticalBox(C);
		PageBody->AddChildToVerticalBox(MakeProgressBar(WidgetTree, 0.f, true))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		UTextBlock* W = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		W->SetText(NSLOCTEXT("SharedWorld", "WaitingGit", "Waiting for you to approve on GitHub..."));
		PageBody->AddChildToVerticalBox(W);
	}
	else
	{
		UTextBlock* B = MakeText(WidgetTree, FontBody, TextMuted);
		B->SetText(FText::FromString(Sign.PlayerMessage.IsEmpty() ? FString(TEXT("Opening GitHub...")) : Sign.PlayerMessage));
		PageBody->AddChildToVerticalBox(B)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
		PageBody->AddChildToVerticalBox(MakeProgressBar(WidgetTree, 0.f, true));
	}
	if (!Sign.Error.IsEmpty())
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=link_signin_error detail=%s"), *Sign.Error);
		PageBody->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Problem,
			NSLOCTEXT("SharedWorld", "LinkFailed", "Couldn't link GitHub"),
			NSLOCTEXT("SharedWorld", "LinkFailedSub", "Check your connection and try again."), Unused))->SetPadding(FMargin(0.f, 16.f, 0.f, 0.f));
	}
	UHorizontalBox* Row = ButtonRow(WidgetTree, PageBody, 16.f);
	TObjectPtr<UTextBlock> L1, L2, L3;
	if (!Sign.UserCode.IsEmpty())
	{
		UButton* Copy = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1, NSLOCTEXT("SharedWorld", "CopyCode", "Copy code"), 16, FMargin(22.f, 10.f));
		Copy->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCopyGitHubCode);
		Row->AddChildToHorizontalBox(Copy)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	UButton* Cancel = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L2, NSLOCTEXT("SharedWorld", "Cancel", "Cancel"), 16, FMargin(22.f, 10.f));
	Cancel->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnCancelGitHubLink);
	Row->AddChildToHorizontalBox(Cancel)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	if (!Sign.UserCode.IsEmpty())
	{
		UButton* Open = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L3, NSLOCTEXT("SharedWorld", "OpenGitHub", "Open GitHub"), 16, FMargin(22.f, 10.f));
		Open->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnOpenGitHubVerify);
		Row->AddChildToHorizontalBox(Open);
	}
}

// ============================================================================ settings page shell

void USharedWorldBrowserWidget::RebuildSettingsPage()
{
	StorageGridBox = nullptr;
	StorageDetailsBox = nullptr;
	ProviderBrowserAnchor = nullptr;
	StorageSearchInput = nullptr;

	// Storage is a two-column screen (browser | selected provider) on wide displays and stacks on narrow ones.
	float LogicalWidth = 3840.f;
	{
		const FVector2D VS = UWidgetLayoutLibrary::GetViewportSize(this);
		const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
		if (VS.X > 1.f) LogicalWidth = VS.X / Scale;
	}
	const bool bStorageTab = SettingsTab == 0;
	const bool bWide = bStorageTab && LogicalWidth >= 1500.f;

	UVerticalBox* OuterBody = PageBody;
	UVerticalBox* LeftCol = PageBody;
	UBorder* DetailsPanel = nullptr;
	if (bWide)
	{
		UHorizontalBox* Columns = WidgetTree->ConstructWidget<UHorizontalBox>();
		OuterBody->AddChildToVerticalBox(Columns)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		LeftCol = WidgetTree->ConstructWidget<UVerticalBox>();
		if (UHorizontalBoxSlot* LS = Columns->AddChildToHorizontalBox(LeftCol))
		{
			LS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			LS->SetPadding(FMargin(0.f, 0.f, 18.f, 0.f));
		}
		// Details panel: about a quarter of the screen, never so narrow that label/value rows collide, never absurdly wide.
		StorageDetailsWidth = FMath::Clamp(LogicalWidth * 0.27f, 440.f, 620.f);
		USizeBox* DetailsSize = WidgetTree->ConstructWidget<USizeBox>();
		DetailsSize->SetWidthOverride(StorageDetailsWidth);
		DetailsPanel = MakePanel(WidgetTree, FLinearColor(0.04f, 0.045f, 0.055f, 0.86f), PanelEdge, FMargin(20.f, 18.f, 20.f, 16.f), RadiusL, 1.f);
		DetailsSize->AddChild(DetailsPanel);
		if (UHorizontalBoxSlot* RS = Columns->AddChildToHorizontalBox(DetailsSize))
		{
			RS->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
		UScrollBox* DetailScroll = WidgetTree->ConstructWidget<UScrollBox>();
		DetailsPanel->SetContent(DetailScroll);
		StorageDetailsBox = WidgetTree->ConstructWidget<UVerticalBox>();
		DetailScroll->AddChild(StorageDetailsBox);
	}

	PageBody = LeftCol;
	AddPageHeader(NSLOCTEXT("SharedWorld", "SettingsTitle", "Shared Worlds Settings"),
		NSLOCTEXT("SharedWorld", "SettingsSub", "Where your worlds are stored, how they sync, and troubleshooting."), true, true);
	TArray<FText> Tabs;
	Tabs.Add(NSLOCTEXT("SharedWorld", "TabStorage", "Storage"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "TabSync", "Sync"));
	Tabs.Add(NSLOCTEXT("SharedWorld", "TabDiag", "Diagnostics"));
	AddTabs(PageBody, 2, Tabs, SettingsTab);

	ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(ListScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(Col);
	ScrollContentBox = Col;
	switch (SettingsTab)
	{
	case 1: AddSettingsSync(Col); break;
	case 2: AddSettingsDiagnostics(Col); break;
	default: AddStoragePage(Col); break;
	}
	PageBody = OuterBody;
	if (bWide && StorageDetailsBox) PopulateProviderDetails(StorageDetailsBox);
	ListScrollOwner = EPage::Settings;
	RestoreListScroll(EPage::Settings);
}

void USharedWorldBrowserWidget::SetCreateSaveTarget(const FString& RemoteName)
{
	if (PendingSaveConnection == RemoteName) return;
	PendingSaveConnection = RemoteName;
	ListScroll = nullptr;
	ScheduleRebuild();
}
