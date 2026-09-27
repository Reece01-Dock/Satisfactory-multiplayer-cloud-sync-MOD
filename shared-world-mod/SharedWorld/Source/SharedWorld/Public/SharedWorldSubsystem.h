#pragma once

#include <atomic>
#include <memory>

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h" // ENetworkFailure
#include "SharedWorldCore/App/LocalSettings.h"
#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "SharedWorldCore/Util/TaskQueue.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "SharedWorldTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SharedWorldSubsystem.generated.h"

class USharedWorldHostController;
class USharedWorldJoinManager;
class UNetDriver;

DECLARE_MULTICAST_DELEGATE(FOnSharedWorldChanged);

/** Everything needed to run one configured world: its own storage, lease manager and session. */
struct FSharedWorldRuntime
{
	sw::WorldEntry Entry;
	std::shared_ptr<sw::IWorldRepository> Repository;
	std::shared_ptr<sw::IObjectStore> Objects;
	std::shared_ptr<sw::LeaseManager> Leases;
	std::shared_ptr<sw::SyncEngine> Sync;
	TUniquePtr<sw::WorldSession> Session;
	/** Set while CreateWorldFromSave runs: Play is refused. */
	bool bCreating = false;
	/** Refreshed on a background thread; read under USharedWorldSubsystem::SummaryMutex. */
	sw::WorldSummary LastSummary;
};

/** State of an in-progress "sign in with GitHub" device-flow attempt. */
struct FSharedWorldSignIn
{
	bool bInProgress = false;
	FString UserCode;
	FString VerificationUri;
	FString Error;
	bool bDone = false;
};

/**
 * The mod's single coordinator. It runs SharedWorldCore natively in-process
 * (no helper, no local server): one sw::WorldSession per configured world,
 * ticked from the game thread, driving the game when a session reaches
 * READY_TO_HOST / JOIN_READY / MIGRATING.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Snapshot for the main-menu panel, safe to call every frame. */
	TArray<FSharedWorldEntryView> GetWorldViews();
	FOnSharedWorldChanged OnChanged;

	/** The "Play Shared World" button. */
	void Play(const FString& WorldId);
	void Dismiss(const FString& WorldId);
	void Cancel(const FString& WorldId);
	void Restore(const FString& WorldId, int64 Revision);
	/** Planned migration to a connected player (name or id). Returns a player message. */
	FString RequestMigrationTo(const FString& WorldId, const FString& Who);

	/** Result of a background operation, delivered on the game thread. */
	using FDone = TFunction<void(bool bOk, const FString& Message)>;

	/** Adds a world that already exists in Provider (verified before it is listed). */
	void AddExistingWorld(const FString& WorldId, const FString& DisplayName, const sw::ProviderConfig& Provider, FDone OnDone);
	/** Converts the local save SaveName (in the game's save directory) into a new Shared World. The original save is not modified. */
	void CreateWorldFromSave(const FString& DisplayName, const FString& SaveName, const sw::ProviderConfig& Provider, bool bRestrictToMembers, FDone OnDone);
	/** Removes the world from this PC's list only. Refused while a session for it is active. */
	FString ForgetWorld(const FString& WorldId);
	std::vector<sw::WorldEntry> GetConfiguredWorlds() const { return Settings.Worlds; }

	// ---- history / players (background; results as player-facing text)
	void FetchHistory(const FString& WorldId, int32 MaxCount, FDone OnDone);
	void FetchPlayers(const FString& WorldId, FDone OnDone);
	/**
	 * Membership is keyed by the game's Steam/Epic account ids: Who is a
	 * connected player's name or id (host), or a raw player id.
	 */
	void AllowPlayer(const FString& WorldId, const FString& Who, sw::Role Role, FDone OnDone);
	void RemovePlayer(const FString& WorldId, const FString& Who, FDone OnDone);
	void SetOpenMembership(const FString& WorldId, bool bOpen, FDone OnDone);
	/**
	 * Hosting needs write access to the world's storage, which Steam/Epic
	 * cannot grant: for GitHub storage this invites the friend's GitHub
	 * account as a collaborator. Players without it can still join games.
	 */
	void GrantHosting(const FString& WorldId, const FString& GitHubUsername, FDone OnDone);

	/** Non-empty when the local world list could not be loaded (shown above the list). */
	const FString& GetSettingsProblem() const { return SettingsProblem; }

	// ---- GitHub sign-in (device flow)
	void BeginGitHubSignIn();
	FSharedWorldSignIn GetSignInStatus() const;
	FString GetGitHubLogin() const { return UTF8_TO_TCHAR(Settings.GitHubLogin.c_str()); }
	void SignOutOfGitHub();

	// ---- chat command surface (see SharedWorldChatCommand)
	FString RequestCheckpoint();
	FString RequestStop();
	FString DescribeActiveSession() const;
	/** The world being hosted or joined, else "" (chat commands default to it). */
	const FString& GetActiveWorldId() const { return ActiveWorldId; }
	FString RecentLog(int32 MaxLines) const;

	/** Lifecycle notifications from the SML world modules. */
	void OnMenuWorldReady(UWorld* World);
	void OnGameWorldReady(UWorld* World);

