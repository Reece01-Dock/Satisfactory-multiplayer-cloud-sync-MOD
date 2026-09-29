#include "UI/SharedWorldInviteRowBinder.h"
#include "UI/SharedWorldSessionWidget.h"

void USharedWorldInviteRowBinder::OnInviteClicked()
{
	if (USharedWorldSessionWidget* S = Session.Get())
	{
		S->InvitePlayer(WorldId, PlayerId, DisplayName);
	}
}
