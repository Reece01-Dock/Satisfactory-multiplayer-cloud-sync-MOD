#include "SharedWorldSubsystem.h"

#include <algorithm>

#include "Async/Async.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FGGameMode.h"
#include "FGMainMenuHUD.h"
#include "FGSaveSystem.h"
#include "FGSavePlatform.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ModLoading/PluginModuleLoader.h"
#include "UObject/UObjectIterator.h"
#include "SharedWorldCore/HostElection/HostElection.h"
#include "SharedWorldCore/Providers/GitHub.h"
#include "SharedWorldCore/World/Creation.h"
#include "SharedWorldCore/World/Membership.h"
#include "SharedWorldCredentialStore.h"
#include "SharedWorldGitHubOAuthConfig.h"
#include "SharedWorldHostController.h"
#include "SharedWorldHttpClient.h"
#include "SharedWorldInviteBridge.h"
#include "SharedWorldJoinManager.h"
#include "SharedWorldLogSink.h"
#include "SharedWorldNetworkQuality.h"
#include "SharedWorldHostResponder.h"
#include "SharedWorldUEHostVerifier.h"
#include "SharedWorldPanel.h"
#include "SharedWorldUeConvert.h"
#include "Services/SharedWorldCreationService.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "Services/SharedWorldInviteService.h"
#include "TimerManager.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "UI/FGUserWidget.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "UI/SharedWorldMigrationOverlay.h"
#include "UI/SharedWorldSessionWidget.h"

using SharedWorldUe::Std;
using SharedWorldUe::ToFString;

namespace
{
	constexpr float TickIntervalSeconds = 1.0f;
	constexpr double SummaryRefreshSeconds = 5.0;
	FString ResolveGitHubClientId()
	{
		const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("SHAREDWORLD_GITHUB_CLIENT_ID")).TrimStartAndEnd();
		if (!FromEnv.IsEmpty())
		{
			return FromEnv;
		}
		const std::string Resolved = sw::ResolveGitHubOAuthClientId(SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED, nullptr);
		return UTF8_TO_TCHAR(Resolved.c_str());
	}

	ESharedWorldGitHubAuthState ToUiState(sw::GitHubAuthState S)
	{
		switch (S)
		{
		case sw::GitHubAuthState::Disconnected: return ESharedWorldGitHubAuthState::Disconnected;
		case sw::GitHubAuthState::Starting: return ESharedWorldGitHubAuthState::Starting;
		case sw::GitHubAuthState::WaitingForUser: return ESharedWorldGitHubAuthState::WaitingForUser;
		case sw::GitHubAuthState::Authorizing: return ESharedWorldGitHubAuthState::Authorizing;
		case sw::GitHubAuthState::Connected: return ESharedWorldGitHubAuthState::Connected;
		case sw::GitHubAuthState::Expired: return ESharedWorldGitHubAuthState::Expired;
		case sw::GitHubAuthState::Denied: return ESharedWorldGitHubAuthState::Denied;
		case sw::GitHubAuthState::Error: return ESharedWorldGitHubAuthState::Error;
		}
		return ESharedWorldGitHubAuthState::Error;
	}

	void ApplyAuthSnapshot(FSharedWorldSignIn& Out, const sw::GitHubAuthSnapshot& Snap, bool bConfigured)
	{
		Out.State = ToUiState(Snap.State);
		Out.bInProgress = Snap.State == sw::GitHubAuthState::Starting
			|| Snap.State == sw::GitHubAuthState::WaitingForUser
			|| Snap.State == sw::GitHubAuthState::Authorizing;
		Out.UserCode = UTF8_TO_TCHAR(Snap.UserCode.c_str());
		Out.VerificationUri = UTF8_TO_TCHAR(Snap.VerificationUri.c_str());
		Out.PlayerMessage = UTF8_TO_TCHAR(Snap.PlayerMessage.c_str());
		Out.Login = UTF8_TO_TCHAR(Snap.User.Login.c_str());
		Out.AvatarUrl = UTF8_TO_TCHAR(Snap.User.AvatarUrl.c_str());
		Out.bConfigured = bConfigured;
		Out.bDone = Snap.State == sw::GitHubAuthState::Connected
			|| Snap.State == sw::GitHubAuthState::Expired
			|| Snap.State == sw::GitHubAuthState::Denied
			|| Snap.State == sw::GitHubAuthState::Error
			|| Snap.State == sw::GitHubAuthState::Disconnected;
		Out.Error.Reset();
		if (Snap.State == sw::GitHubAuthState::Error
			|| Snap.State == sw::GitHubAuthState::Expired
			|| Snap.State == sw::GitHubAuthState::Denied)
		{
			Out.Error = Out.PlayerMessage;
		}
	}

	/** Resolve Name.sav under the player's Steam/Epic save folder (or common/). */
	FString ResolveExistingSavePath(UWorld* World, const FString& SaveName)
	{
		if (SaveName.IsEmpty()) return FString();

		FString Absolute;
		if (World && UFGSaveSystem::GetAbsolutePathForSaveGame(World, SaveName, Absolute) && FPaths::FileExists(Absolute))
		{
			return Absolute;
		}

		// Fallback: search SaveGames recursively — accounts for id-subfolder layouts.
		const FString Root = FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath());
		const FString FileName = SaveName.EndsWith(TEXT(".sav")) ? SaveName : (SaveName + TEXT(".sav"));
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Root, *FileName, true, false, false);
		if (Found.Num() > 0)
		{
			// Several folders can hold the same name (id subfolders): pick the most
			// recently written one so the choice never depends on directory order.
			Found.Sort([](const FString& A, const FString& B)
			{
				return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
			});
			return FPaths::ConvertRelativePathToFull(Found[0]);
		}
		return FString();
	}

	/** Directory where new SharedWorld_*.sav files should be written (user id folder). */
	FString ResolveWritableSaveDirectory(UWorld* World)
	{
		FString UserDir;
		if (World && FFGSavePlatform::GetUserSaveDirectoryPath(World, UserDir) && !UserDir.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(UserDir);
		}
		return FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath());
	}

	UCanvasPanel* FindCanvasPanel(UWidget* Widget)
	{
		if (!Widget)
		{
			return nullptr;
		}
		if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Widget))
		{
			return Canvas;
		}
		if (UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
		{
			if (UCanvasPanel* Canvas = FindCanvasPanel(UserWidget->GetRootWidget()))
			{
				return Canvas;
			}
		}
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			const int32 Count = Panel->GetChildrenCount();
			for (int32 i = 0; i < Count; ++i)
			{
				if (UCanvasPanel* Canvas = FindCanvasPanel(Panel->GetChildAt(i)))
				{
					return Canvas;
				}
			}
		}
		return nullptr;
	}

	/** Writes every SharedWorldCore log line to both UE_LOG and an in-memory ring (diagnostics command). */
	class FTeeLogSink final : public sw::ILogSink
	{
	public:
		explicit FTeeLogSink(std::shared_ptr<sw::MemoryLogSink> InMemory) : Memory(std::move(InMemory)) {}
		void Write(sw::LogLevel Level, const std::string& Event, const sw::LogFields& Fields) override
		{
			UeSink.Write(Level, Event, Fields);
			Memory->Write(Level, Event, Fields);
		}

	private:
		FSharedWorldLogSink UeSink;
		std::shared_ptr<sw::MemoryLogSink> Memory;
	};

	/** Milliseconds since epoch, matching sw::TimeMs, without pulling that header in here. */
	int64 NowMs() { return static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000; }
}

void USharedWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Http = std::make_shared<FSharedWorldHttpClient>(ShuttingDown);
	Credentials = std::make_shared<FSharedWorldCredentialStore>();
	DiagnosticsSink = std::make_shared<sw::MemoryLogSink>(500);
	LogSink = std::make_shared<FTeeLogSink>(DiagnosticsSink);
	{
		const std::string ClientIdUtf8 = Std(ResolveGitHubClientId());
		GitHubAuth = std::make_shared<sw::GitHubAuthService>(Http, Credentials, AuthClock, ClientIdUtf8, sw::GitHubCredentialKey, sw::Logger(LogSink));
	}

	const FString Dir = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"));
	SettingsPath = FPaths::Combine(Dir, TEXT("settings.json"));
	auto Loaded = sw::LoadLocalSettings(Std(SettingsPath));
	if (Loaded.Ok())
	{
		Settings = *Loaded;
	}
	else if (Loaded.Is(sw::ErrorCode::Corrupt))
	{
		// Never silently discard: keep the damaged file next to the new one
		// before anything can write an empty list over it.
		const FString Aside = SettingsPath + FString::Printf(TEXT(".damaged-%lld"), NowMs());
		IFileManager::Get().Move(*Aside, *SettingsPath);
		SettingsProblem = TEXT("Your Shared World list was damaged and has been set aside. Add your worlds again.");
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_damaged kept=\"%s\" reason=\"%s\""), *Aside, *ToFString(Loaded.Err().Describe()));
	}
	else
	{
		// Written by a newer mod version, or unreadable: never overwrite it.
		bSettingsReadOnly = true;
		SettingsProblem = ToFString(Loaded.Err().Message);
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_load_failed reason=\"%s\""), *ToFString(Loaded.Err().Describe()));
	}
	Background = MakeUnique<sw::SerialQueue>();
	if (!Settings.GitHubLogin.empty())
	{
		PreferGitHubCloudStorageIfConnected();
	}
	for (const sw::WorldEntry& Entry : Settings.Worlds)
	{
		FindOrCreateRuntime(Entry);
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldSubsystem::Tick), TickIntervalSeconds);
	TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &USharedWorldSubsystem::OnWorldBeginTearDown);
	ActorsInitializedHandle = FWorldDelegates::OnWorldInitializedActors.AddUObject(this, &USharedWorldSubsystem::HandleActorsInitialized);
	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &USharedWorldSubsystem::OnNetworkFailure);
	}

	Host = NewObject<USharedWorldHostController>(this);
	Host->Init(this);
	Joiner = NewObject<USharedWorldJoinManager>(this);
	Joiner->Init(this);
	NetworkQuality = NewObject<USharedWorldNetworkQuality>(this);
	HostResponder = NewObject<USharedWorldHostResponder>(this);
	HostResponder->Init(this);

	DiscoveryService = MakeUnique<FSharedWorldDiscoveryService>(*this);
	InviteService = MakeUnique<FSharedWorldInviteService>(*this);
	CreationService = MakeUnique<FSharedWorldCreationService>(*this);

	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=subsystem_initialized worlds=%d"), static_cast<int32>(Settings.Worlds.size()));

	// Restore GitHub connection from the OS credential store without blocking the game thread.
	{
		std::shared_ptr<sw::GitHubAuthService> Auth = GitHubAuth;
		TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
		Async(EAsyncExecution::Thread, [WeakThis, Auth]()
		{
			(void)Auth->RestoreSession();
			const sw::GitHubAuthSnapshot Snap = Auth->Snapshot();
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Snap]()
			{
				USharedWorldSubsystem* S = WeakThis.Get();
				if (!S || S->ShuttingDown->load(std::memory_order_acquire)) return;
				bool bConnected = false;
				{
					FScopeLock Lock(&S->SignInMutex);
					ApplyAuthSnapshot(S->SignIn, Snap, S->GitHubAuth && S->GitHubAuth->IsConfigured());
					if (Snap.State == sw::GitHubAuthState::Connected && !Snap.User.Login.empty())
					{
						S->Settings.GitHubLogin = Snap.User.Login;
						S->SaveSettings();
						bConnected = true;
					}
					else if (Snap.State == sw::GitHubAuthState::Disconnected && !S->Settings.GitHubLogin.empty() && !Snap.bHasToken)
					{
						S->Settings.GitHubLogin.clear();
						S->SaveSettings();
					}
				}
				if (bConnected)
				{
					S->PreferGitHubCloudStorageIfConnected();
				}
				UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] event=restore_complete connected=%s user=%s token_present=%s"),
					Snap.State == sw::GitHubAuthState::Connected ? TEXT("yes") : TEXT("no"),
					UTF8_TO_TCHAR(Snap.User.Login.c_str()),
					Snap.bHasToken ? TEXT("yes") : TEXT("no"));
				S->OnChanged.Broadcast();
			});
		});
	}
}

void USharedWorldSubsystem::Deinitialize()
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=subsystem_deinitialize_begin"));

	// Signal first so HttpClient::Send abandons waits without CancelRequest.
	// Joining workers that CancelRequest into a tearing-down FHttpModule can
	// hard-lock the machine; soft-abandon + join is the safe exit path.
	ShuttingDown->store(true, std::memory_order_release);
	SignInCancel->store(true, std::memory_order_release);
	if (GitHubAuth)
	{
		GitHubAuth->CancelLink();
	}

	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	TickHandle = FTSTicker::FDelegateHandle();
	FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);
	TearDownHandle.Reset();
	FWorldDelegates::OnWorldInitializedActors.Remove(ActorsInitializedHandle);
	ActorsInitializedHandle.Reset();
	if (GEngine && NetworkFailureHandle.IsValid())
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		NetworkFailureHandle.Reset();
	}

	if (Host)
	{
		Host->Reset();
	}
	if (HostResponder)
	{
		HostResponder->Clear();
	}

	DiscoveryService.Reset();
	InviteService.Reset();
	CreationService.Reset();

	if (Background)
	{
		Background->Shutdown();
		Background.Reset();
	}

	// Joins session worker threads. HTTP Send soft-abandons when ShuttingDown
	// is set, so this must not CancelRequest into FHttpModule.
	Runtimes.Empty();

	HideMigrationOverlay();
	MenuPanel.Reset();
	MenuWorld.Reset();

	Joiner = nullptr;
	Host = nullptr;
	NetworkQuality = nullptr;
	HostResponder = nullptr;

	// Drop auth/http last — abandoned requests keep themselves alive via the
	// completion lambda until FHttpModule finishes or the process exits.
	GitHubAuth.reset();
	Http.reset();
	Credentials.reset();
	LogSink.reset();
	DiagnosticsSink.reset();

	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=subsystem_deinitialize_end"));
	Super::Deinitialize();
}

