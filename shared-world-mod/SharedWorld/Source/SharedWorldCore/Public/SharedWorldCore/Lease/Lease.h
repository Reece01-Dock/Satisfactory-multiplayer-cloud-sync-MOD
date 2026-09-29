#pragma once
// Host lease, fencing generations and fenced revision commits.
// Port of shared-world-helper/internal/lease (same invariants, same tests):
//
//  * Acquire succeeds only if no lease is live for an observer; it
//    increments WorldState::Generation, which becomes the holder's fencing
//    token. Of N simultaneous acquirers exactly one CAS wins.
//  * Renew / CommitRevision / Release succeed only while the state still
//    carries the holder's generation and nonce (ErrorCode::Fenced otherwise),
//    even if that holder comes back much later.
//  * CommitRevision additionally requires Head == the holder's base revision,
//    so a writer holding revision N can never replace N+1.
//  * Every mutation is one repository commit whose parent is the state it
//    was computed from (compare-and-swap).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Storage/Storage.h"
#include "SharedWorldCore/Util/Log.h"
#include "SharedWorldCore/Util/Random.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	/** A consistent view: the state document at a specific repository commit. */
	struct StateSnapshot
	{
		std::string CommitId;
		WorldState State;
	};

	/** Everything needed to create a new world in an empty repository. */
	struct NewWorld
	{
		WorldInfo Info;
		PlayerList Players;
		WorldSettings Settings;
	};

	/**
	 * Reads and CAS-updates one world's state document. All writers go
	 * through Mutate, so every state change is a fenced, atomic commit.
	 */
	class WorldStore
	{
	public:
		WorldStore(std::shared_ptr<IWorldRepository> InRepo, std::string InWorldId, std::shared_ptr<IClock> InClock, Logger InLog, int InMaxAttempts = 25);

		const std::string& WorldId() const { return Id; }
		IWorldRepository& Repository() { return *Repo; }
		IClock& Clock() { return *ClockPtr; }
		const Logger& Log() const { return Logs; }

		/** NoWorld if the repository holds no Shared World yet. */
		Result<StateSnapshot> Load();
		/** Creates the world (fails with Conflict/AlreadyExists if one exists). */
		Result<StateSnapshot> Create(const NewWorld& World);

		Result<WorldInfo> LoadInfo(const std::string& CommitId);
		Result<PlayerList> LoadPlayers(const std::string& CommitId);
		Result<WorldSettings> LoadSettings(const std::string& CommitId);

		/**
		 * Mutation callback: edit State in place and optionally add extra
		 * file changes to the same commit. Return NoChange() to skip writing.
		 */
		struct Mutation
		{
			std::vector<FileChange> ExtraChanges;
			std::string Message;
		};
		using MutateFn = std::function<Status(WorldState& State, TimeMs Now, Mutation& Out)>;
		static Error NoChange() { return MakeError(ErrorCode::Cancelled, "no change"); }

		/**
		 * Read-modify-CAS loop. On success returns the committed snapshot. If
		 * Fn returns NoChange, returns the unmodified snapshot. Any other error
		 * from Fn is returned as-is (with the state it was computed on in
		 * LastSeen). Lost races are retried with jittered backoff.
		 */
		Result<StateSnapshot> Mutate(const MutateFn& Fn);
		/**
		 * CAS-updates one non-state document (players / settings) without
		 * touching the state. Fn gets the current content ("" if missing).
		 */
		Result<std::string> UpdateDocument(const std::string& Path, const std::function<Result<std::string>(const std::string& Current)>& Fn, const std::string& Message);

		/** State observed by the most recent Mutate attempt (for error reporting). */
		std::optional<StateSnapshot> LastSeen;

	private:
		std::shared_ptr<IWorldRepository> Repo;
		std::string Id;
		std::shared_ptr<IClock> ClockPtr;
		Logger Logs;
		int MaxAttempts;
		SystemRandom Jitter;
	};

	struct LeaseConfig
	{
		/** Host must heartbeat within this window; shorter = faster crash takeover (MW2-like). */
		TimeMs TTL = Seconds(45);
		/** Added when judging SOMEONE ELSE's lease expired (clock skew tolerance). */
		TimeMs SkewGrace = Seconds(15);
		/** How long a planned-migration successor reservation lasts. */
		TimeMs HandoffWindow = Seconds(120);
	};

	/** Local proof of lease ownership; only trusted by its owner. */
	struct LeaseToken
	{
		std::string WorldId;
		int64_t Generation = 0;
		std::string Nonce;
		int64_t BaseRevision = 0;
		Identity Holder;
	};

	enum class AcquireOutcome
	{
		Acquired,             // caller is now host with a fresh generation
		AlreadyHeld,          // caller already holds this exact lease (idempotent retry)
		HeldByOther,          // live lease exists: join
		HeldBySelfElsewhere,  // same player/install, different session (second instance / other PC)
		ReservedForSuccessor, // planned migration in progress for another player
	};
	const char* ToString(AcquireOutcome O);

	struct AcquireResult
	{
		AcquireOutcome Outcome = AcquireOutcome::HeldByOther;
		std::optional<LeaseToken> Token;
		StateSnapshot Snapshot;
		/** Set when the previous lease had expired (crash / disconnect recovery). */
		std::optional<Lease> TookOverExpired;
		/** Set when this acquisition consumed a handoff reservation. */
		bool bFromHandoff = false;
	};

	struct LeaseUpdate
	{
		std::optional<LeasePhase> Phase;
		std::optional<JoinInfo> Join;
		bool ClearJoin = false;
		std::optional<bool> HostReady;
		std::optional<std::vector<SessionPlayer>> Players;
	};

	class LeaseManager
	{
	public:
		LeaseManager(std::shared_ptr<WorldStore> InStore, LeaseConfig InConfig);

		const LeaseConfig& Config() const { return Cfg; }
		WorldStore& Store() { return *StorePtr; }

		/** Whether a lease held by someone else must be respected at Now. */
		bool LiveForObserver(const std::optional<Lease>& L, TimeMs Now) const;

		Result<AcquireResult> Acquire(const Identity& Holder, const std::string& Nonce);
		/** Extends the lease. Works after local expiry as long as nobody else acquired. */
		Result<StateSnapshot> Renew(const LeaseToken& Token, const LeaseUpdate& Update);
		/**
		 * Makes Rev the new head (Rev.Number == Token.BaseRevision + 1). The
		 * object must already be stored. Writes the revision metadata file in
		 * the same commit. On success Token.BaseRevision advances.
		 */
		Result<StateSnapshot> CommitRevision(LeaseToken& Token, const RevisionMeta& Rev);
		/**
		 * Ends the lease. With Successor set, reserves the next acquisition
		 * for that player for HandoffWindow (planned migration).
		 */
		Status Release(const LeaseToken& Token, const std::optional<Identity>& Successor = std::nullopt);

	private:
		std::shared_ptr<WorldStore> StorePtr;
		LeaseConfig Cfg;
	};

	/** Checks that State still belongs to Token (same generation and nonce). */
	Status CheckFence(const WorldState& State, const LeaseToken& Token);
}
