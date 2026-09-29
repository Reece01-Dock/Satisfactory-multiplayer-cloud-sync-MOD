#include "MultiplayerSimulator.h"

#include <algorithm>
#include <sstream>

#include "SharedWorldCore/Util/Sha256.h"
#include "TestHelpers.h"

namespace swsim
{
	using namespace sw;

	MultiplayerSimulator::MultiplayerSimulator(uint64_t Seed)
		: Seed_(Seed), Rng_(Seed), Clock_(std::make_shared<SimulatedClock>(swtest::StartTime))
	{
		Repo_ = std::make_shared<MemoryRepository>();
		Objects_ = std::make_shared<MemoryObjectStore>();
		auto Store = std::make_shared<WorldStore>(Repo_, swtest::TestWorldId, Clock_, Logger(), 200);
		(void)Store->Create(swtest::TestWorld());
		Leases_ = std::make_shared<LeaseManager>(Store, LeaseConfig{});
		Net_ = std::make_shared<SimulatedNetworkQualityProvider>();
		Cfg_.Lease = Leases_->Config();
		Cfg_.FreezeRankingOnMigrationStart = true;
		Cfg_.AllowSuccessorFallback = true;
	}

	SimPlayer* MultiplayerSimulator::Find(const std::string& PlayerId)
	{
		for (auto& P : Players_)
		{
			if (P->Id.PlayerId == PlayerId) return P.get();
		}
		return nullptr;
	}

	SimPlayer* MultiplayerSimulator::CurrentHost()
	{
		for (auto& P : Players_)
		{
			if (P->bAuthoritative) return P.get();
		}
		return nullptr;
	}

	int MultiplayerSimulator::AuthoritativeCount() const
	{
		int N = 0;
		for (const auto& P : Players_)
		{
			if (P->bAuthoritative) ++N;
		}
		return N;
	}

	SimPlayer& MultiplayerSimulator::AddPlayer(const std::string& DisplayName, int Index)
	{
		auto P = std::make_unique<SimPlayer>();
		P->Id = Identity{"player-" + std::to_string(Index), DisplayName, "EOS", "install-" + std::to_string(Index)};
		P->Engine = std::make_unique<HostMigrationEngine>(P->Id, Clock_, Leases_, Cfg_);
		Players_.push_back(std::move(P));
		return *Players_.back();
	}

	void MultiplayerSimulator::SetOnline(const std::string& PlayerId, bool bOnline)
	{
		if (SimPlayer* P = Find(PlayerId)) P->bOnline = bOnline;
	}

	std::vector<std::string> MultiplayerSimulator::OnlineIds() const
	{
		std::vector<std::string> Ids;
		for (const auto& P : Players_)
		{
			if (P->bOnline) Ids.push_back(P->Id.PlayerId);
		}
		std::sort(Ids.begin(), Ids.end());
		return Ids;
	}

	std::vector<HostCandidate> MultiplayerSimulator::BuildCandidates() const
	{
		PeerQualityMatrix Matrix;
		Matrix.SetPlayers(OnlineIds());
		Net_->ApplyTo(Matrix, Clock_->Now());
		std::vector<HostCandidate> Out;
		for (const auto& P : Players_)
		{
			HostCandidate C = MakeCandidate(P->Id, Matrix, OnlineIds(), P->bOnline, P->bCompatible, P->bStorageReachable,
				P->bHasHeadCached, P->bSessionCapable, P->bHostEligible, P->bRecentlyUnstable);
			C.bUploadSufficient = P->bUploadSufficient;
			Out.push_back(C);
		}
		return Out;
	}

	void MultiplayerSimulator::BuildMatrix()
	{
		PeerQualityMatrix Matrix;
		Matrix.SetPlayers(OnlineIds());
		Net_->ApplyTo(Matrix, Clock_->Now());
		for (auto& P : Players_)
		{
			P->Engine->SetMatrix(Matrix);
			P->Engine->Config() = Cfg_;
		}
	}

	void MultiplayerSimulator::RefreshAllRankings(bool bEmergency)
	{
		BuildMatrix();
		const auto Cand = BuildCandidates();
		for (auto& P : Players_)
		{
			if (!P->bOnline) continue;
			P->Engine->UpdateCandidates(Cand);
			P->Engine->RefreshRanking(bEmergency);
		}
	}