sw::ProviderEnvironment USharedWorldSubsystem::MakeEnvironment() const
{
	sw::ProviderEnvironment Env;
	Env.Http = Http;
	Env.Credentials = Credentials;
	return Env;
}

sw::Identity USharedWorldSubsystem::MyIdentity() const
{
	sw::Identity Id;
	Id.Platform = "unknown";
	if (const ULocalPlayer* LP = GetGameInstance() ? GetGameInstance()->GetFirstGamePlayer() : nullptr)
	{
		const FUniqueNetIdRepl NetId = LP->GetPreferredUniqueNetId();
		if (NetId.IsValid())
		{
			Id.PlayerId = Std(NetId.ToString());
			Id.Platform = Std(NetId.GetType().ToString());
		}
		Id.DisplayName = Std(LP->GetNickname());
	}
	if (Id.PlayerId.empty())
	{
		// Not signed in to an online service: a stable per-machine id so the
		// player can still be recognised across sessions (host takeover rank,
		// membership) even though it will not match across their own PCs.
		Id.PlayerId = "local:" + Std(FPlatformMisc::GetLoginId());
	}
	if (Id.DisplayName.empty())
	{
		Id.DisplayName = Std(FPlatformProcess::UserName());
	}
	// Stable per install of the mod's data directory, not per game process:
	// lets a crashed-and-relaunched game on the same PC recognise its own
	// still-valid lease (see LocalWorldState::ActiveLease).
	const FString InstallIdPath = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("install-id"));
	FString InstallId;
	if (!FFileHelper::LoadFileToString(InstallId, *InstallIdPath))
	{
		InstallId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		FFileHelper::SaveStringToFile(InstallId, *InstallIdPath);
	}
	Id.InstallId = Std(InstallId.TrimStartAndEnd());
	return Id;
}

namespace
{
	bool ProvidersMatch(const sw::ProviderConfig& A, const sw::ProviderConfig& B)
	{
		if (A.Kind != B.Kind)
		{
			return false;
		}
		if (A.Kind == sw::ProviderKind::GitHub)
		{
			return A.Owner == B.Owner && A.Repo == B.Repo;
		}
		return A.FolderPath == B.FolderPath;
	}
}

bool USharedWorldSubsystem::RebindRuntimeStorage(FSharedWorldRuntime& Runtime)
{
	const FString WorldId = ToFString(Runtime.Entry.WorldId);
	if (Runtime.Session)
	{
		const sw::SessionState St = Runtime.Session->View().State;
		if (St != sw::SessionState::Idle && St != sw::SessionState::Error)
		{
			UE_LOG(LogSharedWorld, Warning,
				TEXT("[SharedWorld] event=storage_rebind_deferred world=%s session=%s"),
				*WorldId, UTF8_TO_TCHAR(sw::ToString(St)));
			return false;
		}
		Runtime.Session.Reset();
	}
	auto Storage = sw::OpenWorldStorage(Runtime.Entry, MakeEnvironment());
	if (!Storage)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=open_storage_failed world=%s reason=\"%s\""), *WorldId, *ToFString(Storage.Err().Describe()));
		return false;
	}
	Runtime.Repository = Storage->Repository;
	Runtime.Objects = Storage->Objects;
	auto Clock = std::make_shared<sw::SystemClock>();
	auto Store = std::make_shared<sw::WorldStore>(Runtime.Repository, Runtime.Entry.WorldId, Clock, sw::Logger(LogSink));
	Runtime.Leases = std::make_shared<sw::LeaseManager>(Store, sw::LeaseConfig{});
	Runtime.HostMigration = MakeUnique<sw::HostMigrationEngine>(MyIdentity(), Clock, Runtime.Leases);
	const FString DataDir = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("worlds"), WorldId);
	Runtime.Sync = std::make_shared<sw::SyncEngine>(Runtime.Objects, Runtime.Leases, sw::SyncConfig{Std(DataDir), 20});
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=storage_rebound world=%s backend=%s"),
		*WorldId, UTF8_TO_TCHAR(Runtime.Repository->Describe().c_str()));
	return true;
}

FSharedWorldRuntime* USharedWorldSubsystem::FindOrCreateRuntime(const sw::WorldEntry& Entry)
{
	const FString WorldId = ToFString(Entry.WorldId);
	if (TUniquePtr<FSharedWorldRuntime>* Existing = Runtimes.Find(WorldId))
	{
		FSharedWorldRuntime* Runtime = Existing->Get();
		if (!ProvidersMatch(Runtime->Entry.Provider, Entry.Provider))
		{
			Runtime->Entry = Entry;
			(void)RebindRuntimeStorage(*Runtime);
		}
		return Runtime;
	}
	auto Runtime = MakeUnique<FSharedWorldRuntime>();
	Runtime->Entry = Entry;
	if (!RebindRuntimeStorage(*Runtime))
	{
		return nullptr;
	}
	FSharedWorldRuntime* Ptr = Runtime.Get();
	Runtimes.Add(WorldId, MoveTemp(Runtime));
	return Ptr;
}

FSharedWorldRuntime* USharedWorldSubsystem::FindRuntime(const FString& WorldId)
{
	TUniquePtr<FSharedWorldRuntime>* Found = Runtimes.Find(WorldId);
	return Found ? Found->Get() : nullptr;
}

void USharedWorldSubsystem::SaveSettings()
{
	if (bSettingsReadOnly)
	{
		return; // see Initialize: the file on disk must not be replaced
	}
	if (sw::Status R = sw::SaveLocalSettings(Std(SettingsPath), Settings); !R)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_save_failed reason=\"%s\""), *ToFString(R.Err().Describe()));
	}
}

sw::WorldSession& USharedWorldSubsystem::EnsureSession(FSharedWorldRuntime& Runtime)
{
	if (!Runtime.Session)
	{
		sw::SessionConfig Cfg;
		Cfg.Me = MyIdentity();
		Cfg.SaveDirectory = Std(ResolveWritableSaveDirectory(GetWorld()));
		Cfg.SaveName = "SharedWorld_" + Runtime.Entry.WorldId;
		Cfg.Versions = MyVersions();
		Cfg.HostVerifyProbeTimeout = sw::Seconds(5);
		Cfg.HostVerifyTimeout = sw::Seconds(45);
		auto Verifier = std::make_shared<SharedWorldUe::FSharedWorldUEHostVerifier>(
			GetGameInstance(), HostResponder, NetworkQuality, ToFString(Cfg.Me.PlayerId));
		Cfg.HostVerifier = Verifier;
		Runtime.Session = MakeUnique<sw::WorldSession>(Runtime.Leases, Runtime.Sync, Cfg);
	}
	// Keep expected host id for the next Play decide path — updated when summarising / joining.
	return *Runtime.Session;
}

void USharedWorldSubsystem::Play(const FString& WorldId)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || Runtime->bCreating)
	{
		return;
	}
	ActiveWorldId = WorldId;
	EnsureSession(*Runtime).Play();
	Runtime->Entry.LastPlayedAt = NowMs();
	(void)Settings.Upsert(Runtime->Entry); // same world id: replaces the stored entry
	SaveSettings();
}

FString USharedWorldSubsystem::GetMostRecentlyPlayedWorldId() const
{
	sw::TimeMs Best = 0;
	FString BestId;
	for (const sw::WorldEntry& E : Settings.Worlds)
	{
		if (E.LastPlayedAt > Best)
		{
			Best = E.LastPlayedAt;
			BestId = ToFString(E.WorldId);
		}
	}
	return BestId;
}

bool USharedWorldSubsystem::TryContinueLastSharedWorld()
{
	const FString Id = GetMostRecentlyPlayedWorldId();
	if (Id.IsEmpty()) return false;
	Play(Id);
	return true;
}

bool USharedWorldSubsystem::IsSharedWorldSaveName(const FString& SaveName)
{
	return SaveName.StartsWith(TEXT("SharedWorld_"));
}

FString USharedWorldSubsystem::WorldIdFromSaveName(const FString& SaveName)
{
	if (!IsSharedWorldSaveName(SaveName)) return FString();
	return SaveName.RightChop(12); // strlen("SharedWorld_")
}

void USharedWorldSubsystem::Dismiss(const FString& WorldId)
{
	if (FSharedWorldRuntime* Runtime = FindRuntime(WorldId); Runtime && Runtime->Session)
	{
		Runtime->Session->Dismiss();
	}
}

void USharedWorldSubsystem::Cancel(const FString& WorldId)
{
	if (FSharedWorldRuntime* Runtime = FindRuntime(WorldId); Runtime && Runtime->Session)
	{
		Runtime->Session->Cancel();
	}
}

void USharedWorldSubsystem::Restore(const FString& WorldId, int64 Revision)
{
	// Creates revision N+1 with revision `Revision`'s content (history is
	// never rewritten); needs the world to be free and the RestoreRevision role.
	if (FSharedWorldRuntime* Runtime = FindRuntime(WorldId); Runtime && !Runtime->bCreating && Revision > 0)
	{
		EnsureSession(*Runtime).Restore(Revision);
	}
}

FString USharedWorldSubsystem::RequestMigrationTo(const FString& WorldId, const FString& Who)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || !Runtime->Session || !Host->IsHostingWorld(WorldId))
	{
		return TEXT("You are not hosting this Shared World.");
	}
	const sw::Identity Me = MyIdentity();
	sw::Identity Successor;
	std::vector<sw::SuccessorCandidate> Candidates;
	for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
	{
		if (P.PlayerId == Me.PlayerId) continue;
		sw::SuccessorCandidate C;
		C.Who.PlayerId = P.PlayerId;
		C.Who.DisplayName = P.DisplayName;
		C.Who.InstallId = P.InstallId;
		C.bConnected = true;
		C.bCompatible = true;
		C.bStorageReachable = true;
		C.bHasHeadCached = true;
		C.PingMs = 50;
#if !UE_BUILD_SHIPPING
		if (Runtime->DevInjectedLatencyMs > 0) C.PingMs += Runtime->DevInjectedLatencyMs;
#endif
		Candidates.push_back(C);
		if (!Who.IsEmpty() && Who != TEXT("auto") &&
			(ToFString(P.PlayerId) == Who || ToFString(P.DisplayName).Equals(Who, ESearchCase::IgnoreCase)))
		{
			Successor = C.Who;
		}
	}
	if (Successor.PlayerId.empty())
	{
		if (auto Picked = sw::SelectSuccessor(Candidates, {}, Me))
		{
			Successor = *Picked;
		}
	}
	if (Successor.PlayerId.empty())
	{
		return Who.IsEmpty() || Who == TEXT("auto")
			? FString(TEXT("No eligible successor is connected."))
			: FString::Printf(TEXT("No other connected player called %s."), *Who);
	}
	if (Runtime->HostMigration)
	{
		std::vector<sw::HostCandidate> RankedIn;
		sw::PeerQualityMatrix Matrix;
		const FString LocalId = ToFString(Me.PlayerId);
		UWorld* GameW = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		if (NetworkQuality && GameW)
		{
			NetworkQuality->SampleWorld(GameW, Matrix, NowMs(), LocalId);
			Runtime->HostMigration->SetMatrix(Matrix);
		}
		std::vector<std::string> Online;
		Online.push_back(Me.PlayerId);
		for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
		{
			if (P.PlayerId.empty() || P.PlayerId == Me.PlayerId) continue;
			Online.push_back(P.PlayerId);
		}
		for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
		{
			if (P.PlayerId == Me.PlayerId) continue;
			sw::Identity Peer;
			Peer.PlayerId = P.PlayerId;
			Peer.DisplayName = P.DisplayName;
			Peer.InstallId = P.InstallId;
			sw::HostCandidate H = sw::MakeCandidate(Peer, Matrix, Online, true, true, true, true, true, true, false);
#if !UE_BUILD_SHIPPING
			if (Runtime->DevInjectedLatencyMs > 0) H.PingMs += Runtime->DevInjectedLatencyMs;
#endif
			RankedIn.push_back(H);
		}
		Runtime->HostMigration->SetCurrentHost(Me, Runtime->Session->View().Generation, Runtime->Session->View().Revision);
		Runtime->HostMigration->UpdateCandidates(RankedIn);
	}
	Runtime->Session->RequestMigration(Successor);
	return FString::Printf(TEXT("Handing the world to %s. Everyone reconnects once it is saved (they need storage access to host)."), *ToFString(Successor.DisplayName));
}

FString USharedWorldSubsystem::GetHostMigrationDiagnostics(const FString& WorldId)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || !Runtime->HostMigration)
	{
		return TEXT("No Shared World runtime.");
	}
	// Refresh ranking from currently connected players before formatting.
	if (Host && Host->IsHostingWorld(WorldId))
	{
		const sw::Identity Me = MyIdentity();
		std::vector<sw::HostCandidate> RankedIn;
		sw::PeerQualityMatrix Matrix;
		UWorld* GameW = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		if (NetworkQuality && GameW)
		{
			NetworkQuality->SampleWorld(GameW, Matrix, NowMs(), ToFString(Me.PlayerId));
			Runtime->HostMigration->SetMatrix(Matrix);
		}
		std::vector<std::string> Online{Me.PlayerId};
		for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
		{
			if (!P.PlayerId.empty() && P.PlayerId != Me.PlayerId) Online.push_back(P.PlayerId);
		}
		for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
		{
			if (P.PlayerId == Me.PlayerId) continue;
			sw::Identity Who;
			Who.PlayerId = P.PlayerId;
			Who.DisplayName = P.DisplayName;
			Who.InstallId = P.InstallId;
			sw::HostCandidate H = sw::MakeCandidate(Who, Matrix, Online, true, true, true, true);
#if !UE_BUILD_SHIPPING
			H.bStorageReachable = !Runtime->bDevStorageDisabled;
			H.PingMs += Runtime->DevInjectedLatencyMs;
			if (Runtime->DevInjectedLossBp > 0)
			{
				sw::HostNetworkSummary N = H.Network ? *H.Network : sw::NetworkFromPing(H.PingMs);
				N.AvgLossBp = Runtime->DevInjectedLossBp;
				N.WorstLossBp = Runtime->DevInjectedLossBp;
				H.Network = N;
			}
#endif
			RankedIn.push_back(H);
		}
		Runtime->HostMigration->SetCurrentHost(Me,
			Runtime->Session ? Runtime->Session->View().Generation : 0,
			Runtime->Session ? Runtime->Session->View().Revision : 0);
		Runtime->HostMigration->UpdateCandidates(RankedIn);
	}
	return ToFString(sw::FormatDiagnosticsText(Runtime->HostMigration->Diagnostics()));
}

