#include "UI/SharedWorldNativeMenu.h"

#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "UI/SharedWorldFgWidgets.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "SharedWorldTypes.h"

namespace SharedWorldNativeMenu
{
	UClass* LoadFrontEndButtonClass()
	{
		return LoadClass<UUserWidget>(nullptr, FrontEndButtonPath);
	}

	UWidget* FindNamed(UUserWidget* Root, FName Name)
	{
		return SharedWorldFg::FindNamedWidget(Root, Name);
	}

	int32 ChildIndex(UPanelWidget* Panel, UWidget* Child)
	{
		if (!Panel || !Child) return INDEX_NONE;
		const int32 N = Panel->GetChildrenCount();
		for (int32 i = 0; i < N; ++i)
		{
			if (Panel->GetChildAt(i) == Child) return i;
		}
		return INDEX_NONE;
	}

	UUserWidget* CreateFrontEndMenuButton(APlayerController* /*PC*/, const FText& /*Title*/,
		UObject* /*ClickReceiver*/, FName /*ClickFuncName*/)
	{
		return nullptr;
	}

	bool EnsureMainMenuEntry(UUserWidget* MainMenuRoot, USharedWorldGameInstanceModule* Owner)
	{
		if (!MainMenuRoot || !Owner) return false;

		UUserWidget* Button = Cast<UUserWidget>(FindNamed(MainMenuRoot, TEXT("mButtonSharedWorlds")));
		if (!Button)
		{
			UE_LOG(LogSharedWorld, Warning,
				TEXT("[SharedWorld] event=menu_missing_button reason=\"mButtonSharedWorlds not hooked yet\""));
			return false;
		}

		Owner->WireMainMenuSharedWorldsButton(MainMenuRoot, Button);
		return true;
	}
}
