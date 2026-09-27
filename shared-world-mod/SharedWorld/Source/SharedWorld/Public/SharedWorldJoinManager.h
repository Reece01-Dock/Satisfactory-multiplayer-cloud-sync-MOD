#pragma once

#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "UObject/Object.h"
#include "SharedWorldJoinManager.generated.h"

class USharedWorldSubsystem;
class USessionInformation;
class USessionMigrationSequence;

/**
 * Joins the session the host published, using the game's own
 * OnlineIntegration APIs (see docs/research.md, "Join flow"):
 *
 *   online-session-id: UCommonSessionSubsystem::MakeOnlineSessionId
 *                      -> ResolveOnlineSession -> UCommonSessionStatics::JoinSession
 *   address:           UCommonSessionSubsystem::CreateSessionJoiningSequence
 *                      with FSessionJoinParams::RawAddress
 *
 * Both entry points are exported from the game's headers; their runtime
 * behaviour with a session id obtained on another machine still has to be
 * confirmed in game (STATUS.md, blockers).
 */
UCLASS()
class SHAREDWORLD_API USharedWorldJoinManager : public UObject
{
	GENERATED_BODY()

public:
	void Init(USharedWorldSubsystem* InOwner);

	/** Starts joining. Returns false (with a reason) if the join could not be started. */
	bool Join(UWorld* MenuWorld, const FSharedWorldJoinInfo& JoinInfo, FString& OutError);

private:
	void OnSessionResolved(USessionInformation* Session);

	UFUNCTION()
	void OnJoinResponse(USessionMigrationSequence* Sequence);

	UPROPERTY()
	TObjectPtr<USharedWorldSubsystem> Owner;

	TWeakObjectPtr<UWorld> World;
};