#if !UE_BUILD_SHIPPING
FString USharedWorldSubsystem::DevInject(const FString& WorldId, const FString& Action)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime) return TEXT("Unknown world.");
	const FString A = Action.ToLower();
	if (A == TEXT("leave") || A == TEXT("migrate"))
	{
		return RequestMigrationTo(WorldId, TEXT("auto"));
	}
	if (A == TEXT("crash"))
	{
		if (Runtime->Session) Runtime->Session->OnHostConnectionLost();
		return TEXT("Simulated host connection loss.");
	}
	if (A == TEXT("latency"))
	{
		Runtime->DevInjectedLatencyMs = 100;
		return TEXT("Injected +100ms latency into successor scoring.");
	}
	if (A == TEXT("loss"))
	{
		Runtime->DevInjectedLossBp = 500;
		return TEXT("Injected 5% packet loss into successor scoring.");
	}
	if (A == TEXT("nostorage"))
	{
		Runtime->bDevStorageDisabled = true;
		return TEXT("Marked storage disabled for local diagnostics.");
	}
	if (A == TEXT("clear"))
	{
		Runtime->DevInjectedLatencyMs = 0;
		Runtime->DevInjectedLossBp = 0;
		Runtime->bDevStorageDisabled = false;
		return TEXT("Cleared dev inject state.");
	}
	return TEXT("Unknown inject action. Use leave|crash|latency|loss|nostorage|clear.");
}
#endif


// ---------------------------------------------------------------- background work

void USharedWorldSubsystem::RunInBackground(TFunction<TPair<bool, FString>()> Work, TFunction<void(USharedWorldSubsystem&, bool, const FString&)> Then)
{
	if (ShuttingDown->load(std::memory_order_acquire) || !Background)
	{
		return;
	}
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	std::shared_ptr<std::atomic<bool>> Stop = ShuttingDown;
	// Work must only capture shared_ptrs and values, never `this`.
	Background->Post([WeakThis, Work, Then, Stop]()
	{
		if (Stop->load(std::memory_order_acquire))
		{
			return;
		}
		const TPair<bool, FString> Result = Work();
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Then, Result, Stop]()
		{
			if (Stop->load(std::memory_order_acquire))
			{
				return;
			}
			if (USharedWorldSubsystem* Self = WeakThis.Get())
			{
				Then(*Self, Result.Key, Result.Value);
			}
		});
	});
}

void USharedWorldSubsystem::RefreshCloudCache(const FSharedWorldRuntime& Runtime) const
{
	constexpr double MinRefreshSeconds = 2.0;
	if (!Runtime.Leases || !Runtime.CloudCache || !Background)
	{
		return;
	}
	std::shared_ptr<FSharedWorldCloudCache> Cache = Runtime.CloudCache;
	if (!Cache->TryBeginRefresh(FPlatformTime::Seconds(), MinRefreshSeconds))
	{
		return;
	}
	// Capture shared_ptrs only: the runtime may be destroyed before the read returns.
	std::shared_ptr<sw::LeaseManager> Leases = Runtime.Leases;
	Background->Post([Cache, Leases]()
	{
		auto Snap = Leases->Store().Load();
		Cache->Set(Snap ? std::optional<sw::StateSnapshot>(*Snap) : std::nullopt);
		Cache->EndRefresh();
	});
}

sw::LocalVersions USharedWorldSubsystem::MyVersions() const
{
	sw::LocalVersions V;
	// UNVERIFIED (STATUS.md): that the engine changelist is the build number
	// Satisfactory writes into save headers. Compatibility checks only
	// refuse when the cloud save names a NEWER build than this one.
	V.GameBuild = Std(FString::FromInt(FEngineVersion::Current().GetChangelist()));
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SharedWorld")))
	{
		V.ModVersion = Std(Plugin->GetDescriptor().VersionName);
	}
	// InstalledMods stays empty until the SML mod list API is verified; with
	// an empty list new worlds record no required mods (see STATUS.md).
	return V;
}

namespace
{
	/** "Our Factory!" -> "our-factory-3f9a1c": valid sw world id, unique per creation. */
	FString MakeWorldId(const FString& Name)
	{
		FString Slug;
		for (TCHAR C : Name.ToLower())
		{
			if ((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
			{
				Slug.AppendChar(C);
			}
			else if (!Slug.IsEmpty() && !Slug.EndsWith(TEXT("-")))
			{
				Slug.AppendChar('-');
			}
		}
		Slug = Slug.Left(40);
		Slug.RemoveFromEnd(TEXT("-"));
		if (Slug.IsEmpty())
		{
			Slug = TEXT("world");
		}
		return Slug + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6).ToLower();
	}

	FString MakeInviteCode(const FString& WorldId)
	{
		uint32 H = GetTypeHash(WorldId);
		for (TCHAR C : WorldId) H = HashCombine(H, GetTypeHash(C));
		const uint32 A = (H >> 16) & 0xFFFF;
		const uint32 B = H & 0xFFFF;
		static const TCHAR* Alphabet = TEXT("ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
		auto Enc = [&](uint32 V) -> FString
		{
			FString S;
			for (int32 i = 0; i < 4; ++i)
			{
				S.AppendChar(Alphabet[V % 32]);
				V /= 32;
			}
			return S.Reverse();
		};
		return Enc(A) + TEXT("-") + Enc(B);
	}

	TPair<bool, FString> Fail(const sw::Error& E) { return {false, ToFString(E.Message)}; }
}

void USharedWorldSubsystem::AddExistingWorld(const FString& WorldId, const FString& DisplayName, const sw::ProviderConfig& Provider, FDone OnDone)
{
	sw::WorldEntry Entry;
	Entry.WorldId = Std(WorldId.ToLower().TrimStartAndEnd());
	Entry.DisplayName = Std(DisplayName.IsEmpty() ? WorldId : DisplayName);
	Entry.Provider = Provider;
	Entry.AddedAt = NowMs();
	Entry.Relation = sw::WorldRelation::Shared;
	if (sw::Status V = sw::ValidateWorldId(Entry.WorldId); !V)
	{
		OnDone(false, ToFString(V.Err().Message));
		return;
	}
	if (Entry.WorldId.size() > 52)
	{
		OnDone(false, TEXT("World ids longer than 52 characters are not supported.")); // save name SharedWorld_<id> must fit 64
		return;
	}
	if (sw::Status V = Provider.Validate(); !V)
	{
		OnDone(false, ToFString(V.Err().Message));
		return;
	}
	if (Settings.Find(Entry.WorldId) || FindRuntime(ToFString(Entry.WorldId)))
	{
		OnDone(false, TEXT("That world is already in your list."));
		return;
	}
	FSharedWorldRuntime* Runtime = FindOrCreateRuntime(Entry);
	if (!Runtime)
	{
		OnDone(false, TEXT("That storage could not be opened."));
		return;
	}
	Runtime->bCreating = true;
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	RunInBackground([Leases]() -> TPair<bool, FString>
	{
		const sw::WorldSummary Sum = sw::Summarize(*Leases);
		if (Sum.Status == sw::WorldStatus::NotCreated)
		{
			return {false, TEXT("No Shared World with that id exists in that storage.")};
		}
		if (Sum.Status == sw::WorldStatus::Unreachable)
		{
			return {false, TEXT("The storage could not be reached: ") + ToFString(Sum.Problem)};
		}
		return {true, FString::Printf(TEXT("Added %s."), *ToFString(Sum.Name))};
	},
	[Entry, OnDone](USharedWorldSubsystem& Self, bool bOk, const FString& Message)
	{
		const FString Id = ToFString(Entry.WorldId);
		if (!bOk)
		{
			Self.Runtimes.Remove(Id);
		}
		else if (FSharedWorldRuntime* R = Self.FindRuntime(Id))
		{
			R->bCreating = false;
			(void)Self.Settings.Upsert(Entry);
			Self.SaveSettings();
		}
		Self.OnChanged.Broadcast();
		OnDone(bOk, Message);
	});
}

void USharedWorldSubsystem::CreateWorldFromSave(const FString& DisplayName, const FString& SaveName, const sw::ProviderConfig& Provider, bool bRestrictToMembers, FDone OnDone)
{
	const FString Name = DisplayName.TrimStartAndEnd();
	if (Name.IsEmpty() || Name.Len() > 64)
	{
		OnDone(false, TEXT("Give the world a name (up to 64 characters)."));
		return;
	}
	if (sw::Status V = Provider.Validate(); !V)
	{
		OnDone(false, ToFString(V.Err().Message));
		return;
	}
	// Only a plain save name is accepted: never a path from the player.
	if (SaveName.IsEmpty() || FPaths::GetCleanFilename(SaveName) != SaveName || SaveName.Contains(TEXT("..")))
	{
		OnDone(false, TEXT("Pick one of your saves."));
		return;
	}

	// Saves live under SaveGames/<SteamOrEpicId>/Name.sav — never flatten into SaveGames/Name.sav.
	const FString SourcePath = ResolveExistingSavePath(GetWorld(), SaveName);
	if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=create_save_missing name=\"%s\""), *SaveName);
		OnDone(false, TEXT("That save was not found. Pick a save from the list again."));
		return;
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=create_save_resolved name=\"%s\" path=\"%s\""), *SaveName, *SourcePath);

	sw::WorldEntry Entry;
	Entry.WorldId = Std(MakeWorldId(Name));
	Entry.DisplayName = Std(Name);
	Entry.Provider = Provider;
	Entry.AddedAt = NowMs();
	Entry.Relation = sw::WorldRelation::Owned;
	Entry.InviteCode = Std(MakeInviteCode(ToFString(Entry.WorldId)));
	FSharedWorldRuntime* Runtime = FindOrCreateRuntime(Entry);
	if (!Runtime)
	{
		OnDone(false, TEXT("That storage could not be opened."));
		return;
	}
	Runtime->bCreating = true;

	sw::CreateWorldParams Params;
	Params.Name = Std(Name);
	Params.Creator = MyIdentity();
	Params.SourceSavePath = Std(SourcePath);
	Params.OriginalSaveName = Std(SaveName);
	Params.Versions = MyVersions();
	Params.Settings.Name = Std(Name);
	Params.bRestrictToMembers = bRestrictToMembers;
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	std::shared_ptr<sw::SyncEngine> Sync = Runtime->Sync;
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=WorldCreateStarted world=%s save=%s"), *ToFString(Entry.WorldId), *SaveName);
	RunInBackground([Leases, Sync, Params]() -> TPair<bool, FString>
	{
		auto Created = sw::CreateSharedWorld(*Leases, *Sync, Params);
		if (!Created)
		{
			return Fail(Created.Err());
		}
		return {true, FString::Printf(TEXT("Created Shared World \"%s\". Your original save was not changed."), *ToFString(Created->Name))};
	},
	[Entry, OnDone](USharedWorldSubsystem& Self, bool bOk, const FString& Message)
	{
		const FString Id = ToFString(Entry.WorldId);
		if (!bOk)
		{
			// Nothing is deleted from storage: CreateSharedWorld is retry-safe,
			// and a half-created world is harmless (it has no revision yet).
			Self.Runtimes.Remove(Id);
		}
		else if (FSharedWorldRuntime* R = Self.FindRuntime(Id))
		{
			R->bCreating = false;
			(void)Self.Settings.Upsert(Entry);
			Self.SaveSettings();
		}
		Self.OnChanged.Broadcast();
		OnDone(bOk, Message);
	});
}

FString USharedWorldSubsystem::ForgetWorld(const FString& WorldId)
{
	const std::string IdUtf8 = Std(WorldId);
	if (!Settings.Find(IdUtf8))
	{
		return TEXT("Unknown world.");
	}
	if (FSharedWorldRuntime* Runtime = FindRuntime(WorldId))
	{
		if (Runtime->bCreating)
		{
			return TEXT("That world is busy.");
		}
		if (Host && Host->IsHostingWorld(WorldId))
		{
			return TEXT("Leave the world first.");
		}
		if (Runtime->Session)
		{
			const sw::SessionState St = Runtime->Session->View().State;
			if (St != sw::SessionState::Idle && St != sw::SessionState::Error)
			{
				return TEXT("Leave the world first.");
			}
		}
		Runtimes.Remove(WorldId);
	}
	// Only this PC's list changes: the world, its storage and local backups stay.
	Settings.Remove(IdUtf8);
	SaveSettings();
	if (ActiveWorldId == WorldId)
	{
		ActiveWorldId.Reset();
	}
	LastLocalStates.Remove(WorldId);
	LastSequences.Remove(WorldId);
	OnChanged.Broadcast();
	return FString();
}

// ---------------------------------------------------------------- sign-in

bool USharedWorldSubsystem::IsGitHubAuthConfigured() const
{
	return GitHubAuth && GitHubAuth->IsConfigured();
}

