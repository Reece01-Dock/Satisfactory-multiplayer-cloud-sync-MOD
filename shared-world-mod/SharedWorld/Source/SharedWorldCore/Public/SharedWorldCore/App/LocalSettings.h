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

	struct WorldEntry
	{
		std::string WorldId;
		std::string DisplayName;
		ProviderConfig Provider;
		TimeMs AddedAt = 0;
		TimeMs LastPlayedAt = 0;
	};

	struct LocalSettings
	{
		static constexpr int CurrentVersion = 1;
		/** Signed-in GitHub login (display only; the token is in the credential store). */
		std::string GitHubLogin;
		std::vector<WorldEntry> Worlds;

		const WorldEntry* Find(const std::string& WorldId) const;
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
