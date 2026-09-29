#pragma once
// Host-side Shared Worlds handshake responder.
//
// OnlineIntegration does not expose a connectionless Hello/Ack channel before
// join. This object tracks the authoritative local host Ack fields (lease
// generation, revision, published session id) so:
//   * /sharedworld verify can report readiness;
//   * post-join / local probes can obtain a real Ack from the lease holder;
//   * FSharedWorldUEHostVerifier can fill HostPlayerId / SessionId consistently.
//
// It never grants hosting authority — only the cloud lease does.

#include "CoreMinimal.h"
#include "SharedWorldCore/HostHandshake/HostHandshake.h"
#include "UObject/Object.h"
#include "SharedWorldHostResponder.generated.h"

class USharedWorldSubsystem;

UCLASS()
class SHAREDWORLD_API USharedWorldHostResponder : public UObject
{
	GENERATED_BODY()

public:
	void Init(USharedWorldSubsystem* InOwner);

	/** Clear when this process stops hosting. */
	void Clear();

	/** Called when the local host publishes a joinable session (HostReady). */
	void OnHostingReady(const FString& WorldId, const FString& HostPlayerId, int64 Generation, int64 Revision,
		const FString& SessionId, int32 CurrentPlayers, int32 MaxPlayers);

	/** True when this process is the ready Shared Worlds host for WorldId. */
	bool IsReadyHostFor(const FString& WorldId) const;

	/** Build an Ack for Hello when this process is the ready host; empty otherwise. */
	TOptional<sw::SharedWorldHelloAck> TryBuildAck(const sw::SharedWorldHello& Hello) const;

	FString Describe() const;

	const FString& GetWorldId() const { return WorldId; }
	const FString& GetSessionId() const { return SessionId; }
	int64 GetGeneration() const { return Generation; }
	int64 GetRevision() const { return Revision; }
	bool IsReady() const { return bReady; }

private:
	UPROPERTY()
	TObjectPtr<USharedWorldSubsystem> Owner;

	FString WorldId;
	FString HostPlayerId;
	FString SessionId;
	int64 Generation = 0;
	int64 Revision = 0;
	int32 CurrentPlayers = 0;
	int32 MaxPlayers = 4;
	bool bReady = false;
};
