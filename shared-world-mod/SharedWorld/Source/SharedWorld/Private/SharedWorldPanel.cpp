#include "SharedWorldPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "SharedWorldCore/App/LocalSettings.h"
#include "SharedWorldSubsystem.h"
#include "Styling/CoreStyle.h"

namespace SharedWorldStyle
{
	const FLinearColor Panel(0.012f, 0.014f, 0.018f, 0.88f);
	const FLinearColor Card(0.035f, 0.04f, 0.05f, 0.95f);
	const FLinearColor Orange(0.98f, 0.52f, 0.12f, 1.0f);
	const FLinearColor Muted(0.62f, 0.64f, 0.68f, 1.0f);
	const FLinearColor Text(0.92f, 0.92f, 0.9f, 1.0f);
	const FLinearColor Online(0.35f, 0.85f, 0.4f, 1.0f);
	const FLinearColor Error(1.0f, 0.38f, 0.32f, 1.0f);

	UTextBlock* MakeText(UWidgetTree* Tree, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
		T->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size));
		T->SetColorAndOpacity(FSlateColor(Color));
		T->SetAutoWrapText(true);
		return T;
	}

	UButton* MakeButton(UWidgetTree* Tree, const FLinearColor& Color, UTextBlock*& OutLabel, const FText& Label)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(Color);
		OutLabel = MakeText(Tree, 12, FLinearColor::Black, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		B->AddChild(OutLabel);
		return B;
	}

	UEditableTextBox* MakeInput(UWidgetTree* Tree, const FText& Hint)
	{
		UEditableTextBox* T = Tree->ConstructWidget<UEditableTextBox>();
		T->SetHintText(Hint);
		return T;
	}

	FString StatusLabel(const FString& S)
	{
		if (S == TEXT("ONLINE")) return TEXT("Online");
		if (S == TEXT("STARTING")) return TEXT("Starting");
		if (S == TEXT("SAVING")) return TEXT("Online (saving)");
		if (S == TEXT("STOPPING")) return TEXT("Closing");
		if (S == TEXT("MIGRATING")) return TEXT("Changing host");
		if (S == TEXT("RECOVERABLE")) return TEXT("Available (recovery needed)");
		if (S == TEXT("NO_SAVE")) return TEXT("No shared save yet");
		if (S == TEXT("NOT_CREATED")) return TEXT("Not created yet");
		if (S == TEXT("UNREACHABLE")) return TEXT("Storage unreachable");
		return TEXT("Available");
	}

	void Show(UWidget* W, bool bVisible, bool bInteractive = false)
	{
		W->SetVisibility(!bVisible ? ESlateVisibility::Collapsed : bInteractive ? ESlateVisibility::Visible : ESlateVisibility::HitTestInvisible);
	}
}

using namespace SharedWorldStyle;

// ---------------------------------------------------------------- entry

TSharedRef<SWidget> USharedWorldEntry::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		Build();
		Update(Current);
	}
	return Super::RebuildWidget();
}

