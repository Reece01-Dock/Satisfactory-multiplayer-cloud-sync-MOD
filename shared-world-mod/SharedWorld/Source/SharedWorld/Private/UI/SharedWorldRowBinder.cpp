#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "Engine/GameInstance.h"
#include "SharedWorldSubsystem.h"

void USharedWorldRowBinder::OnClicked()
{
	if (USharedWorldBrowserWidget* B = Browser.Get())
	{
		if (bRemoveFromList)
		{
			B->RemoveWorldFromList(WorldId);
			return;
		}
		B->SelectWorld(WorldId);
		if (bPlayOnClick)
		{
			if (UGameInstance* GI = B->GetGameInstance())
			{
				if (USharedWorldSubsystem* SW = GI->GetSubsystem<USharedWorldSubsystem>())
				{
					SW->Play(WorldId);
				}
			}
		}
	}
}
