#include "Services/SharedWorldDiscoveryService.h"

#include "OnlineSubsystem.h"
#include "Interfaces/OnlineFriendsInterface.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Interfaces/OnlinePresenceInterface.h"
#include "SharedWorldSubsystem.h"

namespace
{
	IOnlineSubsystem* ResolveFriendsOss()
	{
		// Satisfactory often defaults to EOS; Steam friends live on the Steam OSS.
		if (IOnlineSubsystem* Steam = IOnlineSubsystem::Get(TEXT("STEAM")))
		{
			return Steam;
		}
		if (IOnlineSubsystem* SteamSockets = IOnlineSubsystem::Get(TEXT("Steam")))
		{
			return SteamSockets;
		}
		return IOnlineSubsystem::Get();
	}

	bool TryReadFriends(IOnlineFriendsPtr Friends, int32 LocalUser, const FString& ListName, TArray<FSharedWorldFriendInfo>& Out)
	{
		if (!Friends.IsValid()) return false;
		TArray<TSharedRef<FOnlineFriend>> List;
		if (!Friends->GetFriendsList(LocalUser, ListName, List))
		{
			Friends->ReadFriendsList(LocalUser, ListName);
			return false;
		}
		for (const TSharedRef<FOnlineFriend>& F : List)
		{
			FSharedWorldFriendInfo Info;
			const TSharedRef<const FUniqueNetId> Id = F->GetUserId();
			Info.PlayerId = Id->ToString();
			Info.DisplayName = F->GetDisplayName();
			const FOnlineUserPresence& Presence = F->GetPresence();
			Info.bOnline = Presence.bIsOnline;
			if (Info.DisplayName.IsEmpty() && Info.PlayerId.IsEmpty()) continue;
			Out.Add(MoveTemp(Info));
		}
		return Out.Num() > 0 || List.Num() == 0; // true once list is loaded (even if empty)
	}
}

FSharedWorldDiscoverySnapshot FSharedWorldDiscoveryService::BuildSnapshot() const
{
	FSharedWorldDiscoverySnapshot Snap;
	const TArray<FSharedWorldEntryView> All = SW.GetWorldViews();
	for (const FSharedWorldEntryView& V : All)
	{
		if (V.bOwned) Snap.OwnedWorlds.Add(V);
		else Snap.SharedWithYou.Add(V);
		if (V.IsHostingNow()) Snap.FriendsPlaying.Add(V);
	}
	Snap.PendingInvites = SW.GetPendingInviteViews();
	Snap.Friends = ListFriends();
	Snap.StatusMessage = All.Num() == 0
		? TEXT("Looking for Shared Worlds...")
		: FString::Printf(TEXT("%d world%s"), All.Num(), All.Num() == 1 ? TEXT("") : TEXT("s"));
	const FString Problem = SW.GetSettingsProblem();
	if (!Problem.IsEmpty()) Snap.ErrorMessage = Problem;
	return Snap;
}

void FSharedWorldDiscoveryService::BeginRefresh()
{
	// Kick a Steam friends pull so later ListFriends calls can succeed.
	if (IOnlineSubsystem* Oss = ResolveFriendsOss())
	{
		if (IOnlineFriendsPtr Friends = Oss->GetFriendsInterface())
		{
			const FString DefaultList = EFriendsLists::ToString(EFriendsLists::Default);
			Friends->ReadFriendsList(0, DefaultList);
		}
	}
	SW.RequestDiscoveryRefresh();
}

TArray<FSharedWorldFriendInfo> FSharedWorldDiscoveryService::ListFriends() const
{
	TArray<FSharedWorldFriendInfo> Out;
	IOnlineSubsystem* Oss = ResolveFriendsOss();
	if (!Oss) return CachedFriends;
	const IOnlineFriendsPtr Friends = Oss->GetFriendsInterface();
	if (!Friends.IsValid()) return CachedFriends;

	const int32 LocalUser = 0;
	static const FString ListNames[] = {
		EFriendsLists::ToString(EFriendsLists::Default),
		EFriendsLists::ToString(EFriendsLists::OnlinePlayers),
		TEXT("Friends"),
	};
	bool bLoaded = false;
	for (const FString& Name : ListNames)
	{
		TArray<FSharedWorldFriendInfo> Batch;
		if (TryReadFriends(Friends, LocalUser, Name, Batch))
		{
			bLoaded = true;
			if (Batch.Num() > 0)
			{
				Out = MoveTemp(Batch);
				break;
			}
		}
	}
	if (Out.Num() > 0)
	{
		CachedFriends = Out;
		return Out;
	}
	if (bLoaded)
	{
		CachedFriends.Reset();
		return Out;
	}
	// Still loading — keep last good cache so the UI doesn't flash empty.
	return CachedFriends;
}
