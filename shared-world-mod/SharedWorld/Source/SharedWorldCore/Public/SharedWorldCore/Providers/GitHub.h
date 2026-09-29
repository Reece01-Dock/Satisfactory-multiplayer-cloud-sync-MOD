#pragma once
// GitHub provider.
//
// Repository: one branch per world, "shared-world/<worldId>", in a private
// repository. Every state change is a commit created with the Git Data API
// (POST git/trees with base_tree, POST git/commits with exactly one parent)
// and published with PATCH git/refs/heads/<branch> {"force": false}. GitHub
// only accepts a fast-forward; because the new commit's only parent is the
// head we read, the update succeeds iff the branch still points at that
// head: an atomic compare-and-swap. (Never force-push this branch; branch
// protection can forbid it.)
//
// Objects: release assets named "<sha256>.sav" in pre-releases tagged
// "shared-world-objects-<worldId>[-N]" (assets < 2 GiB, 1000 per release,
// no total size or bandwidth limit per GitHub's documentation). Downloads
// follow the asset redirect WITHOUT the Authorization header.
//
// Limits respected: 3 content-creating requests per state commit (secondary
// limit 80/min, 500/h); 403/429 with rate-limit headers -> RateLimited.

#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "SharedWorldCore/Providers/Http.h"
#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	struct GitHubConfig
	{
		std::string Owner;
		std::string Repo;
		std::string WorldId;
		std::string ApiBase = "https://api.github.com";
		std::string UploadBase = "https://uploads.github.com";
		std::string UserAgent = "SatisfactorySharedWorld";
		/** Returns the current OAuth access token (never logged, never stored in the repo). */
		std::function<Result<std::string>()> Token;

		std::string Branch() const { return "shared-world/" + WorldId; }
		Status Validate() const;
	};

	/** GitHub user / organisation / repository name rules. */
	bool ValidGitHubName(const std::string& Name);

	/** Shared request plumbing: auth, JSON, error mapping. */
	class GitHubApi
	{
	public:
		GitHubApi(std::shared_ptr<IHttpClient> InHttp, GitHubConfig InConfig) : Http(std::move(InHttp)), Cfg(std::move(InConfig)) {}

		const GitHubConfig& Config() const { return Cfg; }
		/** Sends an authenticated API request; maps 401/403/404/409/422/429/5xx to error codes. */
		Result<HttpResponse> Call(HttpRequest Req, std::initializer_list<int> OkStatuses = {200, 201});
		std::string RepoUrl(const std::string& Suffix) const;
		/**
		 * Ensures github.com/<Owner>/<Repo> exists. If missing, creates a private empty
		 * repository under the authenticated user. Idempotent.
		 */
		Status EnsureRepositoryExists();
		IHttpClient& Client() { return *Http; }

	private:
		std::shared_ptr<IHttpClient> Http;
		GitHubConfig Cfg;
		bool bRepoExistsCached = false;
	};

	class GitHubRepository final : public IWorldRepository
	{
	public:
		GitHubRepository(std::shared_ptr<IHttpClient> Http, GitHubConfig Config);

		Result<std::string> Head() override;
		Result<std::string> ReadFile(const std::string& CommitId, const std::string& Path) override;
		Result<std::vector<std::string>> ListDirectory(const std::string& CommitId, const std::string& Dir) override;
		Result<std::string> Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message) override;
		Result<std::vector<CommitInfo>> Log(const std::string& FromCommit, int MaxCount) override;
		std::string Describe() const override;

	private:
		Result<std::string> TreeOfCommit(const std::string& CommitId);
		Status EnsureRepositoryInitialised();

		GitHubApi Api;
		std::mutex CacheMutex;
		std::map<std::string, std::string> CommitTrees; // commit -> tree (immutable)
	};

	class GitHubReleaseObjectStore final : public IObjectStore
	{
	public:
		GitHubReleaseObjectStore(std::shared_ptr<IHttpClient> Http, GitHubConfig Config);

		Result<bool> Has(const std::string& Sha256) override;
		Status Put(const std::string& Sha256, const std::string& LocalPath) override;
		Status PutBlob(const std::string& ObjectId, const std::string& LocalPath) override;
		Status Get(const std::string& Sha256, const std::string& DestPath) override;
		Status Remove(const std::string& Sha256) override;
		Result<std::vector<std::string>> List() override;
		std::string Describe() const override;

		static constexpr int MaxAssetsPerRelease = 1000;

	private:
		struct Asset
		{
			int64_t Id = 0;
			int64_t ReleaseId = 0;
			int64_t Size = 0;
			bool bComplete = false; // state == "uploaded"
		};
		struct Release
		{
			int64_t Id = 0;
			std::string UploadUrl;
			int AssetCount = 0;
		};
		Status Refresh();
		Result<Release> ReleaseWithRoom();
		std::string TagPrefix() const { return "shared-world-objects-" + Api.Config().WorldId; }

		GitHubApi Api;
		std::mutex Mutex;
		bool bLoaded = false;
		std::map<std::string, Asset> Assets; // sha -> asset
		std::vector<Release> Releases;
	};

	/**
	 * Gives a friend write access to the world's repository (the real access
	 * boundary). Returns true if GitHub created an invitation the friend must
	 * accept, false if they already had access.
	 */
	Result<bool> InviteGitHubCollaborator(std::shared_ptr<IHttpClient> Http, const GitHubConfig& Config, const std::string& Login);
}
