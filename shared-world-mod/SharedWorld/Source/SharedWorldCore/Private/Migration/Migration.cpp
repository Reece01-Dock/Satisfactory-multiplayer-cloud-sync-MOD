#include "SharedWorldCore/Migration/Migration.h"

#include <algorithm>
#include <cstdio>

#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	static int PreferredIndex(const std::vector<std::string>& Preferred, const std::string& Id)
	{
		for (size_t i = 0; i < Preferred.size(); ++i)
		{
			if (Preferred[i] == Id) return static_cast<int>(i);
		}
		return static_cast<int>(Preferred.size());
	}

	std::optional<Identity> SelectSuccessor(const std::vector<SuccessorCandidate>& Candidates, const std::vector<std::string>& PreferredHosts,
		const Identity& CurrentHost, const HostScoreWeights& Weights)
	{
		std::vector<HostCandidate> Converted;
		Converted.reserve(Candidates.size());
		for (const SuccessorCandidate& C : Candidates)
		{
			HostCandidate H;
			H.Who = C.Who;
			H.bConnected = C.bConnected;
			H.bCompatible = C.bCompatible;
			H.bStorageReachable = C.bStorageReachable;
			H.bHasHeadCached = C.bHasHeadCached;
			H.bHostEligible = C.bHostEligible;
			H.bSessionCapable = C.bSessionCapable;
			H.bUploadSufficient = C.bUploadSufficient;
			H.PingMs = C.PingMs;
			H.Network = C.Network;
			Converted.push_back(H);
		}
		std::vector<RankedHost> Ranked = RankHosts(Converted, CurrentHost, Weights, false);
		if (Ranked.empty()) return std::nullopt;
		if (!PreferredHosts.empty())
		{
			std::stable_sort(Ranked.begin(), Ranked.end(), [&](const RankedHost& A, const RankedHost& B)
			{
				const int PA = PreferredIndex(PreferredHosts, A.Who.PlayerId);
				const int PB = PreferredIndex(PreferredHosts, B.Who.PlayerId);
				if (PA != PB) return PA < PB;
				if (A.Score.Total != B.Score.Total) return A.Score.Total > B.Score.Total;
				if (A.Network.WorstRttMs != B.Network.WorstRttMs) return A.Network.WorstRttMs < B.Network.WorstRttMs;
				if (A.Network.AvgLossBp != B.Network.AvgLossBp) return A.Network.AvgLossBp < B.Network.AvgLossBp;
				return A.Who.PlayerId < B.Who.PlayerId;
			});
		}
		return Ranked.front().Who;
	}

	int TakeoverRank(const std::vector<SessionPlayer>& LastPlayers, const std::vector<std::string>& PreferredHosts, const std::string& MyPlayerId)
	{
		std::vector<std::string> Ids;
		for (const SessionPlayer& P : LastPlayers)
		{
			if (!P.PlayerId.empty()) Ids.push_back(P.PlayerId);
		}
		std::sort(Ids.begin(), Ids.end(), [&](const std::string& A, const std::string& B)
		{
			const int PA = PreferredIndex(PreferredHosts, A), PB = PreferredIndex(PreferredHosts, B);
			return PA != PB ? PA < PB : A < B;
		});
		for (size_t i = 0; i < Ids.size(); ++i)
		{
			if (Ids[i] == MyPlayerId) return static_cast<int>(i);
		}
		return static_cast<int>(Ids.size());
	}

	std::optional<RecoveryCandidate> SelectRecoveryCandidate(const std::vector<RecoveryCandidate>& Candidates, int64_t CrashedGeneration, const WorldState& State)
	{
		const std::string HeadSha = State.Head ? State.Head->ObjectSha256 : std::string();
		std::vector<const RecoveryCandidate*> Ok;
		for (const RecoveryCandidate& C : Candidates)
		{
			if (CrashedGeneration > 0 && C.Generation == CrashedGeneration && C.BaseRevision == State.HeadNumber() && C.bValidated &&
				C.bObjectAvailable && Sha256::IsValidHex(C.ObjectSha256) && C.ObjectSha256 != HeadSha && C.Size > 0)
			{
				Ok.push_back(&C);
			}
		}
		if (Ok.empty()) return std::nullopt;
		std::sort(Ok.begin(), Ok.end(), [](const RecoveryCandidate* A, const RecoveryCandidate* B)
		{
			if (A->SavedAt != B->SavedAt) return A->SavedAt > B->SavedAt;
			if (A->Size != B->Size) return A->Size > B->Size;
			return A->ObjectSha256 < B->ObjectSha256;
		});
		return *Ok.front();
	}

	json::Value RecoveryReportToJson(const RecoveryCandidate& C, const Identity& Reporter)
	{
		json::Value V;
		V.Set("generation", C.Generation);
		V.Set("baseRevision", C.BaseRevision);
		V.Set("object", C.ObjectSha256);
		V.Set("size", C.Size);
		V.Set("savedAt", FormatTime(C.SavedAt));
		V.Set("reporter", Reporter.ToJson());
		return V;
	}

	Result<RecoveryCandidate> RecoveryReportFromJson(const json::Value& V)
	{
		RecoveryCandidate C;
		SW_ASSIGN(C.Generation, json::GetInt(V, "generation"));
		SW_ASSIGN(C.BaseRevision, json::GetInt(V, "baseRevision"));
		SW_ASSIGN(C.ObjectSha256, json::GetString(V, "object", 64));
		SW_ASSIGN(C.Size, json::GetInt(V, "size"));
		std::string Saved;
		SW_ASSIGN(Saved, json::GetString(V, "savedAt", 64));
		SW_ASSIGN(C.SavedAt, ParseTime(Saved));
		const json::Value* R = V.Find("reporter");
		if (!R) return MakeError(ErrorCode::Invalid, "recovery report without reporter");
		Identity Reporter;
		SW_ASSIGN(Reporter, Identity::FromJson(*R));
		if (!Sha256::IsValidHex(C.ObjectSha256) || C.Generation < 1 || C.BaseRevision < 0 || C.Size <= 0)
		{
			return MakeError(ErrorCode::Invalid, "recovery report invalid");
		}
		C.Source = "report:" + Reporter.PlayerId;
		return C; // bValidated / bObjectAvailable are established by the reader
	}

	std::string RecoveryReportPath(const RecoveryCandidate& C)
	{
		char Buf[80];
		std::snprintf(Buf, sizeof(Buf), "recovery/g%08lld-%.8s.json", static_cast<long long>(C.Generation), C.ObjectSha256.c_str());
		return Buf;
	}
}