void USharedWorldSubsystem::BeginGitHubSignIn()
{
	if (!GitHubAuth)
	{
		return;
	}
	{
		FScopeLock Lock(&SignInMutex);
		if (SignIn.bInProgress)
		{
			return;
		}
		SignIn = FSharedWorldSignIn{};
		SignIn.bConfigured = GitHubAuth->IsConfigured();
		if (!SignIn.bConfigured)
		{
			SignIn.State = ESharedWorldGitHubAuthState::Error;
			SignIn.Error = TEXT("GitHub integration is not configured in this build.");
			SignIn.PlayerMessage = SignIn.Error;
			SignIn.bDone = true;
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld/GitHub] event=signin_unavailable reason=client_id_missing"));
			OnChanged.Broadcast();
			return;
		}
		SignIn.bInProgress = true;
		SignIn.State = ESharedWorldGitHubAuthState::Starting;
		SignIn.PlayerMessage = TEXT("Opening GitHub...");
	}
	SignInCancel->store(false);
	std::shared_ptr<sw::GitHubAuthService> Auth = GitHubAuth;
	std::shared_ptr<std::atomic<bool>> Stop = ShuttingDown;
	std::shared_ptr<std::atomic<bool>> Cancel = SignInCancel;
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);

	auto PushUi = [WeakThis, Stop]()
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Stop]()
		{
			USharedWorldSubsystem* S = WeakThis.Get();
			if (!S || !S->GitHubAuth || Stop->load(std::memory_order_acquire)) return;
			const sw::GitHubAuthSnapshot Snap = S->GitHubAuth->Snapshot();
			bool bConnected = false;
			{
				FScopeLock Lock(&S->SignInMutex);
				ApplyAuthSnapshot(S->SignIn, Snap, S->GitHubAuth->IsConfigured());
				if (Snap.State == sw::GitHubAuthState::Connected && !Snap.User.Login.empty())
				{
					S->Settings.GitHubLogin = Snap.User.Login;
					S->SaveSettings();
					bConnected = true;
				}
			}
			if (bConnected)
			{
				S->PreferGitHubCloudStorageIfConnected();
			}
			S->OnChanged.Broadcast();
		});
	};

	Async(EAsyncExecution::Thread, [WeakThis, Auth, Stop, Cancel, PushUi]()
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] Device authorization started"));
		if (sw::Status Begin = Auth->BeginLink(); !Begin)
		{
			PushUi();
			return;
		}
		PushUi();

		const sw::GitHubAuthSnapshot Ready = Auth->Snapshot();
		if (!Ready.VerificationUri.empty())
		{
			const FString Uri = UTF8_TO_TCHAR(Ready.VerificationUri.c_str());
			AsyncTask(ENamedThreads::GameThread, [Uri]()
			{
				FPlatformProcess::LaunchURL(*Uri, nullptr, nullptr);
				UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] event=browser_launch attempted=yes"));
			});
		}

		int SleepSec = FMath::Max(Ready.IntervalSeconds, 1);
		for (;;)
		{
			FPlatformProcess::Sleep(static_cast<float>(SleepSec));
			if (Stop->load() || Cancel->load())
			{
				Auth->CancelLink();
				PushUi();
				return;
			}
			auto Poll = Auth->PollOnce();
			if (!Poll)
			{
				PushUi();
				return;
			}
			SleepSec = FMath::Max(Poll->IntervalSeconds, 1);
			PushUi();
			switch (Poll->State)
			{
			case sw::DevicePoll::Pending:
			case sw::DevicePoll::SlowDown:
				continue;
			case sw::DevicePoll::Denied:
			case sw::DevicePoll::Expired:
				return;
			case sw::DevicePoll::Authorized:
				break;
			}
			std::string Token = std::move(Poll->AccessToken);
			Poll->AccessToken.clear();
			if (sw::Status Fin = Auth->FinalizeAuthorized(std::move(Token)); !Fin)
			{
				PushUi();
				return;
			}
			const sw::GitHubAuthSnapshot Done = Auth->Snapshot();
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] Authentication succeeded user=%s"), UTF8_TO_TCHAR(Done.User.Login.c_str()));
			PushUi();
			return;
		}
	});
}

void USharedWorldSubsystem::CancelGitHubSignIn()
{
	SignInCancel->store(true);
	if (GitHubAuth)
	{
		GitHubAuth->CancelLink();
	}
	{
		FScopeLock Lock(&SignInMutex);
		SignIn.bInProgress = false;
		SignIn.State = ESharedWorldGitHubAuthState::Disconnected;
		SignIn.UserCode.Reset();
		SignIn.VerificationUri.Reset();
		SignIn.PlayerMessage = TEXT("GitHub linking cancelled.");
		SignIn.Error.Reset();
		SignIn.bDone = true;
	}
	OnChanged.Broadcast();
}

void USharedWorldSubsystem::TestGitHubAccess(FDone OnDone)
{
	if (!GitHubAuth)
	{
		if (OnDone) OnDone(false, TEXT("GitHub auth is unavailable."));
		return;
	}
	std::shared_ptr<sw::GitHubAuthService> Auth = GitHubAuth;
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Auth, OnDone = MoveTemp(OnDone)]() mutable
	{
		auto R = Auth->TestAccess();
		AsyncTask(ENamedThreads::GameThread, [WeakThis, R, OnDone = MoveTemp(OnDone)]()
		{
			USharedWorldSubsystem* S = WeakThis.Get();
			if (S && S->ShuttingDown->load(std::memory_order_acquire))
			{
				return;
			}
			if (S && S->GitHubAuth)
			{
				const sw::GitHubAuthSnapshot Snap = S->GitHubAuth->Snapshot();
				FScopeLock Lock(&S->SignInMutex);
				ApplyAuthSnapshot(S->SignIn, Snap, S->GitHubAuth->IsConfigured());
				if (R && !R->Login.empty())
				{
					S->Settings.GitHubLogin = R->Login;
					S->SaveSettings();
				}
				S->OnChanged.Broadcast();
			}
			if (OnDone)
			{
				if (R)
				{
					OnDone(true, FString::Printf(TEXT("GitHub OK as %s"), UTF8_TO_TCHAR(R->Login.c_str())));
				}
				else
				{
					OnDone(false, UTF8_TO_TCHAR(sw::GitHubAuthPlayerMessage(sw::GitHubAuthState::Error, sw::GitHubAuthService::ClassifyError(R.Err())).c_str()));
				}
			}
		});
	});
}

FSharedWorldSignIn USharedWorldSubsystem::GetSignInStatus() const
{
	FScopeLock Lock(&SignInMutex);
	FSharedWorldSignIn Copy = SignIn;
	if (GitHubAuth)
	{
		const sw::GitHubAuthSnapshot Snap = GitHubAuth->Snapshot();
		// Prefer live service state when a link is in progress.
		if (Snap.State == sw::GitHubAuthState::Starting
			|| Snap.State == sw::GitHubAuthState::WaitingForUser
			|| Snap.State == sw::GitHubAuthState::Authorizing
			|| Snap.State == sw::GitHubAuthState::Connected)
		{
			ApplyAuthSnapshot(Copy, Snap, GitHubAuth->IsConfigured());
			if (Copy.Login.IsEmpty() && !Settings.GitHubLogin.empty())
			{
				Copy.Login = UTF8_TO_TCHAR(Settings.GitHubLogin.c_str());
			}
		}
		else if (!GitHubAuth->IsConfigured())
		{
			Copy.bConfigured = false;
			if (Copy.PlayerMessage.IsEmpty())
			{
				Copy.PlayerMessage = TEXT("GitHub integration is not configured in this build.");
			}
		}
	}
	return Copy;
}

void USharedWorldSubsystem::SignOutOfGitHub()
{
	SignInCancel->store(true);
	if (GitHubAuth)
	{
		(void)GitHubAuth->Disconnect();
	}
	else if (Credentials)
	{
		(void)Credentials->Remove(sw::GitHubCredentialKey);
	}
	Settings.GitHubLogin.clear();
	SaveSettings();
	{
		FScopeLock Lock(&SignInMutex);
		SignIn = FSharedWorldSignIn{};
		SignIn.PlayerMessage = TEXT("Not connected");
		SignIn.bConfigured = GitHubAuth && GitHubAuth->IsConfigured();
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] event=disconnected token_present=no"));
	OnChanged.Broadcast();
}

// ---------------------------------------------------------------- ticking

bool USharedWorldSubsystem::Tick(float)
{
	if (ShuttingDown->load(std::memory_order_acquire))
	{
		return false;
	}
	const int64 Now = static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000;
	for (auto& [Id, Runtime] : Runtimes)
	{
		if (Runtime->Session)
		{
			Runtime->Session->Tick(Now);
			HandleSessionTransition(*Runtime);
		}
		if (Runtime->HostMigration)
		{
			if (Host && Host->IsHostingWorld(Id) && NetworkQuality)
			{
				if (UWorld* GameW = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
				{
					sw::PeerQualityMatrix Matrix;
					NetworkQuality->SampleWorld(GameW, Matrix, Now, ToFString(MyIdentity().PlayerId));
					Runtime->HostMigration->SetMatrix(Matrix);
				}
			}
			(void)Runtime->HostMigration->Tick(Now);
		}
	}
	const double Wall = FPlatformTime::Seconds();
	if (Host)
	{
		Host->PollPauseMenu();
	}
	if (Wall - LastPauseInjectAttempt >= 1.0)
	{
		LastPauseInjectAttempt = Wall;
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UWorld* World = GI->GetWorld())
			{
				if (!FPluginModuleLoader::IsMainMenuWorld(World))
				{
					for (TObjectIterator<USharedWorldGameInstanceModule> It; It; ++It)
					{
						if (It->GetGameInstance() == GI)
						{
							It->TryEnsureMenuEntries(World);
							break;
						}
					}
				}
			}
		}
	}
	if (!ActiveWorldId.IsEmpty())
	{
		UpdateMigrationOverlay(ActiveWorldId);
	}
	if (!bSummaryRefreshInFlight && Wall - LastSummaryRefresh >= SummaryRefreshSeconds)
	{
		LastSummaryRefresh = Wall;
		RefreshSummariesAsync();
	}
	return true;
}

void USharedWorldSubsystem::RefreshSummariesAsync()
{
	bSummaryRefreshInFlight = true;
	TArray<TPair<FString, std::shared_ptr<sw::LeaseManager>>> Targets;
	for (const auto& [Id, Runtime] : Runtimes)
	{
		Targets.Add({Id, Runtime->Leases});
	}
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Targets]()
	{
		TMap<FString, sw::WorldSummary> Results;
		for (const auto& [Id, Leases] : Targets)
		{
			Results.Add(Id, sw::Summarize(*Leases));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Results]()
		{
			USharedWorldSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			{
				FScopeLock Lock(&Self->SummaryMutex);
				for (const auto& [Id, Summary] : Results)
				{
					if (FSharedWorldRuntime* Runtime = Self->FindRuntime(Id))
					{
						Runtime->LastSummary = Summary;
					}
				}
			}
			Self->bSummaryRefreshInFlight = false;
			Self->OnChanged.Broadcast();
		});
	});
}

namespace
{
	void FillWorldViewFromRuntime(FSharedWorldRuntime& Runtime, FSharedWorldEntryView& V)
	{
		V.WorldId = ToFString(Runtime.Entry.WorldId);
		const sw::WorldSummary& Sum = Runtime.LastSummary;
		V.WorldName = Sum.Name.empty() ? ToFString(Runtime.Entry.DisplayName) : ToFString(Sum.Name);
		V.CloudStatus = ToFString(sw::ToString(Sum.Status));
		V.HostName = ToFString(Sum.HostName);
		V.PlayerCount = Sum.Players.size();
		V.Revision = Sum.Revision;
		V.Generation = Sum.Generation;
		V.Problem = ToFString(Sum.Problem);
		V.LastHostName = ToFString(Sum.LastHostName);
		if (Sum.LastPlayedAt > 0)
		{
			const FTimespan Ago = FDateTime::UtcNow() - FDateTime::FromUnixTimestamp(Sum.LastPlayedAt / 1000);
			V.LastPlayed = Ago.GetTotalMinutes() < 1.0 ? FString(TEXT("just now"))
				: Ago.GetTotalHours() < 1.0 ? FString::Printf(TEXT("%d minutes ago"), FMath::FloorToInt(Ago.GetTotalMinutes()))
				: Ago.GetTotalDays() < 1.0 ? FString::Printf(TEXT("%d hours ago"), FMath::FloorToInt(Ago.GetTotalHours()))
				: FString::Printf(TEXT("%d days ago"), FMath::FloorToInt(Ago.GetTotalDays()));
		}
		V.bCreating = Runtime.bCreating;
		V.bOwned = Runtime.Entry.Relation != sw::WorldRelation::Shared;
		V.InviteCode = ToFString(Runtime.Entry.InviteCode);
		if (Runtime.bCreating)
		{
			V.LocalMessage = TEXT("Setting up...");
		}
		if (Runtime.Session)
		{
			const sw::SessionView SV = Runtime.Session->View();
			V.LocalState = ToFString(sw::ToString(SV.State));
			V.LocalMessage = ToFString(SV.Message);
			for (const std::string& Step : SV.Steps)
			{
				V.Steps.Add(ToFString(Step));
			}
			V.bJoinReady = SV.State == sw::SessionState::JoinReady;
			if (SV.Error)
			{
				V.bHasError = true;
				V.ErrorCode = ToFString(SV.Error->Code);
				V.ErrorMessage = ToFString(SV.Error->Message);
				V.ErrorDetail = ToFString(SV.Error->Detail);
				V.bErrorRetryable = SV.Error->bRetryable;
				V.BackupPath = ToFString(SV.Error->BackupPath);
			}
		}
	}
}

TArray<FSharedWorldEntryView> USharedWorldSubsystem::GetWorldViews()
{
	TArray<FSharedWorldEntryView> Out;
	FScopeLock Lock(&SummaryMutex);
	for (const sw::WorldEntry& Entry : Settings.Worlds)
	{
		FSharedWorldEntryView V;
		const FString Id = ToFString(Entry.WorldId);
		if (TUniquePtr<FSharedWorldRuntime>* Found = Runtimes.Find(Id))
		{
			FillWorldViewFromRuntime(*Found->Get(), V);
		}
		else
		{
			V.WorldId = Id;
			V.WorldName = ToFString(Entry.DisplayName);
			V.bOwned = Entry.Relation != sw::WorldRelation::Shared;
			V.InviteCode = ToFString(Entry.InviteCode);
		}
		Out.Add(MoveTemp(V));
	}
	for (const auto& [Id, Runtime] : Runtimes)
	{
		if (Settings.Find(Std(Id)))
		{
			continue;
		}
		FSharedWorldEntryView V;
		FillWorldViewFromRuntime(*Runtime.Get(), V);
		Out.Add(MoveTemp(V));
	}
	return Out;
}

