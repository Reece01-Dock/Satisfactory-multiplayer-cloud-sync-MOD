#include "SharedWorldCore/HostMigration/HostMigration.h"

#include <sstream>

namespace sw
{
	const char* ToString(MigrationPhase P)
	{
		switch (P)
		{
		case MigrationPhase::Running: return "RUNNING";
		case MigrationPhase::MigrationPreparing: return "MIGRATION_PREPARING";
		case MigrationPhase::FinalSave: return "FINAL_SAVE";
		case MigrationPhase::RevisionSync: return "REVISION_SYNC";
		case MigrationPhase::SuccessorReady: return "SUCCESSOR_READY";
		case MigrationPhase::LeaseHandoff: return "LEASE_HANDOFF";
		case MigrationPhase::SuccessorStarting: return "SUCCESSOR_STARTING";
		case MigrationPhase::SessionPublished: return "SESSION_PUBLISHED";
		case MigrationPhase::ClientReconnect: return "CLIENT_RECONNECT";
		case MigrationPhase::HostLost: return "HOST_LOST";
		case MigrationPhase::RecoveryWait: return "RECOVERY_WAIT";
		case MigrationPhase::LeaseExpired: return "LEASE_EXPIRED";
		case MigrationPhase::SuccessorElection: return "SUCCESSOR_ELECTION";
		case MigrationPhase::RevisionRecovery: return "REVISION_RECOVERY";
		case MigrationPhase::Failed: return "FAILED";
		}
		return "UNKNOWN";
	}

	const char* ToString(ClientMigrationState S)
	{
		switch (S)
		{
		case ClientMigrationState::ConnectedToHost: return "ConnectedToHost";
		case ClientMigrationState::HostLossDetected: return "HostLossDetected";
		case ClientMigrationState::WaitingForLease: return "WaitingForLease";
		case ClientMigrationState::WaitingForSuccessor: return "WaitingForSuccessor";
		case ClientMigrationState::WaitingForSession: return "WaitingForSession";
		case ClientMigrationState::JoiningSuccessor: return "JoiningSuccessor";
		case ClientMigrationState::Connected: return "Connected";
		case ClientMigrationState::Failed: return "Failed";
		}
		return "Unknown";
	}

	HostMigrationEngine::HostMigrationEngine(Identity Me, std::shared_ptr<IClock> Clock, std::shared_ptr<LeaseManager> Leases,
		MigrationConfig Config)
		: Me_(std::move(Me)), Clock_(std::move(Clock)), Leases_(std::move(Leases)), Cfg_(std::move(Config))
	{
		PhaseEnteredAt_ = Clock_->Now();
		RebuildDiagnostics();
	}

	void HostMigrationEngine::Record(const std::string& Kind, const std::string& Detail)
	{
		MigrationEvent E;
		E.At = Clock_->Now();
		E.Kind = Kind;
		E.Detail = Detail;
		Trace_.push_back(E);
		RebuildDiagnostics();
	}

	std::string HostMigrationEngine::DumpTrace() const
	{
		std::ostringstream O;
		for (const MigrationEvent& E : Trace_)
		{
			O << E.At << " " << E.Kind;
			if (!E.Detail.empty()) O << " " << E.Detail;
			O << "\n";
		}
		return O.str();
	}

	void HostMigrationEngine::Enter(MigrationPhase P, const std::string& Detail)
	{
		Phase_ = P;
		PhaseEnteredAt_ = Clock_->Now();
		Record(ToString(P), Detail);
		RebuildDiagnostics();
	}

	bool HostMigrationEngine::TimedOut(TimeMs Limit) const
	{
		return Clock_->Now() - PhaseEnteredAt_ >= Limit;
	}

	void HostMigrationEngine::UpdateCandidates(std::vector<HostCandidate> Candidates)
	{
		if (bRankingFrozen_) return;
		Candidates_ = std::move(Candidates);
		RefreshRanking(Phase_ == MigrationPhase::HostLost || Phase_ == MigrationPhase::SuccessorElection ||
			Phase_ == MigrationPhase::RecoveryWait || Phase_ == MigrationPhase::LeaseExpired);
	}

