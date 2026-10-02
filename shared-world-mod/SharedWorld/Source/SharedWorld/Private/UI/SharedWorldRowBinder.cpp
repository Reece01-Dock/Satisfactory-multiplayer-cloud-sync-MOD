#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "Engine/GameInstance.h"
#include "SharedWorldSubsystem.h"

void USharedWorldRowBinder::OnClicked()
{
	if (USharedWorldBrowserWidget* B = Browser.Get())
	{
		if (TabKind == 4)
		{
			B->SetConnectOption(ConnectOption, ConnectValue);
			return;
		}
		if (TabKind == 5)
		{
			B->SetCreateSaveTarget(ConnectValue);
			return;
		}
		if (TabKind == 6)
		{
			B->LinkWorldSaves(ConnectValue);
			return;
		}
		if (TabKind != 0)
		{
			B->SetTab(TabKind, TabIndex);
			return;
		}
		if (InviteAction != 0)
		{
			B->HandleInvite(InviteId, InviteAction == 1);
			return;
		}
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