	std::vector<RankedHost> MultiplayerSimulator::GlobalRanking() const
	{
		Identity Host;
		for (const auto& P : Players_)
		{
			if (P->bAuthoritative) Host = P->Id;
		}
		return RankHosts(BuildCandidates(), Host, Cfg_.Scores, false);
	}

	Status MultiplayerSimulator::CommitNextRevision(LeaseToken& Tok, const std::string& Reason)
	{
		RevisionMeta R;
		R.Number = Tok.BaseRevision + 1;
		R.Generation = Tok.Generation;
		R.PreviousRevision = Tok.BaseRevision;
		R.ObjectSha256 = Sha256::HexOf("save-" + std::to_string(R.Number) + "-" + Tok.Holder.PlayerId);
		R.Size = 4096;
		R.CreatedAt = Clock_->Now();
		R.Uploader = Tok.Holder;
		R.Reason = Reason;
		SW_TRY(Leases_->CommitRevision(Tok, R));
		MaxSeenRevision_ = R.Number;
		MaxSeenGeneration_ = Tok.Generation;
		return {};
	}

	Status MultiplayerSimulator::BootstrapHost(const std::string& HostPlayerId)
	{
		SimPlayer* H = Find(HostPlayerId);
		if (!H) return MakeError(ErrorCode::NotFound, "host player missing");
		auto Acq = Leases_->Acquire(H->Id, "boot-" + H->Id.InstallId);
		if (!Acq) return Acq.Err();
		if (Acq->Outcome != AcquireOutcome::Acquired && Acq->Outcome != AcquireOutcome::AlreadyHeld)
		{
			return MakeError(ErrorCode::Conflict, ToString(Acq->Outcome));
		}
		H->Token = Acq->Token;
		H->bAuthoritative = true;
		H->Engine->SetLeaseToken(*H->Token);
		SW_TRY(CommitNextRevision(*H->Token, Reason::Import));
		JoinInfo Join;
		Join.Kind = "online-session-id";
		Join.Data = "session-" + H->Id.PlayerId + "-g" + std::to_string(H->Token->Generation);
		LeaseUpdate U;
		U.Phase = LeasePhase::Hosting;
		U.HostReady = true;
		U.Join = Join;
		std::vector<SessionPlayer> Ps;
		for (const auto& P : Players_)
		{
			if (P->bOnline) Ps.push_back({P->Id.DisplayName, P->Id.PlayerId, P->Id.InstallId});
		}
		U.Players = Ps;
		SW_TRY(Leases_->Renew(*H->Token, U));
		for (auto& P : Players_)
		{
			P->Engine->SetCurrentHost(H->Id, H->Token->Generation, H->Token->BaseRevision);
		}
		RefreshAllRankings(false);
		MaxSeenGeneration_ = H->Token->Generation;
		MaxSeenRevision_ = H->Token->BaseRevision;
		return {};
	}

	Status MultiplayerSimulator::JoinExistingHost(const std::string& ClientPlayerId)
	{
		SimPlayer* C = Find(ClientPlayerId);
		SimPlayer* H = CurrentHost();
		if (!C || !H || !H->Token) return MakeError(ErrorCode::BadState, "no live host to join");
		auto Acq = Leases_->Acquire(C->Id, "join-" + C->Id.InstallId);
		if (!Acq) return Acq.Err();
		if (Acq->Outcome == AcquireOutcome::Acquired)
		{
			// Must not happen while host lease is live.
			return MakeError(ErrorCode::Conflict, "client unexpectedly acquired authority");
		}
		if (Acq->Outcome != AcquireOutcome::HeldByOther && Acq->Outcome != AcquireOutcome::ReservedForSuccessor)
		{
			return MakeError(ErrorCode::Conflict, ToString(Acq->Outcome));
		}
		C->bAuthoritative = false;
		C->Engine->SetCurrentHost(H->Id, H->Token->Generation, H->Token->BaseRevision);
		++C->JoinCount;
		C->Engine->Record("JoinedExistingHost", H->Id.PlayerId);
		return {};
	}

