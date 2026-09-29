#include "Services/SharedWorldInviteService.h"

#include "SharedWorldSubsystem.h"
#include "SharedWorldUeConvert.h"

void FSharedWorldInviteService::InviteFriend(const FString& WorldId, const FString& FriendPlayerId, const FString& FriendDisplayName, FDone OnDone)
{
	const FString Who = FriendPlayerId.IsEmpty() ? FriendDisplayName : FriendPlayerId;
	SW.AllowPlayer(WorldId, Who, sw::Role::Member,
		[this, WorldId, FriendPlayerId, FriendDisplayName, Who, OnDone](bool bOk, const FString& Message)
		{
			if (!bOk)
			{
				OnDone(false, Message);
				return;
			}
			const FString Label = FriendDisplayName.IsEmpty() ? Who : FriendDisplayName;
			// Push storage config to the connected client so they auto-add the world.
			const bool bPushed = SW.PushWorldInviteToConnectedPlayer(Who, WorldId);
			if (!bPushed)
			{
				if (const sw::WorldEntry* Entry = SW.FindWorldEntry(WorldId))
				{
					SW.QueueIncomingInvite(WorldId, UTF8_TO_TCHAR(Entry->DisplayName.c_str()),
						FriendPlayerId, FriendDisplayName, Entry->Provider);
				}
				const FString Code = GetOrCreateInviteCode(WorldId);
				OnDone(true, FString::Printf(
					TEXT("Added %s. They were not in this session — share code %s if they need to add it later."),
					*Label, *Code));
				return;
			}
			OnDone(true, FString::Printf(
				TEXT("Added %s — the world is on their list now (no code needed)."),
				*Label));
		});
}

void FSharedWorldInviteService::AcceptInvite(const FString& InviteId, FDone OnDone)
{
	SW.AcceptPendingInvite(InviteId, OnDone);
}

void FSharedWorldInviteService::DeclineInvite(const FString& InviteId, FDone OnDone)
{
	SW.DeclinePendingInvite(InviteId, OnDone);
}

void FSharedWorldInviteService::JoinUsingCode(const FString& Code, FDone OnDone)
{
	SW.JoinUsingShareCode(Code, OnDone);
}

FString FSharedWorldInviteService::GetOrCreateInviteCode(const FString& WorldId)
{
	return SW.EnsureInviteCode(WorldId);
}

void FSharedWorldInviteService::QueuePendingInvite(const FString& WorldId, const FString& WorldName, const FString& FromPlayerId, const FString& FromDisplayName, const sw::ProviderConfig& Provider)
{
	SW.QueueIncomingInvite(WorldId, WorldName, FromPlayerId, FromDisplayName, Provider);
}
