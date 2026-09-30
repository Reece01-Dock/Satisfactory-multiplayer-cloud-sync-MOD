#pragma once

#include <atomic>
#include <memory>

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h" // ENetworkFailure
#include "SharedWorldCore/App/LocalSettings.h"
#include "SharedWorldCore/HostMigration/Diagnostics.h"
#include "SharedWorldCore/HostMigration/HostMigration.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "SharedWorldCore/Util/RefreshCache.h"
#include "SharedWorldCore/Util/TaskQueue.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "SharedWorldTypes.h"
#include "Services/SharedWorldCreationService.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "Services/SharedWorldInviteService.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SharedWorldSubsystem.generated.h"

class USharedWorldHostController;
class USharedWorldJoinManager;
class UNetDriver;

DECLARE_MULTICAST_DELEGATE(FOnSharedWorldChanged);

/**
 * Last cloud state document seen for one world, for game-thread readers.
 *
 * WorldStore::Load() is a network round trip (GitHub / folder). The migration
 * overlay needs "is a handoff in progress?" every second, so it reads this
 * cache and never calls Load() itself. USharedWorldSubsystem::RefreshCloudCache
 * refills it on the background queue. Shared by shared_ptr so an in-flight
 * refresh outlives the runtime that requested it.
 */
using FSharedWorldCloudCache = sw::RefreshCache<sw::StateSnapshot>;

/** Everything needed to run one configured world: its own storage, lease manager and session. */
struct FSharedWorldRuntime
{
	sw::WorldEntry Entry;
	std::shared_ptr<sw::IWorldRepository> Repository;
	std::shared_ptr<sw::IObjectStore> Objects;
	std::shared_ptr<sw::LeaseManager> Leases;
	std::shared_ptr<sw::SyncEngine> Sync;
	TUniquePtr<sw::WorldSession> Session;
	/** Continuous host ranking + migration state machine (same core as tests). */
	TUniquePtr<sw::HostMigrationEngine> HostMigration;
	/** Game-thread view of the cloud state; see FSharedWorldCloudCache. */
	std::shared_ptr<FSharedWorldCloudCache> CloudCache = std::make_shared<FSharedWorldCloudCache>();
	/** Set while CreateWorldFromSave runs: Play is refused. */
	bool bCreating = false;
	/** Refreshed on a background thread; read under USharedWorldSubsystem::SummaryMutex. */
	sw::WorldSummary LastSummary;
#if !UE_BUILD_SHIPPING
	/** Dev-only injected network / storage faults. */
	bool bDevStorageDisabled = false;
	int32 DevInjectedLatencyMs = 0;
	int32 DevInjectedLossBp = 0;
#endif
};

/** UI-facing GitHub link state (mirrors sw::GitHubAuthState). */
enum class ESharedWorldGitHubAuthState : uint8
{
	Disconnected,
	Starting,
	WaitingForUser,
	Authorizing,
	Connected,
	Expired,
	Denied,
	Error,
};

/** Snapshot for the Link GitHub / Settings UI. Tokens are never included. */
struct FSharedWorldSignIn
{
	ESharedWorldGitHubAuthState State = ESharedWorldGitHubAuthState::Disconnected;
	bool bInProgress = false;
	FString UserCode;
	FString VerificationUri;
	FString Error;
	FString PlayerMessage;
	FString Login;
	FString AvatarUrl;
	bool bDone = false;
	bool bConfigured = true;
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

	/** Result of a background operation, delivered on the game thread. */
	using FDone = TFunction<void(bool bOk, const FString& Message)>;

	/** Snapshot for the main-menu panel, safe to call every frame. */
	TArray<FSharedWorldEntryView> GetWorldViews();
	FOnSharedWorldChanged OnChanged;

	/** Player-facing services (owned by the subsystem). */
	class FSharedWorldDiscoveryService& Discovery();
	class FSharedWorldInviteService& Invites();
	class FSharedWorldCreationService& Creation();