	Status MultiplayerSimulator::PlannedLeave(const std::string& HostPlayerId)
	{
		SimPlayer* H = Find(HostPlayerId);
		if (!H || !H->bAuthoritative || !H->Token) return MakeError(ErrorCode::BadState, "not hosting");
		RefreshAllRankings(false);
		H->Engine->SetLeaseToken(*H->Token);
		SW_TRY(H->Engine->BeginPlannedMigration());
		std::optional<RankedHost> Pref = H->Engine->PreferredSuccessor(true);
		if (!Pref) return MakeError(ErrorCode::NotFound, "no successor");

		// Final save + commit under current generation (authority still with H).
		SW_TRY(CommitNextRevision(*H->Token, Reason::Migration));
		auto Snap = Leases_->Store().Load();
		if (!Snap) return Snap.Err();
		// OnRevisionCommitted → CompleteLeaseHandoff releases the lease with successor reservation.
		SW_TRY(H->Engine->OnRevisionCommitted(*Snap->State.Head));
		H->bAuthoritative = false;
		H->Token.reset();
		H->bHostEligible = false; // intentional leave: do not reclaim during this handoff

		Pref = H->Engine->PreferredSuccessor(true);
		if (!Pref) return MakeError(ErrorCode::NotFound, "no successor after handoff");
		SimPlayer* Succ = Find(Pref->Who.PlayerId);
		if (!Succ || !Succ->bOnline)
		{
			// Reservation was for an offline player: wait out the handoff window then recover.
			Clock_->Advance(Leases_->Config().HandoffWindow + Seconds(1));
			return AdvanceRecovery();
		}

		auto Acq = Leases_->Acquire(Succ->Id, "mig-" + Succ->Id.InstallId);
		if (!Acq) return Acq.Err();
		if (Acq->Outcome != AcquireOutcome::Acquired && Acq->Outcome != AcquireOutcome::AlreadyHeld)
		{
			if (Cfg_.AllowSuccessorFallback)
			{
				SW_TRY(H->Engine->OnSuccessorFailed("acquire failed: " + std::string(ToString(Acq->Outcome))));
				auto Fallback = H->Engine->PreferredSuccessor(true);
				if (!Fallback) return MakeError(ErrorCode::NotFound, "no fallback");
				Succ = Find(Fallback->Who.PlayerId);
				if (!Succ) return MakeError(ErrorCode::NotFound, "fallback missing");
				Acq = Leases_->Acquire(Succ->Id, "mig-fb-" + Succ->Id.InstallId);
				if (!Acq) return Acq.Err();
			}
			if (Acq->Outcome != AcquireOutcome::Acquired && Acq->Outcome != AcquireOutcome::AlreadyHeld)
			{
				return MakeError(ErrorCode::Conflict, ToString(Acq->Outcome));
			}
		}
		Succ->Token = Acq->Token;
		Succ->bAuthoritative = true;
		Succ->Engine->SetLeaseToken(*Succ->Token);
		Succ->Engine->Record("LeaseAcquire", "generation=" + std::to_string(Succ->Token->Generation));
		MaxSeenGeneration_ = Succ->Token->Generation;

		if (!Succ->bCanPublishSession)
		{
			// Session failure: release and let fallback take over via crash-like path.
			SW_TRY(Leases_->Release(*Succ->Token, std::nullopt));
			Succ->bAuthoritative = false;
			Succ->Token.reset();
			Succ->bSessionCapable = false;
			SW_TRY(Succ->Engine->OnSuccessorFailed("session creation failed"));
			// No need to wait a full TTL: lease is already clear.
			return AdvanceRecovery(Seconds(1), 40);
		}

		JoinInfo Join;
		Join.Kind = "online-session-id";
		Join.Data = "session-" + Succ->Id.PlayerId + "-g" + std::to_string(Succ->Token->Generation);
		LeaseUpdate U;
		U.Phase = LeasePhase::Hosting;
		U.HostReady = true;
		U.Join = Join;
		SW_TRY(Leases_->Renew(*Succ->Token, U));
		SW_TRY(Succ->Engine->OnSessionPublished(Join));

		for (auto& P : Players_)
		{
			if (!P->bOnline) continue;
			P->Engine->SetCurrentHost(Succ->Id, Succ->Token->Generation, Succ->Token->BaseRevision);
			if (P.get() == Succ) continue;
			P->Engine->Record("Reconnecting", Join.Data);
			++P->JoinCount;
			SW_TRY(P->Engine->OnClientReconnected());
		}
		SW_TRY(Succ->Engine->MarkMigrationCompleted());
		SW_TRY(H->Engine->MarkMigrationCompleted());
		RefreshAllRankings(false);
		return {};
	}