	void HostMigrationEngine::SetCurrentHost(Identity Host, int64_t Generation, int64_t Revision)
	{
		CurrentHost_ = std::move(Host);
		Generation_ = Generation;
		Revision_ = Revision;
		Record("Host", CurrentHost_.DisplayName.empty() ? CurrentHost_.PlayerId : CurrentHost_.DisplayName +
			" generation=" + std::to_string(Generation_) + " revision=" + std::to_string(Revision_));
		RebuildDiagnostics();
	}

	std::vector<RankedHost> HostMigrationEngine::RefreshRanking(bool bEmergency)
	{
		if (!bRankingFrozen_)
		{
			Ranking_ = RankHosts(Candidates_, CurrentHost_, Cfg_.Scores, bEmergency);
			std::string List;
			for (size_t i = 0; i < Ranking_.size(); ++i)
			{
				if (i) List += ",";
				List += Ranking_[i].Who.DisplayName.empty() ? Ranking_[i].Who.PlayerId : Ranking_[i].Who.DisplayName;
			}
			if (!List.empty()) Record("CandidateRanking", List);
		}
		RebuildDiagnostics();
		return Ranking_;
	}

	std::optional<RankedHost> HostMigrationEngine::PreferredSuccessor(bool bRequireReady) const
	{
		for (const RankedHost& R : Ranking_)
		{
			if (!bRequireReady || R.bSuccessorReady) return R;
		}
		return std::nullopt;
	}

	bool HostMigrationEngine::EvaluateProactiveMigration(int CurrentHostScore)
	{
		auto Pref = PreferredSuccessor(true);
		if (!Pref) { AdvantageSince_ = 0; AdvantageCandidate_.reset(); return false; }
		if (!AdvantageCandidate_ || AdvantageCandidate_->PlayerId != Pref->Who.PlayerId)
		{
			AdvantageCandidate_ = Pref->Who;
			AdvantageSince_ = Clock_->Now();
		}
		return ShouldProactivelyMigrate(CurrentHostScore, Pref->Score.Total, AdvantageSince_, Clock_->Now(), Cfg_.Hysteresis);
	}

	Status HostMigrationEngine::PickSuccessor(bool bEmergency)
	{
		if (!bRankingFrozen_) RefreshRanking(bEmergency);
		auto Pref = PreferredSuccessor(true);
		if (!Pref) return MakeError(ErrorCode::NotFound, "no READY successor");
		ChosenSuccessor_ = Pref->Who;
		Record("SuccessorSelected", ChosenSuccessor_->DisplayName.empty() ? ChosenSuccessor_->PlayerId : ChosenSuccessor_->DisplayName);
		return {};
	}

	Status HostMigrationEngine::BeginPlannedMigration()
	{
		if (Phase_ != MigrationPhase::Running) return MakeError(ErrorCode::Invalid, "migration already in progress");
		if (CurrentHost_.PlayerId != Me_.PlayerId) return MakeError(ErrorCode::Unauthorized, "only the current host may begin planned migration");
		Metrics_ = MigrationMetrics{};
		Metrics_.LeaveAt = Clock_->Now();
		RefreshRanking(false);
		if (Cfg_.FreezeRankingOnMigrationStart) bRankingFrozen_ = true;
		SW_TRY(PickSuccessor(false));
		Enter(MigrationPhase::MigrationPreparing, "Preparing " + ChosenSuccessor_->DisplayName);
		if (Hooks_.PrepareSuccessor)
		{
			SW_TRY(Hooks_.PrepareSuccessor(*ChosenSuccessor_));
		}
		Enter(MigrationPhase::FinalSave, "Saving world...");
		if (Hooks_.RequestFinalSave) SW_TRY(Hooks_.RequestFinalSave());
		return {};
	}

	Status HostMigrationEngine::OnFinalSaveCompleted(const std::string& LocalSavePath)
	{
		if (Phase_ != MigrationPhase::FinalSave && Phase_ != MigrationPhase::MigrationPreparing)
		{
			return MakeError(ErrorCode::Invalid, "not awaiting final save");
		}
		Enter(MigrationPhase::RevisionSync, "Uploading revision...");
		if (!Hooks_.UploadAndCommit) return MakeError(ErrorCode::Invalid, "no upload hook");
		auto Rev = Hooks_.UploadAndCommit(LocalSavePath);
		if (!Rev)
		{
			// Planned migration: do not release the lease if storage failed.
			bRankingFrozen_ = false;
			Enter(MigrationPhase::Running, "upload failed; remaining host");
			return Rev.Err();
		}
		return OnRevisionCommitted(*Rev);
	}