	/** Kick a lightweight cloud refresh without freezing the UI. */
	void RequestDiscoveryRefresh();
	TArray<struct FSharedWorldPendingInviteView> GetPendingInviteViews() const;
	bool NeedsWelcomeStorageConnect() const;
	void MarkWelcomeDone();
	bool IsWelcomeDone() const;
	void PreferGitHubCloudStorageIfConnected();
	/** Worlds created while storage was "local folder" are moved to GitHub when linked. */
	void UpgradeLegacyFolderWorldsToGitHubIfConnected();
	sw::ProviderConfig ResolveDefaultStorage(FString& OutNote) const;
	const sw::WorldEntry* FindWorldEntry(const FString& WorldId) const;
	FString EnsureInviteCode(const FString& WorldId);
	void QueueIncomingInvite(const FString& WorldId, const FString& WorldName, const FString& FromPlayerId, const FString& FromDisplayName, const sw::ProviderConfig& Provider);
	/** Soft-add a world pushed over the network by the host (no share code / no storage verify). */
	void ReceivePushedWorldInvite(const FString& WorldId, const FString& WorldName, const FString& FromDisplayName, const FString& ProviderKind, const FString& OwnerOrPath, const FString& Repo);
	/** Host: multicast this world's provider config to a connected player so they auto-add it. */
	bool PushWorldInviteToConnectedPlayer(const FString& TargetPlayerId, const FString& WorldId);
	void AcceptPendingInvite(const FString& InviteId, FDone OnDone);
	void DeclinePendingInvite(const FString& InviteId, FDone OnDone);
	void JoinUsingShareCode(const FString& Code, FDone OnDone);
	/** Convert the active in-game save into a Shared World (Manage Session path). */
	void CreateWorldFromCurrentSession(const FString& DisplayName, FDone OnDone);

	/** The "Play Shared World" button. */
	void Play(const FString& WorldId);
	void Dismiss(const FString& WorldId);
	void Cancel(const FString& WorldId);
	void Restore(const FString& WorldId, int64 Revision);
	FString RequestMigrationTo(const FString& WorldId, const FString& Who);

	/**
	 * Most recently played Shared World id (from local settings), or empty.
	 * Intended for Satisfactory Continue once that menu can be hooked safely:
	 * Continue → Play(GetMostRecentlyPlayedWorldId()) instead of a stale local save.
	 */
	FString GetMostRecentlyPlayedWorldId() const;
	/** If a Shared World was played more recently than 0, invokes Play on it. Returns false if none. */
	bool TryContinueLastSharedWorld();
	/** True when a FG save name belongs to this mod (`SharedWorld_<id>`), for Load-menu protection. */
	static bool IsSharedWorldSaveName(const FString& SaveName);
	/** World id embedded in a SharedWorld_ save name, or empty. */
	static FString WorldIdFromSaveName(const FString& SaveName);

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
	/** Steam/Epic id for the local player (empty until online identity is ready). */
	FString GetLocalPlayerId() const;
	/** Players currently connected to the hosted game (empty if not hosting). */
	TArray<FSharedWorldFriendInfo> GetConnectedSessionPlayers() const;
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

	/**
	 * Development diagnostics: host ranking, peer latency matrix, migration phase.
	 * Safe to call every frame; returns a plain-text snapshot.
	 */
	FString GetHostMigrationDiagnostics(const FString& WorldId);

#if !UE_BUILD_SHIPPING
	/** Dev-only fault injection for two/three-instance Satisfactory tests. */
	FString DevInject(const FString& WorldId, const FString& Action);
#endif

	// ---- GitHub sign-in (device flow; no gh CLI / PAT / SSH)
	void BeginGitHubSignIn();
	void CancelGitHubSignIn();
	void TestGitHubAccess(FDone OnDone);
	FSharedWorldSignIn GetSignInStatus() const;
	FString GetGitHubLogin() const { return UTF8_TO_TCHAR(Settings.GitHubLogin.c_str()); }
	void SignOutOfGitHub();
	/** True when this build has a GitHub OAuth client id (env or packaged). */
	bool IsGitHubAuthConfigured() const;

