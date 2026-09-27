#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "UObject/Object.h"
#include "SharedWorldHostController.generated.h"

class USharedWorldSubsystem;

/**
 * Game-side half of hosting. The helper has already acquired the lease and
 * placed a verified save; this class:
 *   - loads that save through UFGSaveSystem::LoadSaveFile
 *   - publishes the online session id once the hosted session exists
 *   - writes the shared save via UFGSaveSystem::SaveGame and only reports
 *     it to the helper from the save-complete callback, so the helper never
 *     uploads a save the game is still writing
 *   - makes periodic checkpoints and a final upload when hosting stops
 */
UCLASS()
class SHAREDWORLD_API USharedWorldHostController : public UObject
{
	GENERATED_BODY()

public:
	void Init(USharedWorldSubsystem* InOwner);

	/** Loads the placed save and travels into it as host. */
	bool BeginHosting(UWorld* MenuWorld, const FString& WorldId, const FString& SaveName, FString& OutError);

	void OnGameWorldReady(UWorld* World);
	void OnWorldTearDown(UWorld* World);

	/** True from BeginHosting until the session is released or abandoned. */
	bool IsBusy() const { return !WorldId.IsEmpty(); }
	bool IsHostingWorld(const FString& InWorldId) const { return bInGameWorld && WorldId == InWorldId; }
	bool NeedsFinalUpload(const FString& InWorldId) const { return bFinalUploadPending && WorldId == InWorldId; }

	/** Saves the shared slot and uploads it; bFinal also releases the world. Returns a player message. */
	FString SaveAndUpload(bool bFinal);
	/** Reports the last written shared save as final (used after the world has already ended). */
	void SendFinalUpload();

	TArray<FSharedWorldPlayer> GetPlayers() const;

private:
	bool TickPublishSession(float);
	bool TickCheckpoint(float);
	FString FindOnlineSessionId() const;
	void PublishSession(const FString& SessionId);
	void Reset();

	UFUNCTION()
	void OnSaveComplete(bool bSuccess, const FText& ErrorMessage);

	UPROPERTY()
	TObjectPtr<USharedWorldSubsystem> Owner;

	TWeakObjectPtr<UWorld> GameWorld;
	FString WorldId;
	FString SaveName;
	bool bInGameWorld = false;
	bool bSessionPublished = false;
	bool bSaving = false;
	bool bSaveIsFinal = false;
	bool bFinalUploadPending = false;
	bool bFinalUploadInFlight = false;
	double PublishDeadline = 0.0;
	FTSTicker::FDelegateHandle PublishTicker;
	FTSTicker::FDelegateHandle CheckpointTicker;
};