void USharedWorldSubsystem::HandleSessionTransition(FSharedWorldRuntime& Runtime)
{
	using S = sw::SessionState;
	const sw::SessionView View = Runtime.Session->View();
	const FString WorldId = ToFString(Runtime.Entry.WorldId);
	const FString StateStr = ToFString(sw::ToString(View.State));
	const FString Prev = LastLocalStates.FindRef(WorldId);
	LastLocalStates.Add(WorldId, StateStr);
	if (LastSequences.FindRef(WorldId) != View.Sequence)
	{
		LastSequences.Add(WorldId, View.Sequence);
		OnChanged.Broadcast(); // the panel follows every step, not just the 5 s cloud refresh
	}

	if (View.State == S::ReadyToHost && Host->GetWorldId() == WorldId && Host->LoadTimedOut())
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_timeout world=%s"), *WorldId);
		Host->NotifyReleased();
		Runtime.Session->OnWorldEnded(); // releases the lease; the local save is untouched
	}
	else if (View.State == S::ReadyToHost && !Host->IsBusy())
	{
		if (!MenuWorld.IsValid())
		{
			// E.g. the migration successor / takeover winner is still in the
			// old host's world: go to the main menu first, then load. The
			// session keeps the lease alive meanwhile.
			ReturnToMainMenu(NSLOCTEXT("SharedWorld", "BecomingHost", "You are the new host. Loading the Shared World..."));
		}
		else
		{
			ActiveWorldId = WorldId;
			if (!Host->BeginHosting(MenuWorld.Get(), Runtime.Session.Get(), WorldId, ToFString(View.SavePath)))
			{
				UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s"), *WorldId);
				Runtime.Session->OnWorldEnded();
			}
		}
	}
	else if (View.State == S::Hosting)
	{
		if (PendingRejoinWorldId == WorldId)
		{
			PendingRejoinWorldId.Reset(); // the migration upload failed: we stay host
		}
		if (Host->IsHostingWorld(WorldId))
		{
			Runtime.Session->SetPlayers(Host->GetConnectedPlayers());
		}
		else if (Host->GetWorldId() == WorldId)
		{
			// The game world already ended but the session is still trying to
			// upload/release (or an in-flight leave-save timed out).
			Host->RetryPendingRelease();
		}
	}
	else if (View.State == S::Migrating && Host->IsHostingWorld(WorldId) && !Host->IsSaving() && PendingRejoinWorldId != WorldId)
	{
		PendingRejoinWorldId = WorldId; // once released, leave and join the successor
		Host->SaveAndUpload(sw::SaveKind::Migration);
	}
	else if (View.State == S::Idle && Host->GetWorldId() == WorldId && Host->IsBusy())
	{
		const bool bStillInWorld = Host->IsHostingWorld(WorldId);
		Host->NotifyReleased();
		if (PendingRejoinWorldId == WorldId && bStillInWorld)
		{
			// Handed over: close this game (clients reconnect to the successor)
			// and rejoin as a client from the menu (OnMenuWorldReady).
			ReturnToMainMenu(NSLOCTEXT("SharedWorld", "HandedOver", "The Shared World was handed to the new host. Reconnecting..."));
		}
		else
		{
			PendingRejoinWorldId.Reset();
		}
	}
	else if (View.State == S::LeaseLost && Prev != StateStr && Host->IsHostingWorld(WorldId))
	{
		// Another host took over (our lease expired or was fenced): this game
		// must not keep running as the world. Progress is kept as a backup.
		Host->NotifyReleased();
		ReturnToMainMenu(NSLOCTEXT("SharedWorld", "LeaseLost", "Another player is now hosting this Shared World. Your progress was kept as a backup."));
	}
	else if (View.State == S::JoinReady && View.Join)
	{
		// Joining starts from the main menu only. A client still in the old
		// host's world (migration) waits here until that game closes.
		if (!JoinsInFlight.Contains(WorldId) && MenuWorld.IsValid())
		{
			JoinsInFlight.Add(WorldId);
			ActiveWorldId = WorldId;
			FString Error;
			if (!Joiner->Join(MenuWorld.Get(), Runtime.Session.Get(), *View.Join, Error))
			{
				UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=join_failed world=%s reason=\"%s\""), *WorldId, *Error);
				Runtime.Session->OnJoinFailed(Std(Error));
			}
		}
	}
	if (View.State != S::JoinReady)
	{
		JoinsInFlight.Remove(WorldId); // a later JOIN_READY (retry, new host) starts a fresh join
	}
	if (Prev != StateStr)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=session_state world=%s from=%s to=%s"), *WorldId, *Prev, *StateStr);
	}
}

// ---------------------------------------------------------------- lifecycle

void USharedWorldSubsystem::HandleActorsInitialized(const UWorld::FActorsInitializedParams& Params)
{
	UWorld* World = Params.World;
	if (!World || !FPluginModuleLoader::IsMainMenuWorld(World))
	{
		return;
	}
	// Main-menu HUD widget is created slightly after ActorsInitialized; retry a few times.
	FTimerManager& Timers = World->GetTimerManager();
	Timers.SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &USharedWorldSubsystem::RetryShowMenuPanel));
	World->OnWorldBeginPlay.AddUObject(this, &USharedWorldSubsystem::RetryShowMenuPanel);
	FTimerHandle Delay05;
	Timers.SetTimer(Delay05, FTimerDelegate::CreateUObject(this, &USharedWorldSubsystem::RetryShowMenuPanel), 0.5f, false);
	FTimerHandle Delay15;
	Timers.SetTimer(Delay15, FTimerDelegate::CreateUObject(this, &USharedWorldSubsystem::RetryShowMenuPanel), 1.5f, false);
	FTimerHandle Delay30;
	Timers.SetTimer(Delay30, FTimerDelegate::CreateUObject(this, &USharedWorldSubsystem::RetryShowMenuPanel), 3.0f, false);
}

void USharedWorldSubsystem::RetryShowMenuPanel()
{
	UGameInstance* GI = GetGameInstance();
	UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (!World)
	{
		World = MenuWorld.Get();
	}
	if (World && FPluginModuleLoader::IsMainMenuWorld(World))
	{
		OnMenuWorldReady(World);
	}
}

bool USharedWorldSubsystem::TryShowMenuPanel(UWorld* World)
{
	// Native menu entry via hook/inject (or local bake). No corner overlay.
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return false;
	}
	AFGMainMenuHUD* HUD = Cast<AFGMainMenuHUD>(PC->GetHUD());
	UFGUserWidget* MainMenu = HUD ? HUD->mMainMenu.Get() : nullptr;
	if (!MainMenu)
	{
		return false;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		for (TObjectIterator<USharedWorldGameInstanceModule> It; It; ++It)
		{
			if (It->GetGameInstance() == GI)
			{
				It->TryEnsureMenuEntries(World);
				break;
			}
		}
	}
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_ready path=native_entry"));
	return true;
}

void USharedWorldSubsystem::PushMigrationStatusToMenus(const FText& Message)
{
	UGameInstance* GI = GetGameInstance();
	UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (!World) return;
	TArray<UUserWidget*> Existing;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Existing, USharedWorldSessionWidget::StaticClass(), false);
	for (UUserWidget* W : Existing)
	{
		if (USharedWorldSessionWidget* Screen = Cast<USharedWorldSessionWidget>(W))
		{
			Screen->SetStatusMessage(Message);
		}
	}
}

void USharedWorldSubsystem::EnsureMigrationOverlay(UWorld* World)
{
	// Overlays removed — status goes to Manage Session → Shared World only.
	(void)World;
	if (USharedWorldMigrationOverlay* Overlay = MigrationOverlay.Get())
	{
		Overlay->RemoveFromParent();
		MigrationOverlay.Reset();
	}
}

namespace
{
	struct FMigrationProgressInfo
	{
		float Percent = 0.f;
		FText Step;
		bool bIndeterminate = false;
	};

	/** True when cloud lease says the host is handing off / a successor is reserved. */
	bool ObservePlannedHostLeave(FSharedWorldRuntime& Runtime, FString* OutSuccessorName = nullptr)
	{
		// Cached cloud state (refilled off-thread by RefreshCloudCache): this runs on
		// the game thread every overlay update and must never do network I/O.
		const std::optional<sw::StateSnapshot> Snap = Runtime.CloudCache ? Runtime.CloudCache->Get() : std::nullopt;
		if (!Snap) return false;
		const sw::WorldState& St = Snap->State;
		if (St.PendingHandoff)
		{
			if (OutSuccessorName) *OutSuccessorName = ToFString(St.PendingHandoff->Successor.DisplayName);
			return true;
		}
		if (St.CurrentLease && St.CurrentLease->Phase == sw::LeasePhase::Migrating)
		{
			if (OutSuccessorName) *OutSuccessorName = ToFString(St.CurrentLease->Holder.DisplayName);
			return true;
		}
		return false;
	}

	/** Map session + migration phase (+ lease wait) to a MW2-style progress fill. */
	FMigrationProgressInfo ComputeMigrationProgress(FSharedWorldRuntime& Runtime, const sw::SessionView& View)
	{
		using S = sw::SessionState;
		FMigrationProgressInfo Out;

		const sw::MigrationPhase Phase = Runtime.HostMigration
			? Runtime.HostMigration->Diagnostics().Phase
			: sw::MigrationPhase::Running;

		auto Set = [&](float P, const TCHAR* Label)
		{
			Out.Percent = P;
			Out.Step = FText::FromString(Label);
		};

		// Planned migration phases from HostMigrationEngine.
		switch (Phase)
		{
		case sw::MigrationPhase::MigrationPreparing: Set(0.12f, TEXT("Preparing successor")); return Out;
		case sw::MigrationPhase::FinalSave: Set(0.22f, TEXT("Saving world")); return Out;
		case sw::MigrationPhase::RevisionSync: Set(0.38f, TEXT("Uploading world revision")); return Out;
		case sw::MigrationPhase::SuccessorReady:
		case sw::MigrationPhase::LeaseHandoff: Set(0.52f, TEXT("Handing off host lock")); return Out;
		case sw::MigrationPhase::SuccessorStarting: Set(0.72f, TEXT("New host starting session")); return Out;
		case sw::MigrationPhase::SessionPublished: Set(0.88f, TEXT("Session published")); return Out;
		case sw::MigrationPhase::ClientReconnect: Set(0.94f, TEXT("Reconnecting clients")); return Out;
		case sw::MigrationPhase::LeaseExpired: Set(0.48f, TEXT("Former host lock expired")); return Out;
		case sw::MigrationPhase::SuccessorElection: Set(0.55f, TEXT("Selecting next host")); return Out;
		case sw::MigrationPhase::RevisionRecovery: Set(0.62f, TEXT("Loading Shared World for takeover")); return Out;
		case sw::MigrationPhase::HostLost:
		case sw::MigrationPhase::RecoveryWait:
			break; // fall through to lease-wait / session-state logic
		default:
			break;
		}

		// Host intentional leave (session-driven planned migration).
		if (View.State == S::Migrating)
		{
			if (View.Successor)
			{
				Out.Percent = 0.18f;
				Out.Step = FText::Format(
					NSLOCTEXT("SharedWorld", "MigSavingFor", "Saving world for {0}"),
					FText::FromString(ToFString(View.Successor->DisplayName)));
			}
			else
			{
				Set(0.18f, TEXT("Saving world for host handoff"));
			}
			return Out;
		}
		if ((View.State == S::Uploading || View.State == S::Releasing) && View.Successor)
		{
			if (View.State == S::Uploading)
			{
				Out.Percent = 0.40f;
				Out.Step = NSLOCTEXT("SharedWorld", "MigUploadHandoff", "Uploading Shared World for handoff…");
				Out.bIndeterminate = true;
			}
			else
			{
				Out.Percent = 0.70f;
				Out.Step = FText::Format(
					NSLOCTEXT("SharedWorld", "MigReleaseTo", "Handing host lock to {0}"),
					FText::FromString(ToFString(View.Successor->DisplayName)));
			}
			return Out;
		}

		// Client still in the old session while the host is leaving.
		if (View.State == S::Joined)
		{
			FString Who;
			if (ObservePlannedHostLeave(Runtime, &Who) || View.Message.find("HOST MIGRATION") != std::string::npos)
			{
				Set(0.28f, TEXT("Host is handing off — stay here"));
				return Out;
			}
		}
		if (View.State == S::WaitingForHost || View.State == S::WaitingForSession)
		{
			FString Who;
			ObservePlannedHostLeave(Runtime, &Who);
			if (!Who.IsEmpty())
			{
				Out.Percent = 0.78f;
				Out.Step = FText::Format(
					NSLOCTEXT("SharedWorld", "MigWaitNewHost", "Waiting for {0} to start hosting"),
					FText::FromString(Who));
			}
			else
			{
				Set(0.78f, TEXT("Waiting for the new host"));
			}
			return Out;
		}
		if (View.State == S::JoinReady || View.State == S::Joining)
		{
			Set(0.92f, TEXT("Reconnecting to the new host"));
			return Out;
		}

		// Crash recovery: advance the bar while waiting for the dead host's lease.
		if (View.State == S::Reconnecting || View.State == S::RecoveringHost || View.State == S::JoinRetry ||
			View.State == S::HostUnreachable || Phase == sw::MigrationPhase::HostLost ||
			Phase == sw::MigrationPhase::RecoveryWait)
		{
			float WaitFrac = 0.f;
			int32 RemainSec = 0;
			FString HandoffName;
			if (ObservePlannedHostLeave(Runtime, &HandoffName) && !HandoffName.IsEmpty())
			{
				// Intentional leave with a reserved successor — not a dead-lease wait.
				Out.Percent = 0.72f;
				Out.Step = FText::Format(
					NSLOCTEXT("SharedWorld", "MigWaitSuccessor", "{0} is becoming the new host"),
					FText::FromString(HandoffName));
				return Out;
			}
			if (Runtime.Leases)
			{
				const std::optional<sw::StateSnapshot> Snap = Runtime.CloudCache ? Runtime.CloudCache->Get() : std::nullopt;
				if (Snap && Snap->State.CurrentLease)
				{
					const sw::Lease& L = *Snap->State.CurrentLease;
					const sw::TimeMs Now = Runtime.Leases->Store().Clock().Now();
					const sw::TimeMs ObserverExpiry = L.ExpiresAt + Runtime.Leases->Config().SkewGrace;
					const sw::TimeMs Span = FMath::Max<sw::TimeMs>(1, ObserverExpiry - L.RenewedAt);
					const sw::TimeMs Elapsed = FMath::Clamp<sw::TimeMs>(Now - L.RenewedAt, 0, Span);
					WaitFrac = static_cast<float>(Elapsed) / static_cast<float>(Span);
					RemainSec = static_cast<int32>(FMath::Max<sw::TimeMs>(0, ObserverExpiry - Now) / 1000);
				}
			}
			const float Pct = FMath::Lerp(0.08f, 0.45f, FMath::Clamp(WaitFrac, 0.f, 1.f));
			if (RemainSec > 0)
			{
				Out.Percent = Pct;
				Out.Step = FText::Format(
					NSLOCTEXT("SharedWorld", "MigWaitLease", "Waiting for former host lock · {0}s"),
					FText::AsNumber(RemainSec));
			}
			else
			{
				Set(Pct, TEXT("Selecting a new host"));
			}
			return Out;
		}

		switch (View.State)
		{
		case S::ElectingHost: Set(0.55f, TEXT("Selecting next host")); break;
		case S::Acquiring: Set(0.60f, TEXT("Claiming host lock")); break;
		case S::Recovering: Set(0.66f, TEXT("Recovering Shared World")); break;
		case S::Downloading: Set(0.74f, TEXT("Downloading latest revision")); break;
		case S::ReadyToHost: Set(0.82f, TEXT("Ready to host — loading save")); break;
		case S::StartingSession: Set(0.90f, TEXT("Starting multiplayer session")); break;
		case S::PublishingSession: Set(0.95f, TEXT("Publishing session for friends")); break;
		case S::Uploading:
			Out.Percent = 0.40f;
			Out.Step = NSLOCTEXT("SharedWorld", "UploadStepShort", "Uploading Shared World…");
			Out.bIndeterminate = true;
			break;
		case S::Releasing: Set(0.92f, TEXT("Releasing host lock")); break;
		default: Set(0.15f, TEXT("Host migration in progress")); break;
		}
		return Out;
	}
}

