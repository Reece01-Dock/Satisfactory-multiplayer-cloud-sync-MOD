#pragma once
// Shared World data model (repository schema v2).
//
//   world.json            WorldInfo      rarely changes
//   state/current.json    WorldState     THE authoritative document (CAS domain)
//   state/players.json    PlayerList
//   state/settings.json   WorldSettings
//   revisions/<file>.json RevisionMeta   immutable, one per accepted revision
//
// Everything decoded from a repository is validated before use; invalid
// remote data is an error, never "best effort".

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Result.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	constexpr int64_t SchemaVersion = 2;

	/** World ids: lowercase letters, digits and '-', 1..64 chars (UUIDs qualify). */
	Status ValidateWorldId(const std::string& Id);
	/** Printable text without control characters, at most MaxLen bytes of valid UTF-8. */
	Status ValidateText(const char* Field, const std::string& S, size_t MaxLen);

	struct Identity
	{
		std::string PlayerId;    // online account id reported by the game
		std::string DisplayName;
		std::string Platform;    // "EOS", "Steam", ... informational
		std::string InstallId;   // random per installation of the mod

		json::Value ToJson() const;
		static Result<Identity> FromJson(const json::Value& V);
		Status Validate() const;
		bool SamePlayerOrInstall(const Identity& O) const { return PlayerId == O.PlayerId || InstallId == O.InstallId; }
	};

	/** Why a revision was created. */
	namespace Reason
	{
		inline constexpr const char* Import = "import";
		inline constexpr const char* Checkpoint = "checkpoint";
		inline constexpr const char* Autosave = "autosave";
		inline constexpr const char* Final = "final";
		inline constexpr const char* Migration = "migration";
		inline constexpr const char* Recovered = "recovered";
		inline constexpr const char* Restore = "restore";
	}

	/** Metadata of one accepted, immutable revision. */
	struct RevisionMeta
	{
		int64_t Number = 0;
		int64_t Generation = 0;
		int64_t PreviousRevision = 0;
		std::string ObjectSha256; // content address of the .sav bytes
		int64_t Size = 0;
		TimeMs CreatedAt = 0;
		Identity Uploader;
		std::string Reason;
		int64_t RestoredFrom = 0; // for Reason::Restore
		std::string GameBuild;
		std::string ModVersion;

		/** revisions/00000152-g00000027-3f2a1c9e.json (unique even for orphaned attempts). */
		std::string Path() const;
		json::Value ToJson() const;
		static Result<RevisionMeta> FromJson(const json::Value& V);
		Status Validate() const;
	};

	enum class LeasePhase { Preparing, Hosting, Saving, Stopping, Migrating };
	const char* ToString(LeasePhase P);
	Result<LeasePhase> ParseLeasePhase(const std::string& S);

	struct JoinInfo
	{
		std::string Kind;    // "online-session-id" | "address"
		std::string Data;    // session id string or host:port
		std::string Backend;

		json::Value ToJson() const;
		static Result<JoinInfo> FromJson(const json::Value& V);
		Status Validate() const;
	};

	struct SessionPlayer
	{
		std::string DisplayName;
		std::string PlayerId;
	};

	/** Planned host migration: only Successor may acquire until ExpiresAt. */
	struct Handoff
	{
		Identity Successor;
		int64_t Revision = 0;
		TimeMs ExpiresAt = 0;
	};

	struct Lease
	{
		int64_t Generation = 0;
		Identity Holder;
		std::string Nonce;
		TimeMs AcquiredAt = 0;
		TimeMs RenewedAt = 0;
		TimeMs ExpiresAt = 0;
		int64_t BaseRevision = 0;
		LeasePhase Phase = LeasePhase::Preparing;
		std::optional<JoinInfo> Join;
		std::vector<SessionPlayer> Players;
	};

	struct SessionEnd
	{
		Identity Host;
		int64_t Generation = 0;
		TimeMs EndedAt = 0;
		std::string Reason; // "released" | "expired" | "migrated"
	};

	/** state/current.json */
	struct WorldState
	{
		std::string WorldId;
		/** Incremented on every write so no two versions of the document are identical. */
		int64_t StateVersion = 0;
		/** Last fencing token issued. Survives lease release. */
		int64_t Generation = 0;
		std::optional<RevisionMeta> Head;
		std::optional<Lease> CurrentLease;
		std::optional<SessionEnd> LastSession;
		/** Set by a host that releases for a planned migration. */
		std::optional<Handoff> PendingHandoff;
		TimeMs UpdatedAt = 0;

		int64_t HeadNumber() const { return Head ? Head->Number : 0; }

		json::Value ToJson() const;
		static Result<WorldState> FromJson(const json::Value& V, const std::string& ExpectedWorldId);
		Status Validate(const std::string& ExpectedWorldId) const;
	};

	struct RequiredMod
	{
		std::string ModReference;
		std::string Version;
	};

	/** world.json */
	struct WorldInfo
	{
		std::string WorldId;
		std::string Name;
		Identity CreatedBy;
		TimeMs CreatedAt = 0;
		std::string OriginalSaveName;
		std::string GameBuild;
		std::string ModVersion;
		std::vector<RequiredMod> RequiredMods;

		json::Value ToJson() const;
		static Result<WorldInfo> FromJson(const json::Value& V);
		Status Validate() const;
	};

	enum class Role { Owner, Admin, Member, Viewer };
	const char* ToString(Role R);
	Result<Role> ParseRole(const std::string& S);

	enum class Permission { Play, Invite, RemovePlayers, RestoreRevision, DeleteWorld, ModifyStorage, ChangeSettings };
	bool HasPermission(Role R, Permission P);

	struct Member
	{
		std::string PlayerId;
		std::string DisplayName;
		Role MemberRole = Role::Member;
	};

	/** state/players.json */
	struct PlayerList
	{
		std::vector<Member> Members;

		const Member* Find(const std::string& PlayerId) const;
		json::Value ToJson() const;
		static Result<PlayerList> FromJson(const json::Value& V);
		Status Validate() const;
	};

	/** state/settings.json */
	struct WorldSettings
	{
		std::string Name;
		int64_t SyncIntervalMinutes = 10;
		bool HostMigration = true;
		bool PeerRecovery = true;
		int64_t KeepRevisions = 100;
		int64_t MaxPlayers = 4;
		std::vector<std::string> PreferredHosts; // player ids, in order

		json::Value ToJson() const;
		static Result<WorldSettings> FromJson(const json::Value& V);
		Status Validate() const;
	};

	/** Repository paths. */
	namespace Paths
	{
		inline constexpr const char* WorldInfo = "world.json";
		inline constexpr const char* State = "state/current.json";
		inline constexpr const char* Players = "state/players.json";
		inline constexpr const char* Settings = "state/settings.json";
		inline constexpr const char* RevisionsDir = "revisions";
		inline constexpr const char* RecoveryDir = "recovery";
	}

	/** Parse + validate helpers used by every reader of repository files. */
	Result<WorldState> DecodeState(const std::string& Text, const std::string& WorldId);
	std::string EncodeState(const WorldState& S);
}
