#include "SharedWorldHostController.h"

#include "SharedWorldInviteBridge.h"

#include "CommonSessionSubsystem.h"
#include "CommonSessionTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGSaveManagerInterface.h"
#include "FGSaveSystem.h"
#include "FGGamePhase.h"
#include "FGGamePhaseManager.h"
#include "FGPlayerController.h"
#include "UI/FGGameUI.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "LocalUserInfo.h"
#include "Misc/CoreMisc.h"
#include "Misc/Paths.h"
#include "OnlineIntegrationState.h"
#include "OnlineIntegrationSubsystem.h"
#include "SessionInformation.h"
#include "SessionMigrationSequence.h"
#include "SharedWorldGameShims.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldUeConvert.h"
#include "UnrealClient.h"

using SharedWorldUe::Std;

namespace
{
	/** How long to wait for the hosted online session to appear before publishing without join data. */
	constexpr double PublishTimeoutSeconds = 90.0;
}

void USharedWorldHostController::Init(USharedWorldSubsystem* InOwner)
{
	Owner = InOwner;
}

void USharedWorldHostController::Reset()
{
	FTSTicker::GetCoreTicker().RemoveTicker(PublishTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckpointTicker);
	if (ASharedWorldInviteBridge* Bridge = InviteBridge.Get())
	{
		Bridge->Destroy();
	}
	InviteBridge.Reset();
	if (Owner)
	{
		Owner->NotifyHostResponderCleared();
	}
	GameWorld.Reset();
	WorldId.Reset();
	SaveName.Reset();
	Session = nullptr;
	bInGameWorld = bSessionPublished = bSaving = bWorldEnded = false;
	LoadStartedAt = 0.0;
	LastPauseSaveAt = 0.0;
	TearDownAt = 0.0;
	bPauseMenuWasOpen = false;
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
	// LoadSaveFile addresses saves by name, so the game picks the file. Make sure the
	// file it would pick is the one we just verified and wrote; otherwise a stale
	// SharedWorld_<id>.sav in another save folder could be hosted instead.
	{
		FString GameResolved;
		if (UFGSaveSystem::GetAbsolutePathForSaveGame(MenuWorld, LoadName, GameResolved) && !GameResolved.IsEmpty())
		{
			if (!FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(GameResolved), FPaths::ConvertRelativePathToFull(SavePath)))
			{
				UE_LOG(LogSharedWorld, Error,
					TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"save path mismatch\" written=%s game_resolves=%s"),
					*InWorldId, *SavePath, *GameResolved);
				return false;
			}
		}
		else
		{
			UE_LOG(LogSharedWorld, Warning,
				TEXT("[SharedWorld] event=save_path_unverified world=%s written=%s note=\"game could not resolve the save by name\""), *InWorldId, *SavePath);
		}
	}
	FSaveHeader Header;
	if (!SaveSystem->LoadSaveGameHeaderSync(LoadName, Header))
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"could not read %s\""), *InWorldId, *SavePath);
		return false;
	}
	// Without this, LoadSaveFile travels with SessionDef_SinglePlayer (no listen).
	FString SessionDefError;
	if (!SharedWorldShim::EnsureHostingSessionDefinition(MenuWorld, &SessionDefError))
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"could not select a multiplayer session type: %s\""), *InWorldId, *SessionDefError);
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
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/Host] event=host_load_started world=%s save=%s sequence_start=%d"), *InWorldId, *LoadName, bStarted ? 1 : 0);
	Session = InSession;
	WorldId = InWorldId;
	SaveName = LoadName;
	LoadStartedAt = FPlatformTime::Seconds();
	Session->OnHostLoadStarted();
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
	if (Session)
	{
		Session->OnHostPublishingSession();
	}
	PublishTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldHostController::TickPublishSession), 2.0f);
	CheckpointTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldHostController::TickCheckpoint),
		Owner ? Owner->GetCheckpointIntervalSeconds() : static_cast<float>(sw::LocalSettings::DefaultCheckpointSeconds));
	(void)EnsureInviteBridge();
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/Host] event=host_world_ready world=%s"), *WorldId);
}

