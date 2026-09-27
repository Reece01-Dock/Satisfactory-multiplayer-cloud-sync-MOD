#include "SharedWorldJoinManager.h"

#include "Async/Async.h"
#include "CommonSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "LocalUserInfo.h"
#include "OnlineIntegrationState.h"
#include "OnlineIntegrationSubsystem.h"
#include "SessionCreationSettings.h"
#include "SessionInformation.h"
#include "SessionMigrationSequence.h"
#include "SharedWorldSubsystem.h"

void USharedWorldJoinManager::Init(USharedWorldSubsystem* InOwner)
{
	Owner = InOwner;
}

bool USharedWorldJoinManager::Join(UWorld* MenuWorld, const FSharedWorldJoinInfo& JoinInfo, FString& OutError)
{
	UGameInstance* GI = Owner ? Owner->GetGameInstance() : nullptr;
	UCommonSessionSubsystem* Sessions = GI ? GI->GetSubsystem<UCommonSessionSubsystem>() : nullptr;
	APlayerController* PC = MenuWorld ? MenuWorld->GetFirstPlayerController() : nullptr;
	if (!Sessions || !PC)
	{
		OutError = TEXT("Joining is only possible from the main menu.");
		return false;
	}
	World = MenuWorld;

	if (JoinInfo.Kind == TEXT("address"))
	{
		FSessionJoinParams Params;
		Params.Player = PC;
		Params.RawAddress = JoinInfo.Value;
		USessionMigrationSequence* Sequence = Sessions->CreateSessionJoiningSequence(Params);
		if (!Sequence)
		{
			OutError = TEXT("The game could not start joining (another join may be in progress).");
			return false;
		}
		Sequence->Start();
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_started kind=address"));
		return true;
	}

	if (JoinInfo.Kind != TEXT("online-session-id"))
	{
		OutError = FString::Printf(TEXT("Unsupported join type '%s'. Update the mod."), *JoinInfo.Kind);
		return false;
	}
	UOnlineIntegrationSubsystem* Online = GI->GetSubsystem<UOnlineIntegrationSubsystem>();
	UOnlineIntegrationState* State = Online ? Online->GetOnlineIntegrationState() : nullptr;
	ULocalUserInfo* User = State ? State->GetFirstUserInfo() : nullptr;
	if (!User)
	{
		OutError = TEXT("You are not signed in to the game's online services.");
		return false;
	}
	const UE::Online::FOnlineSessionId SessionId = UCommonSessionSubsystem::MakeOnlineSessionId(JoinInfo.Value);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_resolving kind=online-session-id"));
	TWeakObjectPtr<USharedWorldJoinManager> WeakThis(this);
	Sessions->ResolveOnlineSession(User, SessionId).Next([WeakThis](USessionInformation* Session)
	{
		// The future may complete off the game thread.
		TWeakObjectPtr<USessionInformation> WeakSession(Session);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, WeakSession]()
		{
			if (USharedWorldJoinManager* Self = WeakThis.Get())
			{
				Self->OnSessionResolved(WeakSession.Get());
			}
		});
	});
	return true;
}

void USharedWorldJoinManager::OnSessionResolved(USessionInformation* Session)
{
	UWorld* W = World.Get();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (!Session || !PC)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=join_resolve_failed session_found=%d"), Session ? 1 : 0);
		return;
	}
	FJoinSessionResponse Response;
	Response.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USharedWorldJoinManager, OnJoinResponse));
	UCommonSessionStatics::JoinSession(PC, Session, Response);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_started kind=online-session-id"));
}

void USharedWorldJoinManager::OnJoinResponse(USessionMigrationSequence* Sequence)
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_response sequence=%d"), Sequence ? 1 : 0);
}
