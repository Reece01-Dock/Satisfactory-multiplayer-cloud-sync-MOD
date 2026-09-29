#include "UI/SharedWorldMainMenuButton.h"

#include "Blueprint/WidgetTree.h"
#include "Components/SizeBox.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectIterator.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "UI/SharedWorldNativeMenu.h"

// Legacy hook target. Prefer runtime Widget_FrontEnd_Button injection.
// If this class is still spawned, replace its visual with a native FrontEnd button.

TSharedRef<SWidget> USharedWorldMainMenuButton::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
		Size->SetHeightOverride(1.f);
		Size->SetVisibility(ESlateVisibility::Collapsed);
		WidgetTree->RootWidget = Size;
	}
	return Super::RebuildWidget();
}

void USharedWorldMainMenuButton::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::Collapsed);
	RemoveFromParent();
}

void USharedWorldMainMenuButton::OnClicked()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		for (TObjectIterator<USharedWorldGameInstanceModule> It; It; ++It)
		{
			if (It->GetGameInstance() == GI)
			{
				It->OpenSharedWorldsBrowser();
				return;
			}
		}
	}
}