	Status HostMigrationEngine::OnRevisionCommitted(const RevisionMeta& Rev)
	{
		Revision_ = Rev.Number;
		Record("RevisionCommitted", "revision=" + std::to_string(Rev.Number) + " gen=" + std::to_string(Rev.Generation));
		Enter(MigrationPhase::SuccessorReady, ChosenSuccessor_ ? ChosenSuccessor_->DisplayName : "");
		if (Hooks_.PrepareSuccessor && ChosenSuccessor_) SW_TRY(Hooks_.PrepareSuccessor(*ChosenSuccessor_));
		Enter(MigrationPhase::LeaseHandoff, "Releasing lease to successor");
		return CompleteLeaseHandoff();
	}

	Status HostMigrationEngine::OnSuccessorPrepared(const Identity& Successor)
	{
		ChosenSuccessor_ = Successor;
		Record("SuccessorReady", Successor.DisplayName.empty() ? Successor.PlayerId : Successor.DisplayName);
		return {};
	}

	Status HostMigrationEngine::CompleteLeaseHandoff()
	{
		if (!ChosenSuccessor_) return MakeError(ErrorCode::Invalid, "no successor");
		if (!Token_)
		{
			// Simulator / tests may call Release via hooks through Token set externally.
			Record("LeaseReleased", "successor=" + ChosenSuccessor_->PlayerId);
			Enter(MigrationPhase::SuccessorStarting, ChosenSuccessor_->DisplayName);
			return {};
		}
		const Status R = Leases_->Release(*Token_, ChosenSuccessor_);
		if (!R.Ok()) return R;
		Record("LeaseReleased", "successor=" + ChosenSuccessor_->PlayerId);
		Token_.reset();
		Enter(MigrationPhase::SuccessorStarting, ChosenSuccessor_->DisplayName);
		return {};
	}

	Status HostMigrationEngine::OnSuccessorFailed(const std::string& Reason)
	{
		Record("SuccessorFailed", Reason);
		if (!Cfg_.AllowSuccessorFallback) return Fail(Reason);
		bRankingFrozen_ = false;
		// Drop the failed successor from readiness by marking disconnected in candidates.
		for (HostCandidate& C : Candidates_)
		{
			if (ChosenSuccessor_ && C.Who.PlayerId == ChosenSuccessor_->PlayerId) C.bConnected = false;
		}
		ChosenSuccessor_.reset();
		const bool bEmergency = Phase_ == MigrationPhase::HostLost || Phase_ == MigrationPhase::SuccessorElection ||
			Phase_ == MigrationPhase::SuccessorStarting || Phase_ == MigrationPhase::LeaseExpired;
		RefreshRanking(bEmergency);
		auto Pref = PreferredSuccessor(true);
		if (!Pref) return Fail("no fallback successor: " + Reason);
		ChosenSuccessor_ = Pref->Who;
		Record("SuccessorFallback", ChosenSuccessor_->DisplayName.empty() ? ChosenSuccessor_->PlayerId : ChosenSuccessor_->DisplayName);
		if (Phase_ == MigrationPhase::MigrationPreparing || Phase_ == MigrationPhase::FinalSave || Phase_ == MigrationPhase::RevisionSync ||
			Phase_ == MigrationPhase::SuccessorReady)
		{
			Enter(MigrationPhase::MigrationPreparing, "fallback " + ChosenSuccessor_->DisplayName);
			return {};
		}
		if (Phase_ == MigrationPhase::SuccessorStarting || Phase_ == MigrationPhase::LeaseHandoff || Phase_ == MigrationPhase::SessionPublished)
		{
			Enter(MigrationPhase::SuccessorElection, "fallback after start failure");
			return {};
		}
		return {};
	}

	Status HostMigrationEngine::OnHostLost()
	{
		Metrics_ = MigrationMetrics{};
		Metrics_.LeaveAt = Clock_->Now();
		ClientState_ = ClientMigrationState::HostLossDetected;
		Enter(MigrationPhase::HostLost, "HOST MIGRATION");
		Enter(MigrationPhase::RecoveryWait, "Recovering Shared World...");
		ClientState_ = ClientMigrationState::WaitingForLease;
		RebuildDiagnostics();
		return {};
	}

