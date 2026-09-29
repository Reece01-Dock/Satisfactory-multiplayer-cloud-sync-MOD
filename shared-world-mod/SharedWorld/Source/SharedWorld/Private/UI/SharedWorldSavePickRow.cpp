#include "UI/SharedWorldSavePickRow.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldSavePickRow::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UBorder* Root = WidgetTree->ConstructWidget<UBorder>();
		Root->SetBrushColor(RowIdle);
		WidgetTree->RootWidget = Root;
		Button = WidgetTree->ConstructWidget<UButton>();
		Button->SetBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		Button->OnClicked.AddDynamic(this, &USharedWorldSavePickRow::OnClicked);
		Root->SetContent(Button);
		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		Button->AddChild(Col);
		NameText = MakeText(WidgetTree, 16, TextPrimary, true);
		Col->AddChildToVerticalBox(NameText)->SetPadding(FMargin(16, 10, 16, 2));
		MetaText = MakeText(WidgetTree, 12, TextMuted);
		Col->AddChildToVerticalBox(MetaText)->SetPadding(FMargin(16, 0, 16, 10));
	}
	return Super::RebuildWidget();
}

void USharedWorldSavePickRow::Setup(const FString& InSaveName, const FString& InDisplayName, const FString& InLastPlayed, USharedWorldBrowserWidget* Owner)
{
	SaveName = InSaveName;
	DisplayName = InDisplayName;
	OwnerBrowser = Owner;
	if (NameText) NameText->SetText(FText::FromString(DisplayName));
	if (MetaText) MetaText->SetText(FText::FromString(FString::Printf(TEXT("Last played: %s"), *InLastPlayed)));
}

void USharedWorldSavePickRow::OnClicked()
{
	if (USharedWorldBrowserWidget* B = OwnerBrowser.Get())
	{
		B->OnSavePicked(SaveName, DisplayName);
	}
}