ASharedWorldInviteBridge* USharedWorldHostController::EnsureInviteBridge()
{
	if (ASharedWorldInviteBridge* Existing = InviteBridge.Get())
	{
		return Existing;
	}
	UWorld* World = GameWorld.Get();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.Name = TEXT("SharedWorldInviteBridge");
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASharedWorldInviteBridge* Bridge = World->SpawnActor<ASharedWorldInviteBridge>(
		ASharedWorldInviteBridge::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
	InviteBridge = Bridge;
	return Bridge;
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
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/Host] event=session_published world=%s session_id_hash=%u"), *WorldId, GetTypeHash(SessionId));
	}
	else
	{
		Session->OnHostingStarted(std::nullopt);
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld/Host] event=session_id_unavailable world=%s fallback=friends_list"), *WorldId);
	}
	if (Owner && Session)
	{
		const sw::SessionView V = Session->View();
		Owner->NotifyHostResponderReady(WorldId, SessionId, V.Generation, V.Revision, static_cast<int32>(GetConnectedPlayers().size()));
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
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=save_failed world=%s error=\"%s\""), *WorldId, *ErrorMessage.ToString());
		if (bWorldEnded && Session)
		{
			Session->OnWorldEnded();
		}
		return;
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=save_completed world=%s kind=%d"), *WorldId, static_cast<int32>(SavingKind));
	if (Session)
	{
		// Stamp the live game phase onto the next uploaded revision.
		FString PhaseName;
		if (UWorld* W = GameWorld.Get())
		{
			if (AFGGamePhaseManager* Phases = AFGGamePhaseManager::Get(W))
			{
				if (UFGGamePhase* Current = Phases->GetCurrentGamePhase())
				{
					PhaseName = Current->mDisplayName.ToString();
				}
			}
		}
		if (!PhaseName.IsEmpty())
		{
			Session->SetPendingGamePhase(Std(PhaseName));
		}
		// Capture a local preview used by the Shared Worlds details pane.
		{
			const FString ShotPath = FPaths::Combine(
				UFGSaveSystem::GetSaveDirectoryPath(),
				SaveName + TEXT(".png"));
			FScreenshotRequest::RequestScreenshot(ShotPath, /*bInShowUI=*/false, /*bAddFilenameSuffix=*/false);
		}
		Session->OnSaveCompleted(SavingKind);
	}
}

void USharedWorldHostController::OnWorldTearDown(UWorld* World)
{
	if (!bInGameWorld || World != GameWorld.Get())
	{
		return;
	}
	const bool bWasSaving = bSaving;
	UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_world_teardown world=%s engine_exit=%d saving=%d"),
		*WorldId, IsEngineExitRequested() ? 1 : 0, bWasSaving ? 1 : 0);
	FTSTicker::GetCoreTicker().RemoveTicker(PublishTicker);
	FTSTicker::GetCoreTicker().RemoveTicker(CheckpointTicker);

	bInGameWorld = false;
	bWorldEnded = true;
	TearDownAt = FPlatformTime::Seconds();
	GameWorld.Reset();

	if (!Session)
	{
		bSaving = false;
		return;
	}
	const sw::SessionState St = Session->View().State;
	const bool bNeedsRelease =
		St == sw::SessionState::Hosting || St == sw::SessionState::Migrating || St == sw::SessionState::Uploading;

	if (IsEngineExitRequested())
	{
		bSaving = false;
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_abandon_on_process_exit world=%s"), *WorldId);
		Session->AbandonOnProcessExit();
		Reset();
		return;
	}

	// Never start a new SaveGame during teardown — the async callback often never
	// fires and the lease stays open. If a pause/checkpoint SaveGame is already
	// in flight, wait briefly for it (promoted to Final); otherwise upload the
	// last on-disk save and release immediately.
	if (bWasSaving && St == sw::SessionState::Hosting)
	{
		SavingKind = sw::SaveKind::Final;
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_leave_await_inflight_save world=%s"), *WorldId);
		return;
	}
	bSaving = false;
	if (bNeedsRelease)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_leave_upload_last_save world=%s"), *WorldId);
		Session->OnWorldEnded();
	}
}

void USharedWorldHostController::OnPauseMenuOpened()
{
	// Esc/pause usually precedes Exit to Main Menu. Snapshot to disk while the
	// world is still alive so OnWorldTearDown can upload something recent.
	if (!bInGameWorld || bSaving || bWorldEnded || !Session)
	{
		return;
	}
	const sw::SessionState St = Session->View().State;
	if (St != sw::SessionState::Hosting)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now - LastPauseSaveAt < 20.0)
	{
		return;
	}
	LastPauseSaveAt = Now;
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pause_checkpoint_requested world=%s"), *WorldId);
	SaveAndUpload(sw::SaveKind::Checkpoint);
}

void USharedWorldHostController::PollPauseMenu()
{
	if (!bInGameWorld)
	{
		bPauseMenuWasOpen = false;
		return;
	}
	bool bOpen = false;
	if (UWorld* World = GameWorld.Get())
	{
		if (AFGPlayerController* PC = Cast<AFGPlayerController>(World->GetFirstPlayerController()))
		{
			if (UFGGameUI* UI = PC->GetGameUI())
			{
				bOpen = UI->IsPauseMenuOpen();
			}
		}
	}
	if (bOpen && !bPauseMenuWasOpen)
	{
		OnPauseMenuOpened();
	}
	bPauseMenuWasOpen = bOpen;
}

void USharedWorldHostController::RetryPendingRelease()
{
	if (!IsBusy() || bInGameWorld || !Session)
	{
		return; // still in-world, or nothing pending, or already released
	}
	// In-flight SaveGame after leave never completed — don't wait forever.
	if (bSaving && bWorldEnded && TearDownAt > 0.0 && FPlatformTime::Seconds() - TearDownAt > 8.0)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_leave_save_timeout world=%s"), *WorldId);
		bSaving = false;
		Session->OnWorldEnded();
		return;
	}
	if (bSaving)
	{
		return;
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
