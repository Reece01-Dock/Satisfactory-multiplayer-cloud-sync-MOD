#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SharedWorldSubsystem.generated.h"

class USharedWorldIPCClient;
class USharedWorldHostController;
class USharedWorldJoinManager;
class FJsonObject;

DECLARE_MULTICAST_DELEGATE(FOnSharedWorldChanged);

/**
 * The mod's single coordinator. It polls the helper for world status and the
 * local session, and reacts to session states by driving the game:
 *   READY_TO_HOST -> host controller loads the verified save
 *   JOIN_READY    -> join manager joins the published session
 * It never decides HOST vs JOIN itself; that is the helper's job.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Latest status of every configured shared world. */
	const TArray<FSharedWorldStatus>& GetWorlds() const { return Worlds; }
	/** Non-empty when the helper cannot be reached; shown instead of the world list. */
	FString GetConnectionProblem() const;
	/** Fired whenever world status or a local session changes. */
	FOnSharedWorldChanged OnChanged;

	/** The "Play Shared World" button. */
	void Play(const FString& WorldId);
	/** Dismiss an error or a finished join decision. */
	void Dismiss(const FString& WorldId);
	/** Cancel waiting for a host, or cancel hosting before the world loaded. */
	void Cancel(const FString& WorldId);

	/** In-game host controls (chat command). Return a message for the player. */
	FString RequestCheckpoint();
	FString RequestStop();
	FString DescribeActiveSession() const;

	/** Lifecycle notifications from the SML world modules. */
	void OnMenuWorldReady(UWorld* World);
	void OnGameWorldReady(UWorld* World);

	/** Used by the host controller to report game events to the helper. */
	void SendSessionEvent(const FString& WorldId, const FString& Event, const TSharedPtr<FJsonObject>& Body,
		TFunction<void(bool /*bOk*/, int32 /*HttpCode*/)> OnDone = nullptr);

	USharedWorldIPCClient* GetIPC() const { return IPC; }

private:
	bool Tick(float DeltaTime);
	void PollWorlds();
	void SendKeepalives();
	void HandleSession(const FSharedWorldSession& Session);
	void OnWorldBeginTearDown(UWorld* World);
	TSharedPtr<FJsonObject> MakePlayRequest() const;
	const FSharedWorldStatus* FindWorld(const FString& WorldId) const;

	static bool ParseSession(const TSharedPtr<FJsonObject>& Json, FSharedWorldSession& Out);
	static bool ParseStatus(const TSharedPtr<FJsonObject>& Json, FSharedWorldStatus& Out);

	UPROPERTY()
	TObjectPtr<USharedWorldIPCClient> IPC;
	UPROPERTY()
	TObjectPtr<USharedWorldHostController> Host;
	UPROPERTY()
	TObjectPtr<USharedWorldJoinManager> Joiner;

	TArray<FSharedWorldStatus> Worlds;
	/** Last session state seen per world, to act on transitions exactly once. */
	TMap<FString, FString> LastStates;
	/** Join attempts already started, keyed by world + host generation. */
	TSet<FString> JoinsStarted;

	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle TearDownHandle;
	double LastPoll = 0.0;
	double LastKeepalive = 0.0;
	bool bPollInFlight = false;
	TWeakObjectPtr<UWorld> MenuWorld;
};