	Status MultiplayerSimulator::CrashHost(const std::string& HostPlayerId)
	{
		SimPlayer* H = Find(HostPlayerId);
		if (!H || !H->bAuthoritative) return MakeError(ErrorCode::BadState, "not hosting");
		H->bAuthoritative = false;
		H->bOnline = false;
		// Crash recovery may need any remaining player: clear intentional-leave ineligibility.
		for (auto& P : Players_)
		{
			if (P->bOnline) P->bHostEligible = true;
		}
		// Keep stale token so ExpectStaleHostFenced can use it later if copied by caller.
		for (auto& P : Players_)
		{
			if (!P->bOnline) continue;
			SW_TRY(P->Engine->OnHostLost());
		}
		RefreshAllRankings(true);
		return {};
	}

	Status MultiplayerSimulator::AdvanceRecovery(TimeMs Step, int MaxSteps)
	{
		for (int i = 0; i < MaxSteps; ++i)
		{
			Clock_->Advance(Step);
			for (auto& P : Players_)
			{
				if (!P->bOnline) continue;
				SW_TRY(P->Engine->Tick(Clock_->Now()));
			}
			auto Snap = Leases_->Store().Load();
			if (!Snap) return Snap.Err();
			if (Leases_->LiveForObserver(Snap->State.CurrentLease, Clock_->Now()))
			{
				// Existing live lease (someone already acquired).
				if (Snap->State.CurrentLease)
				{
					SimPlayer* H = Find(Snap->State.CurrentLease->Holder.PlayerId);
					if (H && H->bAuthoritative && Snap->State.CurrentLease->Join)
					{
						for (auto& P : Players_)
						{
							if (!P->bOnline || P.get() == H) continue;
							if (P->Engine->Phase() != MigrationPhase::Running)
							{
								++P->JoinCount;
								SW_TRY(P->Engine->OnClientReconnected());
							}
						}
						return {};
					}
				}
				continue;
			}
			// Lease expired: preferred READY successor attempts acquire.
			RefreshAllRankings(true);
			std::vector<RankedHost> Ranked = RankHosts(BuildCandidates(), Identity{}, Cfg_.Scores, true);
			if (Ranked.empty())
			{
				// Last resort: any connected storage-reachable host-eligible player.
				for (auto& P : Players_)
				{
					if (!P->bOnline || !P->bHostEligible || !P->bStorageReachable || !P->bCompatible) continue;
					HostCandidate C;
					C.Who = P->Id;
					C.bConnected = true;
					C.bHostEligible = true;
					C.bCompatible = true;
					C.bStorageReachable = true;
					C.bSessionCapable = P->bSessionCapable;
					C.bHasHeadCached = P->bHasHeadCached;
					C.PingMs = 50;
					RankedHost R;
					R.Who = P->Id;
					R.Score = ScoreHost(C, Cfg_.Scores, true);
					R.bSuccessorReady = R.Score.bEligible;
					if (R.bSuccessorReady) Ranked.push_back(R);
				}
			}
			bool bGotHost = false;
			for (const RankedHost& R : Ranked)
			{
				SimPlayer* Cand = Find(R.Who.PlayerId);
				if (!Cand || !Cand->bOnline || !R.bSuccessorReady) continue;
				auto Acq = Leases_->Acquire(Cand->Id, "crash-" + Cand->Id.InstallId + "-" + std::to_string(i));
				if (!Acq) continue;
				if (Acq->Outcome == AcquireOutcome::Acquired || Acq->Outcome == AcquireOutcome::AlreadyHeld)
				{
					Cand->Token = Acq->Token;
					Cand->bAuthoritative = true;
					Cand->Engine->SetLeaseToken(*Cand->Token);
					MaxSeenGeneration_ = Cand->Token->Generation;
					JoinInfo Join;
					Join.Kind = "online-session-id";
					Join.Data = "session-" + Cand->Id.PlayerId + "-g" + std::to_string(Cand->Token->Generation);
					if (!Cand->bCanPublishSession)
					{
						SW_TRY(Leases_->Release(*Cand->Token, std::nullopt));
						Cand->bAuthoritative = false;
						Cand->Token.reset();
						Cand->bSessionCapable = false;
						continue;
					}
					LeaseUpdate U;
					U.Phase = LeasePhase::Hosting;
					U.HostReady = true;
					U.Join = Join;
					SW_TRY(Leases_->Renew(*Cand->Token, U));
					SW_TRY(Cand->Engine->OnSessionPublished(Join));
					for (auto& P : Players_)
					{
						if (!P->bOnline) continue;
						P->Engine->SetCurrentHost(Cand->Id, Cand->Token->Generation, Cand->Token->BaseRevision);
						if (P.get() == Cand)
						{
							SW_TRY(P->Engine->MarkMigrationCompleted());
							continue;
						}
						++P->JoinCount;
						SW_TRY(P->Engine->OnClientReconnected());
					}
					bGotHost = true;
					break;
				}
			}
			if (bGotHost) return {};
		}
		return MakeError(ErrorCode::BadState, "recovery timed out; online=" + std::to_string(OnlineIds().size()) +
			" auth=" + std::to_string(AuthoritativeCount()));
	}

