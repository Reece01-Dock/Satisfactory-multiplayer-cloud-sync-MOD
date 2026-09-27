#include "SharedWorldHostController.h"

#include "CommonSessionSubsystem.h"
#include "CommonSessionTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGSaveManagerInterface.h"
#include "FGSaveSystem.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/OnlineReplStructs.h"
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

	std::string Std(const FString& S) { return TCHAR_TO_UTF8(*S); }
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
	Session = nullptr;
	bInGameWorld = bSessionPublished = bSaving = bWorldEnded = false;
	LoadStartedAt = 0.0;
}

bool USharedWorldHostController::BeginHosting(UWorld* MenuWorld, sw::WorldSession* InSession, const FString& InWorldId, const FString& SavePath)
{
	if (!MenuWorld || !InSession)
	{
		return false;
	}
	UFGSaveSystem* SaveSystem = UFGSaveSystem::Get(MenuWorld);
	APlayerController* PC = MenuWorld->GetFirstPlayerController();
	if (!SaveSystem || !PC)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"save system unavailable\""), *InWorldId);
		return false;
	}
	const FString LoadName = FPaths::GetBaseFilename(SavePath);
	FSaveHeader Header;
	if (!SaveSystem->LoadSaveGameHeaderSync(LoadName, Header))
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"could not read %s\""), *InWorldId, *SavePath);
		return false;
	}
	USessionMigrationSequence* Sequence = SaveSystem->LoadSaveFile(Header, FLoadSaveFileParameters(), PC);
	if (!Sequence)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"game refused to load the shared save\""), *InWorldId);
		return false;
	}
	// UNVERIFIED (see STATUS.md): whether LoadSaveFile already starts the
	// returned sequence. The sibling APIs document that the caller starts it.
	const bool bStarted = Sequence->Start();
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_load_started world=%s save=%s sequence_start=%d"), *InWorldId, *LoadName, bStarted ? 1 : 0);
	Session = InSession;
	WorldId = InWorldId;
	SaveName = LoadName;
	LoadStartedAt = FPlatformTime::Seconds();
	return true;
}

void USharedWorldHostController::OnGameWorldReady(UWorld* World)
{
	if (!IsBusy() || !World || !Session)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		// We expected to host but ended up as a client: never upload from here.
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_world_is_client world=%s"), *WorldId);
		Session->OnWorldEnded();
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
	USessionInformation* SessionInfo = User ? User->GetGameSession() : nullptr;
	if (!SessionInfo)
	{
		return FString();
	}
	const FCommonSession Handle = SessionInfo->GetSessionHandle();
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
	if (!SessionId.IsEmpty())
	{
		sw::JoinInfo Join;
		Join.Kind = "online-session-id";
		Join.Data = Std(SessionId);
		Session->OnHostingStarted(Join);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=session_published world=%s"), *WorldId);
	}
	else
	{
		// Friends are told to join via the in-game friends list instead.
		Session->OnHostingStarted(std::nullopt);
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=session_id_unavailable world=%s"), *WorldId);
	}
}

bool USharedWorldHostController::TickCheckpoint(float)
{
	if (!bInGameWorld)
	{
		return false;
	}
	if (!bSaving)
	{
		SaveAndUpload(sw::SaveKind::Checkpoint);
	}
	return true;
}

FString USharedWorldHostController::SaveAndUpload(sw::SaveKind Kind)
{
	if (!bInGameWorld || !GameWorld.IsValid() || !Session)
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
	SavingKind = Kind;
	FOnSaveMgrInterfaceSaveGameComplete Done;
	Done.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USharedWorldHostController, OnSaveComplete));
	SaveSystem->SaveGame(SaveName, Done);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=save_requested world=%s kind=%d"), *WorldId, static_cast<int32>(Kind));
	switch (Kind)
	{
	case sw::SaveKind::Checkpoint: return TEXT("Saving and uploading a checkpoint...");
	case sw::SaveKind::Migration: return TEXT("Saving the world for host migration...");
	case sw::SaveKind::Final:
	default: return TEXT("Saving and uploading the shared world. It is released once the upload is verified.");
	}
}

void USharedWorldHostController::OnSaveComplete(bool bSuccess, const FText& ErrorMessage)
{
	bSaving = false;
	if (!bSuccess)
	{
		// Nothing is uploaded: the session only ever hears about completed saves.
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=save_failed world=%s error=\"%s\""), *WorldId, *ErrorMessage.ToString());
		return;
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=save_completed world=%s kind=%d"), *WorldId, static_cast<int32>(SavingKind));
	Session->OnSaveCompleted(SavingKind);
	// Session moves to UPLOADING and, on success, to RELEASING/IDLE for a
	// Final save; the subsystem calls NotifyReleased() once it sees IDLE.
}

void USharedWorldHostController::OnWorldTearDown(UWorld* World)
{
	if (!bInGameWorld || World != GameWorld.Get())
	{
		return;
	}
	// The world is going away without an explicit stop (exit to menu / quit).
	// Progress since the last completed save cannot be saved any more; the
	// session uploads the last completed shared save (read from disk, no
	// game callback needed) and releases. If that upload is refused or the
	// connection drops, the subsystem calls RetryPendingRelease() on later
	// ticks until it lands (WorldId/Session are kept for exactly that).
	UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_world_teardown_without_stop world=%s"), *WorldId);
	FTSTicker::GetCoreTicker().RemoveTicker(PublishTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckpointTicker);
	bInGameWorld = false;
	bWorldEnded = true;
	GameWorld.Reset();
	if (Session)
	{
		Session->OnWorldEnded();
	}
}

void USharedWorldHostController::RetryPendingRelease()
{
	if (!IsBusy() || bInGameWorld || !Session)
	{
		return; // still in-world, or nothing pending, or already released
	}
	Session->OnWorldEnded();
}

std::vector<sw::SessionPlayer> USharedWorldHostController::GetConnectedPlayers() const
{
	std::vector<sw::SessionPlayer> Out;
	const UWorld* World = GameWorld.Get();
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	if (!GS)
	{
		return Out;
	}
	for (const APlayerState* PS : GS->PlayerArray)
	{
		if (!PS)
		{
			continue;
		}
		sw::SessionPlayer P;
		P.DisplayName = Std(PS->GetPlayerName());
		const FUniqueNetIdRepl NetId = PS->GetUniqueId();
		if (NetId.IsValid())
		{
			P.PlayerId = Std(NetId.ToString());
		}
		// InstallId is left empty: a remote client's per-install id is never
		// transmitted to the host (clients never write shared storage). See
		// the WorldSession comment on PendingHandoff matching.
		Out.push_back(std::move(P));
	}
	return Out;
}
