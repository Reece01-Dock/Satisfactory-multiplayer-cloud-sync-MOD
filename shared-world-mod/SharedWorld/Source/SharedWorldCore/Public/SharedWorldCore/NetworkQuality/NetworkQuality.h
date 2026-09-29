#pragma once
// Peer-to-peer network quality: measurements, rolling stats, and the matrix
// every candidate uses to estimate "what if I were host?".
//
// All metrics are integers so ranking is bit-identical across platforms.
// Packet loss is stored in basis points (100 = 1.00%).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	/** One directed link sample: From → To. */
	struct LinkSample
	{
		int RttMs = 9999;
		int JitterMs = 0;
		int LossBp = 0;       // 0..10000+
		bool bReachable = true;
		int UploadKbps = 0;   // estimated uplink of From (same for all To)
	};

	/** Rolling window of samples for one directed link. */
	struct RollingLinkStats
	{
		int SampleCount = 0;
		int RttMs = 9999;     // EWMA
		int JitterMs = 0;     // EWMA of |rtt - prev|
		int LossBp = 0;       // EWMA
		int ReachFailures = 0;
		int UploadKbps = 0;
		bool bReachable = true;
		TimeMs LastSampleAt = 0;

		void Observe(const LinkSample& S, TimeMs Now, int EwmaAlphaBp = 2500); // 25%
	};

	/** Aggregate quality of Candidate if they hosted the given set of clients. */
	struct HostNetworkSummary
	{
		int AvgRttMs = 9999;
		int MedianRttMs = 9999;
		int WorstRttMs = 9999;
		int AvgJitterMs = 0;
		int AvgLossBp = 0;
		int WorstLossBp = 0;
		int ReachFailures = 0;
		int ClientCount = 0;
		int UploadKbps = 0;
		bool bAllReachable = true;
	};

	/**
	 * NxN peer quality matrix. Self-links are ignored. Missing links are
	 * treated as unreachable with max RTT until samples arrive.
	 */
	class PeerQualityMatrix
	{
	public:
		void SetPlayers(std::vector<std::string> PlayerIds);
		const std::vector<std::string>& Players() const { return Ids; }

		void Observe(const std::string& From, const std::string& To, const LinkSample& S, TimeMs Now);
		void SetLink(const std::string& From, const std::string& To, const LinkSample& S, TimeMs Now);
		std::optional<RollingLinkStats> Get(const std::string& From, const std::string& To) const;
		bool IsReachable(const std::string& From, const std::string& To) const;

		/** Group experience if HostId were hosting every other online player. */
		HostNetworkSummary SummarizeAsHost(const std::string& HostId, const std::vector<std::string>& OnlineClients) const;

		/** Row-major dump for diagnostics: [from][to] RTT, "-" for self. */
		std::vector<std::vector<int>> RttGridMs() const;

	private:
		std::vector<std::string> Ids;
		std::map<std::string, std::map<std::string, RollingLinkStats>> Links; // From → To
	};

	/**
	 * Source of live peer measurements. Production probes game/net; tests
	 * inject a SimulatedNetworkQualityProvider.
	 */
	class INetworkQualityProvider
	{
	public:
		virtual ~INetworkQualityProvider() = default;
		virtual LinkSample GetLink(const std::string& From, const std::string& To) const = 0;
		virtual int GetEstimatedUploadKbps(const std::string& PlayerId) const = 0;
		virtual bool IsReachable(const std::string& From, const std::string& To) const = 0;
	};

	/** Mutable test/simulator network model. */
	class SimulatedNetworkQualityProvider final : public INetworkQualityProvider
	{
	public:
		void SetLink(const std::string& From, const std::string& To, LinkSample S);
		void SetSymmetric(const std::string& A, const std::string& B, LinkSample S);
		void SetUploadKbps(const std::string& PlayerId, int Kbps);
		void SetReachable(const std::string& From, const std::string& To, bool bOk);
		void Clear();

		LinkSample GetLink(const std::string& From, const std::string& To) const override;
		int GetEstimatedUploadKbps(const std::string& PlayerId) const override;
		bool IsReachable(const std::string& From, const std::string& To) const override;

		/** Push current links into Matrix at Now. */
		void ApplyTo(PeerQualityMatrix& Matrix, TimeMs Now) const;

	private:
		std::map<std::string, std::map<std::string, LinkSample>> Links;
		std::map<std::string, int> Uploads;
	};
}
