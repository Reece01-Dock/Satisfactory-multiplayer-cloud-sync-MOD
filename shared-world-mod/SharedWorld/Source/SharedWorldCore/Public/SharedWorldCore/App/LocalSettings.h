#pragma once
// Per-player, per-PC settings: which Shared Worlds this player has and
// where each one is stored. Lives in the mod's local data directory
// (%LOCALAPPDATA%/SatisfactorySharedWorld/settings.json), never in a save
// or a world repository. It holds NO secrets: tokens live in the OS
// credential store (ICredentialStore), referenced only by account name.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "SharedWorldCore/Storage/Storage.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	enum class ProviderKind { GitHub, Folder };
	const char* ToString(ProviderKind K);

	struct ProviderConfig
	{
		ProviderKind Kind = ProviderKind::GitHub;
		// GitHub: repository that stores the world (branch shared-world/<worldId>).
		std::string Owner;
		std::string Repo;
		// Folder: absolute path of a local folder or network share.
		std::string FolderPath;

		json::Value ToJson() const;
		static Result<ProviderConfig> FromJson(const json::Value& V);
		Status Validate() const;
	};

	/** How this PC relates to the world (drives Your Worlds vs Shared With You). */
	enum class WorldRelation { Owned, Shared };

	struct WorldEntry
	{
		std::string WorldId;
		std::string DisplayName;
		ProviderConfig Provider;
		TimeMs AddedAt = 0;
		TimeMs LastPlayedAt = 0;
		WorldRelation Relation = WorldRelation::Owned;
		/** Short share code (XXXX-XXXX) for Join Using Code. Empty until generated. */
		std::string InviteCode;
	};

	struct PendingInvite
	{
		std::string InviteId;
		std::string WorldId;
		std::string WorldName;
		std::string FromPlayerId;
		std::string FromDisplayName;
		ProviderConfig Provider;
		TimeMs CreatedAt = 0;
	};

	struct LocalSettings
	{
		static constexpr int CurrentVersion = 1;
		/** Signed-in GitHub login (display only; the token is in the credential store). */
		std::string GitHubLogin;
		std::vector<WorldEntry> Worlds;
		/** True after the one-time welcome / storage connect screen. */
		bool bWelcomeDone = false;
		/** Auto-selected storage for new worlds (folder or GitHub). Empty = derive at runtime. */
		std::optional<ProviderConfig> DefaultProvider;
		/** How often the host saves and uploads a checkpoint. Always within [Min, Max]. */
		static constexpr int MinCheckpointSeconds = 60;
		static constexpr int MaxCheckpointSeconds = 60 * 60;
		static constexpr int DefaultCheckpointSeconds = 5 * 60;
		int CheckpointIntervalSeconds = DefaultCheckpointSeconds;
		/** Clamps into [Min, Max]; a hand-edited value can never disable or hammer checkpoints. */
		static int ClampCheckpointSeconds(int64_t Seconds);
		/** Invites waiting for Accept / Decline on this PC. */
		std::vector<PendingInvite> PendingInvites;

		const WorldEntry* Find(const std::string& WorldId) const;
		WorldEntry* FindMutable(const std::string& WorldId);
		/** Adds or replaces the entry with the same world id. */
		Status Upsert(const WorldEntry& Entry);
		bool Remove(const std::string& WorldId);

		json::Value ToJson() const;
		static Result<LocalSettings> FromJson(const json::Value& V);
	};

	/** Missing file: empty settings. Corrupt file: Corrupt error (never silently reset). */
	Result<LocalSettings> LoadLocalSettings(const std::string& Path);
	Status SaveLocalSettings(const std::string& Path, const LocalSettings& Settings);

	/** Credential store key for the signed-in GitHub account. */
	inline constexpr const char* GitHubCredentialKey = "SatisfactorySharedWorld/github";

	struct ProviderEnvironment
	{
		std::shared_ptr<IHttpClient> Http;
		std::shared_ptr<ICredentialStore> Credentials;
		std::string GitHubApiBase = "https://api.github.com";
		std::string GitHubUploadBase = "https://uploads.github.com";
	};

	struct WorldStorage
	{
		std::shared_ptr<IWorldRepository> Repository;
		std::shared_ptr<IObjectStore> Objects;
	};

	/** Builds the repository + object store for a world entry. */
	Result<WorldStorage> OpenWorldStorage(const WorldEntry& Entry, const ProviderEnvironment& Env);
}
