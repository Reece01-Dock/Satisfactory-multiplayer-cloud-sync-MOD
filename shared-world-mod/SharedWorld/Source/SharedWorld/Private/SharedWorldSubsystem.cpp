#include "SharedWorldSubsystem.h"

#include "Async/Async.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FGSaveSystem.h"
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
#include "SharedWorldCore/World/Creation.h"
#include "SharedWorldCore/World/Membership.h"
#include "SharedWorldCredentialStore.h"
#include "SharedWorldHostController.h"
#include "SharedWorldHttpClient.h"
#include "SharedWorldJoinManager.h"
#include "SharedWorldLogSink.h"
#include "SharedWorldPanel.h"

DEFINE_LOG_CATEGORY(LogSharedWorld);

namespace
{
	constexpr float TickIntervalSeconds = 1.0f;
	constexpr double SummaryRefreshSeconds = 5.0;
	constexpr const char* GitHubClientId = ""; // set by the mod's release build; empty disables sign-in

	std::string Std(const FString& S) { return TCHAR_TO_UTF8(*S); }
	FString UE(const std::string& S) { return UTF8_TO_TCHAR(S.c_str()); }

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

	Http = std::make_shared<FSharedWorldHttpClient>();
	Credentials = std::make_shared<FSharedWorldCredentialStore>();
	DiagnosticsSink = std::make_shared<sw::MemoryLogSink>();
	LogSink = std::make_shared<FTeeLogSink>(DiagnosticsSink);

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
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_damaged kept=\"%s\" reason=\"%s\""), *Aside, *UE(Loaded.Err().Describe()));
	}
	else
	{
		// Written by a newer mod version, or unreadable: never overwrite it.
		bSettingsReadOnly = true;
		SettingsProblem = UE(Loaded.Err().Message);
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_load_failed reason=\"%s\""), *UE(Loaded.Err().Describe()));
	}
	for (const sw::WorldEntry& Entry : Settings.Worlds)
	{
		FindOrCreateRuntime(Entry);
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldSubsystem::Tick), TickIntervalSeconds);
	TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &USharedWorldSubsystem::OnWorldBeginTearDown);
	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &USharedWorldSubsystem::OnNetworkFailure);
	}

	Host = NewObject<USharedWorldHostController>(this);
	Host->Init(this);
	Joiner = NewObject<USharedWorldJoinManager>(this);
	Joiner->Init(this);

	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=subsystem_initialized worlds=%d"), static_cast<int32>(Settings.Worlds.size()));
}

void USharedWorldSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
	}
	Runtimes.Empty(); // joins worker threads (WorldSession dtor drains its queues)
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

FSharedWorldRuntime* USharedWorldSubsystem::FindOrCreateRuntime(const sw::WorldEntry& Entry)
{
	const FString WorldId = UE(Entry.WorldId);
	if (TUniquePtr<FSharedWorldRuntime>* Existing = Runtimes.Find(WorldId))
	{
		return Existing->Get();
	}
	auto Storage = sw::OpenWorldStorage(Entry, MakeEnvironment());
	if (!Storage)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=open_storage_failed world=%s reason=\"%s\""), *WorldId, *UE(Storage.Err().Describe()));
		return nullptr;
	}
	auto Runtime = MakeUnique<FSharedWorldRuntime>();
	Runtime->Entry = Entry;
	Runtime->Repository = Storage->Repository;
	Runtime->Objects = Storage->Objects;
	auto Clock = std::make_shared<sw::SystemClock>();
	auto Store = std::make_shared<sw::WorldStore>(Runtime->Repository, Entry.WorldId, Clock, sw::Logger(LogSink));
	Runtime->Leases = std::make_shared<sw::LeaseManager>(Store, sw::LeaseConfig{});
	const FString DataDir = FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("worlds"), WorldId);
	Runtime->Sync = std::make_shared<sw::SyncEngine>(Runtime->Objects, Runtime->Leases, sw::SyncConfig{Std(DataDir), 20});
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
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_save_failed reason=\"%s\""), *UE(R.Err().Describe()));
	}
}

