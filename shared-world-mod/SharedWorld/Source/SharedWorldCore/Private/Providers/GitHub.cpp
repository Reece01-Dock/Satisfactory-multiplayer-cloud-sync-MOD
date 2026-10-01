#include "SharedWorldCore/Providers/GitHub.h"

#include <algorithm>

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Util/Base64.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	namespace
	{
		bool ValidName(const std::string& S)
		{
			if (S.empty() || S.size() > 100 || S == "." || S == "..") return false;
			for (char C : S)
			{
				if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '-' || C == '_' || C == '.')) return false;
			}
			return true;
		}

		std::string Excerpt(const std::string& Body)
		{
			// Only GitHub's "message" field; never echo arbitrary bodies.
			auto V = json::Parse(Body);
			if (V.Ok())
			{
				if (const json::Value* M = V->Find("message"); M && M->IsString()) return M->AsString().substr(0, 200);
			}
			return std::string();
		}

		Result<json::Value> ParseBody(const HttpResponse& R)
		{
			auto V = json::Parse(R.Body);
			if (!V) return V.Err().Wrap("GitHub returned an invalid response");
			return V;
		}

		Result<std::string> GetSha(const json::Value& V, std::initializer_list<const char*> Path)
		{
			const json::Value* Cur = &V;
			for (const char* Key : Path)
			{
				Cur = Cur->Find(Key);
				if (!Cur) return MakeError(ErrorCode::Invalid, std::string("GitHub response missing ") + Key);
			}
			if (!Cur->IsString() || Cur->AsString().size() != 40) return MakeError(ErrorCode::Invalid, "GitHub response has an invalid SHA");
			for (char C : Cur->AsString())
			{
				if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return MakeError(ErrorCode::Invalid, "GitHub response has an invalid SHA");
			}
			return Cur->AsString();
		}
	}

	bool ValidGitHubName(const std::string& Name) { return ValidName(Name); }

	Status GitHubConfig::Validate() const
	{
		if (!ValidName(Owner) || !ValidName(Repo)) return MakeError(ErrorCode::Invalid, "invalid GitHub owner/repository name");
		SW_TRY(ValidateWorldId(WorldId));
		if (!Token) return MakeError(ErrorCode::Unauthorized, "no GitHub account connected");
		return {};
	}

	std::string GitHubApi::RepoUrl(const std::string& Suffix) const { return Cfg.ApiBase + "/repos/" + Cfg.Owner + "/" + Cfg.Repo + Suffix; }

	Status GitHubApi::EnsureRepositoryExists()
	{
		if (bRepoExistsCached) return {};
		HttpRequest Get;
		Get.Url = Cfg.ApiBase + "/repos/" + Cfg.Owner + "/" + Cfg.Repo;
		auto Existing = Call(Get, {200});
		if (Existing.Ok())
		{
			bRepoExistsCached = true;
			return {};
		}
		if (!Existing.Is(ErrorCode::NotFound)) return Existing.Err().Wrap("check GitHub repository");

		// Auto-create a private empty repo under the signed-in user. Players should
		// never need to create this manually. Name must match Config.Repo.
		HttpRequest Create;
		Create.Method = "POST";
		Create.Url = Cfg.ApiBase + "/user/repos";
		json::Value Body;
		Body.Set("name", Cfg.Repo);
		Body.Set("private", true);
		Body.Set("description", "Satisfactory Shared Worlds cloud storage (created automatically by the Shared Worlds mod)");
		Body.Set("auto_init", false);
		Body.Set("has_issues", false);
		Body.Set("has_projects", false);
		Body.Set("has_wiki", false);
		Create.Body = json::Serialize(Body);
		auto Created = Call(Create, {201});
		if (!Created)
		{
			// Race: another client created it, or it already existed under a naming quirk.
			if (Created.Is(ErrorCode::Conflict))
			{
				auto Again = Call(Get, {200});
				if (Again.Ok())
				{
					bRepoExistsCached = true;
					return {};
				}
			}
			return Created.Err().Wrap("create GitHub repository " + Cfg.Owner + "/" + Cfg.Repo);
		}
		bRepoExistsCached = true;
		return {};
	}

	Result<HttpResponse> GitHubApi::Call(HttpRequest Req, std::initializer_list<int> OkStatuses)
	{
		SW_TRY(Cfg.Validate());
		std::string Tok;
		SW_ASSIGN(Tok, Cfg.Token());
		bool bRetriedAuth = false;
		for (;;)
		{
			HttpRequest Attempt = Req;
			bool bHasAccept = false;
			bool bHasContentType = false;
			bool bHasAuth = false;
			for (const auto& [K, V] : Attempt.Headers)
			{
				bHasAccept |= K == "Accept";
				bHasContentType |= K == "Content-Type";
				bHasAuth |= K == "Authorization";
			}
			// UE CurlHttp asserts when a request has a body and no Content-Type header.
			if (!bHasContentType && (!Attempt.Body.empty() || !Attempt.BodyFile.empty()))
			{
				Attempt.Headers.push_back({"Content-Type", "application/json"});
			}
			if (!bHasAccept) Attempt.Headers.push_back({"Accept", "application/vnd.github+json"});
			if (!bHasAuth) Attempt.Headers.push_back({"Authorization", "Bearer " + Tok});
			Attempt.Headers.push_back({"X-GitHub-Api-Version", "2022-11-28"});
			Attempt.Headers.push_back({"User-Agent", Cfg.UserAgent});
			HttpResponse R;
			SW_ASSIGN(R, Http->Send(Attempt));
			if (std::find(OkStatuses.begin(), OkStatuses.end(), R.Status) != OkStatuses.end()) return R;
			const std::string Msg = Excerpt(R.Body);
			const std::string Where = Attempt.Method + " " + Attempt.Url.substr(Cfg.ApiBase.size() < Attempt.Url.size() ? 0 : 0, 120);
			if (R.Status == 401 && !bRetriedAuth && Cfg.RefreshOnUnauthorized)
			{
				bRetriedAuth = true;
				if (Status Ref = Cfg.RefreshOnUnauthorized(); Ref)
				{
					SW_ASSIGN(Tok, Cfg.Token());
					continue;
				}
			}
			switch (R.Status)
			{
			case 401:
				return MakeError(ErrorCode::Unauthorized, "GitHub rejected the credentials");
			case 403:
			case 429:
				if (R.Status == 429 || R.Header("x-ratelimit-remaining") == "0" || !R.Header("retry-after").empty() || Msg.find("rate limit") != std::string::npos)
				{
					const std::string After = R.Header("retry-after");
					return MakeError(ErrorCode::RateLimited, "GitHub rate limit reached" + (After.empty() ? std::string() : ", retry after " + After + " s"));
				}
				// Permission / scope / SSO issues — not proof the OAuth credential is invalid.
				return MakeError(ErrorCode::BadState, "no access to the GitHub repository: " + Msg);
			case 404:
				return MakeError(ErrorCode::NotFound, "not found: " + Msg);
			case 409:
				if (Msg.find("empty") != std::string::npos) return MakeError(ErrorCode::BadState, "GitHub repository is empty");
				return MakeError(ErrorCode::Conflict, "GitHub conflict: " + Msg);
			case 422:
				return MakeError(ErrorCode::Conflict, "GitHub rejected the update: " + Msg);
			default:
				if (R.Status >= 500) return MakeError(ErrorCode::Network, "GitHub server error " + std::to_string(R.Status));
				return MakeError(ErrorCode::Invalid, "unexpected GitHub response " + std::to_string(R.Status) + " for " + Where + ": " + Msg);
			}
		}
	}

	// ------------------------------------------------------------ repository

	GitHubRepository::GitHubRepository(std::shared_ptr<IHttpClient> Http, GitHubConfig Config) : Api(std::move(Http), std::move(Config)) {}

	std::string GitHubRepository::Describe() const
	{
		return "github.com/" + Api.Config().Owner + "/" + Api.Config().Repo + " (" + Api.Config().Branch() + ")";
	}

	Result<std::string> GitHubRepository::Head()
	{
		HttpRequest Req;
		Req.Url = Api.RepoUrl("/git/ref/heads/" + Api.Config().Branch());
		auto R = Api.Call(Req);
		if (R.Is(ErrorCode::NotFound) || R.Is(ErrorCode::BadState)) return MakeError(ErrorCode::NotFound, "no Shared World in this repository yet");
		if (!R) return R.Err();
		json::Value V;
		SW_ASSIGN(V, ParseBody(*R));
		return GetSha(V, {"object", "sha"});
	}

	Result<std::string> GitHubRepository::TreeOfCommit(const std::string& CommitId)
	{
		{
			std::lock_guard<std::mutex> Lock(CacheMutex);
			if (auto It = CommitTrees.find(CommitId); It != CommitTrees.end()) return It->second;
		}
		HttpRequest Req;
		Req.Url = Api.RepoUrl("/git/commits/" + CommitId);
		HttpResponse R;
		SW_ASSIGN(R, Api.Call(Req));
		json::Value V;
		SW_ASSIGN(V, ParseBody(R));
		std::string Tree;
		SW_ASSIGN(Tree, GetSha(V, {"tree", "sha"}));
		std::lock_guard<std::mutex> Lock(CacheMutex);
		CommitTrees[CommitId] = Tree;
		return Tree;
	}

	Result<std::string> GitHubRepository::ReadFile(const std::string& CommitId, const std::string& Path)
	{
		SW_TRY(ValidateRepoPath(Path));
		HttpRequest Req;
		Req.Url = Api.RepoUrl("/contents/" + Path + "?ref=" + CommitId);
		Req.Headers.push_back({"Accept", "application/vnd.github.raw+json"});
		HttpResponse R;
		SW_ASSIGN(R, Api.Call(Req));
		return R.Body;
	}

	Result<std::vector<std::string>> GitHubRepository::ListDirectory(const std::string& CommitId, const std::string& Dir)
	{
		SW_TRY(ValidateRepoPath(Dir));
		std::string Tree;
		SW_ASSIGN(Tree, TreeOfCommit(CommitId));
		size_t Start = 0;
		while (true)
		{
			HttpRequest Req;
			Req.Url = Api.RepoUrl("/git/trees/" + Tree);
			HttpResponse R;
			SW_ASSIGN(R, Api.Call(Req));
			json::Value V;
			SW_ASSIGN(V, ParseBody(R));
			if (const json::Value* T = V.Find("truncated"); T && T->IsBool() && T->AsBool())
			{
				return MakeError(ErrorCode::Unsupported, "repository directory too large");
			}
			const json::Value* Entries = V.Find("tree");
			if (!Entries || !Entries->IsArray()) return MakeError(ErrorCode::Invalid, "GitHub tree response invalid");
			if (Start > Dir.size())
			{
				std::vector<std::string> Out;
				for (const json::Value& E : Entries->AsArray())
				{
					if (const json::Value* P = E.Find("path"); P && P->IsString()) Out.push_back(P->AsString());
				}
				return Out;
			}
			const size_t Slash = Dir.find('/', Start);
			const std::string Seg = Dir.substr(Start, Slash == std::string::npos ? std::string::npos : Slash - Start);
			std::string Next;
			for (const json::Value& E : Entries->AsArray())
			{
				const json::Value* P = E.Find("path");
				const json::Value* Ty = E.Find("type");
				if (P && Ty && P->IsString() && Ty->IsString() && P->AsString() == Seg && Ty->AsString() == "tree")
				{
					auto Sha = GetSha(E, {"sha"});
					if (Sha.Ok()) Next = *Sha;
				}
			}
			if (Next.empty()) return std::vector<std::string>(); // directory missing
			Tree = Next;
			Start = Slash == std::string::npos ? Dir.size() + 1 : Slash + 1;
		}
	}

	Status GitHubRepository::EnsureRepositoryInitialised()
	{
		SW_TRY(Api.EnsureRepositoryExists());
		// The Git Data API cannot create refs in a repository without any
		// commit; the Contents API can create the first one.
		HttpRequest Req;
		Req.Method = "PUT";
		Req.Url = Api.RepoUrl("/contents/SHARED-WORLD.md");
		json::Value B;
		B.Set("message", "Initialise Shared World storage");
		B.Set("content", Base64Encode("# Satisfactory Shared World storage\n\nManaged by the Shared World mod. Do not edit or force-push the shared-world/* branches.\n"));
		Req.Body = json::Serialize(B);
		auto R = Api.Call(Req, {200, 201});
		if (!R && !R.Is(ErrorCode::Conflict)) return R.Err(); // 422: file exists (someone else initialised)
		return {};
	}

	Result<std::string> GitHubRepository::Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message)
	{
		SW_TRY(Api.EnsureRepositoryExists());
		for (const FileChange& C : Changes) SW_TRY(ValidateRepoPath(C.Path));
		json::Array Entries;
		for (const FileChange& C : Changes)
		{
			json::Value E;
			E.Set("path", C.Path);
			E.Set("mode", "100644");
			E.Set("type", "blob");
			if (C.Content) E.Set("content", *C.Content);
			else E.Set("sha", json::Value()); // null deletes
			Entries.push_back(std::move(E));
		}
		json::Value TreeBody;
		TreeBody.Set("tree", json::Value(std::move(Entries)));
		if (!ExpectedHead.empty())
		{
			std::string Base;
			SW_ASSIGN(Base, TreeOfCommit(ExpectedHead));
			TreeBody.Set("base_tree", Base);
		}
		HttpRequest TreeReq;
		TreeReq.Method = "POST";
		TreeReq.Url = Api.RepoUrl("/git/trees");
		TreeReq.Body = json::Serialize(TreeBody);
		auto TreeRes = Api.Call(TreeReq, {201});
		if (TreeRes.Is(ErrorCode::BadState))
		{
			SW_TRY(EnsureRepositoryInitialised());
			TreeRes = Api.Call(TreeReq, {201});
		}
		if (!TreeRes) return TreeRes.Err().Wrap("create tree");
		json::Value TV;
		SW_ASSIGN(TV, ParseBody(*TreeRes));
		std::string Tree;
		SW_ASSIGN(Tree, GetSha(TV, {"sha"}));

		json::Value CommitBody;
		CommitBody.Set("message", Message);
		CommitBody.Set("tree", Tree);
		json::Array Parents;
		if (!ExpectedHead.empty()) Parents.push_back(json::Value(ExpectedHead));
		CommitBody.Set("parents", json::Value(std::move(Parents)));
		HttpRequest CommitReq;
		CommitReq.Method = "POST";
		CommitReq.Url = Api.RepoUrl("/git/commits");
		CommitReq.Body = json::Serialize(CommitBody);
		HttpResponse CR;
		SW_ASSIGN(CR, Api.Call(CommitReq, {201}));
		json::Value CV;
		SW_ASSIGN(CV, ParseBody(CR));
		std::string NewCommit;
		SW_ASSIGN(NewCommit, GetSha(CV, {"sha"}));
		{
			std::lock_guard<std::mutex> Lock(CacheMutex);
			CommitTrees[NewCommit] = Tree;
		}

		// The compare-and-swap.
		HttpRequest RefReq;
		json::Value RefBody;
		if (ExpectedHead.empty())
		{
			RefReq.Method = "POST";
			RefReq.Url = Api.RepoUrl("/git/refs");
			RefBody.Set("ref", "refs/heads/" + Api.Config().Branch());
			RefBody.Set("sha", NewCommit);
		}
		else
		{
			RefReq.Method = "PATCH";
			RefReq.Url = Api.RepoUrl("/git/refs/heads/" + Api.Config().Branch());
			RefBody.Set("sha", NewCommit);
			RefBody.Set("force", false);
		}
		RefReq.Body = json::Serialize(RefBody);
		auto RefRes = Api.Call(RefReq, {200, 201});
		if (RefRes.Ok()) return NewCommit;
		if (RefRes.Is(ErrorCode::Conflict)) return MakeError(ErrorCode::Conflict, "head moved (" + RefRes.Err().Message + ")");
		if (RefRes.Is(ErrorCode::Network)) return MakeError(ErrorCode::Ambiguous, "branch update outcome unknown: " + RefRes.Err().Message);
		return RefRes.Err();
	}

	Result<std::vector<CommitInfo>> GitHubRepository::Log(const std::string& FromCommit, int MaxCount)
	{
		HttpRequest Req;
		Req.Url = Api.RepoUrl("/commits?sha=" + FromCommit + "&per_page=" + std::to_string((std::min)(std::max(MaxCount, 1), 100)));
		HttpResponse R;
		SW_ASSIGN(R, Api.Call(Req));
		json::Value V;
		SW_ASSIGN(V, ParseBody(R));
		std::vector<CommitInfo> Out;
		for (const json::Value& C : V.AsArray())
		{
			CommitInfo I;
			SW_ASSIGN(I.Id, GetSha(C, {"sha"}));
			if (const json::Value* Commit = C.Find("commit"))
			{
				if (auto M = json::GetString(*Commit, "message", 16384); M.Ok()) I.Message = *M;
				if (const json::Value* Committer = Commit->Find("committer"))
				{
					if (auto D = json::GetString(*Committer, "date", 64); D.Ok())
					{
						if (auto T = ParseTime(*D); T.Ok()) I.Time = *T;
					}
				}
			}
			if (const json::Value* P = C.Find("parents"); P && P->IsArray() && !P->AsArray().empty())
			{
				if (auto Ps = GetSha(P->AsArray()[0], {"sha"}); Ps.Ok()) I.Parent = *Ps;
			}
			Out.push_back(std::move(I));
			if (static_cast<int>(Out.size()) >= MaxCount) break;
		}
		return Out;
	}

	// ------------------------------------------------------------ release objects

	GitHubReleaseObjectStore::GitHubReleaseObjectStore(std::shared_ptr<IHttpClient> Http, GitHubConfig Config) : Api(std::move(Http), std::move(Config)) {}

	std::string GitHubReleaseObjectStore::Describe() const
	{
		return "github.com/" + Api.Config().Owner + "/" + Api.Config().Repo + " releases";
	}

	Status GitHubReleaseObjectStore::Refresh()
	{
		SW_TRY(Api.EnsureRepositoryExists());
		std::map<std::string, Asset> NewAssets;
		std::vector<Release> NewReleases;
		for (int Page = 1; Page < 50; ++Page)
		{
			HttpRequest Req;
			Req.Url = Api.RepoUrl("/releases?per_page=100&page=" + std::to_string(Page));
			HttpResponse R;
			SW_ASSIGN(R, Api.Call(Req));
			json::Value V;
			SW_ASSIGN(V, ParseBody(R));
			if (!V.IsArray()) return MakeError(ErrorCode::Invalid, "GitHub releases response invalid");
			for (const json::Value& Rel : V.AsArray())
			{
				auto Tag = json::GetString(Rel, "tag_name", 200);
				if (!Tag.Ok() || Tag->compare(0, TagPrefix().size(), TagPrefix()) != 0) continue;
				Release Out;
				SW_ASSIGN(Out.Id, json::GetInt(Rel, "id"));
				std::string Upload;
				SW_ASSIGN(Upload, json::GetString(Rel, "upload_url", 500));
				Out.UploadUrl = Upload.substr(0, Upload.find('{'));
				for (int AP = 1; AP < 20; ++AP)
				{
					HttpRequest AReq;
					AReq.Url = Api.RepoUrl("/releases/" + std::to_string(Out.Id) + "/assets?per_page=100&page=" + std::to_string(AP));
					HttpResponse AR;
					SW_ASSIGN(AR, Api.Call(AReq));
					json::Value AV;
					SW_ASSIGN(AV, ParseBody(AR));
					for (const json::Value& A : AV.AsArray())
					{
						auto Name = json::GetString(A, "name", 200);
						if (!Name.Ok() || Name->size() != 68 || Name->compare(64, 4, ".sav") != 0) continue;
						const std::string Sha = Name->substr(0, 64);
						if (!Sha256::IsValidHex(Sha)) continue;
						Asset As;
						SW_ASSIGN(As.Id, json::GetInt(A, "id"));
						SW_ASSIGN(As.Size, json::GetInt(A, "size"));
						As.ReleaseId = Out.Id;
						auto State = json::GetString(A, "state", 32);
						As.bComplete = State.Ok() && *State == "uploaded";
						NewAssets[Sha] = As;
						++Out.AssetCount;
					}
					if (AV.AsArray().size() < 100) break;
				}
				NewReleases.push_back(Out);
			}
			if (V.AsArray().size() < 100) break;
		}
		Assets = std::move(NewAssets);
		Releases = std::move(NewReleases);
		bLoaded = true;
		return {};
	}

	Result<bool> GitHubReleaseObjectStore::Has(const std::string& Sha)
	{
		if (!Sha256::IsValidHex(Sha)) return MakeError(ErrorCode::Invalid, "invalid object id");
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!bLoaded) SW_TRY(Refresh());
		auto It = Assets.find(Sha);
		if (It == Assets.end() || !It->second.bComplete)
		{
			SW_TRY(Refresh()); // another player may have uploaded it
			It = Assets.find(Sha);
		}
		return It != Assets.end() && It->second.bComplete && It->second.Size > 0;
	}

	Result<GitHubReleaseObjectStore::Release> GitHubReleaseObjectStore::ReleaseWithRoom()
	{
		for (const Release& R : Releases)
		{
			if (R.AssetCount < MaxAssetsPerRelease) return R;
		}
		const std::string Tag = Releases.empty() ? TagPrefix() : TagPrefix() + "-" + std::to_string(Releases.size() + 1);
		HttpRequest Req;
		Req.Method = "POST";
		Req.Url = Api.RepoUrl("/releases");
		json::Value B;
		B.Set("tag_name", Tag);
		B.Set("name", "Shared World saves (" + Api.Config().WorldId + ")");
		B.Set("body", "Content-addressed save objects managed by the Shared World mod. Do not edit.");
		B.Set("prerelease", true);
		Req.Body = json::Serialize(B);
		auto R = Api.Call(Req, {201});
		if (R.Is(ErrorCode::Conflict))
		{
			SW_TRY(Refresh()); // someone else created it concurrently
			for (const Release& Rel : Releases)
			{
				if (Rel.AssetCount < MaxAssetsPerRelease) return Rel;
			}
			return R.Err();
		}
		if (!R) return R.Err().Wrap("create release");
		json::Value V;
		SW_ASSIGN(V, ParseBody(*R));
		Release Out;
		SW_ASSIGN(Out.Id, json::GetInt(V, "id"));
		std::string Upload;
		SW_ASSIGN(Upload, json::GetString(V, "upload_url", 500));
		Out.UploadUrl = Upload.substr(0, Upload.find('{'));
		Releases.push_back(Out);
		return Out;
	}

	Status GitHubReleaseObjectStore::Put(const std::string& Sha, const std::string& LocalPath)
	{
		file::HashResult H;
		SW_ASSIGN(H, file::Hash(LocalPath));
		if (H.Sha256 != Sha) return MakeError(ErrorCode::Corrupt, "object content does not match its id");
		return PutBlob(Sha, LocalPath);
	}

	Status GitHubReleaseObjectStore::PutBlob(const std::string& Sha, const std::string& LocalPath)
	{
		bool bHas = false;
		SW_ASSIGN(bHas, Has(Sha));
		if (bHas) return {};
		file::HashResult H;
		SW_ASSIGN(H, file::Hash(LocalPath));
		std::lock_guard<std::mutex> Lock(Mutex);
		if (auto It = Assets.find(Sha); It != Assets.end() && !It->second.bComplete)
		{
			// A previous upload died mid-way: remove the incomplete asset.
			HttpRequest Del;
			Del.Method = "DELETE";
			Del.Url = Api.RepoUrl("/releases/assets/" + std::to_string(It->second.Id));
			SW_TRY(Api.Call(Del, {204}));
			Assets.erase(It);
		}
		Release Rel;
		SW_ASSIGN(Rel, ReleaseWithRoom());
		HttpRequest Up;
		Up.Method = "POST";
		Up.Url = Rel.UploadUrl + "?name=" + Sha + ".sav";
		Up.Headers.push_back({"Content-Type", "application/octet-stream"});
		Up.BodyFile = LocalPath;
		Up.TimeoutSeconds = 1800;
		auto R = Api.Call(Up, {201});
		if (R.Is(ErrorCode::Conflict))
		{
			// "already_exists": uploaded concurrently by someone else.
			SW_TRY(Refresh());
			auto It = Assets.find(Sha);
			if (It != Assets.end() && It->second.bComplete && It->second.Size == H.Size) return {};
			return R.Err();
		}
		if (!R) return R.Err().Wrap("upload object");
		json::Value V;
		SW_ASSIGN(V, ParseBody(*R));
		Asset A;
		SW_ASSIGN(A.Id, json::GetInt(V, "id"));
		SW_ASSIGN(A.Size, json::GetInt(V, "size"));
		auto State = json::GetString(V, "state", 32);
		A.bComplete = State.Ok() && *State == "uploaded";
		A.ReleaseId = Rel.Id;
		if (!A.bComplete || A.Size != H.Size) return MakeError(ErrorCode::Corrupt, "GitHub stored an incomplete object");
		Assets[Sha] = A;
		for (Release& Existing : Releases)
		{
			if (Existing.Id == Rel.Id) ++Existing.AssetCount;
		}
		return {};
	}

	Status GitHubReleaseObjectStore::Get(const std::string& Sha, const std::string& DestPath)
	{
		Asset A;
		{
			bool bHas = false;
			SW_ASSIGN(bHas, Has(Sha));
			if (!bHas) return MakeError(ErrorCode::NotFound, "object " + Sha + " not found");
			std::lock_guard<std::mutex> Lock(Mutex);
			A = Assets[Sha];
		}
		(void)file::Remove(DestPath);
		HttpRequest Req;
		Req.Url = Api.RepoUrl("/releases/assets/" + std::to_string(A.Id));
		Req.Headers.push_back({"Accept", "application/octet-stream"});
		Req.ResponseFile = DestPath;
		Req.TimeoutSeconds = 1800;
		HttpResponse R;
		SW_ASSIGN(R, Api.Call(Req, {200, 302}));
		if (R.Status == 302)
		{
			const std::string Location = R.Header("Location");
			if (Location.compare(0, 8, "https://") != 0) return MakeError(ErrorCode::Invalid, "unexpected object redirect");
			// Follow WITHOUT credentials: the signed URL is the authorisation,
			// and the token must never reach another host.
			HttpRequest Follow;
			Follow.Url = Location;
			Follow.ResponseFile = DestPath;
			Follow.TimeoutSeconds = 1800;
			HttpResponse FR;
			SW_ASSIGN(FR, Api.Client().Send(Follow));
			if (FR.Status != 200)
			{
				(void)file::Remove(DestPath);
				return MakeError(FR.Status >= 500 ? ErrorCode::Network : ErrorCode::NotFound, "object download failed (" + std::to_string(FR.Status) + ")");
			}
		}
		return {};
	}

	Status GitHubReleaseObjectStore::Remove(const std::string& Sha)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!bLoaded) SW_TRY(Refresh());
		auto It = Assets.find(Sha);
		if (It == Assets.end()) return {};
		HttpRequest Del;
		Del.Method = "DELETE";
		Del.Url = Api.RepoUrl("/releases/assets/" + std::to_string(It->second.Id));
		SW_TRY(Api.Call(Del, {204}));
		Assets.erase(It);
		return {};
	}

	Result<std::vector<std::string>> GitHubReleaseObjectStore::List()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		SW_TRY(Refresh());
		std::vector<std::string> Out;
		for (const auto& [Sha, A] : Assets)
		{
			if (A.bComplete) Out.push_back(Sha);
		}
		return Out;
	}

	Result<bool> InviteGitHubCollaborator(std::shared_ptr<IHttpClient> Http, const GitHubConfig& Config, const std::string& Login)
	{
		if (!ValidName(Login) || Login.find('.') != std::string::npos || Login.size() > 39) return MakeError(ErrorCode::Invalid, "invalid GitHub username");
		GitHubApi Api(std::move(Http), Config);
		HttpRequest Req;
		Req.Method = "PUT";
		Req.Url = Api.RepoUrl("/collaborators/" + Login);
		json::Value Body;
		Body.Set("permission", "push");
		Req.Body = json::Serialize(Body);
		HttpResponse R;
		SW_ASSIGN(R, Api.Call(std::move(Req), {201, 204}));
		return R.Status == 201;
	}
}