void USharedWorldEntry::Build()
{
	UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
	Root->SetBrushColor(Card);
	Root->SetPadding(FMargin(14.f, 10.f));
	WidgetTree->RootWidget = Root;

	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	Root->SetContent(Col);

	TitleText = MakeText(WidgetTree, 18, Text, true);
	StatusText = MakeText(WidgetTree, 12, Muted, true);
	InfoText = MakeText(WidgetTree, 11, Muted);
	ProgressText = MakeText(WidgetTree, 11, Orange);
	ErrorText = MakeText(WidgetTree, 11, SharedWorldStyle::Error);
	DetailText = MakeText(WidgetTree, 9, Muted);
	for (UWidget* W : TArray<UWidget*>{TitleText, StatusText, InfoText, ProgressText, ErrorText, DetailText})
	{
		Col->AddChildToVerticalBox(W)->SetPadding(FMargin(0.f, 2.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	PlayButton = MakeButton(WidgetTree, Orange, PlayLabel, NSLOCTEXT("SharedWorld", "Play", "Play Shared World"));
	PlayButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnPlayClicked);
	SecondaryButton = MakeButton(WidgetTree, Muted, SecondaryLabel, FText::GetEmpty());
	SecondaryButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnSecondaryClicked);
	UTextBlock* Unused = nullptr;
	DetailsButton = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "Details", "Details"));
	DetailsButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnDetailsClicked);
	for (UWidget* B : TArray<UWidget*>{PlayButton, SecondaryButton, DetailsButton})
	{
		Row->AddChildToHorizontalBox(B)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}

	// Details: history and local-list removal.
	UHorizontalBox* Details = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Details)->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
	UButton* History = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "History", "History"));
	History->OnClicked.AddDynamic(this, &USharedWorldEntry::OnHistoryClicked);
	UButton* Forget = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "Forget", "Remove from list"));
	Forget->OnClicked.AddDynamic(this, &USharedWorldEntry::OnForgetClicked);
	Details->AddChildToHorizontalBox(History)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	RestoreInput = MakeInput(WidgetTree, NSLOCTEXT("SharedWorld", "RestoreHint", "Rev #"));
	Details->AddChildToHorizontalBox(RestoreInput)->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
	UButton* Restore = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "Restore", "Restore"));
	Restore->OnClicked.AddDynamic(this, &USharedWorldEntry::OnRestoreClicked);
	Details->AddChildToHorizontalBox(Restore)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	Details->AddChildToHorizontalBox(Forget);
	DetailRow = Details;
	HistoryText = MakeText(WidgetTree, 9, Text);
	Col->AddChildToVerticalBox(HistoryText)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
}

USharedWorldSubsystem* USharedWorldEntry::GetSharedWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

void USharedWorldEntry::Update(const FSharedWorldEntryView& V)
{
	Current = V;
	if (!TitleText)
	{
		return; // not built yet; RebuildWidget will run before display
	}
	TitleText->SetText(FText::FromString(V.WorldName.IsEmpty() ? V.WorldId : V.WorldName));

	const bool bOnline = V.CloudStatus == TEXT("ONLINE") || V.CloudStatus == TEXT("SAVING");
	StatusText->SetText(FText::FromString(TEXT("Status: ") + StatusLabel(V.CloudStatus)));
	StatusText->SetColorAndOpacity(FSlateColor(bOnline ? Online : Muted));

	TArray<FString> Info;
	if (!V.HostName.IsEmpty() && bOnline)
	{
		Info.Add(TEXT("Host: ") + V.HostName);
		Info.Add(FString::Printf(TEXT("Players: %d"), V.PlayerCount));
	}
	Info.Add(V.Revision > 0 ? FString::Printf(TEXT("Revision: %lld"), V.Revision) : TEXT("No save yet"));
	if (!bOnline && !V.LastPlayed.IsEmpty())
	{
		Info.Add(TEXT("Last played: ") + V.LastPlayed + (V.LastHostName.IsEmpty() ? TEXT("") : TEXT(" by ") + V.LastHostName));
	}
	InfoText->SetText(FText::FromString(FString::Join(Info, TEXT("   "))));

	const FString& L = V.LocalState;
	const bool bBusy = V.bCreating || (!V.IsLocalIdle() && L != TEXT("ERROR") && L != TEXT("LEASE_LOST"));
	ProgressText->SetText(FText::FromString(V.LocalMessage));
	Show(ProgressText, !V.LocalMessage.IsEmpty());

	FString ErrorMsg = V.bHasError ? V.ErrorMessage : FString();
	if (V.bHasError && !V.BackupPath.IsEmpty())
	{
		ErrorMsg += TEXT("\nBackup kept at: ") + V.BackupPath;
	}
	ErrorText->SetText(FText::FromString(ErrorMsg));
	Show(ErrorText, V.bHasError);

	FString Detail = FString::Join(V.Steps, TEXT("\n"));
	if (V.bHasError && !V.ErrorDetail.IsEmpty())
	{
		Detail += TEXT("\n\nDetails: ") + V.ErrorDetail + TEXT(" (") + V.ErrorCode + TEXT(")");
	}
	if (!V.Problem.IsEmpty())
	{
		Detail += TEXT("\n\nStorage: ") + V.Problem;
	}
	DetailText->SetText(FText::FromString(Detail));
	Show(DetailText, bShowDetails && !Detail.IsEmpty());
	Show(DetailRow, bShowDetails, true);
	HistoryText->SetText(FText::FromString(HistoryCache));
	Show(HistoryText, bShowDetails && !HistoryCache.IsEmpty());

	// One button decides host vs join; disabled while something runs.
	PlayButton->SetIsEnabled(!bBusy && L != TEXT("LEASE_LOST") && V.CloudStatus != TEXT("NO_SAVE") && V.CloudStatus != TEXT("NOT_CREATED"));

	if (L == TEXT("WAITING_FOR_HOST") || L == TEXT("READY_TO_HOST") || L == TEXT("RECONNECTING"))
	{
		Secondary = ESecondary::Cancel;
	}
	else if (L == TEXT("ERROR") && V.bErrorRetryable)
	{
		Secondary = ESecondary::Retry;
	}
	else if (L == TEXT("ERROR") || L == TEXT("LEASE_LOST") || L == TEXT("JOIN_READY"))
	{
		Secondary = ESecondary::Dismiss;
	}
	else
	{
		Secondary = ESecondary::None;
	}
	static const FText Labels[] = {
		FText::GetEmpty(),
		NSLOCTEXT("SharedWorld", "Dismiss", "Dismiss"),
		NSLOCTEXT("SharedWorld", "Cancel", "Cancel"),
		NSLOCTEXT("SharedWorld", "Retry", "Retry"),
	};
	SecondaryLabel->SetText(Labels[static_cast<uint8>(Secondary)]);
	Show(SecondaryButton, Secondary != ESecondary::None, true);
}

