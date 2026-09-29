#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"

#include <algorithm>
#include <climits>

namespace sw
{
	namespace
	{
		int ClampBp(int V) { return V < 0 ? 0 : (V > 10000 ? 10000 : V); }

		int Ewma(int Prev, int Sample, int AlphaBp, int Count)
		{
			if (Count <= 1) return Sample;
			const int A = ClampBp(AlphaBp);
			// (alpha * sample + (1-alpha) * prev) in basis points
			return static_cast<int>((static_cast<int64_t>(A) * Sample + static_cast<int64_t>(10000 - A) * Prev) / 10000);
		}
	}

	void RollingLinkStats::Observe(const LinkSample& S, TimeMs Now, int EwmaAlphaBp)
	{
		++SampleCount;
		if (!S.bReachable)
		{
			++ReachFailures;
			bReachable = false;
			LastSampleAt = Now;
			return;
		}
		bReachable = true;
		const int PrevRtt = RttMs;
		RttMs = Ewma(RttMs, S.RttMs, EwmaAlphaBp, SampleCount);
		const int InstJitter = SampleCount <= 1 ? S.JitterMs : (PrevRtt > S.RttMs ? PrevRtt - S.RttMs : S.RttMs - PrevRtt);
		const int JSample = S.JitterMs > InstJitter ? S.JitterMs : InstJitter;
		JitterMs = Ewma(JitterMs, JSample, EwmaAlphaBp, SampleCount);
		LossBp = Ewma(LossBp, S.LossBp, EwmaAlphaBp, SampleCount);
		if (S.UploadKbps > 0) UploadKbps = S.UploadKbps;
		LastSampleAt = Now;
	}

	void PeerQualityMatrix::SetPlayers(std::vector<std::string> PlayerIds)
	{
		std::sort(PlayerIds.begin(), PlayerIds.end());
		PlayerIds.erase(std::unique(PlayerIds.begin(), PlayerIds.end()), PlayerIds.end());
		Ids = std::move(PlayerIds);
		for (auto It = Links.begin(); It != Links.end();)
		{
			if (std::find(Ids.begin(), Ids.end(), It->first) == Ids.end()) It = Links.erase(It);
			else ++It;
		}
	}

	void PeerQualityMatrix::Observe(const std::string& From, const std::string& To, const LinkSample& S, TimeMs Now)
	{
		if (From.empty() || To.empty() || From == To) return;
		Links[From][To].Observe(S, Now);
	}

	void PeerQualityMatrix::SetLink(const std::string& From, const std::string& To, const LinkSample& S, TimeMs Now)
	{
		if (From.empty() || To.empty() || From == To) return;
		RollingLinkStats& R = Links[From][To];
		R = RollingLinkStats{};
		R.Observe(S, Now, 10000); // replace
	}

	std::optional<RollingLinkStats> PeerQualityMatrix::Get(const std::string& From, const std::string& To) const
	{
		auto A = Links.find(From);
		if (A == Links.end()) return std::nullopt;
		auto B = A->second.find(To);
		if (B == A->second.end()) return std::nullopt;
		return B->second;
	}

	bool PeerQualityMatrix::IsReachable(const std::string& From, const std::string& To) const
	{
		auto S = Get(From, To);
		return S && S->bReachable && S->SampleCount > 0;
	}