	// ---- chat command surface (see SharedWorldChatCommand)
	FString RequestCheckpoint();
	FString RequestStop();
	FString DescribeActiveSession() const;
	/**
	 * Non-mutating host reachability probe for /sharedworld verify. Runs off the
	 * game thread (cloud read + session resolve); OnDone runs on the game thread.
	 */
	void DebugVerifyHost(const FString& WorldId, FDone OnDone);
	/** HostController → HostResponder when the session is published. */
	void NotifyHostResponderReady(const FString& WorldId, const FString& SessionId, int64 Generation, int64 Revision, int32 PlayerCount);
	void NotifyHostResponderCleared();
	/** The world being hosted or joined, else "" (chat commands default to it). */
	const FString& GetActiveWorldId() const { return ActiveWorldId; }
	FString RecentLog(int32 MaxLines) const;

	/** Lifecycle notifications from the SML world modules (and our menu fallback). */
	void OnMenuWorldReady(UWorld* World);
	void OnGameWorldReady(UWorld* World);

	/** Full-screen migration / recovery overlay driven by HostMigrationEngine. */
	void EnsureMigrationOverlay(UWorld* World);
	void UpdateMigrationOverlay(const FString& WorldId);
	void HideMigrationOverlay();
	/** True while crash recovery / successor takeover should keep the MW2-style overlay. */
	bool IsHostMigrationInFlight(const FString& WorldId) const;

private:
	void HandleActorsInitialized(const UWorld::FActorsInitializedParams& Params);
	void RetryShowMenuPanel();
	bool TryShowMenuPanel(UWorld* World);
	FSharedWorldRuntime* FindOrCreateRuntime(const sw::WorldEntry& Entry);
	FSharedWorldRuntime* FindRuntime(const FString& WorldId);
	/** Re-open repository/object store when settings provider changed (e.g. folder → GitHub). */
	bool RebindRuntimeStorage(FSharedWorldRuntime& Runtime);
	sw::WorldSession& EnsureSession(FSharedWorldRuntime& Runtime);
	sw::Identity MyIdentity() const;
	sw::ProviderEnvironment MakeEnvironment() const;
	sw::LocalVersions MyVersions() const;
	void SaveSettings();
	/** Runs Work on the background queue; Then runs on the game thread if the subsystem still exists. */
	/** Starts a background refill of Runtime.CloudCache when it is stale (never blocks). */
	void RefreshCloudCache(FSharedWorldRuntime& Runtime) const;
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
	sw::SystemClock AuthClock; // must outlive GitHubAuth
	std::shared_ptr<sw::GitHubAuthService> GitHubAuth;
	std::shared_ptr<sw::ILogSink> LogSink;
	std::shared_ptr<sw::MemoryLogSink> DiagnosticsSink; // last N lines for the diagnostics command
	FString SettingsPath;
	bool bSettingsReadOnly = false;
	FString SettingsProblem;

	TUniquePtr<class FSharedWorldDiscoveryService> DiscoveryService;
	TUniquePtr<class FSharedWorldInviteService> InviteService;
	TUniquePtr<class FSharedWorldCreationService> CreationService;

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
	UPROPERTY()
	TObjectPtr<class USharedWorldNetworkQuality> NetworkQuality;
	UPROPERTY()
	TObjectPtr<class USharedWorldHostResponder> HostResponder;

	/** The world currently being hosted or joined (only one game world at a time). */
	FString ActiveWorldId;
	/** Set by a planned migration: after release, go to the menu and join the successor. */
	FString PendingRejoinWorldId;
	bool bReturningToMenu = false;

	mutable FCriticalSection SignInMutex;
	FSharedWorldSignIn SignIn;
	std::shared_ptr<std::atomic<bool>> SignInCancel = std::make_shared<std::atomic<bool>>(false);

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle TearDownHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle ActorsInitializedHandle;
	TWeakObjectPtr<UWorld> MenuWorld;
	TWeakObjectPtr<class USharedWorldPanel> MenuPanel;
	TWeakObjectPtr<class USharedWorldMigrationOverlay> MigrationOverlay;
	FString LastOverlayMessage;
	double LastSummaryRefresh = 0.0;
	double LastPauseInjectAttempt = 0.0;
};