void USharedWorldEntry::OnPlayClicked()
{
	if (USharedWorldSubsystem* SW = GetSharedWorld())
	{
		SW->Play(Current.WorldId);
	}
}

void USharedWorldEntry::OnSecondaryClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW)
	{
		return;
	}
	switch (Secondary)
	{
	case ESecondary::Dismiss: SW->Dismiss(Current.WorldId); break;
	case ESecondary::Cancel: SW->Cancel(Current.WorldId); break;
	case ESecondary::Retry: SW->Play(Current.WorldId); break;
	default: break;
	}
}

void USharedWorldEntry::OnDetailsClicked()
{
	bShowDetails = !bShowDetails;
	Update(Current);
}

void USharedWorldEntry::OnHistoryClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW)
	{
		return;
	}
	HistoryCache = TEXT("Loading history...");
	Update(Current);
	TWeakObjectPtr<USharedWorldEntry> WeakThis(this);
	SW->FetchHistory(Current.WorldId, 15, [WeakThis](bool bOk, const FString& Message)
	{
		if (USharedWorldEntry* Self = WeakThis.Get())
		{
			Self->HistoryCache = bOk ? Message + TEXT("\nRestoring makes a NEW revision with that content; nothing is deleted.") : Message;
			Self->Update(Self->Current);
		}
	});
}

void USharedWorldEntry::OnRestoreClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	const FString In = RestoreInput ? RestoreInput->GetText().ToString().TrimStartAndEnd().Replace(TEXT("#"), TEXT("")) : FString();
	if (!SW || !In.IsNumeric())
	{
		return;
	}
	SW->Restore(Current.WorldId, FCString::Atoi64(*In));
}

void USharedWorldEntry::OnForgetClicked()
{
	if (USharedWorldSubsystem* SW = GetSharedWorld())
	{
		const FString Problem = SW->ForgetWorld(Current.WorldId);
		if (!Problem.IsEmpty())
		{
			HistoryCache = Problem;
			Update(Current);
		}
	}
}

// ---------------------------------------------------------------- panel