void USharedWorldSubsystem::UpdateMigrationOverlay(const FString& WorldId)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || !Runtime->Session)
	{
		HideMigrationOverlay();
		return;
	}
	RefreshCloudCache(*Runtime);
	const sw::SessionView View = Runtime->Session->View();
	using S = sw::SessionState;

	const bool bPlannedHandoff = View.Successor.has_value() || View.State == S::Migrating;
	FString ObservedSuccessor;
	const bool bClientSeesLeave = (View.State == S::Joined || View.State == S::WaitingForHost ||
		View.State == S::WaitingForSession || View.State == S::Reconnecting) &&
		(ObservePlannedHostLeave(*Runtime, &ObservedSuccessor) ||
			View.Message.find("HOST MIGRATION") != std::string::npos ||
			View.Message.find("Host migration") != std::string::npos);

	auto Publish = [this](const FString& Headline, const FString& Detail)
	{
		const FString Key = Headline + TEXT("\n") + Detail;
		if (Key == LastOverlayMessage) return;
		LastOverlayMessage = Key;
		PushMigrationStatusToMenus(FText::FromString(Key.Replace(TEXT("\n"), TEXT(" — "))));
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=migration_status headline=\"%s\" detail=\"%s\""),
			*Headline, *Detail);
	};

	// Cloud upload / release — migration handoff uses HOST MIGRATION branding.
	if (View.State == S::Uploading || View.State == S::Releasing)
	{
		if (bPlannedHandoff)
		{
			const FString Succ = View.Successor ? ToFString(View.Successor->DisplayName) : TEXT("the next host");
			const FString Headline = TEXT("HOST MIGRATION");
			const FString Detail = View.State == S::Uploading
				? FString::Printf(TEXT("Uploading the Shared World, then handing off to %s…"), *Succ)
				: FString::Printf(TEXT("Handing the host lock to %s…"), *Succ);
			Publish(Headline, Detail);
			return;
		}

		const FString Headline = View.State == S::Uploading
			? TEXT("UPLOADING SHARED WORLD")
			: TEXT("RELEASING SHARED WORLD");
		Publish(Headline, ToFString(View.Message));
		return;
	}

	FString Message;
	if (Runtime->HostMigration)
	{
		const sw::MigrationDiagnostics& Diag = Runtime->HostMigration->Diagnostics();
		Message = ToFString(Diag.OverlayMessage);
	}
	if (Message.IsEmpty())
	{
		if (View.State == S::Migrating)
		{
			const FString Succ = View.Successor ? ToFString(View.Successor->DisplayName) : TEXT("the next host");
			Message = TEXT("HOST MIGRATION\nSaving and handing the Shared World to ") + Succ + TEXT("…");
		}
		else if (bClientSeesLeave)
		{
			if (!ObservedSuccessor.IsEmpty() && (View.State == S::WaitingForHost || View.State == S::WaitingForSession ||
				View.State == S::Reconnecting))
			{
				Message = TEXT("HOST MIGRATION\n") + ObservedSuccessor + TEXT(" is becoming the new host — stay here.");
			}
			else
			{
				Message = TEXT("HOST MIGRATION\nHost is leaving — selecting the next host. Stay here.");
			}
		}
		else if (View.State == S::Reconnecting || View.State == S::RecoveringHost || View.State == S::JoinRetry ||
			View.State == S::HostUnreachable)
		{
			Message = TEXT("HOST MIGRATION\nSelecting a new host — stay here, the world will continue automatically.");
		}
		else if (View.State == S::WaitingForHost || View.State == S::WaitingForSession)
		{
			Message = TEXT("HOST MIGRATION\n") + ToFString(View.Message);
		}
		else if (View.State == S::Acquiring || View.State == S::ElectingHost || View.State == S::Recovering ||
			View.State == S::Downloading)
		{
			Message = TEXT("HOST MIGRATION\nYou are becoming the host. Loading the Shared World...");
		}
		else if (View.State == S::ReadyToHost || View.State == S::StartingSession || View.State == S::PublishingSession)
		{
			Message = TEXT("HOST MIGRATION\nStarting your session as the new host...");
		}
		else if ((View.State == S::JoinReady || View.State == S::Joining) && IsHostMigrationInFlight(WorldId))
		{
			Message = TEXT("HOST MIGRATION\nReconnecting to the new host…");
		}
	}
	if (Message.IsEmpty())
	{
		if (LastOverlayMessage.IsEmpty()) return;
		const bool bWasUpload = LastOverlayMessage.StartsWith(TEXT("UPLOADING")) || LastOverlayMessage.StartsWith(TEXT("RELEASING"));
		if (bWasUpload)
		{
			HideMigrationOverlay();
			return;
		}
		FString HostName = ToFString(View.HostName);
		if (HostName.IsEmpty() && Runtime->HostMigration)
		{
			HostName = ToFString(Runtime->HostMigration->Diagnostics().CurrentHost.DisplayName);
		}
		if (HostName.IsEmpty()) HostName = TEXT("the new host");
		Publish(TEXT("CONNECTED"), FString::Printf(TEXT("Joined %s"), *HostName));
		LastOverlayMessage.Reset();
		return;
	}

	FString Headline, Detail;
	if (!Message.Split(TEXT("\n"), &Headline, &Detail))
	{
		Headline = Message;
		Detail = ToFString(View.Message);
	}
	Publish(Headline, Detail);
}

void USharedWorldSubsystem::HideMigrationOverlay()
{
	LastOverlayMessage.Reset();
	if (USharedWorldMigrationOverlay* Overlay = MigrationOverlay.Get())
	{
		Overlay->RemoveFromParent();
	}
	MigrationOverlay.Reset();
}

bool USharedWorldSubsystem::IsHostMigrationInFlight(const FString& WorldId) const
{
	if (WorldId.IsEmpty())
	{
		return false;
	}
	const FSharedWorldRuntime* Runtime = nullptr;
	if (const TUniquePtr<FSharedWorldRuntime>* Found = Runtimes.Find(WorldId))
	{
		Runtime = Found->Get();
	}
	if (!Runtime || !Runtime->Session)
	{
		return false;
	}
	using S = sw::SessionState;
	RefreshCloudCache(*Runtime);
	const sw::SessionView View = Runtime->Session->View();
	switch (View.State)
	{
	case S::Reconnecting:
	case S::RecoveringHost:
	case S::JoinRetry:
	case S::HostUnreachable:
	case S::ElectingHost:
	case S::Acquiring:
	case S::Recovering:
	case S::Downloading:
	case S::ReadyToHost:
	case S::StartingSession:
	case S::PublishingSession:
	case S::Migrating:
	case S::WaitingForHost:
	case S::WaitingForSession:
		return true;
	case S::Uploading:
	case S::Releasing:
		return View.Successor.has_value();
	case S::Joined:
	case S::JoinReady:
	case S::Joining:
		if (View.Message.find("HOST MIGRATION") != std::string::npos ||
			View.Message.find("Host migration") != std::string::npos ||
			View.Message.find("Reconnecting") != std::string::npos)
		{
			return true;
		}
		if (Runtime->Leases)
		{
			const std::optional<sw::StateSnapshot> Snap = Runtime->CloudCache ? Runtime->CloudCache->Get() : std::nullopt;
			if (Snap)
			{
				if (Snap->State.PendingHandoff) return true;
				if (Snap->State.CurrentLease && Snap->State.CurrentLease->Phase == sw::LeasePhase::Migrating) return true;
			}
		}
		return !LastOverlayMessage.IsEmpty() && LastOverlayMessage.StartsWith(TEXT("HOST MIGRATION"));
	default:
		return false;
	}
}

void USharedWorldSubsystem::OnMenuWorldReady(UWorld* World)
{
	MenuWorld = World;
	TryShowMenuPanel(World);

	const bool bHostMigrationInFlight = IsHostMigrationInFlight(ActiveWorldId);
	// Keep the upload overlay if a final cloud save is still in flight after leave-to-menu.
	bool bUploading = false;
	if (!ActiveWorldId.IsEmpty())
	{
		if (FSharedWorldRuntime* Runtime = FindRuntime(ActiveWorldId))
		{
			if (Runtime->Session)
			{
				const sw::SessionState St = Runtime->Session->View().State;
				bUploading = St == sw::SessionState::Uploading || St == sw::SessionState::Releasing;
			}
		}
	}
	if (bUploading || bHostMigrationInFlight)
	{
		// Status shows in Manage Session → Shared World (no viewport overlay).
		EnsureMigrationOverlay(World);
		UpdateMigrationOverlay(ActiveWorldId);
	}
	else
	{
		HideMigrationOverlay();
	}
	bReturningToMenu = false;
	if (!PendingRejoinWorldId.IsEmpty())
	{
		const FString Rejoin = PendingRejoinWorldId;
		PendingRejoinWorldId.Reset();
		Play(Rejoin); // decides JOIN: the successor holds the lease now
	}
	OnChanged.Broadcast();
}

void USharedWorldSubsystem::ReturnToMainMenu(const FText& Reason)
{
	if (bReturningToMenu)
	{
		return;
	}
	UGameInstance* GI = GetGameInstance();
	APlayerController* PC = GI ? GI->GetFirstLocalPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	bReturningToMenu = true;
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=return_to_menu reason=\"%s\""), *Reason.ToString());
	// UNVERIFIED (STATUS.md): engine API; Satisfactory's main-menu travel
	// after this call has to be confirmed in game.
	PC->ClientReturnToMainMenuWithTextReason(Reason);
}

void USharedWorldSubsystem::OnGameWorldReady(UWorld* World)
{
	MenuWorld.Reset();
	if (!World || ActiveWorldId.IsEmpty())
	{
		return;
	}
	EnsureMigrationOverlay(World);
	FSharedWorldRuntime* Runtime = FindRuntime(ActiveWorldId);
	if (!Runtime || !Runtime->Session)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		if (Runtime->Session->View().State == sw::SessionState::JoinReady)
		{
			Runtime->Session->OnJoinedAsClient();
		}
	}
	else
	{
		Host->OnGameWorldReady(World);
	}
}

void USharedWorldSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	if (World && MenuWorld.Get() == World)
	{
		MenuWorld.Reset();
		if (USharedWorldPanel* Panel = MenuPanel.Get())
		{
			Panel->RemoveFromParent();
		}
		MenuPanel.Reset();
	}
	Host->OnWorldTearDown(World);
	if (!ActiveWorldId.IsEmpty())
	{
		if (FSharedWorldRuntime* Runtime = FindRuntime(ActiveWorldId))
		{
			if (Runtime->Session && World && World->GetNetMode() == NM_Client)
			{
				const sw::SessionState St = Runtime->Session->View().State;
				// Browse/"Connection failed; returning to Entry" often skips
				// OnNetworkFailure while still JOINING — kick recovery there.
				if (St == sw::SessionState::Joining || St == sw::SessionState::JoinReady || St == sw::SessionState::HostVerified)
				{
					Runtime->Session->OnJoinFailed("disconnected while joining");
				}
				else if (St == sw::SessionState::Joined)
				{
					LastOverlayMessage = TEXT("HOST MIGRATION\nSelecting a new host — stay here, the world will continue automatically.");
					PushMigrationStatusToMenus(FText::FromString(
						TEXT("HOST MIGRATION — Selecting a new host — stay here, the world will continue automatically.")));
					if (Runtime->HostMigration)
					{
						(void)Runtime->HostMigration->OnHostLost();
					}
					Runtime->Session->OnHostConnectionLost();
				}
				// Already Reconnecting / RecoveringHost: leave the session running.
			}
		}
	}
}