	Status HostMigrationEngine::Tick(TimeMs Now)
	{
		(void)Now;
		switch (Phase_)
		{
		case MigrationPhase::FinalSave:
			if (TimedOut(Cfg_.Timeouts.FinalSaveTimeout)) return Fail("FinalSaveTimeout");
			break;
		case MigrationPhase::RevisionSync:
			if (TimedOut(Cfg_.Timeouts.UploadTimeout)) return Fail("UploadTimeout");
			break;
		case MigrationPhase::SuccessorReady:
		case MigrationPhase::MigrationPreparing:
			if (TimedOut(Cfg_.Timeouts.SuccessorReadyTimeout)) return OnSuccessorFailed("SuccessorReadyTimeout");
			break;
		case MigrationPhase::SuccessorStarting:
			if (TimedOut(Cfg_.Timeouts.LeaseAcquireTimeout)) return OnSuccessorFailed("LeaseAcquireTimeout");
			break;
		case MigrationPhase::SessionPublished:
		case MigrationPhase::ClientReconnect:
			if (TimedOut(Cfg_.Timeouts.SessionPublishTimeout) && Phase_ == MigrationPhase::SessionPublished)
				return OnSuccessorFailed("SessionPublishTimeout");
			if (TimedOut(Cfg_.Timeouts.ReconnectTimeout) && Phase_ == MigrationPhase::ClientReconnect)
				return Fail("ReconnectTimeout");
			break;
		case MigrationPhase::RecoveryWait:
		{
			auto Snap = Leases_->Store().Load();
			if (!Snap) return Snap.Err();
			if (!Leases_->LiveForObserver(Snap->State.CurrentLease, Clock_->Now()))
			{
				return OnLeaseExpired();
			}
			break;
		}
		default: break;
		}
		RebuildDiagnostics();
		return {};
	}

	Status HostMigrationEngine::OnLeaseExpired()
	{
		Enter(MigrationPhase::LeaseExpired, "");
		ClientState_ = ClientMigrationState::WaitingForSuccessor;
		Enter(MigrationPhase::SuccessorElection, "");
		SW_TRY(PickSuccessor(true));
		Enter(MigrationPhase::RevisionRecovery, "");
		RebuildDiagnostics();
		return {};
	}

	Status HostMigrationEngine::TryAcquireAsSuccessor()
	{
		if (ChosenSuccessor_ && ChosenSuccessor_->PlayerId != Me_.PlayerId && Phase_ != MigrationPhase::SuccessorElection)
		{
			ClientState_ = ClientMigrationState::WaitingForSuccessor;
			return {}; // not our turn; wait for published session
		}
		auto Acq = Leases_->Acquire(Me_, "migration-" + Me_.InstallId);
		if (!Acq) return Acq.Err();
		if (Acq->Outcome == AcquireOutcome::Acquired || Acq->Outcome == AcquireOutcome::AlreadyHeld)
		{
			Token_ = Acq->Token;
			Generation_ = Acq->Token->Generation;
			Metrics_.NewLeaseAt = Clock_->Now();
			Record("LeaseAcquire", "generation=" + std::to_string(Generation_));
			Enter(MigrationPhase::SuccessorStarting, Me_.DisplayName);
			if (Hooks_.SuccessorStartHosting) SW_TRY(Hooks_.SuccessorStartHosting());
			if (Hooks_.CanPublishSession && !Hooks_.CanPublishSession())
			{
				return OnSuccessorFailed("session creation failed");
			}
			JoinInfo Join;
			Join.Kind = "online-session-id";
			Join.Data = "session-" + Me_.PlayerId + "-" + std::to_string(Generation_);
			if (Hooks_.PublishSession) SW_TRY(Hooks_.PublishSession(Join));
			return OnSessionPublished(Join);
		}
		if (Acq->Outcome == AcquireOutcome::HeldByOther || Acq->Outcome == AcquireOutcome::ReservedForSuccessor)
		{
			ClientState_ = ClientMigrationState::WaitingForSession;
			Enter(MigrationPhase::ClientReconnect, "waiting for new host session");
			return {};
		}
		return MakeError(ErrorCode::Conflict, ToString(Acq->Outcome));
	}

