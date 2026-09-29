#pragma once
// Collects live peer quality samples from the hosted Satisfactory session
// and feeds SharedWorldCore's PeerQualityMatrix / HostElection.

#include "CoreMinimal.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "UObject/Object.h"
#include "SharedWorldNetworkQuality.generated.h"

class UWorld;

UCLASS()
class SHAREDWORLD_API USharedWorldNetworkQuality : public UObject
{
	GENERATED_BODY()

public:
	/** Samples connected players in World into Matrix at NowMs. Returns player ids sampled. */
	TArray<FString> SampleWorld(UWorld* World, sw::PeerQualityMatrix& Matrix, int64 NowMs, const FString& LocalPlayerId);

	/** Record a one-shot verify RTT toward HostPlayerId (pre-join probe). */
	void ObserveVerifySample(const FString& HostPlayerId, int32 RttMs);

	/** Last sampled matrix (copy). */
	const sw::PeerQualityMatrix& Matrix() const { return Cached; }

private:
	sw::PeerQualityMatrix Cached;
	FString LastVerifyHostId;
	int32 LastVerifyRttMs = 0;
};
