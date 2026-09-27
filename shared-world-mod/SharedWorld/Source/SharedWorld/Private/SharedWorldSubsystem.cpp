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


namespace
{
	constexpr float TickIntervalSeconds = 1.0f;
	constexpr double SummaryRefreshSeconds = 5.0;
	constexpr const char* GitHubClientId = ""; // set by the mod's release build; empty disables sign-in

	std::string Std(const FString& S) { return TCHAR_TO_UTF8(*S); }
	FString ToFString(const std::string& S) { return UTF8_TO_TCHAR(S.c_str()); }

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
	DiagnosticsSink = std::make_shared<sw::MemoryLogSink>(500);
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
	ShuttingDown->store(true);
	if (Background)
	{
		Background->Shutdown(); // pending creation/history work is dropped; callbacks check WeakThis
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
	const FString WorldId = ToFString(Entry.WorldId);
	if (TUniquePtr<FSharedWorldRuntime>* Existing = Runtimes.Find(WorldId))
	{
		return Existing->Get();
	}
	auto Storage = sw::OpenWorldStorage(Entry, MakeEnvironment());
	if (!Storage)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=open_storage_failed world=%s reason=\"%s\""), *WorldId, *ToFString(Storage.Err().Describe()));
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
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=settings_save_failed reason=\"%s\""), *ToFString(R.Err().Describe()));
	}
}

sw::WorldSession& USharedWorldSubsystem::EnsureSession(FSharedWorldRuntime& Runtime)
{
	if (!Runtime.Session)
	{
		sw::SessionConfig Cfg;
		Cfg.Me = MyIdentity();
		Cfg.SaveDirectory = Std(FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath()));
		Cfg.SaveName = "SharedWorld_" + Runtime.Entry.WorldId;
		Cfg.Versions = MyVersions();
		Runtime.Session = MakeUnique<sw::WorldSession>(Runtime.Leases, Runtime.Sync, Cfg);
	}
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
	for (const sw::SessionPlayer& P : Host->GetConnectedPlayers())
	{
		if (P.PlayerId != Me.PlayerId && (ToFString(P.PlayerId) == Who || ToFString(P.DisplayName).Equals(Who, ESearchCase::IgnoreCase)))
		{
			Successor.PlayerId = P.PlayerId;
			Successor.DisplayName = P.DisplayName;
			Successor.InstallId = P.InstallId; // usually empty: see the WorldSession comment on PendingHandoff matching
			break;
		}
	}
	if (Successor.PlayerId.empty())
	{
		return FString::Printf(TEXT("No other connected player called %s."), *Who);
	}
	Runtime->Session->RequestMigration(Successor);
	// The successor needs write access to the storage to take over; if they
	// lack it their acquire fails and the handoff window expires, after which
	// normal takeover (any player with access) applies.
	return FString::Printf(TEXT("Handing the world to %s. Everyone reconnects once it is saved (they need storage access to host)."), *ToFString(Successor.DisplayName));
}

// ---------------------------------------------------------------- background work

void USharedWorldSubsystem::RunInBackground(TFunction<TPair<bool, FString>()> Work, TFunction<void(USharedWorldSubsystem&, bool, const FString&)> Then)
{
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	// Work must only capture shared_ptrs and values, never `this`.
	Background->Post([WeakThis, Work, Then]()
	{
		const TPair<bool, FString> Result = Work();
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Then, Result]()
		{
			if (USharedWorldSubsystem* Self = WeakThis.Get())
			{
				Then(*Self, Result.Key, Result.Value);
			}
		});
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

	TPair<bool, FString> Fail(const sw::Error& E) { return {false, ToFString(E.Message)}; }
}