	Status HostMigrationEngine::OnSessionPublished(const JoinInfo& Join)
	{
		PublishedJoin_ = Join;
		Metrics_.SessionAvailableAt = Clock_->Now();
		Record("SessionPublished", Join.Data);
		// Update lease join via renew if we hold the token
		if (Token_)
		{
			LeaseUpdate U;
			U.Phase = LeasePhase::Hosting;
			U.HostReady = true;
			U.Join = Join;
			auto R = Leases_->Renew(*Token_, U);
			if (!R) return R.Err();
			Generation_ = Token_->Generation;
			Revision_ = Token_->BaseRevision;
			CurrentHost_ = Me_;
		}
		Enter(MigrationPhase::SessionPublished, Join.Data);
		Enter(MigrationPhase::ClientReconnect, "");
		ClientState_ = ClientMigrationState::Connected; // host itself
		RebuildDiagnostics();
		return {};
	}

	Status HostMigrationEngine::OnClientReconnected()
	{
		ClientState_ = ClientMigrationState::Connected;
		if (!Metrics_.FirstClientReconnectedAt) Metrics_.FirstClientReconnectedAt = Clock_->Now();
		Metrics_.AllClientsReconnectedAt = Clock_->Now();
		Record("ClientReconnected", Me_.PlayerId);
		return MarkMigrationCompleted();
	}

	Status HostMigrationEngine::MarkMigrationCompleted()
	{
		bRankingFrozen_ = false;
		Enter(MigrationPhase::Running, "MigrationCompleted");
		ClientState_ = ClientMigrationState::ConnectedToHost;
		Record("MigrationCompleted", "");
		RebuildDiagnostics();
		return {};
	}

	Status HostMigrationEngine::Fail(const std::string& Reason)
	{
		Enter(MigrationPhase::Failed, Reason);
		ClientState_ = ClientMigrationState::Failed;
		bRankingFrozen_ = false;
		Record("MigrationFailed", Reason);
		RebuildDiagnostics();
		return MakeError(ErrorCode::BadState, Reason);
	}

	void HostMigrationEngine::SetLeaseToken(LeaseToken Token)
	{
		Token_ = std::move(Token);
	}

	void HostMigrationEngine::RebuildDiagnostics()
	{
		Diag_.Phase = Phase_;
		Diag_.ClientState = ClientState_;
		Diag_.CurrentHost = CurrentHost_;
		Diag_.Generation = Generation_;
		Diag_.Revision = Revision_;
		Diag_.Ranking = Ranking_;
		Diag_.RttGridMs = Matrix_.RttGridMs();
		Diag_.PlayerIds = Matrix_.Players();
		Diag_.Metrics = Metrics_;
		Diag_.Trace = Trace_;
		if (auto P = PreferredSuccessor(true)) Diag_.PreferredSuccessor = P->Who;
		else Diag_.PreferredSuccessor.reset();
		switch (Phase_)
		{
		case MigrationPhase::Running: Diag_.OverlayMessage.clear(); break;
		case MigrationPhase::HostLost:
		case MigrationPhase::RecoveryWait: Diag_.OverlayMessage = "HOST MIGRATION\nSelecting a new host — stay here, the world will continue automatically."; break;
		case MigrationPhase::LeaseExpired: Diag_.OverlayMessage = "HOST MIGRATION\nFormer host lock expired — taking over..."; break;
		case MigrationPhase::SuccessorElection: Diag_.OverlayMessage = "HOST MIGRATION\nSelecting the next host..."; break;
		case MigrationPhase::RevisionRecovery: Diag_.OverlayMessage = "HOST MIGRATION\nLoading the Shared World for takeover..."; break;
		case MigrationPhase::FinalSave: Diag_.OverlayMessage = "HOST MIGRATION\nSaving world..."; break;
		case MigrationPhase::MigrationPreparing: Diag_.OverlayMessage = "HOST MIGRATION\nPreparing successor..."; break;
		case MigrationPhase::RevisionSync: Diag_.OverlayMessage = "HOST MIGRATION\nTransferring world..."; break;
		case MigrationPhase::SuccessorStarting: Diag_.OverlayMessage = "HOST MIGRATION\nStarting new host..."; break;
		case MigrationPhase::ClientReconnect: Diag_.OverlayMessage = "HOST MIGRATION\nReconnecting..."; break;
		default: Diag_.OverlayMessage = std::string("HOST MIGRATION\n") + ToString(Phase_); break;
		}
	}
}
