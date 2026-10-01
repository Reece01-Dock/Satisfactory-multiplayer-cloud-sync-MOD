#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "UObject/Object.h"
#include "SharedWorldHostController.generated.h"

class ASharedWorldInviteBridge;
class USharedWorldSubsystem;

/**
 * Game-side half of hosting. The session has already acquired the lease and
 * placed a verified save; this class:
 *   - loads that save through UFGSaveSystem::LoadSaveFile
 *   - publishes the online session id once the hosted session exists
 *   - writes the shared save via UFGSaveSystem::SaveGame and only reports it
 *     to the session from the save-complete callback, so the session never
 *     uploads a save the game is still writing
 *   - makes periodic checkpoints and reports a final/migration save when
 *     hosting stops or a planned migration is in progress
 */
UCLASS()
class SHAREDWORLD_API USharedWorldHostController : public UObject
{
	GENERATED_BODY()

public:
	void Init(USharedWorldSubsystem* InOwner);

	/** Loads the placed save and travels into it as host. */
	bool BeginHosting(UWorld* MenuWorld, sw::WorldSession* InSession, const FString& InWorldId, const FString& SavePath);

	void OnGameWorldReady(UWorld* World);
	void OnWorldTearDown(UWorld* World);
	/** Esc/pause opened while hosting: write a disk checkpoint before Exit to Menu. */
	void OnPauseMenuOpened();
	/** Poll FG pause menu open edge; call from subsystem tick while hosting. */
	void PollPauseMenu();

	/** True from BeginHosting until the session releases (NotifyReleased) or hosting is abandoned. */
	bool IsBusy() const { return !WorldId.IsEmpty(); }
	bool IsHostingWorld(const FString& InWorldId) const { return bInGameWorld && WorldId == InWorldId; }
	bool IsSaving() const { return bSaving; }
	const FString& GetWorldId() const { return WorldId; }
	/** BeginHosting ran but the game world never appeared (load failed or was cancelled). */
	bool LoadTimedOut() const { return IsBusy() && !bInGameWorld && !bWorldEnded && LoadStartedAt > 0.0 && FPlatformTime::Seconds() - LoadStartedAt > 300.0; }

	/** Saves the shared slot and reports Kind once the save completes. Returns a player message. */
	FString SaveAndUpload(sw::SaveKind Kind);
	/**
	 * The game world already tore down (OnWorldTearDown already told the
	 * session so once) but the session is still Hosting: the upload was
	 * refused or the connection dropped. Asks the session to try again;
	 * harmless to call repeatedly, and a no-op once the session has moved on.
	 */
	void RetryPendingRelease();
	/** The session reached IDLE for this world: release everything held for it. */
	void NotifyReleased() { Reset(); }

	/** Drop tickers / invite bridge / state (also used on subsystem teardown). */
	void Reset();

	/** Currently connected players, best-effort identity (InstallId is only known for this machine's own player). */
	std::vector<sw::SessionPlayer> GetConnectedPlayers() const;

	/** Always-relevant actor used to push world invites to connected clients. */
	ASharedWorldInviteBridge* EnsureInviteBridge();

private:
	bool TickPublishSession(float);
	bool TickCheckpoint(float);
	FString FindOnlineSessionId() const;
	void PublishSession(const FString& SessionId);
	UFUNCTION()
	void OnSaveComplete(bool bSuccess, const FText& ErrorMessage);

	UPROPERTY()
	TObjectPtr<USharedWorldSubsystem> Owner;

	UPROPERTY()
	TWeakObjectPtr<ASharedWorldInviteBridge> InviteBridge;

	sw::WorldSession* Session = nullptr; // owned by the subsystem's runtime map; outlives this controller's use of it
	TWeakObjectPtr<UWorld> GameWorld;
	FString WorldId;
	FString SaveName;
	bool bInGameWorld = false;
	bool bSessionPublished = false;
	bool bSaving = false;
	sw::SaveKind SavingKind = sw::SaveKind::Checkpoint;
	double PublishDeadline = 0.0;
	double LoadStartedAt = 0.0;
	double LastPauseSaveAt = 0.0;
	double TearDownAt = 0.0;
	bool bWorldEnded = false; // the hosted world tore down; waiting for the release
	bool bPauseMenuWasOpen = false;
	FTSTicker::FDelegateHandle PublishTicker;
	FTSTicker::FDelegateHandle CheckpointTicker;
};
