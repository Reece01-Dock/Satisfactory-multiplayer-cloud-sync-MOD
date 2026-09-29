#pragma once
// First-class host migration state machine: planned handoff and crash recovery.
// Algorithms live here; I/O goes through injected LeaseManager / stores / session
// providers so the Satisfactory mod and the headless simulator share one path.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/HostElection/HostElection.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Migration/Migration.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	enum class MigrationPhase
	{
		Running,
		// Planned
		MigrationPreparing,
		FinalSave,
		RevisionSync,
		SuccessorReady,
		LeaseHandoff,
		SuccessorStarting,
		SessionPublished,
		ClientReconnect,
		// Crash
		HostLost,
		RecoveryWait,
		LeaseExpired,
		SuccessorElection,
		RevisionRecovery,
		// Terminal
		Failed,
	};
	const char* ToString(MigrationPhase P);

	enum class ClientMigrationState
	{
		ConnectedToHost,
		HostLossDetected,
		WaitingForLease,
		WaitingForSuccessor,
		WaitingForSession,
		JoiningSuccessor,
		Connected,
		Failed,
	};
	const char* ToString(ClientMigrationState S);

	struct MigrationTimeouts
	{
		TimeMs FinalSaveTimeout = Seconds(120);
		TimeMs UploadTimeout = Seconds(180);
		TimeMs SuccessorReadyTimeout = Seconds(90);
		TimeMs LeaseAcquireTimeout = Seconds(60);
		TimeMs SessionPublishTimeout = Seconds(90);
		TimeMs ReconnectTimeout = Minutes(3);
		TimeMs RecoveryWaitSkew = Seconds(0); // added on top of lease TTL+grace in tests
	};

	struct MigrationConfig
	{
		HostScoreWeights Scores;
		HysteresisConfig Hysteresis;
		MigrationTimeouts Timeouts;
		LeaseConfig Lease;
		/** Freeze ranking once planned migration starts (avoids oscillation). */
		bool FreezeRankingOnMigrationStart = true;
		/** Allow re-picking a READY fallback if the chosen successor drops mid-migration. */
		bool AllowSuccessorFallback = true;
	};

	struct MigrationEvent
	{
		TimeMs At = 0;
		std::string Kind;
		std::string Detail;
	};

	struct MigrationMetrics
	{
		TimeMs LeaveAt = 0;
		TimeMs NewLeaseAt = 0;
		TimeMs SessionAvailableAt = 0;
		TimeMs FirstClientReconnectedAt = 0;
		TimeMs AllClientsReconnectedAt = 0;

		TimeMs TimeToNewLease() const { return LeaveAt && NewLeaseAt ? NewLeaseAt - LeaveAt : 0; }
		TimeMs TimeToSession() const { return LeaveAt && SessionAvailableAt ? SessionAvailableAt - LeaveAt : 0; }
		TimeMs TimeToFirstReconnect() const { return LeaveAt && FirstClientReconnectedAt ? FirstClientReconnectedAt - LeaveAt : 0; }
		TimeMs TimeToAllReconnect() const { return LeaveAt && AllClientsReconnectedAt ? AllClientsReconnectedAt - LeaveAt : 0; }
	};

	struct MigrationDiagnostics
	{
		MigrationPhase Phase = MigrationPhase::Running;
		ClientMigrationState ClientState = ClientMigrationState::ConnectedToHost;
		Identity CurrentHost;
		int64_t Generation = 0;
		int64_t Revision = 0;
		std::optional<Identity> PreferredSuccessor;
		std::vector<RankedHost> Ranking;
		std::vector<std::vector<int>> RttGridMs;
		std::vector<std::string> PlayerIds;
		std::string OverlayMessage;
		MigrationMetrics Metrics;
		std::vector<MigrationEvent> Trace;
	};

	/** Optional hooks the game / simulator implements. */
	struct MigrationHooks
	{
		std::function<Status()> RequestFinalSave;                 // host
		std::function<Result<RevisionMeta>(const std::string& LocalSavePath)> UploadAndCommit; // host
		std::function<Status(const Identity& Successor)> PrepareSuccessor;
		std::function<Status()> SuccessorStartHosting;            // load save + host
		std::function<Status(const JoinInfo&)> PublishSession;
		std::function<Status(const JoinInfo&)> JoinSession;       // clients
		std::function<bool()> CanPublishSession;
		std::function<Result<std::string>()> PrefetchHead;        // successor optional
	};

	/**
	 * Owns continuous ranking + the migration / crash state machine for one
	 * local player. Authority changes go through LeaseManager only.
	 */
	class HostMigrationEngine
	{
	public:
		HostMigrationEngine(Identity Me, std::shared_ptr<IClock> Clock, std::shared_ptr<LeaseManager> Leases,
			MigrationConfig Config = {});

		void SetHooks(MigrationHooks Hooks) { Hooks_ = std::move(Hooks); }
		void SetMatrix(PeerQualityMatrix Matrix) { Matrix_ = std::move(Matrix); }
		PeerQualityMatrix& Matrix() { return Matrix_; }
		const PeerQualityMatrix& Matrix() const { return Matrix_; }

		MigrationPhase Phase() const { return Phase_; }
		ClientMigrationState ClientState() const { return ClientState_; }
		const MigrationDiagnostics& Diagnostics() const { return Diag_; }
		const std::vector<MigrationEvent>& Trace() const { return Trace_; }
		const MigrationMetrics& Metrics() const { return Metrics_; }
		const MigrationConfig& Config() const { return Cfg_; }
		MigrationConfig& Config() { return Cfg_; }

		void UpdateCandidates(std::vector<HostCandidate> Candidates);
		void SetCurrentHost(Identity Host, int64_t Generation, int64_t Revision);
		std::vector<RankedHost> RefreshRanking(bool bEmergency = false);
		std::optional<RankedHost> PreferredSuccessor(bool bRequireReady = true) const;

		/** Continuous hysteresis check (proactive migration). */
		bool EvaluateProactiveMigration(int CurrentHostScore);

		// ---- Planned migration (current host) ----
		Status BeginPlannedMigration();
		Status OnFinalSaveCompleted(const std::string& LocalSavePath);
		Status OnRevisionCommitted(const RevisionMeta& Rev);
		Status OnSuccessorPrepared(const Identity& Successor);
		Status CompleteLeaseHandoff(); // release with successor reservation
		Status OnSuccessorFailed(const std::string& Reason);

		// ---- Crash / client paths ----
		Status OnHostLost();
		Status Tick(TimeMs Now); // advance waits / timeouts
		Status OnLeaseExpired();
		Status TryAcquireAsSuccessor();
		Status OnSessionPublished(const JoinInfo& Join);
		Status OnClientReconnected();
		Status MarkMigrationCompleted();
		Status Fail(const std::string& Reason);

		/** Inject the local host's fencing token (required for lease handoff). */
		void SetLeaseToken(LeaseToken Token);
		const std::optional<LeaseToken>& Token() const { return Token_; }

		void Record(const std::string& Kind, const std::string& Detail = {});
		std::string DumpTrace() const;

	private:
		Identity Me_;
		std::shared_ptr<IClock> Clock_;
		std::shared_ptr<LeaseManager> Leases_;
		MigrationConfig Cfg_;
		MigrationHooks Hooks_;
		PeerQualityMatrix Matrix_;
		std::vector<HostCandidate> Candidates_;
		std::vector<RankedHost> Ranking_;
		bool bRankingFrozen_ = false;
		Identity CurrentHost_;
		int64_t Generation_ = 0;
		int64_t Revision_ = 0;
		std::optional<Identity> ChosenSuccessor_;
		std::optional<JoinInfo> PublishedJoin_;
		MigrationPhase Phase_ = MigrationPhase::Running;
		ClientMigrationState ClientState_ = ClientMigrationState::ConnectedToHost;
		TimeMs PhaseEnteredAt_ = 0;
		TimeMs AdvantageSince_ = 0;
		std::optional<Identity> AdvantageCandidate_;
		MigrationMetrics Metrics_;
		std::vector<MigrationEvent> Trace_;
		MigrationDiagnostics Diag_;
		std::optional<LeaseToken> Token_;

		void Enter(MigrationPhase P, const std::string& Detail = {});
		void RebuildDiagnostics();
		Status PickSuccessor(bool bEmergency);
		bool TimedOut(TimeMs Limit) const;
	};
}
