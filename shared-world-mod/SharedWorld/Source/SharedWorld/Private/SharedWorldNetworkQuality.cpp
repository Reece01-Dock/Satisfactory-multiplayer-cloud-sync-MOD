#include "SharedWorldNetworkQuality.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerState.h"
#include "SharedWorldTypes.h"
#include "SharedWorldUeConvert.h"

TArray<FString> USharedWorldNetworkQuality::SampleWorld(UWorld* World, sw::PeerQualityMatrix& Matrix, int64 NowMs, const FString& LocalPlayerId)
{
	TArray<FString> Ids;
	const AGameStateBase* GS = World ? World->GetGameState() : nullptr;
	if (!GS)
	{
		return Ids;
	}

	struct PeerSample
	{
		FString Id;
		int32 PingMs = 9999;
	};
	TArray<PeerSample> Peers;
	for (const APlayerState* PS : GS->PlayerArray)
	{
		if (!PS) continue;
		PeerSample S;
		const FUniqueNetIdRepl NetId = PS->GetUniqueId();
		S.Id = NetId.IsValid() ? NetId.ToString() : PS->GetPlayerName();
		if (S.Id.IsEmpty()) continue;
		// ExactPing is engine-replicated RTT in ms (Satisfactory/UE PlayerState).
		S.PingMs = FMath::Max(0, FMath::RoundToInt(PS->ExactPing));
		Peers.Add(MoveTemp(S));
		Ids.AddUnique(S.Id);
	}
	if (Ids.Num() == 0) return Ids;

	std::vector<std::string> StdIds;
	for (const FString& Id : Ids) StdIds.push_back(SharedWorldUe::Std(Id));
	Matrix.SetPlayers(StdIds);

	const std::string Local = SharedWorldUe::Std(LocalPlayerId);
	for (const PeerSample& A : Peers)
	{
		for (const PeerSample& B : Peers)
		{
			if (A.Id == B.Id) continue;
			sw::LinkSample Link;
			// Direct A↔B RTT is not exposed by the engine; approximate with
			// each peer's ping-to-host when one endpoint is the local host.
			if (SharedWorldUe::Std(A.Id) == Local)
			{
				Link.RttMs = B.PingMs;
				Link.JitterMs = FMath::Clamp(B.PingMs / 10, 0, 100);
				Link.LossBp = B.PingMs > 200 ? 200 : 0;
				Link.bReachable = B.PingMs < 2000;
			}
			else if (SharedWorldUe::Std(B.Id) == Local)
			{
				Link.RttMs = A.PingMs;
				Link.JitterMs = FMath::Clamp(A.PingMs / 10, 0, 100);
				Link.LossBp = A.PingMs > 200 ? 200 : 0;
				Link.bReachable = A.PingMs < 2000;
			}
			else
			{
				// Unknown cross-link: treat as reachable with elevated RTT so
				// group scoring still prefers low host↔client pings.
				Link.RttMs = A.PingMs + B.PingMs;
				Link.JitterMs = FMath::Clamp((A.PingMs + B.PingMs) / 15, 0, 150);
				Link.LossBp = 0;
				Link.bReachable = true;
			}
			Matrix.Observe(SharedWorldUe::Std(A.Id), SharedWorldUe::Std(B.Id), Link, NowMs);
		}
	}
	Cached = Matrix;
	UE_LOG(LogSharedWorld, Verbose, TEXT("[SharedWorld/Migration] event=peer_quality_sampled players=%d"), Ids.Num());
	return Ids;
}

void USharedWorldNetworkQuality::ObserveVerifySample(const FString& HostPlayerId, int32 RttMs)
{
	if (HostPlayerId.IsEmpty() || RttMs < 0) return;
	LastVerifyHostId = HostPlayerId;
	LastVerifyRttMs = RttMs;
	sw::LinkSample Link;
	Link.RttMs = RttMs;
	Link.JitterMs = FMath::Clamp(RttMs / 10, 0, 100);
	Link.bReachable = RttMs < 5000;
	Link.LossBp = 0;
	// Pre-join: we only know local→host. Use a placeholder local id row.
	const std::string Local = "local";
	const std::string Host = SharedWorldUe::Std(HostPlayerId);
	std::vector<std::string> Players{Local, Host};
	Cached.SetPlayers(Players);
	Cached.Observe(Local, Host, Link, static_cast<sw::TimeMs>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/Verify] event=rtt_sample host=%s rtt_ms=%d"), *HostPlayerId, RttMs);
}
