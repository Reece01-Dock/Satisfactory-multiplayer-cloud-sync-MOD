#pragma once
// Deterministic host scoring and continuous successor ranking.
//
// Network quality dominates. Hard readiness gates disqualify candidates
// regardless of ping. Scores are integer millipoints so every client with
// the same inputs produces the identical ordered list.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	/** Configurable scoring weights (defaults emphasise group network quality). */
	struct HostScoreWeights
	{
		int LatencyWeight = 40000;       // millipoints at ideal RTT
		int WorstClientWeight = 20000;
		int JitterWeight = 15000;
		int PacketLossWeight = 25000;
		int ReachabilityWeight = 30000;  // lost entirely when any client unreachable
		int UploadWeight = 10000;
		int StabilityWeight = 10000;
		int SaveReadyBonus = 8000;
		int StorageReadyBonus = 12000;
		int SessionReadyBonus = 8000;

		int IdealRttMs = 30;
		int BadRttMs = 150;
		int IdealJitterMs = 5;
		int BadJitterMs = 50;
		int BadLossBp = 500;             // 5%
		int GoodUploadKbps = 5000;
		int MinUploadKbps = 1000;

		/** Emergency migration may prefer a slightly worse candidate with the save already cached. */
		bool PreferCachedSaveInEmergency = true;
		int EmergencySaveCacheBonus = 20000;
	};

	struct HysteresisConfig
	{
		/** Candidate must beat current host by at least this (millipoints). */
		int MinScoreDelta = 15000;
		/** Degradation / advantage must persist this long before proactive migration. */
		TimeMs PersistDuration = Seconds(30);
		bool AllowProactiveMigration = false;
	};

	/** Full readiness + metrics for one potential host. */
	struct HostCandidate
	{
		Identity Who;
		bool bConnected = false;
		bool bHostEligible = true;
		bool bCompatible = false;        // game + mod versions
		bool bStorageReachable = false;
		bool bHasHeadCached = false;
		bool bSessionCapable = true;
		bool bUploadSufficient = true;
		bool bRecentlyUnstable = false;
		int RecentDisconnects = 0;

		/** If set, used directly. Otherwise derived from Matrix / PingMs. */
		std::optional<HostNetworkSummary> Network;
		/** Legacy single-sample RTT to current host (used when Network unset). */
		int PingMs = 9999;
	};

	struct HostScoreBreakdown
	{
		int Total = 0; // millipoints
		int Latency = 0;
		int WorstClient = 0;
		int Jitter = 0;
		int PacketLoss = 0;
		int Reachability = 0;
		int Upload = 0;
		int Stability = 0;
		int SaveReady = 0;
		int StorageReady = 0;
		int SessionReady = 0;
		bool bEligible = false;
		std::string DisqualifyReason;
	};

	struct RankedHost
	{
		Identity Who;
		HostScoreBreakdown Score;
		HostNetworkSummary Network;
		bool bSuccessorReady = false;
	};

	enum class SuccessorReadiness
	{
		NotReady,
		Ready,
	};
	const char* ToString(SuccessorReadiness R);

	struct SuccessorStatus
	{
		Identity Who;
		int Score = 0;
		SuccessorReadiness Readiness = SuccessorReadiness::NotReady;
		bool bNetworkReady = false;
		bool bStorageReady = false;
		bool bSessionReady = false;
		bool bSaveReady = false;
		bool bCompatible = false;
		std::string Detail;
	};

	/**
	 * Soft score curve: Ideal → full Weight, Bad → 0, beyond Bad → 0.
	 * Integer-only; Value and bounds in the same units.
	 */
	int ScoreCurve(int Value, int Ideal, int Bad, int Weight);

	HostNetworkSummary NetworkFromPing(int PingMs);
	HostScoreBreakdown ScoreHost(const HostCandidate& C, const HostScoreWeights& W, bool bEmergency = false);
	bool IsSuccessorReady(const HostCandidate& C);

	/**
	 * Deterministic ranking. Tie-break:
	 *   total score ↓ → worst RTT ↑ → avg loss ↑ → player id ↑
	 * CurrentHost is excluded. Ineligible candidates are omitted.
	 */
	std::vector<RankedHost> RankHosts(const std::vector<HostCandidate>& Candidates, const Identity& CurrentHost,
		const HostScoreWeights& Weights = {}, bool bEmergency = false);

	/** Preferred / second / third… READY successors only. */
	std::vector<RankedHost> PreferredSuccessors(const std::vector<HostCandidate>& Candidates, const Identity& CurrentHost,
		const HostScoreWeights& Weights = {}, bool bEmergency = false);

	SuccessorStatus EvaluateSuccessor(const HostCandidate& C, const HostScoreWeights& Weights = {});

	/**
	 * Hysteresis gate for proactive rebalance. Returns true only when enabled,
	 * Candidate beats Host by MinScoreDelta, and AdvantageSince has lasted PersistDuration.
	 */
	bool ShouldProactivelyMigrate(int CurrentHostScore, int CandidateScore, TimeMs AdvantageSince, TimeMs Now,
		const HysteresisConfig& Hyst);

	/** Build candidates from a peer matrix + readiness flags. */
	HostCandidate MakeCandidate(Identity Who, const PeerQualityMatrix& Matrix, const std::vector<std::string>& Online,
		bool bConnected, bool bCompatible, bool bStorage, bool bCached, bool bSessionCapable = true,
		bool bHostEligible = true, bool bUnstable = false);
}
