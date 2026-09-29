#pragma once
// Server → client delivery of Shared World invites so in-session players
// never need a share code. Client replies with GitHub login so the host
// can invite them as a repo collaborator (needed for Play / host takeover).

#include "GameFramework/Actor.h"
#include "SharedWorldInviteBridge.generated.h"

UCLASS()
class SHAREDWORLD_API ASharedWorldInviteBridge : public AActor
{
	GENERATED_BODY()

public:
	ASharedWorldInviteBridge();

	/** Host: push this world's storage config to the connected player with TargetPlayerId. */
	void PushInviteToPlayer(
		const FString& TargetPlayerId,
		const FString& WorldId,
		const FString& WorldName,
		const FString& FromDisplayName,
		const FString& ProviderKind,
		const FString& OwnerOrPath,
		const FString& Repo);

	UFUNCTION(NetMulticast, Reliable)
	void MulticastReceiveInvite(
		const FString& TargetPlayerId,
		const FString& WorldId,
		const FString& WorldName,
		const FString& FromDisplayName,
		const FString& ProviderKind,
		const FString& OwnerOrPath,
		const FString& Repo);

	/** Client → host: GitHub login so the host can grant repo access. */
	UFUNCTION(Server, Reliable)
	void ServerReportGitHubLogin(const FString& WorldId, const FString& GitHubLogin);
};
