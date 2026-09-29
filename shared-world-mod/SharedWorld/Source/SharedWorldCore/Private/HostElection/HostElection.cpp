#include "SharedWorldCore/HostElection/HostElection.h"

#include <algorithm>

namespace sw
{
	const char* ToString(SuccessorReadiness R)
	{
		return R == SuccessorReadiness::Ready ? "SUCCESSOR READY" : "SUCCESSOR NOT READY";
	}

	int ScoreCurve(int Value, int Ideal, int Bad, int Weight)
	{
		if (Weight <= 0) return 0;
		if (Value <= Ideal) return Weight;
		if (Value >= Bad || Bad <= Ideal) return 0;
		const int Span = Bad - Ideal;
		const int Over = Value - Ideal;
		return static_cast<int>((static_cast<int64_t>(Weight) * (Span - Over)) / Span);
	}

	HostNetworkSummary NetworkFromPing(int PingMs)
	{
		HostNetworkSummary N;
		N.AvgRttMs = PingMs;
		N.MedianRttMs = PingMs;
		N.WorstRttMs = PingMs;
		N.ClientCount = 1;
		N.bAllReachable = PingMs < 9000;
		return N;
	}

	static HostNetworkSummary ResolveNetwork(const HostCandidate& C)
	{
		if (C.Network) return *C.Network;
		return NetworkFromPing(C.PingMs);
	}

	bool IsSuccessorReady(const HostCandidate& C)
	{
		return C.bConnected && C.bHostEligible && C.bCompatible && C.bStorageReachable && C.bSessionCapable &&
			C.bUploadSufficient && !C.bRecentlyUnstable;
	}

	HostScoreBreakdown ScoreHost(const HostCandidate& C, const HostScoreWeights& W, bool bEmergency)
	{
		HostScoreBreakdown B;
		const HostNetworkSummary N = ResolveNetwork(C);

		if (!C.bConnected)
		{
			B.DisqualifyReason = "not connected";
			return B;
		}
		if (!C.bHostEligible)
		{
			B.DisqualifyReason = "not host eligible";
			return B;
		}
		if (!C.bCompatible)
		{
			B.DisqualifyReason = "incompatible game/mod version";
			return B;
		}
		if (!C.bStorageReachable)
		{
			B.DisqualifyReason = "storage unreachable";
			return B;
		}
		if (!C.bSessionCapable)
		{
			B.DisqualifyReason = "cannot create multiplayer session";
			return B;
		}
		if (!C.bUploadSufficient || (N.UploadKbps > 0 && N.UploadKbps < W.MinUploadKbps))
		{
			B.DisqualifyReason = "insufficient upload";
			return B;
		}
		if (!N.bAllReachable && N.ClientCount > 0)
		{
			B.DisqualifyReason = "unreachable to some clients";
			return B;
		}

		B.bEligible = true;
		B.Latency = ScoreCurve(N.AvgRttMs, W.IdealRttMs, W.BadRttMs, W.LatencyWeight);
		B.WorstClient = ScoreCurve(N.WorstRttMs, W.IdealRttMs, W.BadRttMs, W.WorstClientWeight);
		B.Jitter = ScoreCurve(N.AvgJitterMs, W.IdealJitterMs, W.BadJitterMs, W.JitterWeight);
		B.PacketLoss = ScoreCurve(N.AvgLossBp, 0, W.BadLossBp, W.PacketLossWeight);
		B.Reachability = N.bAllReachable ? W.ReachabilityWeight : 0;
		if (N.UploadKbps <= 0) B.Upload = W.UploadWeight / 2; // unknown: neutral
		else B.Upload = ScoreCurve(W.GoodUploadKbps - N.UploadKbps, 0, W.GoodUploadKbps - W.MinUploadKbps, W.UploadWeight);
		B.Stability = C.bRecentlyUnstable ? 0 : (C.RecentDisconnects > 0
			? W.StabilityWeight / (1 + C.RecentDisconnects)
			: W.StabilityWeight);
		B.SaveReady = C.bHasHeadCached ? W.SaveReadyBonus : 0;
		if (bEmergency && W.PreferCachedSaveInEmergency && C.bHasHeadCached) B.SaveReady += W.EmergencySaveCacheBonus;
		B.StorageReady = C.bStorageReachable ? W.StorageReadyBonus : 0;
		B.SessionReady = C.bSessionCapable ? W.SessionReadyBonus : 0;
		B.Total = B.Latency + B.WorstClient + B.Jitter + B.PacketLoss + B.Reachability + B.Upload + B.Stability +
			B.SaveReady + B.StorageReady + B.SessionReady;
		return B;
	}

