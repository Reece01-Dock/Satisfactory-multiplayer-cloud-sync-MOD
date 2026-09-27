#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/World/WorldSession.h"
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
 *
 * Failures before the game takes over are reported to the session
 * (OnJoinFailed); a successful join is reported by the subsystem when the
 * client world is ready (OnJoinedAsClient).
 */
UCLASS()
class SHAREDWORLD_API USharedWorldJoinManager : public UObject
{
	GENERATED_BODY()

public:
	void Init(USharedWorldSubsystem* InOwner);

	/** Starts joining. Returns false (with a reason) if the join could not be started. */
	bool Join(UWorld* MenuWorld, sw::WorldSession* InSession, const sw::JoinInfo& JoinInfo, FString& OutError);

private:
	void OnSessionResolved(USessionInformation* Found);

	UFUNCTION()
	void OnJoinResponse(USessionMigrationSequence* Sequence);

	UPROPERTY()
	TObjectPtr<USharedWorldSubsystem> Owner;

	TWeakObjectPtr<UWorld> World;
	sw::WorldSession* Session = nullptr; // owned by the subsystem; outlives the join attempt
	void Fail(const FString& Reason);
};
