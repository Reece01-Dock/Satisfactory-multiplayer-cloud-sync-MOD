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

	std::optional<Identity> SelectSuccessor(const std::vector<SuccessorCandidate>& Candidates, const std::vector<std::string>& PreferredHosts, const Identity& CurrentHost)
	{
		std::vector<const SuccessorCandidate*> Eligible;
		for (const SuccessorCandidate& C : Candidates)
		{
			if (C.bConnected && C.bCompatible && C.bStorageReachable && C.Who.PlayerId != CurrentHost.PlayerId && C.Who.Validate().Ok())
			{
				Eligible.push_back(&C);
			}
		}
		if (Eligible.empty()) return std::nullopt;
		std::sort(Eligible.begin(), Eligible.end(), [&](const SuccessorCandidate* A, const SuccessorCandidate* B)
		{
			const int PA = PreferredIndex(PreferredHosts, A->Who.PlayerId), PB = PreferredIndex(PreferredHosts, B->Who.PlayerId);
			if (PA != PB) return PA < PB;
			if (A->bHasHeadCached != B->bHasHeadCached) return A->bHasHeadCached;
			if (A->PingMs != B->PingMs) return A->PingMs < B->PingMs;
			return A->Who.PlayerId < B->Who.PlayerId;
		});
		return Eligible.front()->Who;
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
