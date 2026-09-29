#include "UI/SharedWorldSessionMenuButton.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldSessionWidget.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldSessionMenuButton::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		// Match Join Game / Manage Session FrontEnd row height.
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetMinDesiredHeight(48.f);
		Size->SetHeightOverride(48.f);
		WidgetTree->RootWidget = Size;

		Button = WidgetTree->ConstructWidget<UButton>();
		StyleAsJoinRow(Button);
		Label = MakeText(WidgetTree, 20, TextPrimary, true);
		Label->SetText(NSLOCTEXT("SharedWorld", "SessionEntry", "Shared World"));
		Label->SetAutoWrapText(false);
		Label->SetJustification(ETextJustify::Left);
		Button->AddChild(Label);
		Size->AddChild(Button);
	}
	return Super::RebuildWidget();
}

void USharedWorldSessionMenuButton::NativeConstruct()
{
	Super::NativeConstruct();
	if (Button && !Button->OnClicked.IsAlreadyBound(this, &USharedWorldSessionMenuButton::OnClicked))
	{
		Button->OnClicked.AddDynamic(this, &USharedWorldSessionMenuButton::OnClicked);
	}
	SetVisibility(ESlateVisibility::Visible);
	SetIsEnabled(true);
}

void USharedWorldSessionMenuButton::OnClicked()
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pause_shared_world_clicked"));
	APlayerController* PC = GetOwningPlayer();
	if (!PC) return;

	USharedWorldSessionWidget* Screen = CreateWidget<USharedWorldSessionWidget>(PC, USharedWorldSessionWidget::StaticClass());
	if (Screen)
	{
		Screen->AddToViewport(20000);
		Screen->SetVisibility(ESlateVisibility::Visible);
	}
}
