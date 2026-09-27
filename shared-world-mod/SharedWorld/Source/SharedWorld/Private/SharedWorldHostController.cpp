#include "SharedWorldHostController.h"

#include "CommonSessionSubsystem.h"
#include "CommonSessionTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGSaveManagerInterface.h"
#include "FGSaveSystem.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "LocalUserInfo.h"
#include "OnlineIntegrationState.h"
#include "OnlineIntegrationSubsystem.h"
#include "SessionInformation.h"
#include "SessionMigrationSequence.h"
#include "SharedWorldSubsystem.h"

namespace
{
	/** How long to wait for the hosted online session to appear before publishing without join data. */
	constexpr double PublishTimeoutSeconds = 90.0;
	/** Interval between checkpoint uploads while hosting. */
	constexpr float CheckpointIntervalSeconds = 15.0f * 60.0f;
}

void USharedWorldHostController::Init(USharedWorldSubsystem* InOwner)
{
	Owner = InOwner;
}

void USharedWorldHostController::Reset()
{
	FTSTicker::GetCoreTicker().RemoveTicker(PublishTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckpointTicker);
	GameWorld.Reset();
	WorldId.Reset();
	SaveName.Reset();
	bInGameWorld = bSessionPublished = bSaving = bSaveIsFinal = bFinalUploadPending = bFinalUploadInFlight = false;
}

bool USharedWorldHostController::BeginHosting(UWorld* MenuWorld, const FString& InWorldId, const FString& InSaveName, FString& OutError)
{
	if (!MenuWorld)
	{
		OutError = TEXT("Hosting can only start from the main menu.");
		return false;
	}
	UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(MenuWorld);
	APlayerController* PC = MenuWorld->GetFirstPlayerController();
	if (!SaveSystem || !PC)
	{
		OutError = TEXT("The game's save system is not available.");
		return false;
	}
	FSaveHeader Header;
	if (!SaveSystem->LoadSaveGameHeaderSync(InSaveName, Header))
	{
		OutError = FString::Printf(TEXT("Could not read the downloaded save %s."), *InSaveName);
		return false;
	}
	USessionMigrationSequence* Sequence = SaveSystem->LoadSaveFile(Header, FLoadSaveFileParameters(), PC);
	if (!Sequence)
	{
		OutError = TEXT("The game refused to load the shared save.");
		return false;
	}
	// UNVERIFIED (see STATUS.md): whether LoadSaveFile already starts the
	// returned sequence. The sibling APIs document that the caller starts it.
	const bool bStarted = Sequence->Start();
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_load_started world=%s save=%s sequence_start=%d"), *InWorldId, *InSaveName, bStarted ? 1 : 0);
	WorldId = InWorldId;
	SaveName = InSaveName;
	return true;
}

void USharedWorldHostController::OnGameWorldReady(UWorld* World)
{
	if (!IsBusy() || !World)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		// We expected to host but ended up as a client: never upload from here.
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_world_is_client world=%s"), *WorldId);
		Owner->SendSessionEvent(WorldId, TEXT("abort"), nullptr);
		Reset();
		return;
	}
	GameWorld = World;
	bInGameWorld = true;
	PublishDeadline = FPlatformTime::Seconds() + PublishTimeoutSeconds;
	PublishTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldHostController::TickPublishSession), 2.0f);
	CheckpointTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldHostController::TickCheckpoint), CheckpointIntervalSeconds);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_world_ready world=%s"), *WorldId);
}

FString USharedWorldHostController::FindOnlineSessionId() const
{
	UGameInstance* GI = Owner ? Owner->GetGameInstance() : nullptr;
	UOnlineIntegrationSubsystem* Online = GI ? GI->GetSubsystem<UOnlineIntegrationSubsystem>() : nullptr;
	UOnlineIntegrationState* State = Online ? Online->GetOnlineIntegrationState() : nullptr;
	ULocalUserInfo* User = State ? State->GetFirstUserInfo() : nullptr;
	USessionInformation* Session = User ? User->GetGameSession() : nullptr;
	if (!Session)
	{
		return FString();
	}
	const FCommonSession Handle = Session->GetSessionHandle();
	if (!Handle.IsValid())
	{
		return FString();
	}
	return UCommonSessionSubsystem::OnlineSessionIdToString(Handle.GetSessionId());
}

bool USharedWorldHostController::TickPublishSession(float)
{
	if (!bInGameWorld || bSessionPublished)
	{
		return false;
	}
	const FString SessionId = FindOnlineSessionId();
	if (!SessionId.IsEmpty() || FPlatformTime::Seconds() > PublishDeadline)
	{
		PublishSession(SessionId);
		return false;
	}
	return true;
}