void USharedWorldSubsystem::Play(const FString& WorldId)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		return;
	}
	if (!Runtime->Session)
	{
		sw::SessionConfig Cfg;
		Cfg.Me = MyIdentity();
		Cfg.SaveDirectory = Std(FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath()));
		Cfg.SaveName = "SharedWorld_" + Runtime->Entry.WorldId;
		Cfg.Versions.GameBuild = Std(FString::FromInt(FEngineVersion::Current().GetChangelist()));
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SharedWorld")))
		{
			Cfg.Versions.ModVersion = Std(Plugin->GetDescriptor().VersionName);
		}
		Runtime->Session = MakeUnique<sw::WorldSession>(Runtime->Leases, Runtime->Sync, Cfg);
	}
	ActiveWorldId = WorldId;
	Runtime->Session->Play();
	Runtime->Entry.LastPlayedAt = NowMs();
	(void)Settings.Upsert(Runtime->Entry); // same world id: replaces the stored entry
	SaveSettings();
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
	if (FSharedWorldRuntime* Runtime = FindRuntime(WorldId); Runtime && Runtime->Session)
	{
		Runtime->Session->Restore(Revision);
	}
}

void USharedWorldSubsystem::RequestMigrationTo(const FString& WorldId, const FString& SuccessorPlayerId)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || !Runtime->Session || !Host->IsHostingWorld(WorldId))
	{
		return;
	}
	sw::Identity Successor;
	for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
	{
		if (UE(P.PlayerId) == SuccessorPlayerId || UE(P.DisplayName) == SuccessorPlayerId)
		{
			Successor.PlayerId = P.PlayerId;
			Successor.DisplayName = P.DisplayName;
			Successor.InstallId = P.InstallId; // usually empty: see the WorldSession comment on PendingHandoff matching
			break;
		}
	}
	if (Successor.PlayerId.empty())
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=migration_target_not_found world=%s target=%s"), *WorldId, *SuccessorPlayerId);
		return;
	}
	Runtime->Session->RequestMigration(Successor);
}

FString USharedWorldSubsystem::AddExistingGitHubWorld(const FString& WorldId, const FString& DisplayName, const FString& Owner, const FString& Repo)
{
	sw::WorldEntry Entry;
	Entry.WorldId = Std(WorldId);
	Entry.DisplayName = Std(DisplayName);
	Entry.Provider.Kind = sw::ProviderKind::GitHub;
	Entry.Provider.Owner = Std(Owner);
	Entry.Provider.Repo = Std(Repo);
	Entry.AddedAt = static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000;
	if (sw::Status R = Settings.Upsert(Entry); !R)
	{
		return UE(R.Err().Describe());
	}
	SaveSettings();
	if (!FindOrCreateRuntime(Entry))
	{
		return TEXT("Could not open that world's storage.");
	}
	OnChanged.Broadcast();
	return FString();
}

FString USharedWorldSubsystem::CreateWorldFromSave(const FString& WorldId, const FString& DisplayName, const FString& SourceSavePath,
	const FString& Owner, const FString& Repo, bool bRestrictToMembers)
{
	sw::WorldEntry Entry;
	Entry.WorldId = Std(WorldId);
	Entry.DisplayName = Std(DisplayName);
	Entry.Provider.Kind = sw::ProviderKind::GitHub;
	Entry.Provider.Owner = Std(Owner);
	Entry.Provider.Repo = Std(Repo);
	Entry.AddedAt = static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000;
	if (sw::Status R = Entry.Provider.Validate(); !R)
	{
		return UE(R.Err().Describe());
	}
	FSharedWorldRuntime* Runtime = FindOrCreateRuntime(Entry);
	if (!Runtime)
	{
		return TEXT("Could not reach that GitHub repository.");
	}
	sw::CreateWorldParams Params;
	Params.Name = Std(DisplayName);
	Params.Creator = MyIdentity();
	Params.SourceSavePath = Std(SourceSavePath);
	Params.OriginalSaveName = Std(FPaths::GetBaseFilename(SourceSavePath));
	Params.Versions.GameBuild = Std(FString::FromInt(FEngineVersion::Current().GetChangelist()));
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SharedWorld")))
	{
		Params.Versions.ModVersion = Std(Plugin->GetDescriptor().VersionName);
	}
	Params.Settings.Name = Std(DisplayName);
	Params.bRestrictToMembers = bRestrictToMembers;
	auto Result = sw::CreateSharedWorld(*Runtime->Leases, *Runtime->Sync, Params);
	if (!Result)
	{
		return UE(Result.Err().Describe());
	}
	if (sw::Status R = Settings.Upsert(Entry); !R)
	{
		return UE(R.Err().Describe());
	}
	SaveSettings();
	OnChanged.Broadcast();
	return FString();
}