	SuccessorStatus EvaluateSuccessor(const HostCandidate& C, const HostScoreWeights& Weights)
	{
		SuccessorStatus S;
		S.Who = C.Who;
		const HostScoreBreakdown B = ScoreHost(C, Weights, false);
		S.Score = B.Total;
		S.bNetworkReady = ResolveNetwork(C).bAllReachable;
		S.bStorageReady = C.bStorageReachable;
		S.bSessionReady = C.bSessionCapable;
		S.bSaveReady = C.bHasHeadCached;
		S.bCompatible = C.bCompatible;
		S.Readiness = (B.bEligible && IsSuccessorReady(C)) ? SuccessorReadiness::Ready : SuccessorReadiness::NotReady;
		S.Detail = B.bEligible ? ToString(S.Readiness) : B.DisqualifyReason;
		return S;
	}

	std::vector<RankedHost> RankHosts(const std::vector<HostCandidate>& Candidates, const Identity& CurrentHost,
		const HostScoreWeights& Weights, bool bEmergency)
	{
		std::vector<RankedHost> Out;
		for (const HostCandidate& C : Candidates)
		{
			if (C.Who.PlayerId == CurrentHost.PlayerId) continue;
			if (!C.Who.Validate().Ok()) continue;
			HostScoreBreakdown B = ScoreHost(C, Weights, bEmergency);
			if (!B.bEligible) continue;
			RankedHost R;
			R.Who = C.Who;
			R.Score = B;
			R.Network = ResolveNetwork(C);
			R.bSuccessorReady = IsSuccessorReady(C);
			Out.push_back(R);
		}
		std::sort(Out.begin(), Out.end(), [](const RankedHost& A, const RankedHost& B)
		{
			if (A.Score.Total != B.Score.Total) return A.Score.Total > B.Score.Total;
			if (A.Network.WorstRttMs != B.Network.WorstRttMs) return A.Network.WorstRttMs < B.Network.WorstRttMs;
			if (A.Network.AvgLossBp != B.Network.AvgLossBp) return A.Network.AvgLossBp < B.Network.AvgLossBp;
			return A.Who.PlayerId < B.Who.PlayerId;
		});
		return Out;
	}

	std::vector<RankedHost> PreferredSuccessors(const std::vector<HostCandidate>& Candidates, const Identity& CurrentHost,
		const HostScoreWeights& Weights, bool bEmergency)
	{
		std::vector<RankedHost> All = RankHosts(Candidates, CurrentHost, Weights, bEmergency);
		std::vector<RankedHost> Ready;
		for (RankedHost& R : All)
		{
			if (R.bSuccessorReady) Ready.push_back(R);
		}
		return Ready;
	}

	bool ShouldProactivelyMigrate(int CurrentHostScore, int CandidateScore, TimeMs AdvantageSince, TimeMs Now,
		const HysteresisConfig& Hyst)
	{
		if (!Hyst.AllowProactiveMigration) return false;
		if (CandidateScore - CurrentHostScore < Hyst.MinScoreDelta) return false;
		if (AdvantageSince <= 0) return false;
		return Now - AdvantageSince >= Hyst.PersistDuration;
	}

	HostCandidate MakeCandidate(Identity Who, const PeerQualityMatrix& Matrix, const std::vector<std::string>& Online,
		bool bConnected, bool bCompatible, bool bStorage, bool bCached, bool bSessionCapable, bool bHostEligible, bool bUnstable)
	{
		HostCandidate C;
		C.Who = std::move(Who);
		C.bConnected = bConnected;
		C.bCompatible = bCompatible;
		C.bStorageReachable = bStorage;
		C.bHasHeadCached = bCached;
		C.bSessionCapable = bSessionCapable;
		C.bHostEligible = bHostEligible;
		C.bRecentlyUnstable = bUnstable;
		C.Network = Matrix.SummarizeAsHost(C.Who.PlayerId, Online);
		C.PingMs = C.Network->AvgRttMs;
		return C;
	}
}
