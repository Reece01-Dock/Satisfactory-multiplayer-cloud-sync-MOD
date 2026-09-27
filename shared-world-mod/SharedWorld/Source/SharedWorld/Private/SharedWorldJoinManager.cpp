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

void USharedWorldJoinManager::Fail(const FString& Reason)
{
	UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=PlayerJoinFailed reason=\"%s\""), *Reason);
	if (Session)
	{
		Session->OnJoinFailed(TCHAR_TO_UTF8(*Reason));
	}
}

bool USharedWorldJoinManager::Join(UWorld* MenuWorld, sw::WorldSession* InSession, const sw::JoinInfo& JoinInfo, FString& OutError)
{
	Session = InSession;
	const FString Kind = UTF8_TO_TCHAR(JoinInfo.Kind.c_str());
	const FString Value = UTF8_TO_TCHAR(JoinInfo.Data.c_str());
	UGameInstance* GI = Owner ? Owner->GetGameInstance() : nullptr;
	UCommonSessionSubsystem* Sessions = GI ? GI->GetSubsystem<UCommonSessionSubsystem>() : nullptr;
	APlayerController* PC = MenuWorld ? MenuWorld->GetFirstPlayerController() : nullptr;
	if (!Sessions || !PC)
	{
		OutError = TEXT("Joining is only possible from the main menu.");
		return false;
	}
	World = MenuWorld;

	if (Kind == TEXT("address"))
	{
		FSessionJoinParams Params;
		Params.Player = PC;
		Params.RawAddress = Value;
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

	if (Kind != TEXT("online-session-id"))
	{
		OutError = FString::Printf(TEXT("Unsupported join type '%s'. Update the mod."), *Kind);
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
	const UE::Online::FOnlineSessionId SessionId = UCommonSessionSubsystem::MakeOnlineSessionId(Value);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=PlayerJoinStarted kind=online-session-id"));
	TWeakObjectPtr<USharedWorldJoinManager> WeakThis(this);
	Sessions->ResolveOnlineSession(User, SessionId).Next([WeakThis](USessionInformation* Found)
	{
		// The future may complete off the game thread.
		TWeakObjectPtr<USessionInformation> WeakSession(Found);
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

void USharedWorldJoinManager::OnSessionResolved(USessionInformation* Found)
{
	UWorld* W = World.Get();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (!Found || !PC)
	{
		Fail(Found ? TEXT("left the main menu while joining") : TEXT("the host's game session could not be found"));
		return;
	}
	FJoinSessionResponse Response;
	Response.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USharedWorldJoinManager, OnJoinResponse));
	UCommonSessionStatics::JoinSession(PC, Found, Response);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_started kind=online-session-id"));
}

void USharedWorldJoinManager::OnJoinResponse(USessionMigrationSequence* Sequence)
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=join_response sequence=%d"), Sequence ? 1 : 0);
	if (!Sequence)
	{
		Fail(TEXT("the game refused to join the host's session"));
	}
}