TSharedRef<SWidget> USharedWorldPanel::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		// Fixed width, height follows the content.
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(460.f);
		WidgetTree->RootWidget = Size;
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
		Root->SetBrushColor(SharedWorldStyle::Panel);
		Root->SetPadding(FMargin(16.f));
		Size->AddChild(Root);
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Root->SetContent(Col);

		UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		Col->AddChildToVerticalBox(HeaderRow)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		UTextBlock* Header = MakeText(WidgetTree, 20, Orange, true);
		Header->SetText(NSLOCTEXT("SharedWorld", "Header", "SHARED WORLDS"));
		UHorizontalBoxSlot* HeaderSlot = HeaderRow->AddChildToHorizontalBox(Header);
		HeaderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UTextBlock* Unused = nullptr;
		UButton* Toggle = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "Setup", "+ Add / Account"));
		Toggle->OnClicked.AddDynamic(this, &USharedWorldPanel::OnToggleSetup);
		HeaderRow->AddChildToHorizontalBox(Toggle);

		ProblemText = MakeText(WidgetTree, 11, SharedWorldStyle::Error);
		Col->AddChildToVerticalBox(ProblemText);
		EmptyText = MakeText(WidgetTree, 11, Muted);
		EmptyText->SetText(NSLOCTEXT("SharedWorld", "Empty", "No Shared Worlds yet. Use \"+ Add / Account\" to convert one of your saves or to add a friend's world."));
		Col->AddChildToVerticalBox(EmptyText);
		List = WidgetTree->ConstructWidget<UVerticalBox>();
		Col->AddChildToVerticalBox(List);

		// ---- setup section (collapsed until toggled)
		UBorder* SetupBorder = WidgetTree->ConstructWidget<UBorder>();
		SetupBorder->SetBrushColor(Card);
		SetupBorder->SetPadding(FMargin(12.f));
		Col->AddChildToVerticalBox(SetupBorder)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
		UVerticalBox* Setup = WidgetTree->ConstructWidget<UVerticalBox>();
		SetupBorder->SetContent(Setup);
		SetupBox = SetupBorder;

		AccountText = MakeText(WidgetTree, 11, Text);
		Setup->AddChildToVerticalBox(AccountText);
		UButton* SignIn = MakeButton(WidgetTree, Orange, SignInLabel, FText::GetEmpty());
		SignIn->OnClicked.AddDynamic(this, &USharedWorldPanel::OnSignInClicked);
		Setup->AddChildToVerticalBox(SignIn)->SetPadding(FMargin(0.f, 4.f, 0.f, 10.f));

		UTextBlock* StorageLabel = MakeText(WidgetTree, 11, Muted);
		StorageLabel->SetText(NSLOCTEXT("SharedWorld", "StorageLabel", "Storage: a GitHub repository (owner/repo) or a shared folder path"));
		Setup->AddChildToVerticalBox(StorageLabel);
		StorageInput = MakeInput(WidgetTree, NSLOCTEXT("SharedWorld", "StorageHint", "e.g. reece/our-factory-saves"));
		Setup->AddChildToVerticalBox(StorageInput)->SetPadding(FMargin(0.f, 2.f, 0.f, 10.f));

		UTextBlock* ConvertLabel = MakeText(WidgetTree, 12, Text, true);
		ConvertLabel->SetText(NSLOCTEXT("SharedWorld", "ConvertLabel", "Convert one of your saves"));
		Setup->AddChildToVerticalBox(ConvertLabel);
		SaveNameInput = MakeInput(WidgetTree, NSLOCTEXT("SharedWorld", "SaveHint", "Save name (as shown in Load Game)"));
		WorldNameInput = MakeInput(WidgetTree, NSLOCTEXT("SharedWorld", "NameHint", "Shared World name"));
		Setup->AddChildToVerticalBox(SaveNameInput)->SetPadding(FMargin(0.f, 2.f));
		Setup->AddChildToVerticalBox(WorldNameInput)->SetPadding(FMargin(0.f, 2.f));
		UHorizontalBox* MembersRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		MembersOnlyCheck = WidgetTree->ConstructWidget<UCheckBox>();
		MembersOnlyCheck->SetIsChecked(false);
		UTextBlock* MembersLabel = MakeText(WidgetTree, 11, Muted);
		MembersLabel->SetText(NSLOCTEXT("SharedWorld", "MembersOnly", " Members only (otherwise anyone with access to the storage may play)"));
		MembersRow->AddChildToHorizontalBox(MembersOnlyCheck);
		MembersRow->AddChildToHorizontalBox(MembersLabel);
		Setup->AddChildToVerticalBox(MembersRow)->SetPadding(FMargin(0.f, 2.f));
		UButton* Create = MakeButton(WidgetTree, Orange, Unused, NSLOCTEXT("SharedWorld", "Create", "Create Shared World"));
		Create->OnClicked.AddDynamic(this, &USharedWorldPanel::OnCreateClicked);
		Setup->AddChildToVerticalBox(Create)->SetPadding(FMargin(0.f, 4.f, 0.f, 10.f));

		UTextBlock* AddLabel = MakeText(WidgetTree, 12, Text, true);
		AddLabel->SetText(NSLOCTEXT("SharedWorld", "AddLabel", "Add a friend's Shared World"));
		Setup->AddChildToVerticalBox(AddLabel);
		WorldIdInput = MakeInput(WidgetTree, NSLOCTEXT("SharedWorld", "IdHint", "World id (your friend sees it with /sharedworld status)"));
		Setup->AddChildToVerticalBox(WorldIdInput)->SetPadding(FMargin(0.f, 2.f));
		UButton* Add = MakeButton(WidgetTree, Orange, Unused, NSLOCTEXT("SharedWorld", "Add", "Add"));
		Add->OnClicked.AddDynamic(this, &USharedWorldPanel::OnAddClicked);
		Setup->AddChildToVerticalBox(Add)->SetPadding(FMargin(0.f, 4.f, 0.f, 6.f));

		ResultText = MakeText(WidgetTree, 11, Muted);
		Setup->AddChildToVerticalBox(ResultText);
		UTextBlock* Invite = MakeText(WidgetTree, 10, Muted);
		Invite->SetText(NSLOCTEXT("SharedWorld", "InviteHint",
			"Friends join your game through the Satisfactory friends list (Steam / Epic) as usual. "
			"Friends who should be able to host or take over need access to the storage: "
			"in game, /sharedworld granthost <github-username>."));
		Setup->AddChildToVerticalBox(Invite)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
		Show(SetupBox, false);
	}
	return Super::RebuildWidget();
}

