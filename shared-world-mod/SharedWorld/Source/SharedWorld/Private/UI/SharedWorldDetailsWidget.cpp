#include "UI/SharedWorldDetailsWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/GameInstance.h"
#include "SharedWorldSubsystem.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldDetailsWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetWidthOverride(720.f);
		Size->SetHeightOverride(640.f);
		WidgetTree->RootWidget = Size;
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
		Root->SetBrushColor(BgDeep);
		Root->SetPadding(FMargin(28.f, 24.f));
		Size->AddChild(Root);
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Root->SetContent(Col);

		UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
		Col->AddChildToVerticalBox(Header)->SetPadding(FMargin(0, 0, 0, 12));
		TitleText = MakeText(WidgetTree, 26, TextPrimary, true);
		Header->AddChildToHorizontalBox(TitleText)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TObjectPtr<UTextBlock> BackLbl;
		UButton* Back = MakeTextLink(WidgetTree, BackLbl, NSLOCTEXT("SharedWorld", "Back", "Back"), 13);
		Back->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnBack);
		Header->AddChildToHorizontalBox(Back);

		UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
		Col->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UVerticalBox* Body = WidgetTree->ConstructWidget<UVerticalBox>();
		Scroll->AddChild(Body);

		BodyText = MakeText(WidgetTree, 14, TextPrimary);
		Body->AddChildToVerticalBox(BodyText)->SetPadding(FMargin(0, 0, 0, 12));

		UHorizontalBox* Actions = WidgetTree->ConstructWidget<UHorizontalBox>();
		Body->AddChildToVerticalBox(Actions)->SetPadding(FMargin(0, 0, 0, 12));
		TObjectPtr<UTextBlock> L1, L2, L3, L4;
		PlayButton = MakePrimaryButton(WidgetTree, L1, NSLOCTEXT("SharedWorld", "Play", "Play"), 16);
		PlayButton->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnPlay);
		Actions->AddChildToHorizontalBox(PlayButton)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* Players = MakeSecondaryButton(WidgetTree, L2, NSLOCTEXT("SharedWorld", "Players", "Players"), 13);
		Players->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnPlayers);
		Actions->AddChildToHorizontalBox(Players)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* History = MakeSecondaryButton(WidgetTree, L3, NSLOCTEXT("SharedWorld", "History", "Save History"), 13);
		History->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnHistory);
		Actions->AddChildToHorizontalBox(History)->SetPadding(FMargin(0, 0, 8, 0));
		UButton* Forget = MakeSecondaryButton(WidgetTree, L4, NSLOCTEXT("SharedWorld", "Forget", "Remove"), 13);
		Forget->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnForget);
		Actions->AddChildToHorizontalBox(Forget);

		UTextBlock* RestoreLbl = MakeText(WidgetTree, 12, TextMuted);
		RestoreLbl->SetText(NSLOCTEXT("SharedWorld", "RestoreLbl", "Restore creates a NEW revision (history is never rewritten)."));
		Body->AddChildToVerticalBox(RestoreLbl)->SetPadding(FMargin(0, 4, 0, 4));
		UHorizontalBox* RestoreRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		Body->AddChildToVerticalBox(RestoreRow)->SetPadding(FMargin(0, 0, 0, 8));
		RestoreInput = WidgetTree->ConstructWidget<UEditableTextBox>();
		RestoreInput->SetHintText(NSLOCTEXT("SharedWorld", "RestoreHint", "Revision number"));
		RestoreRow->AddChildToHorizontalBox(RestoreInput)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UButton* RestoreBtn = MakeButton(WidgetTree, Warn, L2, NSLOCTEXT("SharedWorld", "Restore", "RESTORE"), 12);
		RestoreBtn->OnClicked.AddDynamic(this, &USharedWorldDetailsWidget::OnRestore);
		RestoreRow->AddChildToHorizontalBox(RestoreBtn)->SetPadding(FMargin(8, 0, 0, 0));

		SectionText = MakeText(WidgetTree, 13, TextMuted);
		Body->AddChildToVerticalBox(SectionText)->SetPadding(FMargin(0, 8, 0, 0));
		FlashText = MakeText(WidgetTree, 12, TextMuted);
		Col->AddChildToVerticalBox(FlashText)->SetPadding(FMargin(0, 8, 0, 0));
	}
	return Super::RebuildWidget();
}

void USharedWorldDetailsWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (USharedWorldSubsystem* S = SW())
	{
		ChangedHandle = S->OnChanged.AddUObject(this, &USharedWorldDetailsWidget::Refresh);
	}
	Refresh();
}

void USharedWorldDetailsWidget::NativeDestruct()
{
	if (USharedWorldSubsystem* S = SW()) S->OnChanged.Remove(ChangedHandle);
	Super::NativeDestruct();
}

USharedWorldSubsystem* USharedWorldDetailsWidget::SW() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
}