// ---------------------------------------------------------------- sign-in

void USharedWorldSubsystem::BeginGitHubSignIn()
{
	{
		FScopeLock Lock(&SignInMutex);
		if (SignIn.bInProgress)
		{
			return;
		}
		SignIn = FSharedWorldSignIn{};
		if (GitHubClientId[0] == '\0')
		{
			SignIn.Error = TEXT("GitHub sign-in is not configured for this build.");
			return;
		}
		SignIn.bInProgress = true;
	}
	// The worker only touches these shared objects, never `this`: the
	// subsystem may be destroyed while the player is still on github.com.
	std::shared_ptr<sw::IHttpClient> HttpRef = Http;
	std::shared_ptr<sw::ICredentialStore> CredsRef = Credentials;
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	auto Finish = [WeakThis](FString Error, std::string Login)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Error = MoveTemp(Error), Login = MoveTemp(Login)]()
		{
			USharedWorldSubsystem* S = WeakThis.Get();
			if (!S)
			{
				return;
			}
			{
				FScopeLock Lock(&S->SignInMutex);
				S->SignIn.bInProgress = false;
				S->SignIn.bDone = true;
				S->SignIn.Error = Error;
				S->SignIn.UserCode.Reset();
			}
			if (Error.IsEmpty())
			{
				S->Settings.GitHubLogin = Login; // display name only; the token is in the credential store
				S->SaveSettings();
				UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=github_signed_in login=%s"), *UE(Login));
			}
			S->OnChanged.Broadcast();
		});
	};
	Async(EAsyncExecution::Thread, [WeakThis, HttpRef, CredsRef, Finish]()
	{
		sw::SystemClock Clock;
		sw::GitHubDeviceFlow Flow(HttpRef, GitHubClientId, Clock);
		auto Code = Flow.Start();
		if (!Code)
		{
			Finish(UE(Code.Err().Message), std::string());
			return;
		}
		const FString UserCode = UE(Code->UserCode);
		const FString Uri = UE(Code->VerificationUri);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, UserCode, Uri]()
		{
			if (USharedWorldSubsystem* S = WeakThis.Get())
			{
				FScopeLock Lock(&S->SignInMutex);
				S->SignIn.UserCode = UserCode;
				S->SignIn.VerificationUri = Uri;
				S->OnChanged.Broadcast();
			}
		});
		sw::DeviceCode C = *Code;
		for (;;)
		{
			FPlatformProcess::Sleep(static_cast<float>(FMath::Max(C.IntervalSeconds, 1)));
			if (!WeakThis.IsValid())
			{
				return; // game shutting down: abandon quietly
			}
			auto Poll = Flow.Poll(C);
			if (!Poll)
			{
				Finish(UE(Poll.Err().Message), std::string());
				return;
			}
			C.IntervalSeconds = Poll->IntervalSeconds;
			switch (Poll->State)
			{
			case sw::DevicePoll::Pending:
			case sw::DevicePoll::SlowDown:
				continue;
			case sw::DevicePoll::Denied:
				Finish(TEXT("Sign-in was cancelled on GitHub."), std::string());
				return;
			case sw::DevicePoll::Expired:
				Finish(TEXT("The sign-in code expired. Try again."), std::string());
				return;
			case sw::DevicePoll::Authorized:
				break;
			}
			auto Login = Flow.FetchLogin(Poll->AccessToken);
			if (!Login)
			{
				Finish(UE(Login.Err().Message), std::string());
				return;
			}
			if (sw::Status W = CredsRef->Write(sw::GitHubCredentialKey, Poll->AccessToken); !W)
			{
				Finish(TEXT("Signed in, but Windows could not store the credential: ") + UE(W.Err().Message), std::string());
				return;
			}
			Finish(FString(), *Login);
			return;
		}
	});
}

FSharedWorldSignIn USharedWorldSubsystem::GetSignInStatus() const
{
	FScopeLock Lock(&SignInMutex);
	return SignIn;
}

void USharedWorldSubsystem::SignOutOfGitHub()
{
	(void)Credentials->Remove(sw::GitHubCredentialKey);
	Settings.GitHubLogin.clear();
	SaveSettings();
	OnChanged.Broadcast();
}

// ---------------------------------------------------------------- ticking

