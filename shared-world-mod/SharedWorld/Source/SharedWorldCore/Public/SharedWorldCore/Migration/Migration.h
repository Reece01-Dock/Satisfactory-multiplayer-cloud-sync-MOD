#pragma once
// Pure decision functions for host migration and crash recovery. Kept free
// of I/O so they are deterministic and exhaustively testable.

#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Model/Model.h"

namespace sw
{
	/** Readiness a connected client reports to the host (via game replication). */
	struct SuccessorCandidate
	{
		Identity Who;
		bool bConnected = false;
		bool bCompatible = false;       // same game build / mod versions
		bool bStorageReachable = false; // can read the repository and objects
		bool bHasHeadCached = false;    // already holds the head revision's object
		int PingMs = 9999;
	};

	/**
	 * Chooses the planned-migration successor: eligible = connected,
	 * compatible, storage reachable, not the current host. Ranked by the
	 * world's preferred-host order, then cached head, then ping, then player
	 * id (deterministic). nullopt when nobody is eligible.
	 */
	std::optional<Identity> SelectSuccessor(const std::vector<SuccessorCandidate>& Candidates, const std::vector<std::string>& PreferredHosts, const Identity& CurrentHost);

	/**
	 * Order in which players should try to take over after a host crash
	 * (0 = first). Everyone may still try; the lease CAS picks exactly one
	 * winner. Players not in the list come last.
	 */
	int TakeoverRank(const std::vector<SessionPlayer>& LastPlayers, const std::vector<std::string>& PreferredHosts, const std::string& MyPlayerId);

	/** A save that might hold progress newer than the head revision. */
	struct RecoveryCandidate
	{
		std::string Source;        // "local" or "report:<player>"
		int64_t Generation = 0;    // generation of the host session that produced it
		int64_t BaseRevision = 0;  // revision that session started from
		std::string ObjectSha256;
		int64_t Size = 0;
		TimeMs SavedAt = 0;
		bool bValidated = false;       // full structural validation passed
		bool bObjectAvailable = false; // bytes are available (local file or object store)
	};

	/**
	 * Picks a recovery candidate for the crashed session. A candidate is
	 * acceptable only if it comes from exactly the crashed generation, was
	 * based on the current head, passed validation, its bytes are available
	 * and it differs from the head. Among acceptable candidates the newest
	 * save wins; ties break on size then hash. Being newest is never enough
	 * on its own. nullopt means: continue from the head revision.
	 */
	std::optional<RecoveryCandidate> SelectRecoveryCandidate(const std::vector<RecoveryCandidate>& Candidates, int64_t CrashedGeneration, const WorldState& State);

	/** recovery/g<gen>-<sha8>.json report written by a host that could not commit. */
	json::Value RecoveryReportToJson(const RecoveryCandidate& C, const Identity& Reporter);
	Result<RecoveryCandidate> RecoveryReportFromJson(const json::Value& V);
	std::string RecoveryReportPath(const RecoveryCandidate& C);
}
