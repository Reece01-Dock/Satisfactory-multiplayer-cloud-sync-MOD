#pragma once
// Invite / accept / decline / short share codes.

#include "CoreMinimal.h"
#include "SharedWorldCore/App/LocalSettings.h"

class USharedWorldSubsystem;

class FSharedWorldInviteService
{
public:
	using FDone = TFunction<void(bool bOk, const FString& Message)>;

	explicit FSharedWorldInviteService(USharedWorldSubsystem& InSW) : SW(InSW) {}

	void InviteFriend(const FString& WorldId, const FString& FriendPlayerId, const FString& FriendDisplayName, FDone OnDone);
	void AcceptInvite(const FString& InviteId, FDone OnDone);
	void DeclineInvite(const FString& InviteId, FDone OnDone);
	void JoinUsingCode(const FString& Code, FDone OnDone);
	FString GetOrCreateInviteCode(const FString& WorldId);
	void QueuePendingInvite(const FString& WorldId, const FString& WorldName, const FString& FromPlayerId, const FString& FromDisplayName, const sw::ProviderConfig& Provider);

private:
	USharedWorldSubsystem& SW;
};
