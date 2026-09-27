#include "SharedWorldPanel.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "Misc/DateTime.h"
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

	FString Ago(const FString& Iso)
	{
		FDateTime When;
		if (Iso.IsEmpty() || !FDateTime::ParseIso8601(*Iso, When))
		{
			return FString();
		}
		const FTimespan D = FDateTime::UtcNow() - When;
		if (D.GetTotalMinutes() < 1.0) return TEXT("just now");
		if (D.GetTotalHours() < 1.0) return FString::Printf(TEXT("%d minutes ago"), FMath::FloorToInt(D.GetTotalMinutes()));
		if (D.GetTotalDays() < 1.0) return FString::Printf(TEXT("%d hours ago"), FMath::FloorToInt(D.GetTotalHours()));
		return FString::Printf(TEXT("%d days ago"), FMath::FloorToInt(D.GetTotalDays()));
	}

	FString StatusLabel(const FString& S)
	{
		if (S == TEXT("ONLINE")) return TEXT("Online");
		if (S == TEXT("STARTING")) return TEXT("Starting");
		if (S == TEXT("SAVING")) return TEXT("Online (saving)");
		if (S == TEXT("STOPPING")) return TEXT("Closing");
		if (S == TEXT("RECOVERABLE")) return TEXT("Available (recovery needed)");
		if (S == TEXT("NO_SAVE")) return TEXT("No shared save yet");
		if (S == TEXT("UNREACHABLE")) return TEXT("Cloud unreachable");
		return TEXT("Available");
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
		UVerticalBoxSlot* S = Col->AddChildToVerticalBox(W);
		S->SetPadding(FMargin(0.f, 2.f));
	}

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* RowSlot = Col->AddChildToVerticalBox(Row);
	RowSlot->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));

	PlayButton = MakeButton(WidgetTree, Orange, PlayLabel, NSLOCTEXT("SharedWorld", "Play", "Play Shared World"));
	PlayButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnPlayClicked);
	UTextBlock* Unused = nullptr;
	SecondaryButton = MakeButton(WidgetTree, Muted, SecondaryLabel, FText::GetEmpty());
	SecondaryButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnSecondaryClicked);
	DetailsButton = MakeButton(WidgetTree, Muted, Unused, NSLOCTEXT("SharedWorld", "Details", "Details"));
	DetailsButton->OnClicked.AddDynamic(this, &USharedWorldEntry::OnDetailsClicked);
	for (UWidget* B : TArray<UWidget*>{PlayButton, SecondaryButton, DetailsButton})
	{
		UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(B);
		S->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
}

USharedWorldSubsystem* USharedWorldEntry::GetSharedWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

void USharedWorldEntry::Update(const FSharedWorldStatus& S)
{
	Current = S;
	if (!TitleText)
	{
		return; // not built yet; RebuildWidget will run before display
	}
	const FSharedWorldSession& L = S.Local;
	TitleText->SetText(FText::FromString(S.WorldName.IsEmpty() ? S.WorldId : S.WorldName));

	const bool bOnline = S.Status == TEXT("ONLINE") || S.Status == TEXT("SAVING");
	StatusText->SetText(FText::FromString(TEXT("Status: ") + StatusLabel(S.Status)));
	StatusText->SetColorAndOpacity(FSlateColor(bOnline ? Online : Muted));

	TArray<FString> Info;
	if (!S.HostName.IsEmpty() && S.Status != TEXT("AVAILABLE") && S.Status != TEXT("RECOVERABLE"))
	{
		Info.Add(TEXT("Host: ") + S.HostName);
		Info.Add(FString::Printf(TEXT("Players: %d"), S.PlayerCount));
	}
	Info.Add(S.Revision > 0 ? FString::Printf(TEXT("Revision: %lld"), S.Revision) : TEXT("No save yet"));
	const FString LastPlayed = Ago(S.LastPlayedAt);
	if (!bOnline && !LastPlayed.IsEmpty())
	{
		Info.Add(TEXT("Last played: ") + LastPlayed + (S.LastHostName.IsEmpty() ? TEXT("") : TEXT(" by ") + S.LastHostName));
	}
	InfoText->SetText(FText::FromString(FString::Join(Info, TEXT("   "))));

	// Progress: the helper's notification lines ("Checking shared world...").
	const bool bBusy = !L.IsIdle() && L.State != TEXT("ERROR") && L.State != TEXT("LEASE_LOST");
	ProgressText->SetText(FText::FromString(L.IsIdle() ? FString() : L.Message));
	ProgressText->SetVisibility(L.Message.IsEmpty() || L.IsIdle() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	const bool bError = L.bHasError && (L.State == TEXT("ERROR") || L.State == TEXT("LEASE_LOST") || !L.Error.Message.IsEmpty());
	FString ErrorMsg = bError ? L.Error.Message : FString();
	if (bError && !L.Error.BackupPath.IsEmpty())
	{
		ErrorMsg += TEXT("\nBackup: ") + L.Error.BackupPath;
	}
	ErrorText->SetText(FText::FromString(ErrorMsg));
	ErrorText->SetVisibility(bError ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	TArray<FString> Steps;
	for (const FSharedWorldStep& Step : L.Steps)
	{
		Steps.Add(Step.Message);
	}
	FString Detail = FString::Join(Steps, TEXT("\n"));
	if (bError && !L.Error.Detail.IsEmpty())
	{
		Detail += TEXT("\n\nDetails: ") + L.Error.Detail;
	}
	if (!S.Error.IsEmpty())
	{
		Detail += TEXT("\n\nCloud: ") + S.Error;
	}
	DetailText->SetText(FText::FromString(Detail));
	DetailText->SetVisibility(bShowDetails && !Detail.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	// One button decides host vs join; it is disabled while something runs.
	PlayButton->SetIsEnabled(!bBusy && L.State != TEXT("LEASE_LOST") && S.Status != TEXT("NO_SAVE"));

	if (L.State == TEXT("WAITING_FOR_HOST") || L.State == TEXT("READY_TO_HOST"))
	{
		Secondary = ESecondary::Cancel;
	}
	else if (L.State == TEXT("ERROR") && L.bHasError && L.Error.Retryable)
	{
		Secondary = ESecondary::Retry;
	}
	else if (L.State == TEXT("ERROR") || L.State == TEXT("LEASE_LOST") || L.State == TEXT("JOIN_READY"))
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
	SecondaryButton->SetVisibility(Secondary == ESecondary::None ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
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

// ---------------------------------------------------------------- panel

TSharedRef<SWidget> USharedWorldPanel::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		// Fixed width, height follows the content.
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(440.f);
		WidgetTree->RootWidget = Size;
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
		Root->SetBrushColor(SharedWorldStyle::Panel);
		Root->SetPadding(FMargin(16.f));
		Size->AddChild(Root);
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Root->SetContent(Col);
		UTextBlock* Header = MakeText(WidgetTree, 20, Orange, true);
		Header->SetText(NSLOCTEXT("SharedWorld", "Header", "SHARED WORLDS"));
		Col->AddChildToVerticalBox(Header)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));
		ConnectionText = MakeText(WidgetTree, 11, Muted);
		Col->AddChildToVerticalBox(ConnectionText);
		List = WidgetTree->ConstructWidget<UVerticalBox>();
		Col->AddChildToVerticalBox(List);
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
	const FString Problem = SW->GetConnectionProblem();
	ConnectionText->SetText(FText::FromString(Problem));
	ConnectionText->SetVisibility(Problem.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	const TArray<FSharedWorldStatus>& Worlds = SW->GetWorlds();
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
}