void USharedWorldDetailsWidget::OpenForWorld(const FString& InWorldId)
{
	WorldId = InWorldId;
	Refresh();
}

void USharedWorldDetailsWidget::Close() { RemoveFromParent(); }
void USharedWorldDetailsWidget::OnBack() { Close(); }

void USharedWorldDetailsWidget::Refresh()
{
	if (!BodyText || WorldId.IsEmpty()) return;
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const TArray<FSharedWorldEntryView> Views = S->GetWorldViews();
	const FSharedWorldEntryView* Match = nullptr;
	for (const FSharedWorldEntryView& V : Views)
	{
		if (V.WorldId == WorldId) { Match = &V; break; }
	}
	if (!Match)
	{
		TitleText->SetText(FText::FromString(WorldId));
		BodyText->SetText(NSLOCTEXT("SharedWorld", "MissingWorld", "This Shared World is not in the local list."));
		return;
	}
	TitleText->SetText(FText::FromString(Match->WorldName.IsEmpty() ? Match->WorldId : Match->WorldName));
	const FString Badge = StatusBadge(Match->CloudStatus, Match->LocalState);
	FString Body;
	Body += FString::Printf(TEXT("Status:\n%s\n\n"), *Badge);
	if (!Match->HostName.IsEmpty()) Body += FString::Printf(TEXT("Host:\n%s\n\n"), *Match->HostName);
	else Body += TEXT("Host:\nNo current host\n\n");
	if (Match->PlayerCount > 0) Body += FString::Printf(TEXT("Players:\n%d\n\n"), Match->PlayerCount);
	Body += FString::Printf(TEXT("Revision:\n%lld\n\n"), Match->Revision);
	if (Match->Generation > 0) Body += FString::Printf(TEXT("Generation:\n%lld\n\n"), Match->Generation);
	if (!Match->LastPlayed.IsEmpty()) Body += FString::Printf(TEXT("Last Save:\n%s\n\n"), *Match->LastPlayed);
	if (!Match->LocalMessage.IsEmpty()) Body += Match->LocalMessage + TEXT("\n");
	if (Match->bHasError) Body += FString::Printf(TEXT("\nError: %s\n%s"), *Match->ErrorMessage, *Match->ErrorDetail);
	BodyText->SetText(FText::FromString(Body));
}

void USharedWorldDetailsWidget::OnPlay()
{
	if (USharedWorldSubsystem* S = SW()) S->Play(WorldId);
}

void USharedWorldDetailsWidget::OnPlayers()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	bBusy = true;
	if (FlashText) FlashText->SetText(NSLOCTEXT("SharedWorld", "LoadingPlayers", "Loading players…"));
	S->FetchPlayers(WorldId, [this](bool bOk, const FString& Message)
	{
		bBusy = false;
		if (SectionText) SectionText->SetText(FText::FromString(bOk ? Message : TEXT("Failed: ") + Message));
		if (FlashText) FlashText->SetText(FText::GetEmpty());
	});
}

void USharedWorldDetailsWidget::OnHistory()
{
	USharedWorldSubsystem* S = SW();
	if (!S || bBusy) return;
	bBusy = true;
	if (FlashText) FlashText->SetText(NSLOCTEXT("SharedWorld", "LoadingHistory", "Loading save history…"));
	S->FetchHistory(WorldId, 20, [this](bool bOk, const FString& Message)
	{
		bBusy = false;
		if (SectionText) SectionText->SetText(FText::FromString(bOk ? Message : TEXT("Failed: ") + Message));
		if (FlashText) FlashText->SetText(FText::GetEmpty());
	});
}

void USharedWorldDetailsWidget::OnRestore()
{
	USharedWorldSubsystem* S = SW();
	if (!S || !RestoreInput) return;
	const int64 Rev = FCString::Atoi64(*RestoreInput->GetText().ToString().TrimStartAndEnd());
	if (Rev <= 0)
	{
		if (FlashText) FlashText->SetText(NSLOCTEXT("SharedWorld", "BadRev", "Enter a valid revision number."));
		return;
	}
	S->Restore(WorldId, Rev);
	if (FlashText)
	{
		FlashText->SetColorAndOpacity(FSlateColor(Ok));
		FlashText->SetText(FText::FromString(FString::Printf(TEXT("Restoring revision %lld as a new revision…"), Rev)));
	}
}

void USharedWorldDetailsWidget::OnForget()
{
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FString Msg = S->ForgetWorld(WorldId);
	if (FlashText)
	{
		FlashText->SetColorAndOpacity(FSlateColor(Msg.IsEmpty() ? Ok : Err));
		FlashText->SetText(FText::FromString(Msg.IsEmpty() ? TEXT("Removed from this PC's list.") : Msg));
	}
	if (Msg.IsEmpty()) Close();
}
