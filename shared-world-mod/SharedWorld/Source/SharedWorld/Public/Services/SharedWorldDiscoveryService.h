#pragma once
// Player-facing discovery snapshots.

#include "CoreMinimal.h"
#include "SharedWorldTypes.h"

class USharedWorldSubsystem;

struct FSharedWorldFriendInfo
{
	FString PlayerId;
	FString DisplayName;
	bool bOnline = false;
};

struct FSharedWorldPendingInviteView
{
	FString InviteId;
	FString WorldId;
	FString WorldName;
	FString FromPlayerId;
	FString FromDisplayName;
};

struct FSharedWorldDiscoverySnapshot
{
	TArray<FSharedWorldEntryView> OwnedWorlds;
	TArray<FSharedWorldEntryView> SharedWithYou;
	TArray<FSharedWorldEntryView> FriendsPlaying;
	TArray<FSharedWorldPendingInviteView> PendingInvites;
	TArray<FSharedWorldFriendInfo> Friends;
	bool bRefreshing = false;
	FString StatusMessage;
	FString ErrorMessage;
};

struct FSharedWorldSaveInfo
{
	FString SaveName;
	FString SessionName;
	FString LastPlayedText;
	FDateTime SaveDate;
};

class FSharedWorldDiscoveryService
{
public:
	explicit FSharedWorldDiscoveryService(USharedWorldSubsystem& InSW) : SW(InSW) {}

	FSharedWorldDiscoverySnapshot BuildSnapshot() const;
	void BeginRefresh();
	TArray<FSharedWorldFriendInfo> ListFriends() const;

private:
	USharedWorldSubsystem& SW;
	/** Last non-empty friends snapshot (avoids empty flash while Steam list loads). */
	mutable TArray<FSharedWorldFriendInfo> CachedFriends;
};
