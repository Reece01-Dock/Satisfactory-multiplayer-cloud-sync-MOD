#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h" // ENetworkFailure
#include "SharedWorldCore/App/LocalSettings.h"
#include "SharedWorldCore/Providers/GitHubAuth.h"
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
	void RequestMigrationTo(const FString& WorldId, const FString& SuccessorPlayerId);

	/** Adds a world backed by an existing GitHub repository (owner/repo already created). */
	FString AddExistingGitHubWorld(const FString& WorldId, const FString& DisplayName, const FString& Owner, const FString& Repo);
	/** Converts the local save at SavePath into a brand-new Shared World. */
	FString CreateWorldFromSave(const FString& WorldId, const FString& DisplayName, const FString& SourceSavePath,
		const FString& Owner, const FString& Repo, bool bRestrictToMembers);
	std::vector<sw::WorldEntry> GetConfiguredWorlds() const { return Settings.Worlds; }
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
	FString DescribeHistory(const FString& WorldId, int32 MaxCount);
	FString InvitePlayer(const FString& WorldId, const FString& GitHubUsername);
	FString RecentLog(int32 MaxLines) const;

	/** Lifecycle notifications from the SML world modules. */
	void OnMenuWorldReady(UWorld* World);
	void OnGameWorldReady(UWorld* World);

private:
	FSharedWorldRuntime* FindOrCreateRuntime(const sw::WorldEntry& Entry);
	FSharedWorldRuntime* FindRuntime(const FString& WorldId);
	sw::Identity MyIdentity() const;
	sw::ProviderEnvironment MakeEnvironment() const;
	void SaveSettings();

	bool Tick(float DeltaTime);
	void RefreshSummariesAsync();
	void HandleSessionTransition(FSharedWorldRuntime& Runtime);
	void OnNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void OnWorldBeginTearDown(UWorld* World);

	sw::LocalSettings Settings;
	std::shared_ptr<sw::IHttpClient> Http;
	std::shared_ptr<sw::ICredentialStore> Credentials;
	std::shared_ptr<sw::ILogSink> LogSink;
	std::shared_ptr<sw::MemoryLogSink> DiagnosticsSink; // last N lines for the diagnostics command
	FString SettingsPath;
	bool bSettingsReadOnly = false;
	FString SettingsProblem;

	TMap<FString, TUniquePtr<FSharedWorldRuntime>> Runtimes;
	mutable FCriticalSection SummaryMutex;
	TMap<FString, FString> LastLocalStates; // edge-detection for HandleSessionTransition
	TSet<FString> JoinsStarted;
	bool bSummaryRefreshInFlight = false;

	UPROPERTY()
	TObjectPtr<USharedWorldHostController> Host;
	UPROPERTY()
	TObjectPtr<USharedWorldJoinManager> Joiner;

	/** The world currently being hosted or joined (only one game world at a time). */
	FString ActiveWorldId;

	mutable FCriticalSection SignInMutex;
	FSharedWorldSignIn SignIn;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle TearDownHandle;
	FDelegateHandle NetworkFailureHandle;
	TWeakObjectPtr<UWorld> MenuWorld;
	double LastSummaryRefresh = 0.0;
};