private:
	FSharedWorldRuntime* FindOrCreateRuntime(const sw::WorldEntry& Entry);
	FSharedWorldRuntime* FindRuntime(const FString& WorldId);
	sw::WorldSession& EnsureSession(FSharedWorldRuntime& Runtime);
	sw::Identity MyIdentity() const;
	sw::ProviderEnvironment MakeEnvironment() const;
	sw::LocalVersions MyVersions() const;
	void SaveSettings();
	/** Runs Work on the background queue; Then runs on the game thread if the subsystem still exists. */
	void RunInBackground(TFunction<TPair<bool, FString>()> Work, TFunction<void(USharedWorldSubsystem&, bool, const FString&)> Then);
	/** Resolves a connected player's name/id to a platform player id (or returns Who). */
	FString ResolvePlayerId(const FString& Who, FString& OutDisplayName) const;

	bool Tick(float DeltaTime);
	void RefreshSummariesAsync();
	void HandleSessionTransition(FSharedWorldRuntime& Runtime);
	void OnNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void OnWorldBeginTearDown(UWorld* World);
	void ReturnToMainMenu(const FText& Reason);

	sw::LocalSettings Settings;
	std::shared_ptr<sw::IHttpClient> Http;
	std::shared_ptr<sw::ICredentialStore> Credentials;
	std::shared_ptr<sw::ILogSink> LogSink;
	std::shared_ptr<sw::MemoryLogSink> DiagnosticsSink; // last N lines for the diagnostics command
	FString SettingsPath;
	bool bSettingsReadOnly = false;
	FString SettingsProblem;

	TMap<FString, TUniquePtr<FSharedWorldRuntime>> Runtimes;
	/** Creation, history, membership and invites: network I/O kept off the game thread. */
	TUniquePtr<sw::SerialQueue> Background;
	/** Read by long-running workers (sign-in polling) that must not touch `this`. */
	std::shared_ptr<std::atomic<bool>> ShuttingDown = std::make_shared<std::atomic<bool>>(false);
	mutable FCriticalSection SummaryMutex;
	TMap<FString, FString> LastLocalStates; // edge-detection for HandleSessionTransition
	TMap<FString, uint64> LastSequences;    // SessionView::Sequence last broadcast
	TSet<FString> JoinsInFlight; // worlds whose join attempt the game is running
	bool bSummaryRefreshInFlight = false;

	UPROPERTY()
	TObjectPtr<USharedWorldHostController> Host;
	UPROPERTY()
	TObjectPtr<USharedWorldJoinManager> Joiner;

	/** The world currently being hosted or joined (only one game world at a time). */
	FString ActiveWorldId;
	/** Set by a planned migration: after release, go to the menu and join the successor. */
	FString PendingRejoinWorldId;
	bool bReturningToMenu = false;

	mutable FCriticalSection SignInMutex;
	FSharedWorldSignIn SignIn;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle TearDownHandle;
	FDelegateHandle NetworkFailureHandle;
	TWeakObjectPtr<UWorld> MenuWorld;
	double LastSummaryRefresh = 0.0;
};
