#pragma once
// Per-player, per-PC settings: which Shared Worlds this player has and
// where each one is stored. Lives in the mod's local data directory
// (%LOCALAPPDATA%/SatisfactorySharedWorld/settings.json), never in a save
// or a world repository. It holds NO secrets: tokens live in the OS
// credential store (ICredentialStore), referenced only by account name.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "SharedWorldCore/Storage/LogRepository.h"
#include "SharedWorldCore/Storage/Storage.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	enum class ProviderKind { GitHub, Folder, Rclone };
	const char* ToString(ProviderKind K);

	struct ProviderConfig
	{
		ProviderKind Kind = ProviderKind::GitHub;
		// GitHub: repository that stores the world (branch shared-world/<worldId>).
		std::string Owner;
		std::string Repo;
		// Folder: absolute path of a local folder or network share.
		std::string FolderPath;
		// Rclone: the whole world (record, state, locks and saves) on an rclone provider. Remote is this PC's rclone path
		// (remote:folder); the world lives in its <worldId> subfolder. Backend/Label name the provider for the UI.
		std::string Remote;
		std::string Backend;
		std::string Label;

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
		/**
		 * This PC's link to the world's save storage when world.json names one (see WorldInfo::SaveStorage):
		 * an rclone path (remote:folder) whose <worldId> subfolder holds the save files. Empty = not linked here, so
		 * this PC can still join while someone else hosts but cannot host until linked. SaveBackend/SaveLabel cache
		 * what world.json says, for the UI.
		 */
		std::string SaveRemote;
		std::string SaveBackend;
		std::string SaveLabel;
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
		/** Public OAuth App client id; required to refresh expiring access tokens. */
		std::string GitHubOAuthClientId;
		std::string GitHubApiBase = "https://api.github.com";
		std::string GitHubUploadBase = "https://uploads.github.com";
		std::string GitHubWebBase = "https://github.com";
		/** Opens plain file storage for a world record on an rclone path (game module supplies it). Null = unavailable. */
		std::function<Result<std::shared_ptr<ILogStore>>(const std::string& Fs)> OpenRemoteLogStore;
		/** Wait before trusting a commit on rclone storage (must exceed twice the provider's listing delay). */
		TimeMs RemoteSettleMs = Seconds(10);
		/** Wall clock used for token expiry / refresh. Null → SystemClock per call. */
		const IClock* Clock = nullptr;
		/** Opens an object store on an rclone path (remote:folder). Supplied by the game module: core knows no rclone. Null = unavailable. */
		std::function<Result<std::shared_ptr<IObjectStore>>(const std::string& Fs)> OpenRemoteObjects;
		/** Called (on a worker thread) when a world's world.json names its save storage, so the UI can show it. */
		std::function<void(const std::string& WorldId, const std::string& Backend, const std::string& Label)> OnSaveStorageSeen;
	};

	struct WorldStorage
	{
		std::shared_ptr<IWorldRepository> Repository;
		std::shared_ptr<IObjectStore> Objects;
	};

	/** Builds the repository + object store for a world entry. */
	Result<WorldStorage> OpenWorldStorage(const WorldEntry& Entry, const ProviderEnvironment& Env);
}