	Status MultiplayerSimulator::ExpectStaleHostFenced(const std::string& OldHostId, const LeaseToken& StaleToken)
	{
		(void)OldHostId;
		LeaseUpdate U;
		U.Phase = LeasePhase::Hosting;
		auto Renew = Leases_->Renew(StaleToken, U);
		if (Renew) return MakeError(ErrorCode::BadState, "stale renew unexpectedly succeeded");
		if (Renew.Err().Code != ErrorCode::Fenced) return Renew.Err();
		RevisionMeta R;
		R.Number = StaleToken.BaseRevision + 1;
		R.Generation = StaleToken.Generation;
		R.PreviousRevision = StaleToken.BaseRevision;
		R.ObjectSha256 = Sha256::HexOf("stale");
		R.Size = 1;
		R.CreatedAt = Clock_->Now();
		R.Uploader = StaleToken.Holder;
		R.Reason = Reason::Checkpoint;
		auto Commit = Leases_->CommitRevision(const_cast<LeaseToken&>(StaleToken), R);
		if (Commit) return MakeError(ErrorCode::BadState, "stale commit unexpectedly succeeded");
		if (Commit.Err().Code != ErrorCode::Fenced && Commit.Err().Code != ErrorCode::StaleRevision) return Commit.Err();
		auto Rel = Leases_->Release(StaleToken, std::nullopt);
		if (Rel.Ok()) return MakeError(ErrorCode::BadState, "stale release unexpectedly succeeded");
		if (Rel.Err().Code != ErrorCode::Fenced) return Rel.Err();
		return {};
	}

	std::vector<InvariantViolation> MultiplayerSimulator::CheckInvariants() const
	{
		std::vector<InvariantViolation> V;
		if (AuthoritativeCount() > 1) V.push_back({"more than one authoritative host"});
		auto Snap = Leases_->Store().Load();
		if (!Snap)
		{
			V.push_back({"cannot load world state"});
			return V;
		}
		const WorldState& S = Snap->State;
		if (S.Head && S.Head->Number < MaxSeenRevision_) V.push_back({"revision moved backwards"});
		if (S.Generation < MaxSeenGeneration_ && MaxSeenGeneration_ > 0) V.push_back({"generation moved backwards"});
		// Deterministic ranking agreement among online observers (recomputed from shared inputs).
		if (AuthoritativeCount() == 1)
		{
			Identity HostId;
			for (const auto& P : Players_)
			{
				if (P->bAuthoritative) HostId = P->Id;
			}
			const auto Cand = BuildCandidates();
			const auto Ref = RankHosts(Cand, HostId, Cfg_.Scores, false);
			for (const auto& P : Players_)
			{
				if (!P->bOnline) continue;
				const auto Ranked = RankHosts(Cand, HostId, Cfg_.Scores, false);
				if (Ranked.size() != Ref.size())
				{
					V.push_back({"ranking size disagreement involving " + P->Id.PlayerId});
					break;
				}
				for (size_t i = 0; i < Ref.size(); ++i)
				{
					if (Ranked[i].Who.PlayerId != Ref[i].Who.PlayerId || Ranked[i].Score.Total != Ref[i].Score.Total)
					{
						V.push_back({"ranking disagreement involving " + P->Id.PlayerId});
						break;
					}
				}
			}
		}
		return V;
	}

	std::string MultiplayerSimulator::DumpAllTraces() const
	{
		std::ostringstream O;
		O << "seed=" << Seed_ << "\n";
		for (const auto& P : Players_)
		{
			O << "=== " << P->Id.DisplayName << " ===\n" << P->Engine->DumpTrace();
		}
		return O.str();
	}
}