void USharedWorldSubsystem::OnNetworkFailure(UWorld* World, UNetDriver*, ENetworkFailure::Type, const FString& ErrorString)
{
	if (ActiveWorldId.IsEmpty())
	{
		return;
	}
	FSharedWorldRuntime* Runtime = FindRuntime(ActiveWorldId);
	if (!Runtime || !Runtime->Session || (World && World->GetNetMode() != NM_Client))
	{
		return;
	}
	const sw::SessionState State = Runtime->Session->View().State;
	if (State == sw::SessionState::Joined)
	{
		LastOverlayMessage = TEXT("HOST MIGRATION\nSelecting a new host — stay here, the world will continue automatically.");
		PushMigrationStatusToMenus(FText::FromString(
			TEXT("HOST MIGRATION — Selecting a new host — stay here, the world will continue automatically.")));
		if (Runtime->HostMigration)
		{
			(void)Runtime->HostMigration->OnHostLost();
		}
		Runtime->Session->OnHostConnectionLost();
	}
	else if (State == sw::SessionState::Joining || State == sw::SessionState::JoinReady || State == sw::SessionState::HostVerified)
	{
		LastOverlayMessage = TEXT("HOST MIGRATION\nFormer host is gone — waiting to take over automatically.");
		PushMigrationStatusToMenus(FText::FromString(
			TEXT("HOST MIGRATION — Former host is gone — waiting to take over automatically.")));
		Runtime->Session->OnJoinFailed(ErrorString.IsEmpty() ? std::string("network failure while joining") : Std(ErrorString));
	}
}

// ---------------------------------------------------------------- chat surface

FString USharedWorldSubsystem::RequestCheckpoint()
{
	return Host->SaveAndUpload(sw::SaveKind::Checkpoint);
}

FString USharedWorldSubsystem::RequestStop()
{
	// MW2-style leave: if anyone else is in the session, hand off to them
	// (HOST MIGRATION overlay + progress) instead of a cold release.
	if (!ActiveWorldId.IsEmpty() && Host && Host->IsHostingWorld(ActiveWorldId))
	{
		const FString Migrate = RequestMigrationTo(ActiveWorldId, TEXT("auto"));
		if (Migrate.StartsWith(TEXT("Handing")))
		{
			return Migrate;
		}
	}
	return Host->SaveAndUpload(sw::SaveKind::Final);
}

FString USharedWorldSubsystem::DescribeActiveSession() const
{
	for (const auto& [Id, Runtime] : Runtimes)
	{
		if (Runtime->Session && Runtime->Session->View().State != sw::SessionState::Idle)
		{
			const sw::SessionView V = Runtime->Session->View();
			return FString::Printf(TEXT("%s [id %s]: %s (revision %lld). %s"), *ToFString(Runtime->Entry.DisplayName), *Id, *ToFString(sw::ToString(V.State)), V.Revision, *ToFString(V.Message));
		}
	}
	return TEXT("No shared world session is active.");
}

void USharedWorldSubsystem::NotifyHostResponderReady(const FString& WorldId, const FString& SessionId, int64 Generation, int64 Revision, int32 PlayerCount)
{
	if (!HostResponder) return;
	const sw::Identity Me = MyIdentity();
	HostResponder->OnHostingReady(WorldId, ToFString(Me.PlayerId), Generation, Revision, SessionId, PlayerCount, 4);
}

void USharedWorldSubsystem::NotifyHostResponderCleared()
{
	if (HostResponder) HostResponder->Clear();
}