void USharedWorldHostController::PublishSession(const FString& SessionId)
{
	bSessionPublished = true;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> Join = MakeShared<FJsonObject>();
		Join->SetStringField(TEXT("kind"), TEXT("online-session-id"));
		Join->SetStringField(TEXT("value"), SessionId);
		Body->SetObjectField(TEXT("join"), Join);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=session_published world=%s"), *WorldId);
	}
	else
	{
		// Friends are told to join via the in-game friends list instead.
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=session_id_unavailable world=%s"), *WorldId);
	}
	Owner->SendSessionEvent(WorldId, TEXT("started"), Body);
}

bool USharedWorldHostController::TickCheckpoint(float)
{
	if (!bInGameWorld)
	{
		return false;
	}
	if (!bSaving)
	{
		SaveAndUpload(false);
	}
	return true;
}

FString USharedWorldHostController::SaveAndUpload(bool bFinal)
{
	if (!bInGameWorld || !GameWorld.IsValid())
	{
		return TEXT("You are not hosting a shared world.");
	}
	if (bSaving)
	{
		return TEXT("A shared save is already in progress.");
	}
	UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(GameWorld.Get());
	if (!SaveSystem)
	{
		return TEXT("The game's save system is not available.");
	}
	bSaving = true;
	bSaveIsFinal = bFinal;
	FOnSaveMgrInterfaceSaveGameComplete Done;
	Done.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USharedWorldHostController, OnSaveComplete));
	SaveSystem->SaveGame(SaveName, Done);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=save_requested world=%s final=%d"), *WorldId, bFinal ? 1 : 0);
	return bFinal ? TEXT("Saving and uploading the shared world. It is released once the upload is verified.")
	              : TEXT("Saving and uploading a checkpoint...");
}

void USharedWorldHostController::OnSaveComplete(bool bSuccess, const FText& ErrorMessage)
{
	bSaving = false;
	if (!bSuccess)
	{
		// Nothing is uploaded: the helper only ever hears about completed saves.
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=save_failed world=%s error=\"%s\""), *WorldId, *ErrorMessage.ToString());
		return;
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=save_completed world=%s final=%d"), *WorldId, bSaveIsFinal ? 1 : 0);
	if (bSaveIsFinal)
	{
		bFinalUploadPending = true;
		SendFinalUpload();
		return;
	}
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("saveName"), SaveName);
	Body->SetBoolField(TEXT("final"), false);
	Owner->SendSessionEvent(WorldId, TEXT("saved"), Body);
}

void USharedWorldHostController::OnWorldTearDown(UWorld* World)
{
	if (!bInGameWorld || World != GameWorld.Get())
	{
		return;
	}
	// The world is going away without an explicit stop (exit to menu / quit).
	// Progress since the last completed save cannot be saved any more; upload
	// the last completed shared save and release the world.
	UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_world_teardown_without_stop world=%s"), *WorldId);
	FTSTicker::GetCoreTicker().RemoveTicker(PublishTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckpointTicker);
	bInGameWorld = false;
	GameWorld.Reset();
	bFinalUploadPending = true;
	SendFinalUpload();
}

void USharedWorldHostController::SendFinalUpload()
{
	if (!bFinalUploadPending || bFinalUploadInFlight)
	{
		return;
	}
	bFinalUploadInFlight = true;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("saveName"), SaveName);
	Body->SetBoolField(TEXT("final"), true);
	TWeakObjectPtr<USharedWorldHostController> WeakThis(this);
	Owner->SendSessionEvent(WorldId, TEXT("saved"), Body, [WeakThis](bool bOk, int32 Code)
	{
		USharedWorldHostController* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		Self->bFinalUploadInFlight = false;
		if (bOk)
		{
			// The helper now owns the upload and the release.
			Self->Reset();
		}
		// 409: a checkpoint upload is still running. The subsystem calls
		// SendFinalUpload again as soon as the session is back in HOSTING.
		// Other failures (helper unreachable) are retried the same way.
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=final_upload_requested ok=%d http=%d"), bOk ? 1 : 0, Code);
	});
}

TArray<FSharedWorldPlayer> USharedWorldHostController::GetPlayers() const
{
	TArray<FSharedWorldPlayer> Out;
	const UWorld* World = GameWorld.Get();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	if (!GS)
	{
		return Out;
	}
	for (const APlayerState* PS : GS->PlayerArray)
	{
		if (PS)
		{
			FSharedWorldPlayer P;
			P.DisplayName = PS->GetPlayerName();
			Out.Add(P);
		}
	}
	return Out;
}