USharedWorldSubsystem* USharedWorldPanel::GetSharedWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

void USharedWorldPanel::NativeConstruct()
{
	Super::NativeConstruct();
	// Top-right corner of the main menu, clear of the game's own menu column.
	SetAnchorsInViewport(FAnchors(1.f, 0.f));
	SetAlignmentInViewport(FVector2D(1.f, 0.f));
	SetPositionInViewport(FVector2D(-48.f, 96.f), /*bRemoveDPIScale*/ false);
	if (USharedWorldSubsystem* SW = GetSharedWorld())
	{
		ChangedHandle = SW->OnChanged.AddUObject(this, &USharedWorldPanel::Refresh);
	}
	Refresh();
}

void USharedWorldPanel::NativeDestruct()
{
	if (USharedWorldSubsystem* SW = GetSharedWorld())
	{
		SW->OnChanged.Remove(ChangedHandle);
	}
	Super::NativeDestruct();
}

void USharedWorldPanel::Refresh()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW || !List)
	{
		return;
	}
	const FString& Problem = SW->GetSettingsProblem();
	ProblemText->SetText(FText::FromString(Problem));
	Show(ProblemText, !Problem.IsEmpty());

	const TArray<FSharedWorldEntryView> Worlds = SW->GetWorldViews();
	Show(EmptyText, Worlds.Num() == 0);
	if (Entries.Num() != Worlds.Num())
	{
		List->ClearChildren();
		Entries.Reset();
		for (int32 i = 0; i < Worlds.Num(); ++i)
		{
			USharedWorldEntry* E = CreateWidget<USharedWorldEntry>(this, USharedWorldEntry::StaticClass());
			List->AddChildToVerticalBox(E)->SetPadding(FMargin(0.f, 6.f, 0.f, 0.f));
			Entries.Add(E);
		}
	}
	for (int32 i = 0; i < Worlds.Num(); ++i)
	{
		Entries[i]->Update(Worlds[i]);
	}

	// Account line. The token itself is never shown or logged.
	const FSharedWorldSignIn SignIn = SW->GetSignInStatus();
	const FString Login = SW->GetGitHubLogin();
	FString Account;
	if (SignIn.bInProgress && !SignIn.UserCode.IsEmpty())
	{
		Account = FString::Printf(TEXT("Open %s in your browser and enter the code:  %s"), *SignIn.VerificationUri, *SignIn.UserCode);
	}
	else if (SignIn.bInProgress)
	{
		Account = TEXT("Contacting GitHub...");
	}
	else if (!Login.IsEmpty())
	{
		Account = TEXT("GitHub: signed in as ") + Login + TEXT(" (needed only for GitHub storage)");
	}
	else
	{
		Account = TEXT("GitHub: not signed in (needed only for worlds stored on GitHub)");
	}
	if (!SignIn.Error.IsEmpty())
	{
		Account += TEXT("\n") + SignIn.Error;
	}
	AccountText->SetText(FText::FromString(Account));
	SignInLabel->SetText(!Login.IsEmpty() ? NSLOCTEXT("SharedWorld", "SignOut", "Sign out of GitHub") : NSLOCTEXT("SharedWorld", "SignIn", "Sign in with GitHub"));
}