void USharedWorldSubsystem::AddExistingWorld(const FString& WorldId, const FString& DisplayName, const sw::ProviderConfig& Provider, FDone OnDone)
{
	sw::WorldEntry Entry;
	Entry.WorldId = Std(WorldId.ToLower().TrimStartAndEnd());
	Entry.DisplayName = Std(DisplayName.IsEmpty() ? WorldId : DisplayName);
	Entry.Provider = Provider;
	Entry.AddedAt = NowMs();
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
	const FString SaveDir = FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath());
	const FString SourcePath = FPaths::Combine(SaveDir, SaveName + TEXT(".sav"));
	if (!FPaths::FileExists(SourcePath))
	{
		OnDone(false, TEXT("That save was not found."));
		return;
	}

	sw::WorldEntry Entry;
	Entry.WorldId = Std(MakeWorldId(Name));
	Entry.DisplayName = Std(Name);
	Entry.Provider = Provider;
	Entry.AddedAt = NowMs();
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
	FSharedWorldRuntime* Runtime = FindRuntime(WorldId);
	if (!Runtime)
	{
		return TEXT("Unknown world.");
	}
	if (Runtime->bCreating || Host->GetWorldId() == WorldId)
	{
		return TEXT("That world is busy.");
	}
	if (Runtime->Session)
	{
		const sw::SessionState St = Runtime->Session->View().State;
		if (St != sw::SessionState::Idle && St != sw::SessionState::Error)
		{
			return TEXT("Leave the world first.");
		}
	}
	// Only this PC's list changes: the world, its storage and local backups stay.
	Settings.Remove(Std(WorldId));
	SaveSettings();
	Runtimes.Remove(WorldId);
	LastLocalStates.Remove(WorldId);
	LastSequences.Remove(WorldId);
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
				UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=github_signed_in login=%s"), *ToFString(Login));
			}
			S->OnChanged.Broadcast();
		});
	};
	std::shared_ptr<std::atomic<bool>> Stop = ShuttingDown;
	Async(EAsyncExecution::Thread, [WeakThis, HttpRef, CredsRef, Finish, Stop]()
	{
		sw::SystemClock Clock;
		sw::GitHubDeviceFlow Flow(HttpRef, GitHubClientId, Clock);
		auto Code = Flow.Start();
		if (!Code)
		{
			Finish(ToFString(Code.Err().Message), std::string());
			return;
		}
		const FString UserCode = ToFString(Code->UserCode);
		const FString Uri = ToFString(Code->VerificationUri);
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
			if (Stop->load())
			{
				return; // game shutting down: abandon quietly
			}
			auto Poll = Flow.Poll(C);
			if (!Poll)
			{
				Finish(ToFString(Poll.Err().Message), std::string());
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
				Finish(ToFString(Login.Err().Message), std::string());
				return;
			}
			if (sw::Status W = CredsRef->Write(sw::GitHubCredentialKey, Poll->AccessToken); !W)
			{
				Finish(TEXT("Signed in, but Windows could not store the credential: ") + ToFString(W.Err().Message), std::string());
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
		V.WorldName = Sum.Name.empty() ? ToFString(Runtime->Entry.DisplayName) : ToFString(Sum.Name);
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
		V.bCreating = Runtime->bCreating;
		if (Runtime->bCreating)
		{
			V.LocalMessage = TEXT("Setting up...");
		}
		if (Runtime->Session)
		{
			const sw::SessionView SV = Runtime->Session->View();
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
		else if (Host->GetWorldId() == WorldId && !Host->IsSaving())
		{
			// The game world already ended but the session is still trying to
			// upload/release: the last attempt was refused or the connection
			// dropped. Harmless to call every tick; a no-op once it lands.
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
			// A connection loss has already moved the session to RECONNECTING
			// (OnNetworkFailure runs before teardown); still JOINED here means
			// the player left on purpose, so stop following the world.
			if (Runtime->Session && World && World->GetNetMode() == NM_Client && Runtime->Session->View().State == sw::SessionState::Joined)
			{
				Runtime->Session->OnLeftAsClient();
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
	else if (State == sw::SessionState::JoinReady && JoinsInFlight.Contains(ActiveWorldId))
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
			return FString::Printf(TEXT("%s [id %s]: %s (revision %lld). %s"), *ToFString(Runtime->Entry.DisplayName), *Id, *ToFString(sw::ToString(V.State)), V.Revision, *ToFString(V.Message));
		}
	}
	return TEXT("No shared world session is active.");
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
		return {true, *R ? TEXT("GitHub invitation sent. Once accepted, they can host this world.") : TEXT("They can already host this world.")};
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