bool USharedWorldSubsystem::Tick(float)
{
	const int64 Now = static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000;
	for (auto& [Id, Runtime] : Runtimes)
	{
		if (Runtime->Session)
		{
			Runtime->Session->Tick(Now);
			HandleSessionTransition(*Runtime);
		}
	}
	const double Wall = FPlatformTime::Seconds();
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

TArray<FSharedWorldEntryView> USharedWorldSubsystem::GetWorldViews()
{
	TArray<FSharedWorldEntryView> Out;
	FScopeLock Lock(&SummaryMutex);
	for (const auto& [Id, Runtime] : Runtimes)
	{
		FSharedWorldEntryView V;
		V.WorldId = Id;
		const sw::WorldSummary& Sum = Runtime->LastSummary;
		V.WorldName = Sum.Name.empty() ? UE(Runtime->Entry.DisplayName) : UE(Sum.Name);
		V.CloudStatus = UE(sw::ToString(Sum.Status));
		V.HostName = UE(Sum.HostName);
		V.PlayerCount = Sum.Players.size();
		V.Revision = Sum.Revision;
		V.Generation = Sum.Generation;
		V.Problem = UE(Sum.Problem);
		if (Runtime->Session)
		{
			const sw::SessionView SV = Runtime->Session->View();
			V.LocalState = UE(sw::ToString(SV.State));
			V.LocalMessage = UE(SV.Message);
			V.bJoinReady = SV.State == sw::SessionState::JoinReady;
			if (SV.Error)
			{
				V.bHasError = true;
				V.ErrorCode = UE(SV.Error->Code);
				V.ErrorMessage = UE(SV.Error->Message);
				V.ErrorDetail = UE(SV.Error->Detail);
				V.bErrorRetryable = SV.Error->bRetryable;
			}
		}
		Out.Add(MoveTemp(V));
	}
	return Out;
}

void USharedWorldSubsystem::HandleSessionTransition(FSharedWorldRuntime& Runtime)
{
	using S = sw::SessionState;
	const sw::SessionView View = Runtime.Session->View();
	const FString WorldId = UE(Runtime.Entry.WorldId);
	const FString StateStr = UE(sw::ToString(View.State));
	const FString Prev = LastLocalStates.FindRef(WorldId);
	LastLocalStates.Add(WorldId, StateStr);

	if (View.State == S::ReadyToHost && !Host->IsBusy())
	{
		FString Error;
		if (!Host->BeginHosting(MenuWorld.Get(), Runtime.Session.Get(), WorldId, UE(View.SavePath)))
		{
			UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s"), *WorldId);
			Runtime.Session->OnWorldEnded();
		}
	}
	else if (View.State == S::Hosting)
	{
		if (Host->IsHostingWorld(WorldId))
		{
			Runtime.Session->SetPlayers(Host->GetConnectedPlayers());
		}
		else if (Host->GetWorldId() == WorldId && !Host->IsSaving())
		{
			// The game world already ended but the session is still trying to
			// upload/release: the last attempt was refused or the connection
			// dropped. Harmless to call every tick; a no-op once it lands.
			Host->RetryPendingRelease();
		}
	}
	else if (View.State == S::Migrating && Host->IsHostingWorld(WorldId) && !Host->IsSaving())
	{
		Host->SaveAndUpload(sw::SaveKind::Migration);
	}
	else if (View.State == S::Idle && Host->GetWorldId() == WorldId && Host->IsBusy())
	{
		Host->NotifyReleased();
	}
	else if (View.State == S::JoinReady && View.Join)
	{
		const FString Key = WorldId + TEXT("#") + FString::FromInt(static_cast<int32>(View.Generation));
		if (!JoinsStarted.Contains(Key))
		{
			JoinsStarted.Add(Key);
			ActiveWorldId = WorldId;
			FString Error;
			if (!Joiner->Join(MenuWorld.Get(), Runtime.Session.Get(), *View.Join, Error))
			{
				UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=join_failed world=%s reason=\"%s\""), *WorldId, *Error);
				Runtime.Session->OnJoinFailed(Std(Error));
			}
		}
	}
	if (Prev != StateStr)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=session_state world=%s from=%s to=%s"), *WorldId, *Prev, *StateStr);
	}
}

// ---------------------------------------------------------------- lifecycle

void USharedWorldSubsystem::OnMenuWorldReady(UWorld* World)
{
	MenuWorld = World;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC)
	{
		if (USharedWorldPanel* Panel = CreateWidget<USharedWorldPanel>(PC, USharedWorldPanel::StaticClass()))
		{
			Panel->AddToViewport(10);
		}
	}
	OnChanged.Broadcast();
}

