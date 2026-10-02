#include "SharedWorldCore/World/WorldSession.h"

#include <algorithm>

#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	const char* ToString(SessionState S)
	{
		switch (S)
		{
		case SessionState::Idle: return "IDLE";
		case SessionState::Checking: return "CHECKING";
		case SessionState::WaitingForHost: return "WAITING_FOR_HOST";
		case SessionState::WaitingForSession: return "WAITING_FOR_SESSION";
		case SessionState::CheckingHost: return "CHECKING_HOST";
		case SessionState::HostVerified: return "HOST_VERIFIED";
		case SessionState::HostUnreachable: return "HOST_UNREACHABLE";
		case SessionState::JoinReady: return "JOIN_READY";
		case SessionState::Joining: return "JOINING";
		case SessionState::JoinRetry: return "JOIN_RETRY";
		case SessionState::Joined: return "JOINED";
		case SessionState::Reconnecting: return "RECONNECTING";
		case SessionState::RecoveringHost: return "RECOVERING_HOST";
		case SessionState::ElectingHost: return "ELECTING_HOST";
		case SessionState::Acquiring: return "ACQUIRING";
		case SessionState::Recovering: return "RECOVERING";
		case SessionState::Downloading: return "DOWNLOADING";
		case SessionState::ReadyToHost: return "READY_TO_HOST";
		case SessionState::StartingSession: return "STARTING_SESSION";
		case SessionState::PublishingSession: return "PUBLISHING_SESSION";
		case SessionState::Hosting: return "HOSTING";
		case SessionState::Uploading: return "UPLOADING";
		case SessionState::Migrating: return "MIGRATING";
		case SessionState::Releasing: return "RELEASING";
		case SessionState::Restoring: return "RESTORING";
		case SessionState::LeaseLost: return "LEASE_LOST";
		case SessionState::Error: return "ERROR";
		}
		return "?";
	}

	namespace
	{
		using S = SessionState;

		/** Every legal transition. Anything else is a bug and is refused. */
		bool Allowed(S From, S To)
		{
			if (From == To) return true;
			switch (From)
			{
			case S::Idle: return To == S::Checking || To == S::Restoring;
			case S::Checking:
				return To == S::WaitingForHost || To == S::WaitingForSession || To == S::CheckingHost || To == S::JoinReady ||
					To == S::Acquiring || To == S::ElectingHost || To == S::Error;
			case S::WaitingForHost:
				return To == S::WaitingForSession || To == S::CheckingHost || To == S::JoinReady || To == S::Checking ||
					To == S::Acquiring || To == S::ElectingHost || To == S::Error || To == S::Idle;
			case S::WaitingForSession:
				return To == S::CheckingHost || To == S::WaitingForHost || To == S::JoinReady || To == S::Checking ||
					To == S::Acquiring || To == S::Error || To == S::Idle;
			case S::CheckingHost:
				return To == S::HostVerified || To == S::HostUnreachable || To == S::JoinReady || To == S::WaitingForSession ||
					To == S::JoinRetry || To == S::RecoveringHost || To == S::Error || To == S::Idle;
			case S::HostVerified: return To == S::JoinReady || To == S::Joining || To == S::RecoveringHost || To == S::JoinRetry || To == S::Error || To == S::Idle;
			case S::HostUnreachable:
				return To == S::JoinRetry || To == S::CheckingHost || To == S::RecoveringHost || To == S::WaitingForHost ||
					To == S::ElectingHost || To == S::Acquiring || To == S::Error || To == S::Idle;
			case S::JoinReady:
				return To == S::Idle || To == S::Joining || To == S::Joined || To == S::Checking || To == S::CheckingHost ||
					To == S::JoinRetry || To == S::Reconnecting || To == S::Error;
			case S::Joining:
				return To == S::Joined || To == S::JoinRetry || To == S::JoinReady || To == S::HostUnreachable ||
					To == S::RecoveringHost || To == S::Reconnecting || To == S::Error || To == S::Idle;
			case S::JoinRetry:
				return To == S::CheckingHost || To == S::JoinReady || To == S::Joining || To == S::WaitingForSession ||
					To == S::RecoveringHost || To == S::Error || To == S::Idle;
			case S::Joined:
				return To == S::JoinReady || To == S::Reconnecting || To == S::RecoveringHost || To == S::Acquiring || To == S::Idle;
			case S::Reconnecting:
				return To == S::JoinReady || To == S::CheckingHost || To == S::RecoveringHost || To == S::ElectingHost ||
					To == S::Acquiring || To == S::Idle || To == S::Error;
			case S::RecoveringHost:
				return To == S::CheckingHost || To == S::JoinReady || To == S::ElectingHost || To == S::Acquiring ||
					To == S::Reconnecting || To == S::Error || To == S::Idle;
			case S::ElectingHost:
				return To == S::Acquiring || To == S::Checking || To == S::WaitingForHost || To == S::Error || To == S::Idle;
			case S::Acquiring:
				return To == S::Checking || To == S::Recovering || To == S::Downloading || To == S::ReadyToHost ||
					To == S::Error || To == S::LeaseLost || To == S::WaitingForHost || To == S::WaitingForSession ||
					To == S::JoinReady || To == S::CheckingHost;
			case S::Recovering: return To == S::Downloading || To == S::ReadyToHost || To == S::Error || To == S::LeaseLost;
			case S::Downloading: return To == S::ReadyToHost || To == S::Error || To == S::LeaseLost;
			case S::ReadyToHost:
				return To == S::StartingSession || To == S::PublishingSession || To == S::Hosting || To == S::Releasing ||
					To == S::Error || To == S::LeaseLost;
			case S::StartingSession:
				return To == S::PublishingSession || To == S::Hosting || To == S::Releasing || To == S::Error || To == S::LeaseLost || To == S::Idle;
			case S::PublishingSession:
				return To == S::Hosting || To == S::Releasing || To == S::Error || To == S::LeaseLost || To == S::Idle;
			case S::Hosting: return To == S::Uploading || To == S::Migrating || To == S::LeaseLost || To == S::Error || To == S::Idle;
			case S::Uploading: return To == S::Hosting || To == S::Releasing || To == S::Migrating || To == S::LeaseLost || To == S::Error || To == S::Idle;
			case S::Migrating: return To == S::Uploading || To == S::Releasing || To == S::Hosting || To == S::LeaseLost || To == S::Error || To == S::Idle;
			case S::Releasing: return To == S::Idle || To == S::Error;
			case S::Restoring: return To == S::Idle || To == S::Error;
			case S::LeaseLost: return To == S::Idle;
			case S::Error: return To == S::Idle || To == S::Checking;
			}
			return false;
		}

		SystemRandom GRandom;
		std::string NewNonce() { return GRandom.Hex(16); }

		/** Technical error -> player-facing message (details stay in logs / diagnostics). */
		ErrorInfo Friendly(const Error& E, const std::string& Code, const std::string& Fallback, bool bRetryable)
		{
			ErrorInfo I;
			I.Code = Code;
			I.Detail = E.Describe();
			I.bRetryable = bRetryable;
			switch (E.Code)
			{
			case ErrorCode::Network:
			case ErrorCode::RateLimited:
				I.Message = "The Shared World storage could not be reached.\n\nYour local save was NOT modified.";
				I.bRetryable = true;
				break;
			case ErrorCode::Unauthorized:
				I.Message = "Your Shared World storage account needs to be reconnected (Settings > Storage).\n\nYour local save was NOT modified.";
				break;
			case ErrorCode::Contention:
			case ErrorCode::Conflict:
				I.Message = "Another player updated the Shared World at the same moment.\n\nPlease try again.";
				I.bRetryable = true;
				break;
			case ErrorCode::Corrupt:
				I.Message = "The shared save failed verification.\n\nYour local save was NOT modified.";
				I.bRetryable = true;
				break;
			case ErrorCode::Unsupported:
				I.Message = E.Message; // already written for players
				break;
			default:
				I.Message = Fallback;
			}
			return I;
		}
	}

	WorldSession::WorldSession(std::shared_ptr<LeaseManager> InLeases, std::shared_ptr<SyncEngine> InSync, SessionConfig InConfig)
		: Leases(std::move(InLeases)), Sync(std::move(InSync)), Cfg(std::move(InConfig))
	{
		Current.WorldId = Leases->Store().WorldId();
	}

	WorldSession::~WorldSession()
	{
		// Leases are deliberately not released here: a crash or quit simply
		// stops heartbeats and the lease expires (see architecture docs).
		Ops.Shutdown();
		Heartbeats.Shutdown();
	}

	SessionView WorldSession::View() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Current;
	}

	void WorldSession::WaitIdle()
	{
		Ops.Drain();
		Heartbeats.Drain();
	}

	bool WorldSession::IsLeaseState(SessionState St) const
	{
		return St == S::Acquiring || St == S::Recovering || St == S::Downloading || St == S::ReadyToHost ||
			St == S::StartingSession || St == S::PublishingSession || St == S::Hosting || St == S::Uploading || St == S::Migrating;
	}

	// ------------------------------------------------------------ view helpers (lock held)

	void WorldSession::Set(SessionState To, const std::string& Message)
	{
		const SessionState From = Current.State;
		if (!Allowed(From, To))
		{
			Leases->Store().Log().Error("IllegalStateTransition", {{"world", Current.WorldId}, {"from", ToString(From)}, {"to", ToString(To)}});
			return;
		}
		Current.State = To;
		if (To == S::Idle)
		{
			Current.TheDecision = Decision::None;
			Current.Error.reset();
			Current.PlayerNotice.reset();
			Current.Join.reset();
			Current.HostName.clear();
			Current.SavePath.clear();
			Current.Successor.reset();
		}
		if (!Message.empty()) Note(Message);
		++Current.Sequence;
		if (From != To)
		{
			Leases->Store().Log().Info("SessionState", {{"world", Current.WorldId}, {"from", ToString(From)}, {"to", ToString(To)}, {"message", Message}});
		}
	}

	void WorldSession::Note(const std::string& Message)
	{
		Current.Message = Message;
		Current.Steps.push_back(Message);
		if (Current.Steps.size() > 30) Current.Steps.erase(Current.Steps.begin());
		++Current.Sequence;
	}

	void WorldSession::Fail(ErrorInfo E)
	{
		Leases->Store().Log().Warn("SessionError", {{"world", Current.WorldId}, {"code", E.Code}, {"detail", E.Detail}});
		const std::string Msg = E.Message;
		Current.Error = std::move(E);
		Set(S::Error, Msg);
	}

	void WorldSession::FailFrom(const Error& E, const std::string& Code, const std::string& Message, bool bRetryable)
	{
		Fail(Friendly(E, Code, Message, bRetryable));
	}

	void WorldSession::PersistActiveLease(const std::optional<LeaseToken>& Tok)
	{
		LocalWorldState L = Sync->LocalState().Load(Current.WorldId);
		L.ActiveLease = Tok;
		(void)Sync->LocalState().Save(Current.WorldId, L);
	}

	void WorldSession::LoseLease()
	{
		// Lock held by caller.
		Token.reset();
		PersistActiveLease(std::nullopt);
		ErrorInfo E;
		E.Code = "LEASE_LOST";
		E.Message = "Another player has taken over this Shared World because your connection timed out.\n\nYour progress since the last upload is kept as a Recovery Backup on this PC. Please return to the main menu.";
		Current.Error = E;
		Leases->Store().Log().Warn("LeaseLost", {{"world", Current.WorldId}});
		Set(S::LeaseLost, "Hosting authority lost.");
	}

	void WorldSession::AbortHosting(ErrorInfo E)
	{
		std::optional<LeaseToken> Tok;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Tok = Token;
			Token.reset();
		}
		if (Tok)
		{
			const Status R = Leases->Release(*Tok);
			if (!R && !R.Is(ErrorCode::Fenced))
			{
				Leases->Store().Log().Warn("ReleaseAfterFailureFailed", {{"world", Tok->WorldId}, {"error", R.Err().Describe()}});
			}
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		PersistActiveLease(std::nullopt);
		Fail(std::move(E));
	}

	std::optional<std::string> WorldSession::PermissionProblem(const std::string& CommitId, Permission P)
	{
		auto List = Leases->Store().LoadPlayers(CommitId);
		if (!List.Ok() || List->Members.empty()) return std::nullopt; // no member list: open world
		const Member* M = List->Find(Cfg.Me.PlayerId);
		if (!M)
		{
			if (P == Permission::Play && List->bOpen) return std::nullopt;
			return std::string("You are not a member of this Shared World. Ask its owner to add you.");
		}
		if (!HasPermission(M->MemberRole, P)) return std::string("Your role in this Shared World does not allow that.");
		return std::nullopt;
	}

	// ------------------------------------------------------------ menu actions

	void WorldSession::Play()
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (Current.State != S::Idle && Current.State != S::Error && Current.State != S::JoinReady)
			{
				return; // already in progress (double click)
			}
			if (Current.State == S::JoinReady) Set(S::Idle, "");
			Current.Steps.clear();
			Current.Error.reset();
			Set(S::Checking, "Checking Shared World...");
			Leases->Store().Log().Info("WorldPlayRequested", {{"world", Current.WorldId}, {"player", Cfg.Me.PlayerId}});
		}
		Ops.Post([this]() { DoCheckAndDecide(); });
	}

	void WorldSession::Cancel()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::WaitingForHost || Current.State == S::WaitingForSession || Current.State == S::CheckingHost ||
			Current.State == S::HostUnreachable || Current.State == S::JoinRetry || Current.State == S::RecoveringHost ||
			Current.State == S::Reconnecting || Current.State == S::Joining)
		{
			Set(S::Idle, "Cancelled.");
		}
		else if (Current.State == S::ReadyToHost || Current.State == S::StartingSession || Current.State == S::PublishingSession)
		{
			Set(S::Releasing, "Cancelling...");
			Ops.Post([this]() { DoRelease("Cancelled.", std::nullopt); });
		}
	}

	void WorldSession::Dismiss()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::JoinReady || Current.State == S::Error || Current.State == S::LeaseLost)
		{
			Set(S::Idle, "");
		}
	}

	void WorldSession::AcknowledgeNotice()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!Current.PlayerNotice) return;
		Current.PlayerNotice.reset();
		++Current.Sequence;
	}

	void WorldSession::Restore(int64_t FromRevision)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (Current.State != S::Idle) return;
			Current.Steps.clear();
			Set(S::Restoring, "Restoring revision " + std::to_string(FromRevision) + "...");
		}
		Ops.Post([this, FromRevision]() { DoRestore(FromRevision); });
	}

	void WorldSession::SetPendingGamePhase(std::string Phase)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		PendingGamePhase = std::move(Phase);
	}

	// ------------------------------------------------------------ decide

	void WorldSession::DoCheckAndDecide()
	{
		auto Snap = Leases->Store().Load();
		std::unique_lock<std::mutex> Lock(Mutex);
		if (Current.State != S::Checking && Current.State != S::WaitingForHost && Current.State != S::WaitingForSession &&
			Current.State != S::JoinRetry && Current.State != S::RecoveringHost && Current.State != S::HostUnreachable)
		{
			return; // cancelled
		}
		if (!Snap)
		{
			if (Snap.Is(ErrorCode::NoWorld))
			{
				ErrorInfo E;
				E.Code = "WORLD_NOT_FOUND";
				E.Message = "This Shared World has not been created yet.";
				E.Detail = Snap.Err().Describe();
				Fail(E);
			}
			else
			{
				FailFrom(Snap.Err(), "STORAGE_UNREACHABLE", "The Shared World could not be checked.\n\nYour local save was NOT modified.", true);
			}
			return;
		}
		Lock.unlock();
		const std::optional<std::string> Denied = PermissionProblem(Snap->CommitId, Permission::Play);
		auto Info = Leases->Store().LoadInfo(Snap->CommitId);
		Lock.lock();
		if (Denied)
		{
			ErrorInfo E;
			E.Code = "NOT_A_MEMBER";
			E.Message = *Denied;
			Fail(E);
			return;
		}
		if (Info.Ok() && !Info->MovedTo.IsDefault())
		{
			// This copy was frozen when the world moved: hosting it would fork the world.
			ErrorInfo E;
			E.Code = "WORLD_MOVED";
			E.Message = "This world moved to " + (Info->MovedTo.Label.empty() ? Info->MovedTo.Backend : Info->MovedTo.Label)
				+ ". Link " + (Info->MovedTo.Label.empty() ? Info->MovedTo.Backend : Info->MovedTo.Label)
				+ " in the world's Storage tab to keep playing.";
			Fail(E);
			return;
		}
		if (Info.Ok())
		{
			if (Status C = CheckCompatibility(Cfg.Versions, *Info, Snap->State.Head); !C)
			{
				FailFrom(C.Err(), "INCOMPATIBLE", C.Err().Message, false);
				return;
			}
			// Host/client on a newer mod stamps world.json so friends must update too.
			if (!Cfg.Versions.ModVersion.empty() && !Info->ModVersion.empty()
				&& CompareVersions(Cfg.Versions.ModVersion, Info->ModVersion) > 0)
			{
				WorldInfo Bumped = *Info;
				Bumped.ModVersion = Cfg.Versions.ModVersion;
				if (!Cfg.Versions.GameBuild.empty()) Bumped.GameBuild = Cfg.Versions.GameBuild;
				const std::string FromVer = Info->ModVersion;
				const std::string ToVer = Cfg.Versions.ModVersion;
				Lock.unlock();
				auto Written = Leases->Store().UpdateDocument(Paths::WorldInfo,
					[Bumped](const std::string&) -> Result<std::string>
					{
						return json::Serialize(Bumped.ToJson(), 2);
					},
					"require Shared World mod " + ToVer);
				Lock.lock();
				if (Written)
				{
					const std::string Notice =
						"This Shared World now requires Shared World mod " + ToVer
						+ ".\n\nFriends must update to the same version before they can join.";
					Current.PlayerNotice = Notice;
					Note(Notice);
					Leases->Store().Log().Info("WorldModVersionBumped", {
						{"world", Current.WorldId},
						{"from", FromVer},
						{"to", ToVer},
					});
				}
				else
				{
					Leases->Store().Log().Warn("WorldModVersionBumpFailed", {
						{"world", Current.WorldId},
						{"from", FromVer},
						{"to", ToVer},
						{"reason", Written.Err().Describe()},
					});
				}
			}
		}
		const WorldState& St = Snap->State;
		const TimeMs Now = Leases->Store().Clock().Now();
		const LocalWorldState Local = Sync->LocalState().Load(Current.WorldId);

		if (St.CurrentLease && Leases->LiveForObserver(St.CurrentLease, Now))
		{
			const Lease& L = *St.CurrentLease;
			// After a failed join / verify, wait for the dead host's lease to
			// expire instead of looping join attempts against a crashed session.
			if (Current.State == S::RecoveringHost && !L.Holder.SamePlayerOrInstall(Cfg.Me))
			{
				Current.HostName = L.Holder.DisplayName;
				FollowGeneration = L.Generation;
				LastSeenPlayers = L.Players;
				Note("HOST MIGRATION — waiting for " + L.Holder.DisplayName + "'s lock to expire...");
				return;
			}
			if (L.Holder.SamePlayerOrInstall(Cfg.Me))
			{
				const bool bOurCrashedSession = Local.ActiveLease && Local.ActiveLease->Generation == L.Generation && Local.ActiveLease->Nonce == L.Nonce;
				if (!bOurCrashedSession)
				{
					ErrorInfo E;
					E.Code = "ALREADY_HOSTING_ELSEWHERE";
					E.Message = "You are already hosting this Shared World from another game or PC.\n\nIf the game crashed, the world becomes available again in a few minutes.";
					E.bRetryable = true;
					Fail(E);
					return;
				}
				// Our own lease from before a crash: resume it instead of waiting for expiry.
				Note("Resuming your previous hosting session...");
				Set(S::Acquiring, "Acquiring host...");
				Lock.unlock();
				DoHost(*Snap);
				return;
			}
			Current.HostName = L.Holder.DisplayName;
			Current.TheDecision = Decision::Join;
			Current.Revision = St.HeadNumber();
			LastSeenPlayers = L.Players;
			FollowGeneration = L.Generation;

			if (!L.IsJoinable())
			{
				if (Current.State != S::WaitingForHost && Current.State != S::WaitingForSession)
				{
					WaitDeadline = Now + Cfg.JoinWaitTimeout;
				}
				if (L.Phase == LeasePhase::Hosting && !L.bHostReady)
				{
					Set(S::WaitingForSession, L.Holder.DisplayName + " is finishing session setup...");
				}
				else
				{
					Set(S::WaitingForHost, L.Holder.DisplayName + " is starting the world...");
				}
				if (Now > WaitDeadline)
				{
					ErrorInfo E;
					E.Code = "HOST_NOT_READY";
					E.Message = L.Holder.DisplayName + " is still starting the world. Try again in a moment.";
					E.bRetryable = true;
					Fail(E);
				}
				return;
			}

			Current.Join = L.Join;
			EnterJoinPath(L, St.HeadNumber(), "Checking host " + L.Holder.DisplayName + "...");
			Lock.unlock();
			ScheduleHostVerify(*Snap);
			return;
		}
		if (St.PendingHandoff && Now < St.PendingHandoff->ExpiresAt && St.PendingHandoff->Successor.PlayerId != Cfg.Me.PlayerId)
		{
			if (Current.State != S::WaitingForHost && Current.State != S::WaitingForSession)
			{
				WaitDeadline = Now + Cfg.JoinWaitTimeout;
				Current.TheDecision = Decision::Join;
				Set(S::WaitingForHost, "Host migration: " + St.PendingHandoff->Successor.DisplayName + " is starting the world...");
			}
			return;
		}
		Set(S::Acquiring, St.CurrentLease ? "Previous host stopped responding. Acquiring host..." : "Nobody is playing. Acquiring host...");
		Lock.unlock();
		DoHost(*Snap);
	}

	void WorldSession::EnterJoinPath(const Lease& L, int64_t /*HeadRevision*/, const std::string& Message)
	{
		// Lock held by caller.
		HostVerifyAttempts = 0;
		HostVerifyDeadline = Leases->Store().Clock().Now() + Cfg.HostVerifyTimeout;
		if (Cfg.HostVerifier)
		{
			Set(S::CheckingHost, Message.empty() ? ("Checking host " + L.Holder.DisplayName + "...") : Message);
		}
		else if (L.Join)
		{
			Set(S::JoinReady, "Connecting to " + L.Holder.DisplayName + "...");
			Leases->Store().Log().Info("PlayerJoinStarted", {{"world", Current.WorldId}, {"host", L.Holder.PlayerId}, {"generation", std::to_string(L.Generation)}});
		}
		else
		{
			Set(S::JoinReady, L.Holder.DisplayName + " is hosting. Join them from the Satisfactory friends list.");
		}
	}

	void WorldSession::ScheduleHostVerify(const StateSnapshot& Snap)
	{
		if (!Cfg.HostVerifier || bVerifyInFlight.exchange(true)) return;
		Ops.Post([this, Snap]()
		{
			DoVerifyHost(Snap);
			bVerifyInFlight = false;
		});
	}

	void WorldSession::DoVerifyHost(StateSnapshot Snap)
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		if (Current.State != S::CheckingHost && Current.State != S::JoinRetry && Current.State != S::HostUnreachable) return;
		if (!Cfg.HostVerifier)
		{
			Set(S::JoinReady, Current.Join ? ("Connecting to " + Current.HostName + "...") : (Current.HostName + " is hosting. Join them from the Satisfactory friends list."));
			return;
		}
		if (!Snap.State.CurrentLease || !Leases->LiveForObserver(Snap.State.CurrentLease, Leases->Store().Clock().Now()))
		{
			Set(S::RecoveringHost, "HOST MIGRATION — recovering Shared World...");
			Lock.unlock();
			DoCheckAndDecide();
			return;
		}
		const Lease L = *Snap.State.CurrentLease;
		if (!L.IsJoinable())
		{
			Set(S::WaitingForSession, L.Holder.DisplayName + " is starting the world...");
			return;
		}
		Current.Join = L.Join;
		JoinInfo Join = L.Join.value_or(JoinInfo{"online-session-id", "", ""});
		const std::string Nonce = NewNonce();
		const TimeMs Now = Leases->Store().Clock().Now();
		SharedWorldHello Hello = MakeHello(Cfg.Me, Current.WorldId, L, Snap.State.HeadNumber(), Nonce, Now);
		++HostVerifyAttempts;
		Lock.unlock();

		HostVerifyResult VR = Cfg.HostVerifier->Probe(Hello, Join, Cfg.HostVerifyProbeTimeout);

		Lock.lock();
		if (Current.State != S::CheckingHost && Current.State != S::JoinRetry && Current.State != S::HostUnreachable) return;

		// Re-read cloud authority after the probe: verification never grants hosting,
		// and a takeover during the wait must cancel join.
		{
			Lock.unlock();
			auto Latest = Leases->Store().Load();
			Lock.lock();
			if (!Latest || !Latest->State.CurrentLease ||
				!Leases->LiveForObserver(Latest->State.CurrentLease, Leases->Store().Clock().Now()) ||
				Latest->State.CurrentLease->Generation != L.Generation ||
				Latest->State.CurrentLease->Holder.PlayerId != L.Holder.PlayerId)
			{
				Set(S::RecoveringHost, "HOST MIGRATION — checking Shared World...");
				Lock.unlock();
				DoCheckAndDecide();
				return;
			}
		}

		auto RetryOrRecover = [&](const std::string& PlayerMessage, const std::string& Detail)
		{
			Leases->Store().Log().Warn("HostVerifyFailed", {{"world", Current.WorldId}, {"host", L.Holder.PlayerId},
				{"generation", std::to_string(L.Generation)}, {"outcome", ToString(VR.Outcome)}, {"detail", Detail},
				{"attempt", std::to_string(HostVerifyAttempts)}});
			const TimeMs T = Leases->Store().Clock().Now();
			if (T < HostVerifyDeadline && Leases->LiveForObserver(Snap.State.CurrentLease, T))
			{
				Set(S::JoinRetry, PlayerMessage);
				return;
			}
			Set(S::RecoveringHost, "HOST MIGRATION — waiting for the Shared World to become available...");
			// Do not break the lease: wait for expiry via normal poll / reconnect path.
		};

		if (VR.Outcome == HostVerifyOutcome::Timeout || VR.Outcome == HostVerifyOutcome::Unreachable || VR.Outcome == HostVerifyOutcome::Unsupported)
		{
			RetryOrRecover("Could not reach " + L.Holder.DisplayName + ". Retrying...", VR.Detail.empty() ? ToString(VR.Outcome) : VR.Detail);
			return;
		}
		if (!VR.Ack)
		{
			RetryOrRecover("Could not reach " + L.Holder.DisplayName + ". Retrying...", "empty ack");
			return;
		}
		SharedWorldHelloAck Ack = *VR.Ack;
		if (Ack.HostPlayerId.empty()) Ack.HostPlayerId = L.Holder.PlayerId;
		if (Status V = ValidateHelloAck(Hello, Ack, L, Snap.State.HeadNumber(), L.Join); !V)
		{
			VR.Outcome = HostVerifyOutcome::Mismatch;
			RetryOrRecover("The host session did not match the Shared World. Retrying...", V.Err().Describe());
			return;
		}
		Set(S::HostVerified, "Host verified. Connecting to " + L.Holder.DisplayName + "...");
		if (L.Join)
		{
			Current.Join = L.Join;
			Set(S::JoinReady, "Connecting to " + L.Holder.DisplayName + "...");
			Leases->Store().Log().Info("HostVerified", {{"world", Current.WorldId}, {"host", L.Holder.PlayerId},
				{"generation", std::to_string(L.Generation)}, {"revision", std::to_string(Snap.State.HeadNumber())}});
		}
		else
		{
			Current.Join.reset();
			Set(S::JoinReady, L.Holder.DisplayName + " is hosting. Join them from the Satisfactory friends list.");
		}
	}

	// ------------------------------------------------------------ host path

	void WorldSession::DoHost(const StateSnapshot& Snap)
	{
		const std::string SavePath = file::Join(Cfg.SaveDirectory, Cfg.SaveName + save::Extension);
		const LocalWorldState Local = Sync->LocalState().Load(Current.WorldId);
		const std::string Nonce = Local.ActiveLease ? Local.ActiveLease->Nonce : NewNonce();
		Leases->Store().Log().Info("LeaseAcquireAttempt", {{"world", Current.WorldId}, {"generation_seen", std::to_string(Snap.State.Generation)}});
		auto Res = Leases->Acquire(Cfg.Me, Nonce);
		if (!Res)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			FailFrom(Res.Err(), "LEASE_FAILED", "Could not claim the Shared World.\n\nYour local save was NOT modified.", true);
			return;
		}
		switch (Res->Outcome)
		{
		case AcquireOutcome::HeldByOther:
		case AcquireOutcome::ReservedForSuccessor:
		{
			// Lost the race: someone else is host now. Decide again (-> join).
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				Set(S::Checking, "Another player started hosting first.");
			}
			DoCheckAndDecide();
			return;
		}
		case AcquireOutcome::HeldBySelfElsewhere:
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ErrorInfo E;
			E.Code = "ALREADY_HOSTING_ELSEWHERE";
			E.Message = "You are already hosting this Shared World from another game or PC.";
			E.bRetryable = true;
			Fail(E);
			return;
		}
		default:
			break;
		}
		LeaseToken Tok = *Res->Token;
		const WorldState St = Res->Snapshot.State;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Token = Tok;
			PersistActiveLease(Tok);
			LastHeartbeat = Leases->Store().Clock().Now();
			Current.TheDecision = Decision::Host;
			Current.Generation = Tok.Generation;
			Current.Revision = Tok.BaseRevision;
			if (Res->TookOverExpired) Note("Previous host " + Res->TookOverExpired->Holder.DisplayName + " stopped responding. Recovering Shared World...");
			if (Res->bFromHandoff) Note("Host migration: you are the new host.");
		}

		// ---- recovery of progress that never reached the shared world
		auto Insp = Sync->InspectLocal(SavePath);
		if (!Insp)
		{
			AbortHosting(Friendly(Insp.Err(), "LOCAL_SAVE_UNREADABLE", "Your local copy of the Shared World could not be read.\n\nIt was NOT modified.", true));
			return;
		}
		int64_t CrashedGeneration = 0;
		if (Res->Outcome == AcquireOutcome::AlreadyHeld) CrashedGeneration = Tok.Generation; // resumed our own lease
		else if (Res->TookOverExpired) CrashedGeneration = Res->TookOverExpired->Generation;

		std::vector<RecoveryCandidate> Candidates;
		if (Insp->Status == LocalStatus::Modified)
		{
			RecoveryCandidate C;
			C.Source = "local";
			C.Generation = Local.ActiveLease ? Local.ActiveLease->Generation : 0;
			C.BaseRevision = Insp->State.SyncedRevision;
			C.ObjectSha256 = Insp->Sha256;
			C.Size = file::Size(SavePath).Ok() ? file::Size(SavePath).Value() : 0;
			C.SavedAt = Leases->Store().Clock().Now();
			C.bValidated = save::ValidateFile(SavePath).Ok();
			C.bObjectAvailable = true;
			Candidates.push_back(C);
		}
		if (CrashedGeneration > 0)
		{
			// Reports from a host that uploaded a save but could not commit it.
			if (auto Names = Leases->Store().Repository().ListDirectory(Res->Snapshot.CommitId, Paths::RecoveryDir); Names.Ok())
			{
				for (const std::string& N : *Names)
				{
					auto Text = Leases->Store().Repository().ReadFile(Res->Snapshot.CommitId, std::string(Paths::RecoveryDir) + "/" + N);
					if (!Text) continue;
					auto V = json::Parse(*Text);
					if (!V) continue;
					auto C = RecoveryReportFromJson(*V);
					if (!C || C->Generation != CrashedGeneration) continue;
					auto Has = Sync->Objects().Has(C->ObjectSha256);
					C->bObjectAvailable = Has.Ok() && *Has;
					C->bValidated = C->bObjectAvailable; // re-validated below before use
					Candidates.push_back(*C);
				}
			}
		}
		std::optional<RecoveryCandidate> Chosen = SelectRecoveryCandidate(Candidates, CrashedGeneration, St);
		if (Chosen)
		{
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				Leases->Store().Log().Info("RecoveryCandidateFound", {{"world", Current.WorldId}, {"source", Chosen->Source}, {"generation", std::to_string(Chosen->Generation)},
					{"base_revision", std::to_string(Chosen->BaseRevision)}, {"sha256", Chosen->ObjectSha256}});
				Set(S::Recovering, "Unsynchronised progress found. Publishing it...");
			}
			if (Chosen->Source == "local")
			{
				UploadOptions O = Cfg.Upload;
				O.Reason = Reason::Recovered;
				O.GameBuild = Cfg.Versions.GameBuild;
				O.ModVersion = Cfg.Versions.ModVersion;
				auto Up = Sync->Upload(Tok, SavePath, O);
				if (Up.Ok())
				{
					std::lock_guard<std::mutex> Lock(Mutex);
					Token = Tok;
					PersistActiveLease(Tok);
					Current.Revision = Up->Revision.Number;
					Current.SavePath = SavePath;
					Set(S::ReadyToHost, "Recovered progress saved as revision " + std::to_string(Up->Revision.Number) + ". Starting host...");
					return;
				}
				std::lock_guard<std::mutex> Lock(Mutex);
				Note("Could not publish the unsynchronised save: " + Up.Err().Message);
			}
			else
			{
				// Adopt a reported object: it must download, verify and validate first.
				RevisionMeta Rev;
				Rev.Number = Tok.BaseRevision + 1;
				Rev.Generation = Tok.Generation;
				Rev.PreviousRevision = Tok.BaseRevision;
				Rev.ObjectSha256 = Chosen->ObjectSha256;
				Rev.Size = Chosen->Size;
				Rev.CreatedAt = Leases->Store().Clock().Now();
				Rev.Uploader = Cfg.Me;
				Rev.Reason = Reason::Recovered;
				WorldState Probe = St;
				Probe.Head = Rev;
				const std::string ProbePath = file::TempSibling(SavePath, "probe");
				auto Fetched = Sync->Download(Probe, ProbePath);
				(void)file::Remove(ProbePath);
				if (Fetched.Ok() && Leases->CommitRevision(Tok, Rev).Ok())
				{
					std::lock_guard<std::mutex> Lock(Mutex);
					Token = Tok;
					PersistActiveLease(Tok);
					Note("Recovered newer progress reported by the previous host.");
				}
			}
		}
		if (Insp->Status == LocalStatus::Modified && (!Chosen || Chosen->Source != "local"))
		{
			auto Bp = Sync->Backups().Preserve(Current.WorldId, SavePath, "conflict-local-r" + std::to_string(Insp->State.SyncedRevision));
			if (!Bp)
			{
				// Cannot secure the local progress: do not overwrite it.
				AbortHosting(Friendly(Bp.Err(), "BACKUP_FAILED", "Your local save has progress that is not in the Shared World and could not be backed up. Nothing was changed.", false));
				return;
			}
			std::lock_guard<std::mutex> Lock(Mutex);
			Note("Unsynchronised progress found. A newer Shared World revision already exists. Your previous save has been preserved under Recovery Backups.");
		}

		// ---- install the authoritative revision
		auto Latest = Leases->Store().Load();
		if (!Latest)
		{
			AbortHosting(Friendly(Latest.Err(), "STORAGE_UNREACHABLE", "The Shared World could not be read.\n\nYour local save was NOT modified.", true));
			return;
		}
		if (!Latest->State.Head)
		{
			ErrorInfo E;
			E.Code = "NO_SAVE";
			E.Message = "This Shared World has no save yet.";
			AbortHosting(E);
			return;
		}
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Set(S::Downloading, "Downloading latest world (revision " + std::to_string(Latest->State.HeadNumber()) + ")...");
		}
		auto Dl = Sync->Download(Latest->State, SavePath);
		if (!Dl)
		{
			AbortHosting(Friendly(Dl.Err(), Dl.Is(ErrorCode::Corrupt) ? "CORRUPT_DOWNLOAD" : "DOWNLOAD_FAILED",
				"The shared save could not be downloaded.\n\nYour local save was NOT modified.", true));
			return;
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!Token) return; // lease lost meanwhile (heartbeat)
		Token->BaseRevision = Latest->State.HeadNumber();
		Current.Revision = Dl->Revision;
		Current.SavePath = SavePath;
		Note(Dl->bAlreadyLocal ? "Local save already matches the latest revision." : "Save verified.");
		Set(S::ReadyToHost, "Loading Shared World...");
	}

	// ------------------------------------------------------------ host events

	void WorldSession::OnHostingStarted(const std::optional<JoinInfo>& Join)
	{
		std::optional<LeaseToken> Tok;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if ((Current.State != S::ReadyToHost && Current.State != S::StartingSession && Current.State != S::PublishingSession) || !Token) return;
			PublishedJoin = Join;
			Tok = Token;
		}
		Ops.Post([this, Tok, Join]()
		{
			LeaseUpdate U;
			U.Phase = LeasePhase::Hosting;
			U.HostReady = true;
			if (Join) U.Join = Join;
			else U.ClearJoin = true;
			auto R = Leases->Renew(*Tok, U);
			std::lock_guard<std::mutex> Lock(Mutex);
			if (R.Is(ErrorCode::Fenced))
			{
				LoseLease();
				return;
			}
			if (!R)
			{
				Note("World started, but publishing the session failed; retrying...");
			}
			else
			{
				Leases->Store().Log().Info("HostSessionPublished", {{"world", Current.WorldId}, {"generation", std::to_string(Tok->Generation)},
					{"join", Join ? Join->Kind : "none"}, {"hostReady", "true"}});
			}
			Set(S::Hosting, "Hosting Shared World");
		});
	}

	void WorldSession::OnHostLoadStarted()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State != S::ReadyToHost || !Token) return;
		Set(S::StartingSession, "Loading Shared World...");
	}

	void WorldSession::OnHostPublishingSession()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if ((Current.State != S::ReadyToHost && Current.State != S::StartingSession) || !Token) return;
		Set(S::PublishingSession, "Publishing multiplayer session...");
	}

	void WorldSession::SetPlayers(std::vector<SessionPlayer> InPlayers)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Players = std::move(InPlayers);
	}

	void WorldSession::RequestMigration(const Identity& Successor)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State != S::Hosting || !Token) return;
		PendingSuccessor = Successor;
		Current.Successor = Successor;
		Leases->Store().Log().Info("HostMigrationStarted", {{"world", Current.WorldId}, {"successor", Successor.PlayerId}});
		Set(S::Migrating, "HOST MIGRATION — saving world for " + Successor.DisplayName + "...");
		// Publish Migrating on the lease immediately so clients can show the overlay
		// before the host session tears down (don't wait for the next heartbeat).
		LastHeartbeat = 0;
	}

	void WorldSession::OnSaveCompleted(SaveKind Kind)
	{
		std::optional<Identity> Successor;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (!Token || (Current.State != S::Hosting && Current.State != S::Migrating)) return;
			if (Kind == SaveKind::Migration) Successor = PendingSuccessor;
			Current.Error.reset();
			Set(S::Uploading, Kind == SaveKind::Checkpoint ? "Uploading revision..." : "Saving and uploading the Shared World...");
		}
		Ops.Post([this, Kind, Successor]() { DoUpload(Kind, Successor); });
	}

	void WorldSession::OnWorldEnded()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::Hosting || Current.State == S::Migrating)
		{
			// The last completed save is the best we have; publish it and release.
			Set(S::Uploading, "Uploading the last saved state of the Shared World...");
			Ops.Post([this]() { DoUpload(SaveKind::Final, std::nullopt); });
		}
		else if (Current.State == S::Uploading)
		{
			// Already uploading (checkpoint/final mid-flight); let it finish.
			return;
		}
		else if (Current.State == S::ReadyToHost)
		{
			Set(S::Releasing, "The world did not start. Releasing it...");
			Ops.Post([this]() { DoRelease("Released.", std::nullopt); });
		}
	}

	void WorldSession::AbandonOnProcessExit()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::Idle || Current.State == S::Error)
		{
			return;
		}
		// Engine exit tears down HTTP; a Final upload here often hangs ~60s then fails.
		// Keep the last successful cloud revision; lease expires without a clean release.
		Leases->Store().Log().Warn("HostAbandonedOnProcessExit", {{"world", Leases->Store().WorldId()},
			{"state", ToString(Current.State)}, {"revision", std::to_string(Current.Revision)}});
		Token.reset();
		PersistActiveLease(std::nullopt);
		PendingSuccessor.reset();
		Current.Successor.reset();
		Current.Steps.clear();
		Set(S::Idle, "Game exited while hosting. Cloud lease will expire.");
	}

	void WorldSession::DoUpload(SaveKind Kind, std::optional<Identity> Successor)
	{
		LeaseToken Tok;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (!Token) return;
			Tok = *Token;
		}
		UploadOptions O = Cfg.Upload;
		O.GameBuild = Cfg.Versions.GameBuild;
		O.ModVersion = Cfg.Versions.ModVersion;
		O.Reason = Kind == SaveKind::Checkpoint ? Reason::Autosave : Kind == SaveKind::Final ? Reason::Final : Reason::Migration;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			O.GamePhase = PendingGamePhase;
		}
		ConflictInfo Conflict;
		const std::string SavePath = file::Join(Cfg.SaveDirectory, Cfg.SaveName + save::Extension);
		auto Up = Sync->Upload(Tok, SavePath, O, &Conflict);
		std::unique_lock<std::mutex> Lock(Mutex);
		if (!Up)
		{
			if (Up.Is(ErrorCode::Fenced))
			{
				LoseLease();
				if (Current.Error) Current.Error->BackupPath = Conflict.BackupPath;
				return;
			}
			if (Up.Is(ErrorCode::StaleRevision))
			{
				Token.reset();
				PersistActiveLease(std::nullopt);
				ErrorInfo E;
				E.Code = "NEWER_SAVE_EXISTS";
				E.Message = "A newer Shared World revision exists.\n\nYour local save has been preserved as a Recovery Backup.\n\nCloud revision: " +
					std::to_string(Conflict.CloudRevision) + "\nLocal revision: " + std::to_string(Conflict.LocalBaseRevision);
				E.BackupPath = Conflict.BackupPath;
				E.CloudRevision = Conflict.CloudRevision;
				E.LocalRevision = Conflict.LocalBaseRevision;
				E.Detail = Up.Err().Describe();
				Fail(E);
				return;
			}
			// Transient: keep hosting authority; the save stays on this PC.
			ErrorInfo E = Friendly(Up.Err(), "UPLOAD_FAILED", "Uploading the Shared World failed. Your save is safe on this PC and will be uploaded again.", true);
			Current.Error = E;
			PendingSuccessor.reset();
			Current.Successor.reset();
			Set(S::Hosting, Kind == SaveKind::Migration ? "Host migration cancelled: the world could not be uploaded." : "Upload failed; will retry.");
			return;
		}
		Token = Tok; // BaseRevision advanced
		PersistActiveLease(Tok);
		Current.Revision = Up->Revision.Number;
		if (Kind == SaveKind::Checkpoint)
		{
			Set(S::Hosting, Up->bUnchanged ? "No changes since the last upload." : "Revision " + std::to_string(Up->Revision.Number) + " uploaded.");
			return;
		}
		if (Kind == SaveKind::Migration)
		{
			Set(S::Migrating, "Uploaded revision " + std::to_string(Up->Revision.Number) + ". Selecting new host...");
		}
		Set(S::Releasing, "Releasing the Shared World...");
		Lock.unlock();
		DoRelease(Successor ? "Handed over to " + Successor->DisplayName + ". Reconnecting..." : "Shared World saved and released.", Successor);
	}

	void WorldSession::DoRelease(const std::string& DoneMessage, std::optional<Identity> Successor)
	{
		std::optional<LeaseToken> Tok;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Tok = Token;
		}
		if (Tok)
		{
			const Status R = Leases->Release(*Tok, Successor);
			if (!R && !R.Is(ErrorCode::Fenced))
			{
				// Everything is uploaded; the lease simply expires on its own.
				Leases->Store().Log().Warn("ReleaseFailed", {{"world", Tok->WorldId}, {"error", R.Err().Describe()}});
			}
			if (Successor && R.Ok())
			{
				Leases->Store().Log().Info("HostMigrationCompleted", {{"world", Tok->WorldId}, {"successor", Successor->PlayerId}, {"revision", std::to_string(Tok->BaseRevision)}});
			}
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		Token.reset();
		PendingSuccessor.reset();
		PublishedJoin.reset();
		PersistActiveLease(std::nullopt);
		Set(S::Idle, DoneMessage);
	}

	// ------------------------------------------------------------ heartbeat

	void WorldSession::DoHeartbeat()
	{
		LeaseToken Tok;
		LeaseUpdate U;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (!Token) return;
			Tok = *Token;
			switch (Current.State)
			{
			case S::Hosting:
				U.Phase = LeasePhase::Hosting;
				U.HostReady = true;
				if (PublishedJoin) U.Join = PublishedJoin;
				break;
			case S::Uploading: U.Phase = LeasePhase::Saving; U.HostReady = true; break;
			case S::Migrating: U.Phase = LeasePhase::Migrating; U.HostReady = false; break;
			default:
				U.Phase = LeasePhase::Preparing;
				U.HostReady = false;
				break;
			}
			U.Players = Players;
		}
		auto R = Leases->Renew(Tok, U);
		std::lock_guard<std::mutex> Lock(Mutex);
		if (R.Is(ErrorCode::Fenced))
		{
			if (Token && Token->Generation == Tok.Generation) LoseLease();
			return;
		}
		if (!R) Note("Connection to Shared World storage lost; retrying...");
	}

	// ------------------------------------------------------------ client side

	void WorldSession::OnJoinedAsClient()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::JoinReady || Current.State == S::Joining) Set(S::Joined, "Playing in " + Current.HostName + "'s Shared World.");
	}

	void WorldSession::OnJoinStarted()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State == S::JoinReady) Set(S::Joining, "Connecting to " + Current.HostName + "...");
	}

	void WorldSession::OnJoinFailed(const std::string& Reason)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State != S::JoinReady && Current.State != S::Joining && Current.State != S::CheckingHost &&
			Current.State != S::HostVerified)
		{
			return;
		}
		Leases->Store().Log().Warn("PlayerJoinFailed", {{"world", Current.WorldId}, {"reason", Reason}});
		// We were following a live lease (Play join or reconnect). The host's
		// Steam session is gone / crashed: wait for lease expiry, then take over.
		if (FollowGeneration > 0)
		{
			const TimeMs Now = Leases->Store().Clock().Now();
			ConnectionLostAt = Now;
			TakeoverNotBefore = Now + Cfg.TakeoverStagger * TakeoverRank(LastSeenPlayers, {}, Cfg.Me.PlayerId);
			WaitDeadline = Now + Cfg.JoinWaitTimeout * 2;
			Set(S::RecoveringHost, "HOST MIGRATION — waiting to take over...");
			return;
		}
		ErrorInfo E;
		E.Code = "JOIN_FAILED";
		E.Message = "Could not join " + Current.HostName + "'s game. Try again, or join them from the Satisfactory friends list.";
		E.Detail = Reason;
		E.bRetryable = true;
		Fail(E);
	}

	void WorldSession::OnHostConnectionLost()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State != S::Joined) return;
		const TimeMs Now = Leases->Store().Clock().Now();
		const int Rank = TakeoverRank(LastSeenPlayers, {}, Cfg.Me.PlayerId);
		TakeoverNotBefore = Now + Cfg.TakeoverStagger * Rank;
		ConnectionLostAt = Now;
		WaitDeadline = Now + Cfg.JoinWaitTimeout * 2;
		Set(S::Reconnecting, "HOST MIGRATION — selecting a new host...");
	}

	void WorldSession::OnLeftAsClient()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Current.State != S::Joined) return;
		Leases->Store().Log().Info("PlayerLeft", {{"world", Current.WorldId}, {"generation", std::to_string(FollowGeneration)}});
		FollowGeneration = 0;
		LastSeenPlayers.clear();
		Current.Join.reset();
		Set(S::Idle, "Left the Shared World.");
	}

	void WorldSession::DoFollowHost()
	{
		auto Snap = Leases->Store().Load();
		std::unique_lock<std::mutex> Lock(Mutex);
		bPollInFlight = false;
		if (Current.State != S::Joined || !Snap) return;
		const WorldState& St = Snap->State;
		const TimeMs Now = Leases->Store().Clock().Now();
		if (St.CurrentLease && Leases->LiveForObserver(St.CurrentLease, Now))
		{
			const Lease& L = *St.CurrentLease;
			LastSeenPlayers = L.Players;
			if (L.Phase == LeasePhase::Migrating)
			{
				// Host is leaving on purpose — stay Joined until the session drops,
				// but surface migration copy so the UE overlay can show progress.
				Current.HostName = L.Holder.DisplayName;
				Note("HOST MIGRATION — " + L.Holder.DisplayName + " is handing off the Shared World...");
				return;
			}
			if (L.Generation != FollowGeneration && L.Join)
			{
				// Someone else is host now (migration / recovery): reconnect.
				Current.HostName = L.Holder.DisplayName;
				Current.Join = L.Join;
				FollowGeneration = L.Generation;
				Set(S::JoinReady, "Reconnecting to " + L.Holder.DisplayName + "...");
			}
			return;
		}
		if (St.PendingHandoff && St.PendingHandoff->Successor.PlayerId == Cfg.Me.PlayerId &&
			(St.PendingHandoff->Successor.InstallId.empty() || St.PendingHandoff->Successor.InstallId == Cfg.Me.InstallId)) // empty: the host could not learn the successor's install id (clients never write shared storage)
		{
			Set(S::Acquiring, "Host migration: starting new host...");
			Lock.unlock();
			DoHost(*Snap);
			return;
		}
		WaitDeadline = Now + Cfg.JoinWaitTimeout * 2;
		TakeoverNotBefore = Now + Cfg.TakeoverStagger * TakeoverRank(LastSeenPlayers, {}, Cfg.Me.PlayerId);
		ConnectionLostAt = Now;
		Set(S::Reconnecting, St.PendingHandoff ? "Host migration: " + St.PendingHandoff->Successor.DisplayName + " is starting the world..." : "The host left. Recovering Shared World...");
	}

	void WorldSession::DoReconnect()
	{
		auto Snap = Leases->Store().Load();
		std::unique_lock<std::mutex> Lock(Mutex);
		bPollInFlight = false;
		if (Current.State != S::Reconnecting) return;
		const TimeMs Now = Leases->Store().Clock().Now();
		if (!Snap)
		{
			Note("Waiting for Shared World storage...");
			return;
		}
		const WorldState& St = Snap->State;
		if (St.CurrentLease && Leases->LiveForObserver(St.CurrentLease, Now))
		{
			const Lease& L = *St.CurrentLease;
			// Rejoin a new host at once. Rejoin the SAME host only if it renewed
			// its lease after we lost the connection (proof it is alive); a
			// crashed host's lease still looks valid until it expires.
			const bool bNewHost = L.Generation != FollowGeneration;
			const bool bSameHostAlive = !bNewHost && L.RenewedAt > ConnectionLostAt && L.Phase == LeasePhase::Hosting;
			if (L.Join && (bNewHost || bSameHostAlive))
			{
				Current.HostName = L.Holder.DisplayName;
				Current.Join = L.Join;
				FollowGeneration = L.Generation;
				Set(S::JoinReady, "Reconnecting to " + L.Holder.DisplayName + "...");
				return;
			}
			Note(L.Generation == FollowGeneration ? "Waiting for host lease..." : L.Holder.DisplayName + " is starting the world...");
		}
		else if (St.PendingHandoff && Now < St.PendingHandoff->ExpiresAt)
		{
			if (St.PendingHandoff->Successor.PlayerId == Cfg.Me.PlayerId &&
				(St.PendingHandoff->Successor.InstallId.empty() || St.PendingHandoff->Successor.InstallId == Cfg.Me.InstallId))
			{
				Set(S::Acquiring, "Host migration: starting new host...");
				Lock.unlock();
				DoHost(*Snap);
				return;
			}
			Note("Waiting for " + St.PendingHandoff->Successor.DisplayName + " to start the world...");
		}
		else if (Now >= (St.CurrentLease
			// Stagger takeovers from the moment the dead host's lease expires
			// (rank 0 first). Everyone may still try; the CAS picks one.
			? St.CurrentLease->ExpiresAt + Leases->Config().SkewGrace + Cfg.TakeoverStagger * TakeoverRank(LastSeenPlayers, {}, Cfg.Me.PlayerId)
			: TakeoverNotBefore))
		{
			Set(S::Acquiring, "Selecting new host...");
			Lock.unlock();
			DoHost(*Snap);
			return;
		}
		else
		{
			Note("Checking recovery saves...");
		}
		if (Now > WaitDeadline)
		{
			ErrorInfo E;
			E.Code = "RECONNECT_TIMEOUT";
			E.Message = "No new host appeared. Press PLAY to try again.";
			E.bRetryable = true;
			Fail(E);
		}
	}

	// ------------------------------------------------------------ restore

	void WorldSession::DoRestore(int64_t FromRevision)
	{
		auto Snap = Leases->Store().Load();
		if (!Snap)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			FailFrom(Snap.Err(), "STORAGE_UNREACHABLE", "The Shared World could not be read.", true);
			return;
		}
		if (auto Denied = PermissionProblem(Snap->CommitId, Permission::RestoreRevision))
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ErrorInfo E;
			E.Code = "NOT_ALLOWED";
			E.Message = *Denied;
			Fail(E);
			return;
		}
		auto History = Sync->History(Snap->CommitId, 100000);
		const RevisionMeta* From = nullptr;
		if (History.Ok())
		{
			for (const RevisionMeta& R : *History)
			{
				if (R.Number == FromRevision) From = &R;
			}
		}
		if (!From)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ErrorInfo E;
			E.Code = "NO_SUCH_REVISION";
			E.Message = "Revision " + std::to_string(FromRevision) + " was not found.";
			Fail(E);
			return;
		}
		auto Res = Leases->Acquire(Cfg.Me, NewNonce());
		if (!Res || Res->Outcome != AcquireOutcome::Acquired)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ErrorInfo E;
			E.Code = "WORLD_IN_USE";
			E.Message = "Someone is playing this Shared World. Restore it when it is available.";
			E.bRetryable = true;
			if (!Res) E.Detail = Res.Err().Describe();
			Fail(E);
			return;
		}
		LeaseToken Tok = *Res->Token;
		auto Restored = Sync->Restore(Tok, *From, Cfg.Me);
		const Status Rel = Leases->Release(Tok);
		(void)Rel;
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!Restored)
		{
			FailFrom(Restored.Err(), "RESTORE_FAILED", "The revision could not be restored. Nothing was changed.", true);
			return;
		}
		Current.Revision = Restored->Number;
		Set(S::Idle, "Revision " + std::to_string(FromRevision) + " restored as revision " + std::to_string(Restored->Number) + ".");
	}

	// ------------------------------------------------------------ tick

	void WorldSession::Tick(TimeMs Now)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Token && IsLeaseState(Current.State) && Now - LastHeartbeat >= Cfg.HeartbeatInterval && !bHeartbeatInFlight)
		{
			LastHeartbeat = Now;
			bHeartbeatInFlight = true;
			Heartbeats.Post([this]()
			{
				DoHeartbeat();
				bHeartbeatInFlight = false;
			});
		}
		if (Now - LastPoll < Cfg.PollInterval || bPollInFlight || Ops.Busy()) return;
		switch (Current.State)
		{
		case S::WaitingForHost:
		case S::WaitingForSession:
		case S::JoinRetry:
		case S::RecoveringHost:
		case S::HostUnreachable:
			LastPoll = Now;
			bPollInFlight = true;
			Ops.Post([this]() { DoCheckAndDecide(); bPollInFlight = false; });
			break;
		case S::Joined:
			LastPoll = Now;
			bPollInFlight = true;
			Ops.Post([this]() { DoFollowHost(); });
			break;
		case S::Reconnecting:
			LastPoll = Now;
			bPollInFlight = true;
			Ops.Post([this]() { DoReconnect(); });
			break;
		default:
			break;
		}
	}

	// ------------------------------------------------------------ summary

	const char* ToString(WorldStatus S)
	{
		switch (S)
		{
		case WorldStatus::Available: return "AVAILABLE";
		case WorldStatus::Starting: return "STARTING";
		case WorldStatus::Online: return "ONLINE";
		case WorldStatus::Saving: return "SAVING";
		case WorldStatus::Stopping: return "STOPPING";
		case WorldStatus::Migrating: return "MIGRATING";
		case WorldStatus::Recoverable: return "RECOVERABLE";
		case WorldStatus::NoSave: return "NO_SAVE";
		case WorldStatus::Unreachable: return "UNREACHABLE";
		case WorldStatus::NotCreated: return "NOT_CREATED";
		}
		return "?";
	}

	WorldSummary Summarize(LeaseManager& Leases)
	{
		WorldSummary Sum;
		Sum.WorldId = Leases.Store().WorldId();
		auto Snap = Leases.Store().Load();
		if (!Snap)
		{
			Sum.Status = Snap.Is(ErrorCode::NoWorld) ? WorldStatus::NotCreated : WorldStatus::Unreachable;
			Sum.StatusText = Snap.Is(ErrorCode::NoWorld) ? "Not created yet" : "Storage unreachable";
			Sum.Problem = Snap.Err().Describe();
			return Sum;
		}
		const WorldState& St = Snap->State;
		const TimeMs Now = Leases.Store().Clock().Now();
		if (auto Info = Leases.Store().LoadInfo(Snap->CommitId); Info.Ok())
		{
			Sum.Name = Info->Name;
			Sum.OriginalSaveName = Info->OriginalSaveName;
			Sum.RequiredModCount = static_cast<int>(Info->RequiredMods.size());
		}
		if (auto Settings = Leases.Store().LoadSettings(Snap->CommitId); Settings.Ok())
		{
			Sum.MaxPlayers = static_cast<int>(Settings->MaxPlayers > 0 ? Settings->MaxPlayers : 4);
		}
		if (St.Head)
		{
			Sum.MapName = St.Head->MapName;
			Sum.MapLabel = St.Head->MapLabel;
			Sum.PlayDurationSeconds = St.Head->PlayDurationSeconds;
			Sum.GamePhase = St.Head->GamePhase;
			// Legacy heads: fill from a cheap header peek of the local synced save when present.
			if ((Sum.MapLabel.empty() || Sum.PlayDurationSeconds <= 0) && !St.Head->ObjectSha256.empty())
			{
				// Local install path is known to SyncEngine; Summarize only has the store.
				// OriginalSaveName / MapLabel from info already help; header fields land on next upload.
			}
		}
		Sum.Revision = St.HeadNumber();
		Sum.Generation = St.Generation;
		Sum.UpdatedAt = St.UpdatedAt;
		if (St.LastSession)
		{
			Sum.LastPlayedAt = St.LastSession->EndedAt;
			Sum.LastHostName = St.LastSession->Host.DisplayName;
		}
		else if (St.Head)
		{
			Sum.LastPlayedAt = St.Head->CreatedAt;
			Sum.LastHostName = St.Head->Uploader.DisplayName;
		}
		if (St.CurrentLease)
		{
			const Lease& L = *St.CurrentLease;
			Sum.HostName = L.Holder.DisplayName;
			Sum.Players = L.Players;
			if (Leases.LiveForObserver(St.CurrentLease, Now))
			{
				switch (L.Phase)
				{
				case LeasePhase::Preparing: Sum.Status = WorldStatus::Starting; Sum.StatusText = L.Holder.DisplayName + " is starting the world"; break;
				case LeasePhase::Saving: Sum.Status = WorldStatus::Saving; Sum.StatusText = "Online (saving)"; break;
				case LeasePhase::Stopping: Sum.Status = WorldStatus::Stopping; Sum.StatusText = L.Holder.DisplayName + " is closing the world"; break;
				case LeasePhase::Migrating: Sum.Status = WorldStatus::Migrating; Sum.StatusText = "Host migration in progress"; break;
				default:
					if (!L.bHostReady)
					{
						Sum.Status = WorldStatus::Starting;
						Sum.StatusText = L.Holder.DisplayName + " is starting the world";
					}
					else
					{
						Sum.Status = WorldStatus::Online;
						Sum.StatusText = "Online";
					}
					break;
				}
			}
			else
			{
				Sum.Status = WorldStatus::Recoverable;
				Sum.StatusText = "Available (" + L.Holder.DisplayName + " stopped responding; press PLAY to recover)";
				Sum.Players.clear();
			}
			return Sum;
		}
		if (St.PendingHandoff && Now < St.PendingHandoff->ExpiresAt)
		{
			Sum.Status = WorldStatus::Migrating;
			Sum.StatusText = "Host migration to " + St.PendingHandoff->Successor.DisplayName;
			return Sum;
		}
		Sum.Status = St.Head ? WorldStatus::Available : WorldStatus::NoSave;
		Sum.StatusText = St.Head ? "Available" : "No shared save yet";
		return Sum;
	}
}
