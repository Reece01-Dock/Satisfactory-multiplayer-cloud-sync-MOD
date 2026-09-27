#include "SharedWorldCore/Lease/Lease.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace sw
{
	// ------------------------------------------------------------ WorldStore

	WorldStore::WorldStore(std::shared_ptr<IWorldRepository> InRepo, std::string InWorldId, std::shared_ptr<IClock> InClock, Logger InLog, int InMaxAttempts)
		: Repo(std::move(InRepo)), Id(std::move(InWorldId)), ClockPtr(std::move(InClock)), Logs(std::move(InLog)), MaxAttempts(InMaxAttempts)
	{
	}

	Result<StateSnapshot> WorldStore::Load()
	{
		SW_TRY(ValidateWorldId(Id));
		auto Head = Repo->Head();
		if (Head.Is(ErrorCode::NotFound))
		{
			return MakeError(ErrorCode::NoWorld, "this Shared World does not exist yet");
		}
		if (!Head)
		{
			return Head.Err().Wrap("read repository head");
		}
		auto Text = Repo->ReadFile(*Head, Paths::State);
		if (Text.Is(ErrorCode::NotFound))
		{
			return MakeError(ErrorCode::NoWorld, "repository contains no Shared World state");
		}
		if (!Text)
		{
			return Text.Err().Wrap("read world state");
		}
		auto State = DecodeState(*Text, Id);
		if (!State)
		{
			return State.Err().Wrap("world state failed validation");
		}
		return StateSnapshot{*Head, std::move(State.Value())};
	}

	Result<StateSnapshot> WorldStore::Create(const NewWorld& World)
	{
		SW_TRY(World.Info.Validate());
		if (World.Info.WorldId != Id)
		{
			return MakeError(ErrorCode::Invalid, "world id mismatch");
		}
		SW_TRY(World.Players.Validate());
		SW_TRY(World.Settings.Validate());
		WorldState S;
		S.WorldId = Id;
		S.StateVersion = 1;
		S.UpdatedAt = ClockPtr->Now();
		SW_TRY(S.Validate(Id));
		const std::vector<FileChange> Files = {
			{Paths::WorldInfo, json::Serialize(World.Info.ToJson(), 2)},
			{Paths::State, EncodeState(S)},
			{Paths::Players, json::Serialize(World.Players.ToJson(), 2)},
			{Paths::Settings, json::Serialize(World.Settings.ToJson(), 2)},
		};
		auto Head = Repo->Head();
		if (Head.Ok())
		{
			// The repository exists; only allowed if it holds no world yet.
			if (Repo->ReadFile(*Head, Paths::State).Ok())
			{
				return MakeError(ErrorCode::AlreadyExists, "a Shared World already exists in this repository");
			}
		}
		else if (!Head.Is(ErrorCode::NotFound))
		{
			return Head.Err();
		}
		auto C = Repo->Commit(Head.Ok() ? *Head : std::string(), Files, "Create Shared World: " + World.Info.Name);
		if (!C)
		{
			return C.Err();
		}
		Logs.Info("WorldCreated", {{"world", Id}, {"name", World.Info.Name}});
		return StateSnapshot{*C, std::move(S)};
	}

	Result<WorldInfo> WorldStore::LoadInfo(const std::string& CommitId)
	{
		std::string Text;
		SW_ASSIGN(Text, Repo->ReadFile(CommitId, Paths::WorldInfo));
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text));
		WorldInfo W;
		SW_ASSIGN(W, WorldInfo::FromJson(V));
		if (W.WorldId != Id) return MakeError(ErrorCode::Invalid, "world.json belongs to another world");
		return W;
	}

	Result<PlayerList> WorldStore::LoadPlayers(const std::string& CommitId)
	{
		std::string Text;
		SW_ASSIGN(Text, Repo->ReadFile(CommitId, Paths::Players));
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text));
		return PlayerList::FromJson(V);
	}

	Result<WorldSettings> WorldStore::LoadSettings(const std::string& CommitId)
	{
		std::string Text;
		SW_ASSIGN(Text, Repo->ReadFile(CommitId, Paths::Settings));
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text));
		return WorldSettings::FromJson(V);
	}

	Result<std::string> WorldStore::UpdateDocument(const std::string& Path, const std::function<Result<std::string>(const std::string& Current)>& Fn, const std::string& Message)
	{
		if (Path == Paths::State) return MakeError(ErrorCode::Invalid, "use Mutate for the state document");
		SW_TRY(ValidateRepoPath(Path));
		for (int Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			StateSnapshot Snap;
			SW_ASSIGN(Snap, Load()); // also proves the world exists and is valid
			auto Current = Repo->ReadFile(Snap.CommitId, Path);
			if (!Current && !Current.Is(ErrorCode::NotFound)) return Current.Err();
			std::string Next;
			SW_ASSIGN(Next, Fn(Current.Ok() ? *Current : std::string()));
			auto C = Repo->Commit(Snap.CommitId, {{Path, Next}}, Message);
			if (C.Ok()) return *C;
			if (!C.Is(ErrorCode::Conflict)) return C.Err();
			std::this_thread::sleep_for(std::chrono::milliseconds(2 + Attempt * 3));
		}
		return MakeError(ErrorCode::Contention, "too much concurrent activity on this world, try again");
	}

	Result<StateSnapshot> WorldStore::Mutate(const MutateFn& Fn)
	{
		for (int Attempt = 0; Attempt < MaxAttempts; ++Attempt)
		{
			StateSnapshot Snap;
			SW_ASSIGN(Snap, Load());
			LastSeen = Snap;
			WorldState Next = Snap.State;
			const TimeMs Now = ClockPtr->Now();
			Mutation M;
			Status S = Fn(Next, Now, M);
			if (!S)
			{
				if (S.Is(ErrorCode::Cancelled) && S.Err().Message == "no change")
				{
					return Snap;
				}
				return S.Err();
			}
			Next.StateVersion = Snap.State.StateVersion + 1;
			Next.UpdatedAt = Now;
			if (Status V = Next.Validate(Id); !V)
			{
				return V.Err().Wrap("refusing to write invalid state");
			}
			std::vector<FileChange> Changes = std::move(M.ExtraChanges);
			Changes.push_back({Paths::State, EncodeState(Next)});
			auto C = Repo->Commit(Snap.CommitId, Changes, M.Message.empty() ? std::string("Update Shared World state") : M.Message);
			if (C.Ok())
			{
				StateSnapshot Out{*C, std::move(Next)};
				LastSeen = Out;
				return Out;
			}
			if (C.Is(ErrorCode::Ambiguous))
			{
				// The commit may or may not have landed. Only a re-read can tell,
				// and only the caller knows how to recognise its own change.
				return C.Err();
			}
			if (!C.Is(ErrorCode::Conflict))
			{
				return C.Err().Wrap("commit world state");
			}
			Logs.Debug("CasConflict", {{"world", Id}, {"attempt", std::to_string(Attempt + 1)}});
			// Jittered backoff so simultaneous players do not stay in lockstep.
			const int Ms = 2 + static_cast<int>(std::strtoul(Jitter.Hex(2).c_str(), nullptr, 16) % (10 * (Attempt + 1)));
			std::this_thread::sleep_for(std::chrono::milliseconds(Ms));
		}
		return MakeError(ErrorCode::Contention, "too much concurrent activity on this world, try again");
	}

	// ------------------------------------------------------------ Lease

	const char* ToString(AcquireOutcome O)
	{
		switch (O)
		{
		case AcquireOutcome::Acquired: return "ACQUIRED";
		case AcquireOutcome::AlreadyHeld: return "ALREADY_HELD";
		case AcquireOutcome::HeldByOther: return "HELD_BY_OTHER";
		case AcquireOutcome::HeldBySelfElsewhere: return "HELD_BY_SELF_ELSEWHERE";
		case AcquireOutcome::ReservedForSuccessor: return "RESERVED_FOR_SUCCESSOR";
		}
		return "?";
	}

	LeaseManager::LeaseManager(std::shared_ptr<WorldStore> InStore, LeaseConfig InConfig) : StorePtr(std::move(InStore)), Cfg(InConfig) {}

	bool LeaseManager::LiveForObserver(const std::optional<Lease>& L, TimeMs Now) const
	{
		return L.has_value() && Now < L->ExpiresAt + Cfg.SkewGrace;
	}

	Status CheckFence(const WorldState& State, const LeaseToken& Token)
	{
		const std::optional<Lease>& L = State.CurrentLease;
		if (!L || State.Generation != Token.Generation || L->Generation != Token.Generation || L->Nonce != Token.Nonce)
		{
			return MakeError(ErrorCode::Fenced, "lease lost: another host has taken over this world");
		}
		return {};
	}

	static bool SameSession(const Identity& A, const Identity& B) { return A.PlayerId == B.PlayerId && A.InstallId == B.InstallId; }

	Result<AcquireResult> LeaseManager::Acquire(const Identity& Holder, const std::string& Nonce)
	{
		SW_TRY(Holder.Validate());
		if (Nonce.empty() || Nonce.size() > 64)
		{
			return MakeError(ErrorCode::Invalid, "invalid session nonce");
		}
		AcquireResult Res;
		auto Snap = StorePtr->Mutate([&](WorldState& S, TimeMs Now, WorldStore::Mutation& M) -> Status
		{
			Res = AcquireResult{};
			if (S.CurrentLease)
			{
				const Lease& L = *S.CurrentLease;
				if (L.Nonce == Nonce && L.Holder.InstallId == Holder.InstallId)
				{
					Res.Outcome = AcquireOutcome::AlreadyHeld;
					return WorldStore::NoChange();
				}
				if (LiveForObserver(S.CurrentLease, Now))
				{
					Res.Outcome = L.Holder.SamePlayerOrInstall(Holder) ? AcquireOutcome::HeldBySelfElsewhere : AcquireOutcome::HeldByOther;
					return WorldStore::NoChange();
				}
				Res.TookOverExpired = L;
				S.LastSession = SessionEnd{L.Holder, L.Generation, L.ExpiresAt, "expired"};
			}
			if (S.PendingHandoff)
			{
				if (Now < S.PendingHandoff->ExpiresAt && !SameSession(S.PendingHandoff->Successor, Holder))
				{
					Res.Outcome = AcquireOutcome::ReservedForSuccessor;
					return WorldStore::NoChange();
				}
				Res.bFromHandoff = SameSession(S.PendingHandoff->Successor, Holder);
				S.PendingHandoff.reset(); // consumed or expired
			}
			S.Generation += 1;
			Lease L;
			L.Generation = S.Generation;
			L.Holder = Holder;
			L.Nonce = Nonce;
			L.AcquiredAt = Now;
			L.RenewedAt = Now;
			L.ExpiresAt = Now + Cfg.TTL;
			L.BaseRevision = S.HeadNumber();
			L.Phase = LeasePhase::Preparing;
			S.CurrentLease = L;
			Res.Outcome = AcquireOutcome::Acquired;
			M.Message = "Host lease acquired\n\nPlayer: " + Holder.DisplayName + "\nGeneration: " + std::to_string(S.Generation) +
				"\nRevision: " + std::to_string(S.HeadNumber()) + (Res.TookOverExpired ? "\nRecovered from expired generation " + std::to_string(Res.TookOverExpired->Generation) : "");
			return {};
		});
		if (!Snap)
		{
			return Snap.Err();
		}
		Res.Snapshot = std::move(Snap.Value());
		if (Res.Outcome == AcquireOutcome::Acquired || Res.Outcome == AcquireOutcome::AlreadyHeld)
		{
			const Lease& L = *Res.Snapshot.State.CurrentLease;
			Res.Token = LeaseToken{StorePtr->WorldId(), L.Generation, L.Nonce, L.BaseRevision, L.Holder};
		}
		if (Res.Outcome == AcquireOutcome::Acquired)
		{
			LogFields F = {{"world", StorePtr->WorldId()}, {"owner", Holder.PlayerId}, {"generation", std::to_string(Res.Snapshot.State.Generation)},
				{"revision", std::to_string(Res.Snapshot.State.HeadNumber())}};
			if (Res.TookOverExpired) F.push_back({"recovered_from_generation", std::to_string(Res.TookOverExpired->Generation)});
			if (Res.bFromHandoff) F.push_back({"handoff", "true"});
			StorePtr->Log().Info("LeaseAcquired", F);
		}
		return Res;
	}

	Result<StateSnapshot> LeaseManager::Renew(const LeaseToken& Token, const LeaseUpdate& Update)
	{
		auto R = StorePtr->Mutate([&](WorldState& S, TimeMs Now, WorldStore::Mutation& M) -> Status
		{
			SW_TRY(CheckFence(S, Token));
			Lease& L = *S.CurrentLease;
			if (Now > L.ExpiresAt)
			{
				StorePtr->Log().Warn("LeaseRevivedAfterExpiry", {{"world", Token.WorldId}, {"generation", std::to_string(Token.Generation)}, {"expired_ms", std::to_string(Now - L.ExpiresAt)}});
			}
			L.RenewedAt = Now;
			L.ExpiresAt = Now + Cfg.TTL;
			if (Update.Phase) L.Phase = *Update.Phase;
			if (Update.ClearJoin) L.Join.reset();
			else if (Update.Join) L.Join = Update.Join;
			if (Update.Players) L.Players = *Update.Players;
			M.Message = std::string("Heartbeat: generation ") + std::to_string(Token.Generation) + " (" + ToString(L.Phase) + ")";
			return {};
		});
		if (R.Is(ErrorCode::Fenced))
		{
			StorePtr->Log().Warn("LeaseLost", {{"world", Token.WorldId}, {"generation", std::to_string(Token.Generation)},
				{"current_generation", StorePtr->LastSeen ? std::to_string(StorePtr->LastSeen->State.Generation) : "?"}});
		}
		return R;
	}

	Result<StateSnapshot> LeaseManager::CommitRevision(LeaseToken& Token, const RevisionMeta& Rev)
	{
		SW_TRY(Rev.Validate());
		if (Rev.Number != Token.BaseRevision + 1 || Rev.Generation != Token.Generation || Rev.PreviousRevision != Token.BaseRevision)
		{
			return MakeError(ErrorCode::Invalid, "revision does not follow the lease's base revision");
		}
		auto R = StorePtr->Mutate([&](WorldState& S, TimeMs Now, WorldStore::Mutation& M) -> Status
		{
			SW_TRY(CheckFence(S, Token));
			if (S.HeadNumber() != Token.BaseRevision)
			{
				return MakeError(ErrorCode::StaleRevision, "a newer shared save exists (cloud revision " + std::to_string(S.HeadNumber()) + ")");
			}
			S.Head = Rev;
			Lease& L = *S.CurrentLease;
			L.BaseRevision = Rev.Number;
			L.RenewedAt = Now;
			L.ExpiresAt = Now + Cfg.TTL;
			M.ExtraChanges.push_back({Rev.Path(), json::Serialize(Rev.ToJson(), 2)});
			M.Message = "Shared World revision " + std::to_string(Rev.Number) + "\n\nWorld: " + Token.WorldId + "\nPlayer: " + Rev.Uploader.DisplayName +
				"\nGeneration: " + std::to_string(Rev.Generation) + "\nReason: " + Rev.Reason + "\nSave SHA256: " + Rev.ObjectSha256 +
				"\nPrevious Revision: " + std::to_string(Rev.PreviousRevision) + (Rev.RestoredFrom ? "\nRestored From: " + std::to_string(Rev.RestoredFrom) : "");
			return {};
		});
		if (!R)
		{
			StorePtr->Log().Warn("CommitRejected", {{"world", Token.WorldId}, {"generation", std::to_string(Token.Generation)},
				{"base_revision", std::to_string(Token.BaseRevision)}, {"error", R.Err().Describe()}});
			return R;
		}
		Token.BaseRevision = Rev.Number;
		StorePtr->Log().Info("RevisionCommitted", {{"world", Token.WorldId}, {"generation", std::to_string(Rev.Generation)}, {"revision", std::to_string(Rev.Number)},
			{"sha256", Rev.ObjectSha256}, {"size", std::to_string(Rev.Size)}, {"reason", Rev.Reason}});
		return R;
	}

	Status LeaseManager::Release(const LeaseToken& Token, const std::optional<Identity>& Successor)
	{
		auto R = StorePtr->Mutate([&](WorldState& S, TimeMs Now, WorldStore::Mutation& M) -> Status
		{
			SW_TRY(CheckFence(S, Token));
			S.LastSession = SessionEnd{S.CurrentLease->Holder, S.CurrentLease->Generation, Now, Successor ? "migrated" : "released"};
			S.CurrentLease.reset();
			if (Successor)
			{
				S.PendingHandoff = Handoff{*Successor, S.HeadNumber(), Now + Cfg.HandoffWindow};
				M.Message = "Host migration: handing over to " + Successor->DisplayName + " at revision " + std::to_string(S.HeadNumber());
			}
			else
			{
				M.Message = "Host lease released: generation " + std::to_string(Token.Generation);
			}
			return {};
		});
		if (!R)
		{
			return R.Err();
		}
		StorePtr->Log().Info("LeaseReleased", {{"world", Token.WorldId}, {"generation", std::to_string(Token.Generation)}, {"revision", std::to_string(Token.BaseRevision)},
			{"successor", Successor ? Successor->PlayerId : ""}});
		return {};
	}
}