void USharedWorldSubsystem::OnGameWorldReady(UWorld* World)
{
	MenuWorld.Reset();
	if (!World || ActiveWorldId.IsEmpty())
	{
		return;
	}
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
	Host->OnWorldTearDown(World);
	if (!ActiveWorldId.IsEmpty())
	{
		if (FSharedWorldRuntime* Runtime = FindRuntime(ActiveWorldId))
		{
			if (Runtime->Session && World && World->GetNetMode() == NM_Client && Runtime->Session->View().State == sw::SessionState::Joined)
			{
				// A client leaving normally is not a connection loss; nothing
				// to report - the world simply stops following the host.
			}
		}
	}
}

void USharedWorldSubsystem::OnNetworkFailure(UWorld* World, UNetDriver*, ENetworkFailure::Type, const FString&)
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
		Runtime->Session->OnHostConnectionLost();
	}
	else if (State == sw::SessionState::JoinReady)
	{
		Runtime->Session->OnJoinFailed("network failure while joining");
	}
}

// ---------------------------------------------------------------- chat surface

FString USharedWorldSubsystem::RequestCheckpoint()
{
	return Host->SaveAndUpload(sw::SaveKind::Checkpoint);
}

FString USharedWorldSubsystem::RequestStop()
{
	return Host->SaveAndUpload(sw::SaveKind::Final);
}

FString USharedWorldSubsystem::DescribeActiveSession() const
{
	for (const auto& [Id, Runtime] : Runtimes)
	{
		if (Runtime->Session && Runtime->Session->View().State != sw::SessionState::Idle)
		{
			const sw::SessionView V = Runtime->Session->View();
			return FString::Printf(TEXT("%s: %s (revision %lld). %s"), *UE(Runtime->Entry.DisplayName), *UE(sw::ToString(V.State)), V.Revision, *UE(V.Message));
		}
	}
	return TEXT("No shared world session is active.");
}

FString USharedWorldSubsystem::DescribeHistory(const FString& WorldId, int32 MaxCount)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		return TEXT("Unknown world.");
	}
	auto Snap = Runtime->Leases->Store().Load();
	if (!Snap)
	{
		return UE(Snap.Err().Describe());
	}
	auto History = Runtime->Sync->History(Snap->CommitId, static_cast<size_t>(FMath::Max(1, MaxCount)));
	if (!History)
	{
		return UE(History.Err().Describe());
	}
	FString Out;
	for (const sw::RevisionMeta& R : *History)
	{
		Out += FString::Printf(TEXT("rev %lld  gen %lld  %s  by %s\n"), R.Number, R.Generation, *UE(R.Reason), *UE(R.Uploader.DisplayName));
	}
	return Out.IsEmpty() ? TEXT("No revisions yet.") : Out;
}

FString USharedWorldSubsystem::InvitePlayer(const FString& WorldId, const FString& GitHubUsername)
{
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime || Runtime->Entry.Provider.Kind != sw::ProviderKind::GitHub)
	{
		return TEXT("This world is not stored on GitHub.");
	}
	sw::GitHubConfig Cfg;
	Cfg.Owner = Runtime->Entry.Provider.Owner;
	Cfg.Repo = Runtime->Entry.Provider.Repo;
	Cfg.WorldId = Runtime->Entry.WorldId;
	std::shared_ptr<sw::ICredentialStore> Creds = Credentials;
	Cfg.Token = [Creds]() -> sw::Result<std::string> { return Creds->Read(sw::GitHubCredentialKey); };
	auto R = sw::InviteGitHubCollaborator(Http, Cfg, Std(GitHubUsername));
	if (!R)
	{
		return UE(R.Err().Describe());
	}
	return *R ? TEXT("Invitation sent.") : TEXT("That player already has access.");
}

FString USharedWorldSubsystem::RecentLog(int32 MaxLines) const
{
	const std::vector<std::string> Lines = DiagnosticsSink->Lines();
	FString Out;
	const int32 Start = FMath::Max(0, static_cast<int32>(Lines.size()) - MaxLines);
	for (int32 i = Start; i < static_cast<int32>(Lines.size()); ++i)
	{
		Out += UE(Lines[static_cast<size_t>(i)]) + TEXT("\n");
	}
	return Out;
}
