#pragma once
// A faithful in-process fake of the GitHub REST endpoints the provider uses.
// It enforces the documented semantics that the design depends on:
//  * PATCH git/refs with force=false only succeeds as a fast-forward, and is
//    evaluated atomically (one lock around every request);
//  * POST git/refs fails (422) if the ref exists;
//  * an empty repository rejects Git Data API writes (409) until the
//    Contents API creates the first commit;
//  * release asset downloads answer 302 to a storage host that REJECTS
//    requests carrying an Authorization header (like S3 signed URLs);
//  * assets can be left in state "starter" by an interrupted upload.

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "SharedWorldCore/Providers/Http.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace swtest
{
	class FakeGitHub final : public sw::IHttpClient
	{
	public:
		std::string Token = "gho_TESTTOKEN_should_never_be_logged";

		// Fault injection.
		std::atomic<bool> AllowNonFastForward{false}; // mutation testing: a broken server
		std::atomic<bool> LoseNextRefUpdateResponse{false};
		std::atomic<int> RateLimitNextRequests{0};
		std::atomic<bool> FailNextUploadMidway{false};
		// Observations.
		std::atomic<int> ContentCreatingRequests{0};
		std::atomic<int> Requests{0};
		std::atomic<bool> AuthLeakedToObjectHost{false};

		sw::Result<sw::HttpResponse> Send(const sw::HttpRequest& Req) override
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			++Requests;
			std::string Host, Path, Query;
			SplitUrl(Req.Url, Host, Path, Query);
			const std::string Auth = HeaderOf(Req, "Authorization");
			if (Host == "objects.fake")
			{
				if (!Auth.empty())
				{
					AuthLeakedToObjectHost = true;
					return Reply(400, R"({"message":"Only one auth mechanism allowed"})");
				}
				auto It = Signed.find(Path);
				if (It == Signed.end()) return Reply(403, "{}");
				return WriteBody(Req, 200, AssetData[It->second]);
			}
			if (Auth != "Bearer " + Token) return Reply(401, R"({"message":"Bad credentials"})");
			if (RateLimitNextRequests > 0)
			{
				--RateLimitNextRequests;
				sw::HttpResponse R = Reply(403, R"({"message":"API rate limit exceeded"})").Value();
				R.Headers.push_back({"x-ratelimit-remaining", "0"});
				R.Headers.push_back({"retry-after", "60"});
				return R;
			}
			if (Req.Method != "GET") ++ContentCreatingRequests;
			const std::string Prefix = "/repos/owner/repo";
			if (Host == "uploads.fake") return Upload(Req, Path, Query);
			if (Path.compare(0, Prefix.size(), Prefix) != 0) return Reply(404, R"({"message":"Not Found"})");
			const std::string P = Path.substr(Prefix.size());
			return Route(Req, P, Query);
		}

		std::string BranchHead(const std::string& Branch)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			auto It = Refs.find("refs/heads/" + Branch);
			return It == Refs.end() ? std::string() : It->second;
		}
		size_t AssetCount()
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			return AssetNames.size();
		}
		/** Force-moves a branch backwards (what "never force-push" forbids). */
		void ForceSetRef(const std::string& Branch, const std::string& Sha)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Refs["refs/heads/" + Branch] = Sha;
		}

	private:
		struct CommitObj
		{
			std::string Tree;
			std::vector<std::string> Parents;
			std::string Message;
		};
		struct AssetObj
		{
			int64_t Id = 0;
			int64_t ReleaseId = 0;
			std::string Name;
			std::string State;
		};
		struct ReleaseObj
		{
			int64_t Id = 0;
			std::string Tag;
		};

		std::mutex Mutex;
		std::map<std::string, std::string> Blobs;                              // sha -> content
		std::map<std::string, std::map<std::string, std::string>> Trees;       // sha -> flat path -> blob
		std::map<std::string, CommitObj> Commits;
		std::map<std::string, std::string> Refs;
		std::vector<ReleaseObj> Releases;
		std::map<int64_t, AssetObj> AssetsById;
		std::map<int64_t, std::string> AssetData;
		std::set<std::pair<int64_t, std::string>> AssetNames; // (release, name)
		std::map<std::string, int64_t> Signed;                // object path -> asset id
		int64_t NextId = 100;

		static std::string Id40(const std::string& S) { return sw::Sha256::HexOf(S).substr(0, 40); }

		static std::string HeaderOf(const sw::HttpRequest& R, const std::string& Name)
		{
			for (const auto& [K, V] : R.Headers)
			{
				if (K == Name) return V;
			}
			return std::string();
		}

		static void SplitUrl(const std::string& Url, std::string& Host, std::string& Path, std::string& Query)
		{
			const size_t S = Url.find("://");
			const size_t HostStart = S == std::string::npos ? 0 : S + 3;
			const size_t PathStart = Url.find('/', HostStart);
			Host = Url.substr(HostStart, PathStart - HostStart);
			const std::string Rest = PathStart == std::string::npos ? "/" : Url.substr(PathStart);
			const size_t Q = Rest.find('?');
			Path = Rest.substr(0, Q);
			Query = Q == std::string::npos ? "" : Rest.substr(Q + 1);
		}

		static std::string QueryParam(const std::string& Query, const std::string& Key)
		{
			size_t Pos = 0;
			while (Pos <= Query.size())
			{
				const size_t Amp = Query.find('&', Pos);
				const std::string Pair = Query.substr(Pos, Amp == std::string::npos ? std::string::npos : Amp - Pos);
				if (Pair.compare(0, Key.size() + 1, Key + "=") == 0) return Pair.substr(Key.size() + 1);
				if (Amp == std::string::npos) break;
				Pos = Amp + 1;
			}
			return std::string();
		}

		static sw::Result<sw::HttpResponse> Reply(int Status, std::string Body)
		{
			sw::HttpResponse R;
			R.Status = Status;
			R.Body = std::move(Body);
			return R;
		}

		static sw::Result<sw::HttpResponse> Json(int Status, const sw::json::Value& V) { return Reply(Status, sw::json::Serialize(V)); }

		sw::Result<sw::HttpResponse> WriteBody(const sw::HttpRequest& Req, int Status, const std::string& Data)
		{
			if (!Req.ResponseFile.empty())
			{
				(void)sw::file::Remove(Req.ResponseFile);
				(void)sw::file::CreateExclusive(Req.ResponseFile, Data);
				return Reply(Status, "");
			}
			return Reply(Status, Data);
		}

		bool Empty() const { return Commits.empty(); }

		std::string StoreTree(std::map<std::string, std::string> Flat)
		{
			std::string Key;
			for (const auto& [P, B] : Flat) Key += P + "=" + B + "\n";
			const std::string Sha = Id40("tree\n" + Key);
			Trees[Sha] = std::move(Flat);
			return Sha;
		}

		bool IsAncestor(const std::string& Ancestor, const std::string& Of)
		{
			std::vector<std::string> Stack{Of};
			std::set<std::string> Seen;
			while (!Stack.empty())
			{
				const std::string C = Stack.back();
				Stack.pop_back();
				if (C == Ancestor) return true;
				if (!Seen.insert(C).second) continue;
				auto It = Commits.find(C);
				if (It != Commits.end()) for (const auto& P : It->second.Parents) Stack.push_back(P);
			}
			return false;
		}

		sw::Result<sw::HttpResponse> Route(const sw::HttpRequest& Req, const std::string& P, const std::string& Query)
		{
			using sw::json::Value;
			const std::string& M = Req.Method;
			auto Body = Req.Body.empty() ? sw::Result<Value>(Value()) : sw::json::Parse(Req.Body);
			if (!Body) return Reply(400, R"({"message":"Problems parsing JSON"})");
			const Value& B = *Body;

			if (M == "GET" && P.compare(0, 14, "/git/ref/heads") == 0)
			{
				if (Empty()) return Reply(409, R"({"message":"Git Repository is empty."})");
				auto It = Refs.find("refs/heads" + P.substr(14));
				if (It == Refs.end()) return Reply(404, R"({"message":"Not Found"})");
				Value V;
				Value O;
				O.Set("sha", It->second);
				O.Set("type", "commit");
				V.Set("ref", It->first);
				V.Set("object", O);
				return Json(200, V);
			}
			if (M == "GET" && P.compare(0, 13, "/git/commits/") == 0)
			{
				auto It = Commits.find(P.substr(13));
				if (It == Commits.end()) return Reply(404, R"({"message":"Not Found"})");
				Value V;
				V.Set("sha", It->first);
				Value T;
				T.Set("sha", It->second.Tree);
				V.Set("tree", T);
				V.Set("message", It->second.Message);
				return Json(200, V);
			}
			if (M == "GET" && P.compare(0, 11, "/git/trees/") == 0)
			{
				auto It = Trees.find(P.substr(11));
				if (It == Trees.end()) return Reply(404, R"({"message":"Not Found"})");
				// Immediate children; subtrees become tree objects on demand.
				std::map<std::string, std::map<std::string, std::string>> Sub;
				sw::json::Array Entries;
				for (const auto& [Path, Blob] : It->second)
				{
					const size_t Slash = Path.find('/');
					if (Slash == std::string::npos)
					{
						Value E;
						E.Set("path", Path);
						E.Set("type", "blob");
						E.Set("sha", Blob);
						Entries.push_back(E);
					}
					else
					{
						Sub[Path.substr(0, Slash)][Path.substr(Slash + 1)] = Blob;
					}
				}
				for (auto& [Dir, Flat] : Sub)
				{
					Value E;
					E.Set("path", Dir);
					E.Set("type", "tree");
					E.Set("sha", StoreTree(Flat));
					Entries.push_back(E);
				}
				Value V;
				V.Set("tree", Value(std::move(Entries)));
				V.Set("truncated", false);
				return Json(200, V);
			}
			if (M == "GET" && P.compare(0, 10, "/contents/") == 0)
			{
				const std::string Ref = QueryParam(Query, "ref");
				auto C = Commits.find(Ref);
				if (C == Commits.end()) return Reply(404, R"({"message":"No commit found for the ref"})");
				const auto& Flat = Trees[C->second.Tree];
				auto F = Flat.find(P.substr(10));
				if (F == Flat.end()) return Reply(404, R"({"message":"Not Found"})");
				return Reply(200, Blobs[F->second]);
			}
			if (M == "PUT" && P.compare(0, 10, "/contents/") == 0)
			{
				if (!Empty()) return Reply(422, R"({"message":"Invalid request. \"sha\" wasn't supplied."})");
				std::map<std::string, std::string> Flat;
				const std::string Content = "init";
				const std::string BlobSha = Id40("blob" + Content);
				Blobs[BlobSha] = Content;
				Flat[P.substr(10)] = BlobSha;
				const std::string Tree = StoreTree(Flat);
				const std::string Commit = Id40("commit-init");
				Commits[Commit] = CommitObj{Tree, {}, "init"};
				Refs["refs/heads/main"] = Commit;
				return Reply(201, "{}");
			}
			if (M == "POST" && P == "/git/trees")
			{
				if (Empty()) return Reply(409, R"({"message":"Git Repository is empty."})");
				std::map<std::string, std::string> Flat;
				if (const Value* Base = B.Find("base_tree"))
				{
					auto It = Trees.find(Base->AsString());
					if (It == Trees.end()) return Reply(422, R"({"message":"base_tree not found"})");
					Flat = It->second;
				}
				for (const Value& E : B.Find("tree")->AsArray())
				{
					const std::string Path = E.Find("path")->AsString();
					if (const Value* Content = E.Find("content"))
					{
						const std::string BlobSha = Id40("blob" + Content->AsString());
						Blobs[BlobSha] = Content->AsString();
						Flat[Path] = BlobSha;
					}
					else if (const Value* S = E.Find("sha"); S && S->IsNull())
					{
						if (!Flat.erase(Path)) return Reply(422, R"({"message":"file does not exist"})");
					}
				}
				Value V;
				V.Set("sha", StoreTree(Flat));
				return Json(201, V);
			}
			if (M == "POST" && P == "/git/commits")
			{
				if (Empty()) return Reply(409, R"({"message":"Git Repository is empty."})");
				CommitObj C;
				C.Tree = B.Find("tree")->AsString();
				if (!Trees.count(C.Tree)) return Reply(422, R"({"message":"Tree SHA does not exist"})");
				for (const Value& Pa : B.Find("parents")->AsArray())
				{
					if (!Commits.count(Pa.AsString())) return Reply(422, R"({"message":"Parent SHA does not exist"})");
					C.Parents.push_back(Pa.AsString());
				}
				C.Message = B.Find("message")->AsString();
				const std::string Sha = Id40("commit" + C.Tree + C.Message + std::to_string(NextId++));
				Commits[Sha] = C;
				Value V;
				V.Set("sha", Sha);
				return Json(201, V);
			}
			if (M == "POST" && P == "/git/refs")
			{
				if (Empty()) return Reply(409, R"({"message":"Git Repository is empty."})");
				const std::string Ref = B.Find("ref")->AsString();
				if (Refs.count(Ref)) return Reply(422, R"({"message":"Reference already exists"})");
				Refs[Ref] = B.Find("sha")->AsString();
				return Reply(201, "{}");
			}
			if (M == "PATCH" && P.compare(0, 15, "/git/refs/heads") == 0)
			{
				const std::string Ref = "refs/heads" + P.substr(15);
				auto It = Refs.find(Ref);
				if (It == Refs.end()) return Reply(422, R"({"message":"Reference does not exist"})");
				const std::string New = B.Find("sha")->AsString();
				const bool bForce = B.Find("force") && B.Find("force")->AsBool();
				if (!bForce && !AllowNonFastForward && !IsAncestor(It->second, New))
				{
					return Reply(422, R"({"message":"Update is not a fast forward"})");
				}
				It->second = New;
				if (LoseNextRefUpdateResponse.exchange(false))
				{
					return sw::MakeError(sw::ErrorCode::Network, "connection reset");
				}
				return Reply(200, "{}");
			}
			if (M == "GET" && P == "/commits")
			{
				std::string C = QueryParam(Query, "sha");
				const int N = std::stoi(QueryParam(Query, "per_page"));
				sw::json::Array Out;
				while (!C.empty() && static_cast<int>(Out.size()) < N && Commits.count(C))
				{
					const CommitObj& O = Commits[C];
					Value V;
					V.Set("sha", C);
					Value Inner;
					Inner.Set("message", O.Message);
					Value Committer;
					Committer.Set("date", "2026-09-27T12:00:00Z");
					Inner.Set("committer", Committer);
					V.Set("commit", Inner);
					sw::json::Array Ps;
					for (const auto& Pa : O.Parents)
					{
						Value PV;
						PV.Set("sha", Pa);
						Ps.push_back(PV);
					}
					V.Set("parents", Value(std::move(Ps)));
					Out.push_back(V);
					C = O.Parents.empty() ? std::string() : O.Parents[0];
				}
				return Json(200, Value(std::move(Out)));
			}
			if (M == "POST" && P == "/releases")
			{
				const std::string Tag = B.Find("tag_name")->AsString();
				for (const ReleaseObj& R : Releases)
				{
					if (R.Tag == Tag) return Reply(422, R"({"message":"Validation Failed: already_exists"})");
				}
				ReleaseObj R{NextId++, Tag};
				Releases.push_back(R);
				return Json(201, ReleaseJson(R));
			}
			if (M == "GET" && P == "/releases")
			{
				const int Page = std::stoi(QueryParam(Query, "page"));
				const int Per = std::stoi(QueryParam(Query, "per_page"));
				sw::json::Array Out;
				for (size_t i = static_cast<size_t>((Page - 1) * Per); i < Releases.size() && Out.size() < static_cast<size_t>(Per); ++i)
				{
					Out.push_back(ReleaseJson(Releases[i]));
				}
				return Json(200, Value(std::move(Out)));
			}
			if (M == "GET" && P.compare(0, 10, "/releases/") == 0 && P.find("/assets") != std::string::npos && P.compare(0, 17, "/releases/assets/") != 0)
			{
				const int64_t Rid = std::stoll(P.substr(10));
				const int Page = std::stoi(QueryParam(Query, "page"));
				const int Per = std::stoi(QueryParam(Query, "per_page"));
				std::vector<const AssetObj*> Mine;
				for (const auto& [Id, A] : AssetsById)
				{
					if (A.ReleaseId == Rid) Mine.push_back(&A);
				}
				sw::json::Array Out;
				for (size_t i = static_cast<size_t>((Page - 1) * Per); i < Mine.size() && Out.size() < static_cast<size_t>(Per); ++i)
				{
					Out.push_back(AssetJson(*Mine[i]));
				}
				return Json(200, Value(std::move(Out)));
			}
			if (P.compare(0, 17, "/releases/assets/") == 0)
			{
				const int64_t Aid = std::stoll(P.substr(17));
				auto It = AssetsById.find(Aid);
				if (It == AssetsById.end()) return Reply(404, R"({"message":"Not Found"})");
				if (M == "DELETE")
				{
					AssetNames.erase({It->second.ReleaseId, It->second.Name});
					AssetData.erase(Aid);
					AssetsById.erase(It);
					return Reply(204, "");
				}
				if (M == "GET" && HeaderOf(Req, "Accept") == "application/octet-stream")
				{
					const std::string ObjPath = "/signed/" + std::to_string(Aid) + "-" + std::to_string(NextId++);
					Signed[ObjPath] = Aid;
					sw::HttpResponse R;
					R.Status = 302;
					R.Headers.push_back({"Location", "https://objects.fake" + ObjPath});
					return R;
				}
				return Json(200, AssetJson(It->second));
			}
			return Reply(404, R"({"message":"Not Found - fake"})");
		}

		sw::Result<sw::HttpResponse> Upload(const sw::HttpRequest& Req, const std::string& Path, const std::string& Query)
		{
			// /repos/owner/repo/releases/<id>/assets?name=...
			const size_t R0 = Path.find("/releases/");
			const int64_t Rid = std::stoll(Path.substr(R0 + 10));
			const std::string Name = QueryParam(Query, "name");
			if (AssetNames.count({Rid, Name})) return Reply(422, R"({"message":"Validation Failed: already_exists"})");
			std::string Data;
			if (!Req.BodyFile.empty())
			{
				auto D = sw::file::ReadAll(Req.BodyFile, int64_t(1) << 31);
				if (!D) return D.Err();
				Data = *D;
			}
			else
			{
				Data = Req.Body;
			}
			AssetObj A{NextId++, Rid, Name, "uploaded"};
			if (FailNextUploadMidway.exchange(false))
			{
				A.State = "starter"; // what GitHub leaves behind after an aborted upload
				AssetsById[A.Id] = A;
				AssetNames.insert({Rid, Name});
				AssetData[A.Id] = Data.substr(0, Data.size() / 2);
				return sw::MakeError(sw::ErrorCode::Network, "connection dropped during upload");
			}
			AssetsById[A.Id] = A;
			AssetNames.insert({Rid, Name});
			AssetData[A.Id] = Data;
			return Json(201, AssetJson(A));
		}

		sw::json::Value ReleaseJson(const ReleaseObj& R) const
		{
			sw::json::Value V;
			V.Set("id", R.Id);
			V.Set("tag_name", R.Tag);
			V.Set("upload_url", "https://uploads.fake/repos/owner/repo/releases/" + std::to_string(R.Id) + "/assets{?name,label}");
			return V;
		}

		sw::json::Value AssetJson(const AssetObj& A) const
		{
			sw::json::Value V;
			V.Set("id", A.Id);
			V.Set("name", A.Name);
			auto D = AssetData.find(A.Id);
			V.Set("size", static_cast<int64_t>(D == AssetData.end() ? 0 : D->second.size()));
			V.Set("state", A.State);
			return V;
		}
	};
}
