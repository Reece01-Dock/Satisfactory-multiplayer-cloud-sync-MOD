#include "SharedWorldInviteBridge.h"

#include "Engine/GameInstance.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"

ASharedWorldInviteBridge::ASharedWorldInviteBridge()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	PrimaryActorTick.bCanEverTick = false;
	bOnlyRelevantToOwner = false;
}

static APlayerController* FindControllerForPlayerId(UWorld* World, const FString& PlayerId)
{
	if (!World || PlayerId.IsEmpty())
	{
		return nullptr;
	}
	const AGameStateBase* GS = World->GetGameState();
	if (!GS)
	{
		return nullptr;
	}
	for (APlayerState* PS : GS->PlayerArray)
	{
		if (!PS)
		{
			continue;
		}
		const FUniqueNetIdRepl NetId = PS->GetUniqueId();
		if (NetId.IsValid() && NetId.ToString().Equals(PlayerId, ESearchCase::IgnoreCase))
		{
			return PS->GetPlayerController();
		}
	}
	return nullptr;
}

void ASharedWorldInviteBridge::PushInviteToPlayer(
	const FString& TargetPlayerId,
	const FString& WorldId,
	const FString& WorldName,
	const FString& FromDisplayName,
	const FString& ProviderKind,
	const FString& OwnerOrPath,
	const FString& Repo)
{
	if (!HasAuthority() || TargetPlayerId.IsEmpty() || WorldId.IsEmpty())
	{
		return;
	}
	// Own the target's connection so they can ServerReportGitHubLogin.
	if (APlayerController* PC = FindControllerForPlayerId(GetWorld(), TargetPlayerId))
	{
		SetOwner(PC);
	}
	MulticastReceiveInvite(TargetPlayerId, WorldId, WorldName, FromDisplayName, ProviderKind, OwnerOrPath, Repo);
}

void ASharedWorldInviteBridge::MulticastReceiveInvite_Implementation(
	const FString& TargetPlayerId,
	const FString& WorldId,
	const FString& WorldName,
	const FString& FromDisplayName,
	const FString& ProviderKind,
	const FString& OwnerOrPath,
	const FString& Repo)
{
	UGameInstance* GI = GetGameInstance();
	USharedWorldSubsystem* SW = GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
	if (!SW)
	{
		return;
	}
	const FString LocalId = SW->GetLocalPlayerId();
	if (LocalId.IsEmpty() || !LocalId.Equals(TargetPlayerId, ESearchCase::IgnoreCase))
	{
		return;
	}
	SW->ReceivePushedWorldInvite(WorldId, WorldName, FromDisplayName, ProviderKind, OwnerOrPath, Repo);

	// Private GitHub worlds need collaborator access for Play. Report our login to the host.
	if (ProviderKind.Equals(TEXT("github"), ESearchCase::IgnoreCase))
	{
		const FString Login = SW->GetGitHubLogin().TrimStartAndEnd();
		if (!Login.IsEmpty())
		{
			ServerReportGitHubLogin(WorldId, Login);
		}
	}
}

void ASharedWorldInviteBridge::ServerReportGitHubLogin_Implementation(const FString& WorldId, const FString& GitHubLogin)
{
	if (!HasAuthority())
	{
		return;
	}
	const FString Login = GitHubLogin.TrimStartAndEnd();
	if (WorldId.IsEmpty() || Login.IsEmpty())
	{
		return;
	}
	UGameInstance* GI = GetGameInstance();
	USharedWorldSubsystem* SW = GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
	if (!SW)
	{
		return;
	}
	SW->GrantHosting(WorldId, Login, [Login, WorldId](bool bOk, const FString& Message)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=auto_granthost world=%s user=%s ok=%d msg=\"%s\""),
			*WorldId, *Login, bOk ? 1 : 0, *Message);
	});
}