void USharedWorldPanel::SetResult(bool bOk, const FString& Message)
{
	bBusy = false;
	ResultText->SetText(FText::FromString(Message));
	ResultText->SetColorAndOpacity(FSlateColor(bOk ? Online : SharedWorldStyle::Error));
}

bool USharedWorldPanel::ParseStorage(sw::ProviderConfig& Out, FString& OutError) const
{
	const FString In = StorageInput->GetText().ToString().TrimStartAndEnd();
	const bool bPath = In.StartsWith(TEXT("\\\\")) || In.StartsWith(TEXT("/")) || (In.Len() > 2 && In[1] == TEXT(':'));
	if (bPath)
	{
		Out.Kind = sw::ProviderKind::Folder;
		Out.FolderPath = TCHAR_TO_UTF8(*In);
	}
	else
	{
		FString Owner, Repo;
		if (!In.Split(TEXT("/"), &Owner, &Repo) || Owner.IsEmpty() || Repo.IsEmpty())
		{
			OutError = TEXT("Enter the storage as owner/repo (GitHub) or a folder path.");
			return false;
		}
		Out.Kind = sw::ProviderKind::GitHub;
		Out.Owner = TCHAR_TO_UTF8(*Owner);
		Out.Repo = TCHAR_TO_UTF8(*Repo.Replace(TEXT(".git"), TEXT("")));
	}
	if (sw::Status V = Out.Validate(); !V)
	{
		OutError = UTF8_TO_TCHAR(V.Err().Message.c_str());
		return false;
	}
	return true;
}

void USharedWorldPanel::OnToggleSetup()
{
	Show(SetupBox, SetupBox->GetVisibility() == ESlateVisibility::Collapsed, true);
	Refresh();
}

void USharedWorldPanel::OnSignInClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW)
	{
		return;
	}
	if (!SW->GetGitHubLogin().IsEmpty())
	{
		SW->SignOutOfGitHub();
	}
	else
	{
		SW->BeginGitHubSignIn();
	}
	Refresh();
}

void USharedWorldPanel::OnCreateClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW || bBusy)
	{
		return;
	}
	sw::ProviderConfig Provider;
	FString Error;
	if (!ParseStorage(Provider, Error))
	{
		SetResult(false, Error);
		return;
	}
	bBusy = true;
	ResultText->SetText(NSLOCTEXT("SharedWorld", "Creating", "Uploading your save as revision 1..."));
	TWeakObjectPtr<USharedWorldPanel> WeakThis(this);
	SW->CreateWorldFromSave(WorldNameInput->GetText().ToString(), SaveNameInput->GetText().ToString().TrimStartAndEnd(), Provider,
		MembersOnlyCheck->IsChecked(), [WeakThis](bool bOk, const FString& Message)
	{
		if (USharedWorldPanel* Self = WeakThis.Get())
		{
			Self->SetResult(bOk, Message);
		}
	});
}

void USharedWorldPanel::OnAddClicked()
{
	USharedWorldSubsystem* SW = GetSharedWorld();
	if (!SW || bBusy)
	{
		return;
	}
	sw::ProviderConfig Provider;
	FString Error;
	if (!ParseStorage(Provider, Error))
	{
		SetResult(false, Error);
		return;
	}
	bBusy = true;
	ResultText->SetText(NSLOCTEXT("SharedWorld", "Checking", "Checking the world..."));
	TWeakObjectPtr<USharedWorldPanel> WeakThis(this);
	const FString Id = WorldIdInput->GetText().ToString().TrimStartAndEnd();
	SW->AddExistingWorld(Id, Id, Provider, [WeakThis](bool bOk, const FString& Message)
	{
		if (USharedWorldPanel* Self = WeakThis.Get())
		{
			Self->SetResult(bOk, Message);
		}
	});
}
