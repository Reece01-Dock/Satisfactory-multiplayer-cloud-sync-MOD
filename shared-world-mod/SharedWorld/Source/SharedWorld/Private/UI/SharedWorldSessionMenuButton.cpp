#include "UI/SharedWorldSessionMenuButton.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/GameInstance.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "UI/SharedWorldUiStyle.h"
#include "UObject/UObjectIterator.h"

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
	if (UGameInstance* GI = GetGameInstance())
	{
		for (TObjectIterator<USharedWorldGameInstanceModule> It; It; ++It)
		{
			if (It->GetGameInstance() == GI)
			{
				It->OpenSharedWorldSession();
				return;
			}
		}
	}
	UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=session_open_fail reason=\"no game instance module\""));
}