void USharedWorldSubsystem::DebugVerifyHost(const FString& InWorldId, FDone OnDone)
{
	const FString WorldId = InWorldId.IsEmpty() ? ActiveWorldId : InWorldId;
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || !Runtime->Leases)
	{
		OnDone(false, TEXT("Unknown Shared World."));
		return;
	}
	EnsureSession(*Runtime);
	// The cloud read and the (blocking) probe run on the background queue. Local
	// host state is captured here, on the game thread, because it lives in UObjects.
	FString LocalHostDescription;
	if (HostResponder && HostResponder->IsReadyHostFor(WorldId))
	{
		LocalHostDescription = HostResponder->Describe() + TEXT("\n");
	}
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	const sw::Identity Me = MyIdentity();
	TWeakObjectPtr<UGameInstance> WeakGI(GetGameInstance());
	TWeakObjectPtr<USharedWorldHostResponder> WeakResponder(HostResponder);
	TWeakObjectPtr<USharedWorldNetworkQuality> WeakQuality(NetworkQuality);
	RunInBackground([Leases, Me, WeakGI, WeakResponder, WeakQuality, WorldId, LocalHostDescription]() -> TPair<bool, FString>
	{
		auto Snap = Leases->Store().Load();
		if (!Snap)
		{
			return {false, TEXT("Could not read Shared World cloud state.")};
		}
		FString Out = LocalHostDescription;
		if (!Snap->State.CurrentLease || !Leases->LiveForObserver(Snap->State.CurrentLease, Leases->Store().Clock().Now()))
		{
			return {true, Out + TEXT("Cloud lease: none (nobody hosting)\nVerification: n/a")};
		}
		const sw::Lease& L = *Snap->State.CurrentLease;
		Out += FString::Printf(TEXT("Cloud host: %s\nLease generation: %lld\nHostReady: %s\nSession published: %s\n"),
			*ToFString(L.Holder.DisplayName), L.Generation, L.bHostReady ? TEXT("yes") : TEXT("no"),
			(L.Join && !L.Join->Data.empty()) ? TEXT("yes") : TEXT("no"));
		if (!L.IsJoinable())
		{
			return {true, Out + TEXT("Verification: WAIT (host still starting)")};
		}
		auto Verifier = std::make_shared<SharedWorldUe::FSharedWorldUEHostVerifier>(
			WeakGI, WeakResponder, WeakQuality, ToFString(L.Holder.PlayerId));
		sw::SharedWorldHello Hello = sw::MakeHello(Me, Std(WorldId), L, Snap->State.HeadNumber(), "debug-verify", Leases->Store().Clock().Now());
		sw::JoinInfo Join = L.Join.value_or(sw::JoinInfo{});
		const sw::HostVerifyResult VR = Verifier->Probe(Hello, Join, sw::Seconds(5));
		sw::SharedWorldHelloAck Ack = VR.Ack.value_or(sw::SharedWorldHelloAck{});
		if (Ack.HostPlayerId.empty()) Ack.HostPlayerId = L.Holder.PlayerId;
		FString Verdict = UTF8_TO_TCHAR(sw::ToString(VR.Outcome));
		if (VR.Outcome == sw::HostVerifyOutcome::Verified)
		{
			if (sw::Status V = sw::ValidateHelloAck(Hello, Ack, L, Snap->State.HeadNumber(), L.Join); !V)
			{
				Verdict = TEXT("FAIL (") + ToFString(V.Err().Message) + TEXT(")");
			}
			else
			{
				Verdict = TEXT("PASS");
			}
		}
		return {true, Out + FString::Printf(TEXT("Verification: %s\nRTT: %d ms\nDetail: %s"), *Verdict, VR.RttMs, *ToFString(VR.Detail))};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

void USharedWorldSubsystem::FetchHistory(const FString& WorldId, int32 MaxCount, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	std::shared_ptr<sw::SyncEngine> Sync = Runtime->Sync;
	const size_t Max = static_cast<size_t>(FMath::Clamp(MaxCount, 1, 50));
	RunInBackground([Leases, Sync, Max]() -> TPair<bool, FString>
	{
		auto Snap = Leases->Store().Load();
		if (!Snap)
		{
			return Fail(Snap.Err());
		}
		auto History = Sync->History(Snap->CommitId, Max);
		if (!History)
		{
			return Fail(History.Err());
		}
		FString Out;
		for (const sw::RevisionMeta& R : *History)
		{
			Out += FString::Printf(TEXT("#%lld  %s  %s by %s%s\n"), R.Number, *ToFString(sw::FormatTime(R.CreatedAt)), *ToFString(R.Reason),
				*ToFString(R.Uploader.DisplayName), R.RestoredFrom > 0 ? *FString::Printf(TEXT(" (restored #%lld)"), R.RestoredFrom) : TEXT(""));
		}
		return {true, Out.IsEmpty() ? TEXT("No revisions yet.") : Out};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

void USharedWorldSubsystem::FetchPlayers(const FString& WorldId, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	RunInBackground([Leases]() -> TPair<bool, FString>
	{
		auto Snap = Leases->Store().Load();
		if (!Snap)
		{
			return Fail(Snap.Err());
		}
		auto List = Leases->Store().LoadPlayers(Snap->CommitId);
		if (!List)
		{
			return Fail(List.Err());
		}
		FString Out = List->bOpen ? TEXT("Open: anyone who can reach the world may play.\n") : TEXT("Members only.\n");
		for (const sw::Member& M : List->Members)
		{
			Out += FString::Printf(TEXT("%s  (%s)\n"), *ToFString(M.DisplayName), *ToFString(sw::ToString(M.MemberRole)));
		}
		return {true, Out};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

TArray<FSharedWorldFriendInfo> USharedWorldSubsystem::GetConnectedSessionPlayers() const
{
	TArray<FSharedWorldFriendInfo> Out;
	if (!Host)
	{
		return Out;
	}
	for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
	{
		FSharedWorldFriendInfo Info;
		Info.PlayerId = ToFString(P.PlayerId);
		Info.DisplayName = ToFString(P.DisplayName);
		Info.bOnline = true;
		if (Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = Info.PlayerId.IsEmpty() ? TEXT("Unknown player") : Info.PlayerId;
		}
		Out.Add(MoveTemp(Info));
	}
	return Out;
}

FString USharedWorldSubsystem::GetLocalPlayerId() const
{
	return ToFString(MyIdentity().PlayerId);
}

FString USharedWorldSubsystem::ResolvePlayerId(const FString& Who, FString& OutDisplayName) const
{
	for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
	{
		if (ToFString(P.PlayerId) == Who || ToFString(P.DisplayName).Equals(Who, ESearchCase::IgnoreCase))
		{
			OutDisplayName = ToFString(P.DisplayName);
			return ToFString(P.PlayerId);
		}
	}
	OutDisplayName = Who;
	return Who; // not connected: taken as a Steam/Epic player id
}

void USharedWorldSubsystem::AllowPlayer(const FString& WorldId, const FString& Who, sw::Role Role, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	FString Name;
	const FString Id = ResolvePlayerId(Who, Name);
	if (Id.IsEmpty())
	{
		OnDone(false, TEXT("That player has no Steam/Epic id."));
		return;
	}
	const sw::Member M{Std(Id), Std(Name), Role};
	const sw::Identity Actor = MyIdentity();
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	RunInBackground([Leases, Actor, M]() -> TPair<bool, FString>
	{
		auto R = sw::AddMember(Leases->Store(), Actor, M);
		if (!R)
		{
			return Fail(R.Err());
		}
		return {true, FString::Printf(TEXT("%s can now play as %s."), *ToFString(M.DisplayName), *ToFString(sw::ToString(M.MemberRole)))};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

void USharedWorldSubsystem::RemovePlayer(const FString& WorldId, const FString& Who, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	FString Name;
	const std::string Id = Std(ResolvePlayerId(Who, Name));
	const sw::Identity Actor = MyIdentity();
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	RunInBackground([Leases, Actor, Id, Name]() -> TPair<bool, FString>
	{
		auto R = sw::RemoveMember(Leases->Store(), Actor, Id);
		if (!R)
		{
			return Fail(R.Err());
		}
		return {true, FString::Printf(TEXT("%s was removed from the world's members."), *Name)};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

void USharedWorldSubsystem::SetOpenMembership(const FString& WorldId, bool bOpen, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	const sw::Identity Actor = MyIdentity();
	std::shared_ptr<sw::LeaseManager> Leases = Runtime->Leases;
	RunInBackground([Leases, Actor, bOpen]() -> TPair<bool, FString>
	{
		auto R = sw::SetOpenMembership(Leases->Store(), Actor, bOpen);
		if (!R)
		{
			return Fail(R.Err());
		}
		return {true, bOpen ? TEXT("Anyone who can reach the world may now play.") : TEXT("Only members may play now.")};
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

void USharedWorldSubsystem::GrantHosting(const FString& WorldId, const FString& GitHubUsername, FDone OnDone)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		OnDone(false, TEXT("Unknown world."));
		return;
	}
	if (Runtime->Entry.Provider.Kind != sw::ProviderKind::GitHub)
	{
		OnDone(false, TEXT("This world is stored in a folder: give your friend access to that folder instead."));
		return;
	}
	sw::GitHubConfig Cfg;
	Cfg.Owner = Runtime->Entry.Provider.Owner;
	Cfg.Repo = Runtime->Entry.Provider.Repo;
	Cfg.WorldId = Runtime->Entry.WorldId;
	std::shared_ptr<sw::ICredentialStore> Creds = Credentials;
	Cfg.Token = [Creds]() -> sw::Result<std::string>
	{
		auto T = Creds->Read(sw::GitHubCredentialKey);
		if (!T && T.Is(sw::ErrorCode::NotFound))
		{
			return sw::MakeError(sw::ErrorCode::Unauthorized, "sign in to GitHub first");
		}
		return T;
	};
	std::shared_ptr<sw::IHttpClient> HttpRef = Http;
	const std::string Login = Std(GitHubUsername.TrimStartAndEnd());
	RunInBackground([HttpRef, Cfg, Login]() -> TPair<bool, FString>
	{
		auto R = sw::InviteGitHubCollaborator(HttpRef, Cfg, Login);
		if (!R)
		{
			return Fail(R.Err());
		}
		return TPair<bool, FString>(true, *R ? TEXT("GitHub invitation sent. Once accepted, they can host this world.") : TEXT("They can already host this world."));
	},
	[OnDone](USharedWorldSubsystem&, bool bOk, const FString& Message) { OnDone(bOk, Message); });
}

FString USharedWorldSubsystem::RecentLog(int32 MaxLines) const
{
	const std::vector<std::string> Lines = DiagnosticsSink->Lines();
	FString Out;
	const int32 Start = FMath::Max(0, static_cast<int32>(Lines.size()) - MaxLines);
	for (int32 i = Start; i < static_cast<int32>(Lines.size()); ++i)
	{
		Out += ToFString(Lines[static_cast<size_t>(i)]) + TEXT("\n");
	}
	return Out;
}

FSharedWorldDiscoveryService& USharedWorldSubsystem::Discovery()
{
	check(DiscoveryService.IsValid());
	return *DiscoveryService;
}

FSharedWorldInviteService& USharedWorldSubsystem::Invites()
{
	check(InviteService.IsValid());
	return *InviteService;
}

FSharedWorldCreationService& USharedWorldSubsystem::Creation()
{
	check(CreationService.IsValid());
	return *CreationService;
}

void USharedWorldSubsystem::RequestDiscoveryRefresh()
{
	RefreshSummariesAsync();
	OnChanged.Broadcast();
}

TArray<FSharedWorldPendingInviteView> USharedWorldSubsystem::GetPendingInviteViews() const
{
	TArray<FSharedWorldPendingInviteView> Out;
	for (const sw::PendingInvite& I : Settings.PendingInvites)
	{
		FSharedWorldPendingInviteView V;
		V.InviteId = ToFString(I.InviteId);
		V.WorldId = ToFString(I.WorldId);
		V.WorldName = ToFString(I.WorldName);
		V.FromPlayerId = ToFString(I.FromPlayerId);
		V.FromDisplayName = ToFString(I.FromDisplayName);
		Out.Add(MoveTemp(V));
	}
	return Out;
}

bool USharedWorldSubsystem::NeedsWelcomeStorageConnect() const
{
	return !Settings.bWelcomeDone && GetGitHubLogin().IsEmpty() && !Settings.DefaultProvider.has_value();
}

void USharedWorldSubsystem::MarkWelcomeDone()
{
	Settings.bWelcomeDone = true;
	if (!Settings.DefaultProvider)
	{
		FString Note;
		Settings.DefaultProvider = ResolveDefaultStorage(Note);
	}
	SaveSettings();
	OnChanged.Broadcast();
}

bool USharedWorldSubsystem::IsWelcomeDone() const
{
	return Settings.bWelcomeDone || Settings.DefaultProvider.has_value() || !Settings.Worlds.empty();
}

void USharedWorldSubsystem::UpgradeLegacyFolderWorldsToGitHubIfConnected()
{
	const FString Login = GetGitHubLogin();
	if (Login.IsEmpty())
	{
		return;
	}
	const FString DefaultFolder = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("worlds")));
	sw::ProviderConfig Gh;
	Gh.Kind = sw::ProviderKind::GitHub;
	Gh.Owner = Std(Login);
	Gh.Repo = "satisfactory-shared-worlds";
	int32 Migrated = 0;
	for (sw::WorldEntry& E : Settings.Worlds)
	{
		if (E.Provider.Kind != sw::ProviderKind::Folder)
		{
			continue;
		}
		const FString WorldFolder = FPaths::ConvertRelativePathToFull(UTF8_TO_TCHAR(E.Provider.FolderPath.c_str()));
		if (!WorldFolder.Equals(DefaultFolder, ESearchCase::IgnoreCase))
		{
			continue;
		}
		E.Provider = Gh;
		++Migrated;
		if (FSharedWorldRuntime* R = FindRuntime(ToFString(E.WorldId)))
		{
			R->Entry.Provider = Gh;
			(void)RebindRuntimeStorage(*R);
		}
	}
	if (Migrated > 0)
	{
		SaveSettings();
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] event=worlds_migrated_to_github count=%d owner=%s repo=satisfactory-shared-worlds"),
			Migrated, *Login);
		OnChanged.Broadcast();
	}
}

void USharedWorldSubsystem::PreferGitHubCloudStorageIfConnected()
{
	const FString Login = GetGitHubLogin();
	if (Login.IsEmpty())
	{
		return;
	}
	sw::ProviderConfig C;
	C.Kind = sw::ProviderKind::GitHub;
	C.Owner = Std(Login);
	C.Repo = "satisfactory-shared-worlds";
	const bool bDefaultChanged = !Settings.DefaultProvider
		|| Settings.DefaultProvider->Kind != sw::ProviderKind::GitHub
		|| Settings.DefaultProvider->Owner != C.Owner
		|| Settings.DefaultProvider->Repo != C.Repo;
	if (bDefaultChanged)
	{
		Settings.DefaultProvider = C;
		Settings.bWelcomeDone = true;
		SaveSettings();
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/GitHub] event=default_storage_github owner=%s repo=%s"), *Login, TEXT("satisfactory-shared-worlds"));
	}
	UpgradeLegacyFolderWorldsToGitHubIfConnected();
}

sw::ProviderConfig USharedWorldSubsystem::ResolveDefaultStorage(FString& OutNote) const
{
	// Prefer live GitHub when connected, even if an older welcome chose folder storage.
	const FString Login = GetGitHubLogin();
	if (!Login.IsEmpty())
	{
		if (Settings.DefaultProvider && Settings.DefaultProvider->Kind == sw::ProviderKind::GitHub
			&& !Settings.DefaultProvider->Owner.empty() && !Settings.DefaultProvider->Repo.empty())
		{
			OutNote = TEXT("Cloud storage via your GitHub account.");
			return *Settings.DefaultProvider;
		}
		sw::ProviderConfig C;
		C.Kind = sw::ProviderKind::GitHub;
		C.Owner = Std(Login);
		C.Repo = "satisfactory-shared-worlds";
		OutNote = TEXT("Cloud storage via your GitHub account.");
		return C;
	}
	if (Settings.DefaultProvider)
	{
		OutNote = TEXT("Using your saved storage.");
		return *Settings.DefaultProvider;
	}
	sw::ProviderConfig C;
	C.Kind = sw::ProviderKind::Folder;
	const FString Path = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("worlds")));
	C.FolderPath = Std(Path);
	OutNote = TEXT("Local shared folder (connect GitHub in Settings for cloud sync between PCs).");
	return C;
}

const sw::WorldEntry* USharedWorldSubsystem::FindWorldEntry(const FString& WorldId) const
{
	return Settings.Find(Std(WorldId));
}

FString USharedWorldSubsystem::EnsureInviteCode(const FString& WorldId)
{
	if (sw::WorldEntry* E = Settings.FindMutable(Std(WorldId)))
	{
		if (E->InviteCode.empty())
		{
			E->InviteCode = Std(MakeInviteCode(WorldId));
			SaveSettings();
			if (FSharedWorldRuntime* R = FindRuntime(WorldId))
			{
				R->Entry.InviteCode = E->InviteCode;
			}
		}
		return ToFString(E->InviteCode);
	}
	return MakeInviteCode(WorldId);
}

void USharedWorldSubsystem::QueueIncomingInvite(const FString& WorldId, const FString& WorldName, const FString& FromPlayerId, const FString& FromDisplayName, const sw::ProviderConfig& Provider)
{
	for (const sw::PendingInvite& I : Settings.PendingInvites)
	{
		if (I.WorldId == Std(WorldId)) return;
	}
	sw::PendingInvite Inv;
	Inv.InviteId = Std(FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Inv.WorldId = Std(WorldId);
	Inv.WorldName = Std(WorldName);
	Inv.FromPlayerId = Std(FromPlayerId);
	Inv.FromDisplayName = Std(FromDisplayName);
	Inv.Provider = Provider;
	Inv.CreatedAt = NowMs();
	Settings.PendingInvites.push_back(std::move(Inv));
	SaveSettings();
	OnChanged.Broadcast();
}

bool USharedWorldSubsystem::PushWorldInviteToConnectedPlayer(const FString& TargetPlayerId, const FString& WorldId)
{
	if (TargetPlayerId.IsEmpty() || WorldId.IsEmpty() || !Host)
	{
		return false;
	}
	const sw::WorldEntry* Entry = FindWorldEntry(WorldId);
	if (!Entry)
	{
		return false;
	}
	ASharedWorldInviteBridge* Bridge = Host->EnsureInviteBridge();
	if (!Bridge)
	{
		return false;
	}
	const FString Kind = Entry->Provider.Kind == sw::ProviderKind::GitHub ? TEXT("github") : TEXT("folder");
	const FString OwnerOrPath = Entry->Provider.Kind == sw::ProviderKind::GitHub
		? ToFString(Entry->Provider.Owner)
		: ToFString(Entry->Provider.FolderPath);
	const FString Repo = Entry->Provider.Kind == sw::ProviderKind::GitHub
		? ToFString(Entry->Provider.Repo)
		: FString();
	const FString FromName = ToFString(MyIdentity().DisplayName);
	Bridge->PushInviteToPlayer(
		TargetPlayerId,
		ToFString(Entry->WorldId),
		ToFString(Entry->DisplayName),
		FromName,
		Kind,
		OwnerOrPath,
		Repo);
	return true;
}

void USharedWorldSubsystem::ReceivePushedWorldInvite(
	const FString& WorldId,
	const FString& WorldName,
	const FString& FromDisplayName,
	const FString& ProviderKind,
	const FString& OwnerOrPath,
	const FString& Repo)
{
	sw::WorldEntry Entry;
	Entry.WorldId = Std(WorldId.ToLower().TrimStartAndEnd());
	Entry.DisplayName = Std(WorldName.IsEmpty() ? WorldId : WorldName);
	Entry.AddedAt = NowMs();
	Entry.Relation = sw::WorldRelation::Shared;
	if (ProviderKind.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
	{
		Entry.Provider.Kind = sw::ProviderKind::Folder;
		Entry.Provider.FolderPath = Std(OwnerOrPath);
	}
	else
	{
		Entry.Provider.Kind = sw::ProviderKind::GitHub;
		Entry.Provider.Owner = Std(OwnerOrPath);
		Entry.Provider.Repo = Std(Repo.IsEmpty() ? TEXT("satisfactory-shared-worlds") : Repo);
	}
	if (sw::Status V = sw::ValidateWorldId(Entry.WorldId); !V)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=pushed_invite_rejected world=%s reason=\"%s\""),
			*WorldId, *ToFString(V.Err().Message));
		return;
	}
	if (Settings.Find(Entry.WorldId))
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pushed_invite_already_listed world=%s"), *ToFString(Entry.WorldId));
		return;
	}
	// Soft-add: trust the host. Storage verify can fail (private GitHub without
	// collaborator yet) but the world must still appear so they never need a code.
	(void)Settings.Upsert(Entry);
	SaveSettings();
	(void)FindOrCreateRuntime(Entry);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pushed_invite_accepted world=%s from=\"%s\""),
		*ToFString(Entry.WorldId), *FromDisplayName);
	OnChanged.Broadcast();
}

void USharedWorldSubsystem::AcceptPendingInvite(const FString& InviteId, FDone OnDone)
{
	const std::string Id = Std(InviteId);
	auto It = std::find_if(Settings.PendingInvites.begin(), Settings.PendingInvites.end(),
		[&](const sw::PendingInvite& I) { return I.InviteId == Id; });
	if (It == Settings.PendingInvites.end())
	{
		OnDone(false, TEXT("That invite is no longer available."));
		return;
	}
	const sw::PendingInvite Inv = *It;
	Settings.PendingInvites.erase(It);
	SaveSettings();
	AddExistingWorld(ToFString(Inv.WorldId), ToFString(Inv.WorldName), Inv.Provider, OnDone);
}

void USharedWorldSubsystem::DeclinePendingInvite(const FString& InviteId, FDone OnDone)
{
	const std::string Id = Std(InviteId);
	const auto NewEnd = std::remove_if(Settings.PendingInvites.begin(), Settings.PendingInvites.end(),
		[&](const sw::PendingInvite& I) { return I.InviteId == Id; });
	if (NewEnd == Settings.PendingInvites.end())
	{
		OnDone(false, TEXT("That invite is no longer available."));
		return;
	}
	Settings.PendingInvites.erase(NewEnd, Settings.PendingInvites.end());
	SaveSettings();
	OnChanged.Broadcast();
	OnDone(true, TEXT("Invite declined."));
}

void USharedWorldSubsystem::JoinUsingShareCode(const FString& Code, FDone OnDone)
{
	const FString Clean = Code.TrimStartAndEnd().ToUpper();
	if (Clean.IsEmpty())
	{
		OnDone(false, TEXT("Enter a share code."));
		return;
	}
	// Match against known invite codes on this PC's worlds (owner sharing).
	for (const sw::WorldEntry& E : Settings.Worlds)
	{
		if (ToFString(E.InviteCode).Equals(Clean, ESearchCase::IgnoreCase))
		{
			OnDone(true, TEXT("That world is already in your list."));
			return;
		}
	}
	// Raw world id fallback (advanced): use default storage.
	FString Note;
	const sw::ProviderConfig Provider = ResolveDefaultStorage(Note);
	const FString WorldId = Clean.Replace(TEXT("-"), TEXT("")).ToLower();
	// Prefer looking up by invite code across pending invites.
	for (const sw::PendingInvite& I : Settings.PendingInvites)
	{
		if (ToFString(I.WorldId).Equals(Clean, ESearchCase::IgnoreCase)
			|| MakeInviteCode(ToFString(I.WorldId)).Equals(Clean, ESearchCase::IgnoreCase))
		{
			AcceptPendingInvite(ToFString(I.InviteId), OnDone);
			return;
		}
	}
	AddExistingWorld(WorldId, WorldId, Provider, OnDone);
}

void USharedWorldSubsystem::CreateWorldFromCurrentSession(const FString& DisplayName, FDone OnDone)
{
	// Prefer the live session save name if we are already hosting a Shared World copy.
	FString SaveName;
	if (!ActiveWorldId.IsEmpty())
	{
		OnDone(false, TEXT("This session is already a Shared World."));
		return;
	}
	// Use the most recent non-SharedWorld save as the source (player just saved / is playing it).
	TArray<FSharedWorldSaveInfo> Saves = Creation().ListLocalSaves();
	if (Saves.Num() == 0)
	{
		OnDone(false, TEXT("No local save found to convert. Save the game once, then try again."));
		return;
	}
	SaveName = Saves[0].SaveName;
	const FString Name = DisplayName.TrimStartAndEnd().IsEmpty() ? Saves[0].SessionName : DisplayName.TrimStartAndEnd();
	FString Note;
	const sw::ProviderConfig Provider = ResolveDefaultStorage(Note);
	if (!Settings.DefaultProvider)
	{
		Settings.DefaultProvider = Provider;
		Settings.bWelcomeDone = true;
		SaveSettings();
	}
	CreateWorldFromSave(Name.IsEmpty() ? SaveName : Name, SaveName, Provider, true, OnDone);
}