	HostNetworkSummary PeerQualityMatrix::SummarizeAsHost(const std::string& HostId, const std::vector<std::string>& OnlineClients) const
	{
		HostNetworkSummary Out;
		std::vector<int> Rtts;
		int64_t SumRtt = 0, SumJitter = 0, SumLoss = 0;
		for (const std::string& Client : OnlineClients)
		{
			if (Client == HostId) continue;
			++Out.ClientCount;
			auto S = Get(HostId, Client);
			if (!S || !S->bReachable || S->SampleCount == 0)
			{
				Out.bAllReachable = false;
				++Out.ReachFailures;
				Rtts.push_back(9999);
				SumRtt += 9999;
				continue;
			}
			Rtts.push_back(S->RttMs);
			SumRtt += S->RttMs;
			SumJitter += S->JitterMs;
			SumLoss += S->LossBp;
			if (S->RttMs > Out.WorstRttMs || Out.ClientCount == 1) Out.WorstRttMs = S->RttMs;
			if (S->LossBp > Out.WorstLossBp) Out.WorstLossBp = S->LossBp;
			if (S->UploadKbps > Out.UploadKbps) Out.UploadKbps = S->UploadKbps;
		}
		if (Out.ClientCount == 0)
		{
			Out.AvgRttMs = 0;
			Out.MedianRttMs = 0;
			Out.WorstRttMs = 0;
			return Out;
		}
		Out.AvgRttMs = static_cast<int>(SumRtt / Out.ClientCount);
		Out.AvgJitterMs = static_cast<int>(SumJitter / Out.ClientCount);
		Out.AvgLossBp = static_cast<int>(SumLoss / Out.ClientCount);
		std::sort(Rtts.begin(), Rtts.end());
		Out.MedianRttMs = Rtts[Rtts.size() / 2];
		if (Out.WorstRttMs == 9999 && !Rtts.empty()) Out.WorstRttMs = Rtts.back();
		return Out;
	}

	std::vector<std::vector<int>> PeerQualityMatrix::RttGridMs() const
	{
		std::vector<std::vector<int>> Grid(Ids.size(), std::vector<int>(Ids.size(), -1));
		for (size_t R = 0; R < Ids.size(); ++R)
		{
			for (size_t C = 0; C < Ids.size(); ++C)
			{
				if (R == C) continue;
				auto S = Get(Ids[R], Ids[C]);
				Grid[R][C] = S ? S->RttMs : 9999;
			}
		}
		return Grid;
	}

	void SimulatedNetworkQualityProvider::SetLink(const std::string& From, const std::string& To, LinkSample S)
	{
		Links[From][To] = S;
	}

	void SimulatedNetworkQualityProvider::SetSymmetric(const std::string& A, const std::string& B, LinkSample S)
	{
		SetLink(A, B, S);
		SetLink(B, A, S);
	}

	void SimulatedNetworkQualityProvider::SetUploadKbps(const std::string& PlayerId, int Kbps)
	{
		Uploads[PlayerId] = Kbps;
	}

	void SimulatedNetworkQualityProvider::SetReachable(const std::string& From, const std::string& To, bool bOk)
	{
		LinkSample S = GetLink(From, To);
		S.bReachable = bOk;
		Links[From][To] = S;
	}

	void SimulatedNetworkQualityProvider::Clear()
	{
		Links.clear();
		Uploads.clear();
	}

	LinkSample SimulatedNetworkQualityProvider::GetLink(const std::string& From, const std::string& To) const
	{
		auto A = Links.find(From);
		if (A == Links.end()) return LinkSample{};
		auto B = A->second.find(To);
		if (B == A->second.end()) return LinkSample{};
		LinkSample S = B->second;
		auto U = Uploads.find(From);
		if (U != Uploads.end()) S.UploadKbps = U->second;
		return S;
	}

	int SimulatedNetworkQualityProvider::GetEstimatedUploadKbps(const std::string& PlayerId) const
	{
		auto U = Uploads.find(PlayerId);
		return U == Uploads.end() ? 0 : U->second;
	}

	bool SimulatedNetworkQualityProvider::IsReachable(const std::string& From, const std::string& To) const
	{
		return GetLink(From, To).bReachable;
	}

	void SimulatedNetworkQualityProvider::ApplyTo(PeerQualityMatrix& Matrix, TimeMs Now) const
	{
		for (const auto& From : Links)
		{
			for (const auto& To : From.second)
			{
				LinkSample S = To.second;
				auto U = Uploads.find(From.first);
				if (U != Uploads.end()) S.UploadKbps = U->second;
				Matrix.SetLink(From.first, To.first, S, Now);
			}
		}
	}
}
