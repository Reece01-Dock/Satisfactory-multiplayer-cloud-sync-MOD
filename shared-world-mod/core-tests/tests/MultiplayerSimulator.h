#pragma once
// Headless multiplayer simulator: N virtual players sharing one repository,
// using the real SharedWorldCore host-election and migration algorithms.

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "SharedWorldCore/HostMigration/HostMigration.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/Time.h"

namespace swsim
{
	struct SimPlayer
	{
		sw::Identity Id;
		bool bOnline = true;
		bool bHostEligible = true;
		bool bCompatible = true;
		bool bStorageReachable = true;
		bool bHasHeadCached = true;
		bool bSessionCapable = true;
		bool bUploadSufficient = true;
		bool bRecentlyUnstable = false;
		bool bAuthoritative = false;
		bool bCanPublishSession = true;
		int JoinCount = 0;
		std::unique_ptr<sw::HostMigrationEngine> Engine;
		std::optional<sw::LeaseToken> Token;
	};

	struct InvariantViolation
	{
		std::string What;
	};

	class MultiplayerSimulator
	{
	public:
		explicit MultiplayerSimulator(uint64_t Seed = 1);

		sw::SimulatedClock& Clock() { return *Clock_; }
		sw::SimulatedNetworkQualityProvider& Net() { return *Net_; }
		sw::MemoryRepository& Repo() { return *Repo_; }
		sw::MemoryObjectStore& Objects() { return *Objects_; }
		sw::LeaseManager& Leases() { return *Leases_; }
		uint64_t Seed() const { return Seed_; }
		const std::vector<std::unique_ptr<SimPlayer>>& Players() const { return Players_; }
		sw::MigrationConfig& Config() { return Cfg_; }

		SimPlayer* Find(const std::string& PlayerId);
		SimPlayer* CurrentHost();
		int AuthoritativeCount() const;

		SimPlayer& AddPlayer(const std::string& DisplayName, int Index);
		void SetOnline(const std::string& PlayerId, bool bOnline);
		/** Client recognises an existing host and joins (no lease acquire). */
		sw::Status JoinExistingHost(const std::string& ClientPlayerId);
		void BuildMatrix();
		void RefreshAllRankings(bool bEmergency = false);
		std::vector<sw::RankedHost> GlobalRanking() const;

		sw::Status BootstrapHost(const std::string& HostPlayerId);
		/** Planned migration: final save → commit → handoff → successor acquires → session. */
		sw::Status PlannedLeave(const std::string& HostPlayerId);
		sw::Status CrashHost(const std::string& HostPlayerId);
		/** Advance clock and drive crash recovery until Running or failure. */
		sw::Status AdvanceRecovery(sw::TimeMs Step = sw::Seconds(5), int MaxSteps = 100);
		/** Old host tries authority ops; all must be fenced. */
		sw::Status ExpectStaleHostFenced(const std::string& OldHostId, const sw::LeaseToken& StaleToken);

		std::vector<InvariantViolation> CheckInvariants() const;
		std::string DumpAllTraces() const;

	private:
		uint64_t Seed_;
		std::mt19937_64 Rng_;
		std::shared_ptr<sw::SimulatedClock> Clock_;
		std::shared_ptr<sw::MemoryRepository> Repo_;
		std::shared_ptr<sw::MemoryObjectStore> Objects_;
		std::shared_ptr<sw::LeaseManager> Leases_;
		std::shared_ptr<sw::SimulatedNetworkQualityProvider> Net_;
		std::vector<std::unique_ptr<SimPlayer>> Players_;
		sw::MigrationConfig Cfg_;
		int64_t MaxSeenRevision_ = 0;
		int64_t MaxSeenGeneration_ = 0;

		std::vector<sw::HostCandidate> BuildCandidates() const;
		std::vector<std::string> OnlineIds() const;
		sw::Status CommitNextRevision(sw::LeaseToken& Tok, const std::string& Reason);
	};
}
