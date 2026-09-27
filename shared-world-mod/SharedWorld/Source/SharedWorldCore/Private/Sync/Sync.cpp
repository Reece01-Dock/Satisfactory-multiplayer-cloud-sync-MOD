#include "SharedWorldCore/Sync/Sync.h"

#include <algorithm>
#include <cstdio>

#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	// ------------------------------------------------------------ LocalStateStore

	LocalWorldState LocalStateStore::Load(const std::string& WorldId) const
	{
		LocalWorldState S;
		if (!ValidateWorldId(WorldId)) return S;
		auto Text = file::ReadAll(file::Join(Dir, WorldId + ".json"), 1 << 20);
		if (!Text) return S;
		auto V = json::Parse(*Text);
		if (!V) return S;
		if (auto R = json::GetOptionalInt(*V, "syncedRevision"); R.Ok() && R->has_value()) S.SyncedRevision = **R;
		if (auto H = json::GetOptionalString(*V, "syncedSha256", 64); H.Ok() && H->has_value() && Sha256::IsValidHex(**H)) S.SyncedSha256 = **H;
		if (auto T = json::GetOptionalInt(*V, "syncedAt"); T.Ok() && T->has_value()) S.SyncedAt = **T;
		if (const json::Value* L = json::GetOptional(*V, "activeLease"))
		{
			LeaseToken Tok;
			auto G = json::GetInt(*L, "generation");
			auto N = json::GetString(*L, "nonce", 64);
			auto B = json::GetInt(*L, "baseRevision");
			const json::Value* H = L->Find("holder");
			if (G.Ok() && N.Ok() && B.Ok() && H)
			{
				if (auto Holder = Identity::FromJson(*H); Holder.Ok())
				{
					S.ActiveLease = LeaseToken{WorldId, *G, *N, *B, *Holder};
				}
			}
		}
		return S;
	}

	Status LocalStateStore::Save(const std::string& WorldId, const LocalWorldState& S) const
	{
		SW_TRY(ValidateWorldId(WorldId));
		SW_TRY(file::CreateDirectories(Dir));
		json::Value V;
		V.Set("syncedRevision", S.SyncedRevision);
		V.Set("syncedSha256", S.SyncedSha256);
		V.Set("syncedAt", S.SyncedAt);
		if (S.ActiveLease)
		{
			json::Value L;
			L.Set("generation", S.ActiveLease->Generation);
			L.Set("nonce", S.ActiveLease->Nonce);
			L.Set("baseRevision", S.ActiveLease->BaseRevision);
			L.Set("holder", S.ActiveLease->Holder.ToJson());
			V.Set("activeLease", std::move(L));
		}
		return file::WriteAtomic(file::Join(Dir, WorldId + ".json"), json::Serialize(V, 2));
	}

	// ------------------------------------------------------------ BackupManager

	Result<std::string> BackupManager::Preserve(const std::string& WorldId, const std::string& Src, const std::string& Label)
	{
		SW_TRY(ValidateWorldId(WorldId));
		SW_TRY(save::ValidateSaveName(Label));
		const std::string WorldDir = file::Join(Dir, WorldId);
		SW_TRY(file::CreateDirectories(WorldDir));
		// Sortable UTC timestamp, then label.
		std::string Stamp = FormatTime(Clock->Now());
		Stamp.erase(std::remove_if(Stamp.begin(), Stamp.end(), [](char C) { return C == '-' || C == ':'; }), Stamp.end());
		std::string Dst = file::Join(WorldDir, Stamp + "_" + Label + save::Extension);
		for (int i = 2; file::Exists(Dst); ++i)
		{
			Dst = file::Join(WorldDir, Stamp + "_" + Label + "-" + std::to_string(i) + save::Extension);
		}
		Status S = file::CopyExclusive(Src, Dst);
		if (!S) return S.Err().Wrap("backup " + Label);
		Prune(WorldId);
		return Dst;
	}

	static bool IsProtected(const std::string& Name)
	{
		return Name.find("_conflict-") != std::string::npos || Name.find("_recovery-") != std::string::npos;
	}

	Result<std::vector<LocalBackup>> BackupManager::List(const std::string& WorldId) const
	{
		SW_TRY(ValidateWorldId(WorldId));
		const std::string WorldDir = file::Join(Dir, WorldId);
		std::vector<std::string> Names;
		SW_ASSIGN(Names, file::ListNames(WorldDir));
		std::vector<LocalBackup> Out;
		for (const std::string& N : Names)
		{
			if (N.size() < 4 || N.compare(N.size() - 4, 4, save::Extension) != 0) continue;
			LocalBackup B;
			B.Name = N;
			B.Path = file::Join(WorldDir, N);
			B.Size = file::Size(B.Path).Ok() ? file::Size(B.Path).Value() : 0;
			B.bProtected = IsProtected(N);
			Out.push_back(std::move(B));
		}
		std::sort(Out.begin(), Out.end(), [](const LocalBackup& A, const LocalBackup& B) { return A.Name > B.Name; });
		return Out;
	}

	void BackupManager::Prune(const std::string& WorldId)
	{
		if (Keep <= 0) return;
		auto L = List(WorldId);
		if (!L) return;
		int Kept = 0;
		for (const LocalBackup& B : *L)
		{
			if (B.bProtected) continue; // progress that exists nowhere else
			if (++Kept > Keep) (void)file::Remove(B.Path);
		}
	}

	// ------------------------------------------------------------ ObjectCache

	std::string ObjectCache::PathFor(const std::string& Sha) const { return file::Join(file::Join(Dir, Sha.substr(0, 2)), Sha); }

	std::optional<std::string> ObjectCache::Find(const std::string& Sha)
	{
		if (!Sha256::IsValidHex(Sha)) return std::nullopt;
		const std::string P = PathFor(Sha);
		if (!file::Exists(P)) return std::nullopt;
		auto H = file::Hash(P);
		if (!H.Ok() || H->Sha256 != Sha)
		{
			(void)file::Remove(P); // damaged cache entry: never trust it
			return std::nullopt;
		}
		return P;
	}

	Status ObjectCache::Add(const std::string& Sha, const std::string& VerifiedFile)
	{
		if (!Sha256::IsValidHex(Sha)) return MakeError(ErrorCode::Invalid, "invalid object id");
		const std::string P = PathFor(Sha);
		if (file::Exists(P)) return {};
		SW_TRY(file::CreateDirectories(file::Parent(P)));
		const std::string Tmp = file::TempSibling(P, "partial");
		SW_TRY(file::CopyExclusive(VerifiedFile, Tmp));
		Status S = file::LinkExclusive(Tmp, P);
		(void)file::Remove(Tmp);
		return (S.Ok() || S.Is(ErrorCode::AlreadyExists)) ? Status() : S;
	}

	// ------------------------------------------------------------ SyncEngine

	const char* ToString(LocalStatus S)
	{
		switch (S)
		{
		case LocalStatus::Missing: return "MISSING";
		case LocalStatus::InSync: return "IN_SYNC";
		case LocalStatus::Modified: return "MODIFIED";
		case LocalStatus::Unknown: return "UNKNOWN";
		}
		return "?";
	}

	SyncEngine::SyncEngine(std::shared_ptr<IObjectStore> InObjects, std::shared_ptr<LeaseManager> InLeases, SyncConfig InConfig)
		: ObjectStore(std::move(InObjects)), Leases(std::move(InLeases)), Cfg(std::move(InConfig)),
		  States(file::Join(Cfg.DataDir, "state")),
		  BackupMgr(file::Join(Cfg.DataDir, "backups"), Cfg.KeepLocalBackups, std::shared_ptr<IClock>(Leases, &Leases->Store().Clock())),
		  CacheMgr(file::Join(Cfg.DataDir, "cache/objects"))
	{
	}

	std::string SyncEngine::WorldDir() const { return file::Join(file::Join(Cfg.DataDir, "worlds"), Leases->Store().WorldId()); }

	Status SyncEngine::MarkSynced(int64_t Revision, const std::string& Sha)
	{
		const std::string& Id = Leases->Store().WorldId();
		LocalWorldState S = States.Load(Id);
		S.SyncedRevision = Revision;
		S.SyncedSha256 = Sha;
		S.SyncedAt = Leases->Store().Clock().Now();
		return States.Save(Id, S);
	}

	Result<LocalInspection> SyncEngine::InspectLocal(const std::string& SavePath)
	{
		LocalInspection I;
		I.State = States.Load(Leases->Store().WorldId());
		auto H = file::Hash(SavePath);
		if (H.Is(ErrorCode::NotFound))
		{
			I.Status = LocalStatus::Missing;
			return I;
		}
		if (!H) return H.Err();
		I.Sha256 = H->Sha256;
		if (I.State.SyncedSha256.empty()) I.Status = LocalStatus::Unknown;
		else if (I.State.SyncedSha256 == I.Sha256) I.Status = LocalStatus::InSync;
		else I.Status = LocalStatus::Modified;
		return I;
	}

	Result<std::string> SyncEngine::FetchVerified(const RevisionMeta& Head, bool& bFromCache)
	{
		if (auto Cached = CacheMgr.Find(Head.ObjectSha256))
		{
			bFromCache = true;
			return *Cached;
		}
		bFromCache = false;
		const std::string Staging = file::Join(WorldDir(), "staging");
		SW_TRY(file::CreateDirectories(Staging));
		const std::string Tmp = file::TempSibling(file::Join(Staging, "download"), "partial");
		Status G = ObjectStore->Get(Head.ObjectSha256, Tmp);
		if (!G)
		{
			(void)file::Remove(Tmp);
			return G.Err().Wrap("download revision " + std::to_string(Head.Number));
		}
		auto H = file::Hash(Tmp);
		if (!H.Ok() || H->Sha256 != Head.ObjectSha256 || H->Size != Head.Size)
		{
			(void)file::Remove(Tmp);
			Leases->Store().Log().Error("RevisionVerificationFailed", {{"world", Leases->Store().WorldId()}, {"revision", std::to_string(Head.Number)},
				{"expected_sha256", Head.ObjectSha256}, {"got_sha256", H.Ok() ? H->Sha256 : "?"}, {"expected_size", std::to_string(Head.Size)}, {"got_size", H.Ok() ? std::to_string(H->Size) : "?"}});
			return MakeError(ErrorCode::Corrupt, "downloaded save failed verification (revision " + std::to_string(Head.Number) + ")");
		}
		if (auto V = save::ValidateFile(Tmp); !V)
		{
			(void)file::Remove(Tmp);
			return MakeError(ErrorCode::Corrupt, "downloaded save is not a valid Satisfactory save: " + V.Err().Message);
		}
		SW_TRY(CacheMgr.Add(Head.ObjectSha256, Tmp));
		(void)file::Remove(Tmp);
		auto Cached = CacheMgr.Find(Head.ObjectSha256);
		if (!Cached) return MakeError(ErrorCode::Io, "object cache write failed");
		return *Cached;
	}

	Result<DownloadResult> SyncEngine::Download(const WorldState& State, const std::string& Target)
	{
		if (!State.Head) return MakeError(ErrorCode::NotFound, "this Shared World has no save yet");
		const RevisionMeta& Head = *State.Head;
		const Logger& Log = Leases->Store().Log();
		DownloadResult Res;
		Res.Revision = Head.Number;
		if (auto Local = file::Hash(Target); Local.Ok() && Local->Sha256 == Head.ObjectSha256)
		{
			Res.bAlreadyLocal = true;
			SW_TRY(MarkSynced(Head.Number, Head.ObjectSha256));
			Log.Info("SaveInstalled", {{"world", State.WorldId}, {"revision", std::to_string(Head.Number)}, {"source", "already-local"}});
			return Res;
		}
		Log.Info("RevisionDownloadStarted", {{"world", State.WorldId}, {"revision", std::to_string(Head.Number)}, {"size", std::to_string(Head.Size)}});
		std::string Verified;
		SW_ASSIGN(Verified, FetchVerified(Head, Res.bFromCache));
		Log.Info("RevisionVerified", {{"world", State.WorldId}, {"revision", std::to_string(Head.Number)}, {"sha256", Head.ObjectSha256}, {"cache", Res.bFromCache ? "hit" : "miss"}});

		// Stage next to the target so the final rename is atomic.
		SW_TRY(file::CreateDirectories(file::Parent(Target)));
		const std::string Tmp = file::TempSibling(Target, "swtmp");
		SW_TRY(file::CopyExclusive(Verified, Tmp));
		auto Check = file::Hash(Tmp);
		if (!Check.Ok() || Check->Sha256 != Head.ObjectSha256)
		{
			(void)file::Remove(Tmp);
			return MakeError(ErrorCode::Corrupt, "local copy failed verification");
		}
		if (file::Exists(Target))
		{
			auto Bp = BackupMgr.Preserve(State.WorldId, Target, "pre-download-r" + std::to_string(Head.Number));
			if (!Bp)
			{
				(void)file::Remove(Tmp);
				return Bp.Err().Wrap("could not back up the existing local save; it was not replaced");
			}
			Res.BackupPath = *Bp;
		}
		SW_TRY(file::ReplaceWith(Tmp, Target));
		SW_TRY(MarkSynced(Head.Number, Head.ObjectSha256));
		Log.Info("SaveInstalled", {{"world", State.WorldId}, {"revision", std::to_string(Head.Number)}, {"path", Target}});
		return Res;
	}

	Result<UploadResult> SyncEngine::Upload(LeaseToken& Token, const std::string& Src, const UploadOptions& Options, ConflictInfo* Conflict)
	{
		WorldStore& Store = Leases->Store();
		const Logger& Log = Store.Log();
		SW_TRY(save::WaitStable(Src, Options.StableQuiet, Options.StableTimeout));
		if (auto V = save::ValidateFile(Src); !V)
		{
			return V.Err().Wrap("refusing to upload a file that is not a complete save");
		}
		// Private snapshot: the game may keep writing Src; we upload exactly what we hashed.
		const std::string Staging = file::Join(WorldDir(), "staging");
		SW_TRY(file::CreateDirectories(Staging));
		const std::string Snap = file::TempSibling(file::Join(Staging, "snapshot"), "sav");
		SW_TRY(file::CopyExclusive(Src, Snap));
		struct Cleanup { std::string P; ~Cleanup() { (void)file::Remove(P); } } SnapGuard{Snap};
		file::HashResult H;
		SW_ASSIGN(H, file::Hash(Snap));
		if (auto V = save::ValidateFile(Snap); !V)
		{
			return V.Err().Wrap("save changed while it was being snapshotted");
		}

		auto Preserve = [&](const Error& Cause, int64_t CloudHead) -> Error
		{
			auto Bp = BackupMgr.Preserve(Token.WorldId, Snap, "conflict-g" + std::to_string(Token.Generation) + "-r" + std::to_string(Token.BaseRevision));
			if (Conflict)
			{
				Conflict->BackupPath = Bp.Ok() ? *Bp : std::string();
				Conflict->CloudRevision = CloudHead;
				Conflict->LocalBaseRevision = Token.BaseRevision;
			}
			Log.Warn("UploadRefused", {{"world", Token.WorldId}, {"generation", std::to_string(Token.Generation)}, {"reason", Cause.Describe()},
				{"cloud_revision", std::to_string(CloudHead)}, {"backup", Bp.Ok() ? *Bp : "FAILED: " + Bp.Err().Message}});
			return Cause;
		};

		StateSnapshot Current;
		SW_ASSIGN(Current, Store.Load());
		if (Status F = CheckFence(Current.State, Token); !F)
		{
			return Preserve(F.Err(), Current.State.HeadNumber());
		}
		if (Current.State.HeadNumber() != Token.BaseRevision)
		{
			return Preserve(MakeError(ErrorCode::StaleRevision, "a newer shared save exists"), Current.State.HeadNumber());
		}
		UploadResult Res;
		if (Current.State.Head && Current.State.Head->ObjectSha256 == H.Sha256)
		{
			Res.Revision = *Current.State.Head;
			Res.bUnchanged = true;
			SW_TRY(MarkSynced(Res.Revision.Number, H.Sha256));
			Log.Info("UploadSkippedUnchanged", {{"world", Token.WorldId}, {"revision", std::to_string(Res.Revision.Number)}});
			return Res;
		}

		RevisionMeta Rev;
		Rev.Number = Token.BaseRevision + 1;
		Rev.Generation = Token.Generation;
		Rev.PreviousRevision = Token.BaseRevision;
		Rev.ObjectSha256 = H.Sha256;
		Rev.Size = H.Size;
		Rev.CreatedAt = Store.Clock().Now();
		Rev.Uploader = Token.Holder;
		Rev.Reason = Options.Reason;
		Rev.GameBuild = Options.GameBuild;
		Rev.ModVersion = Options.ModVersion;

		bool bHas = false;
		SW_ASSIGN(bHas, ObjectStore->Has(H.Sha256));
		Res.bDeduplicated = bHas;
		Log.Info("RevisionUploadStarted", {{"world", Token.WorldId}, {"revision", std::to_string(Rev.Number)}, {"sha256", H.Sha256}, {"size", std::to_string(H.Size)},
			{"dedup", bHas ? "true" : "false"}});
		if (!bHas)
		{
			if (Status P = ObjectStore->Put(H.Sha256, Snap); !P)
			{
				Log.Error("RevisionUploadFailed", {{"world", Token.WorldId}, {"error", P.Err().Describe()}});
				return P.Err().Wrap("upload save");
			}
		}
		if (Options.bVerifyByReadBack)
		{
			const std::string Back = file::TempSibling(file::Join(Staging, "verify"), "sav");
			Status G = ObjectStore->Get(H.Sha256, Back);
			auto BH = G.Ok() ? file::Hash(Back) : Result<file::HashResult>(G.Err());
			(void)file::Remove(Back);
			if (!BH.Ok() || BH->Sha256 != H.Sha256 || BH->Size != H.Size)
			{
				// The object id is its hash: a mismatching object is garbage and unreferenced.
				Log.Error("RevisionUploadVerificationFailed", {{"world", Token.WorldId}, {"sha256", H.Sha256}});
				return MakeError(ErrorCode::Corrupt, "uploaded save failed verification");
			}
		}
		(void)CacheMgr.Add(H.Sha256, Snap); // speeds up migration / restore

		auto Committed = Leases->CommitRevision(Token, Rev);
		if (!Committed)
		{
			if (Committed.Is(ErrorCode::Fenced) || Committed.Is(ErrorCode::StaleRevision))
			{
				return Preserve(Committed.Err(), Store.LastSeen ? Store.LastSeen->State.HeadNumber() : 0);
			}
			if (Committed.Is(ErrorCode::Ambiguous))
			{
				// The commit may have landed; the head tells.
				auto Now = Store.Load();
				if (Now.Ok() && Now->State.Head && Now->State.Head->Number == Rev.Number && Now->State.Head->ObjectSha256 == Rev.ObjectSha256 &&
					Now->State.Generation == Token.Generation)
				{
					Token.BaseRevision = Rev.Number;
					Log.Warn("CommitOutcomeRecovered", {{"world", Token.WorldId}, {"revision", std::to_string(Rev.Number)}});
				}
				else
				{
					return Committed.Err().Wrap("commit revision");
				}
			}
			else
			{
				return Committed.Err().Wrap("commit revision");
			}
		}
		SW_TRY(MarkSynced(Rev.Number, H.Sha256));
		Res.Revision = Rev;
		return Res;
	}

	Result<RevisionMeta> SyncEngine::Restore(LeaseToken& Token, const RevisionMeta& From, const Identity& By)
	{
		bool bHas = false;
		SW_ASSIGN(bHas, ObjectStore->Has(From.ObjectSha256));
		if (!bHas) return MakeError(ErrorCode::NotFound, "the save of revision " + std::to_string(From.Number) + " is no longer stored");
		RevisionMeta Rev;
		Rev.Number = Token.BaseRevision + 1;
		Rev.Generation = Token.Generation;
		Rev.PreviousRevision = Token.BaseRevision;
		Rev.ObjectSha256 = From.ObjectSha256;
		Rev.Size = From.Size;
		Rev.CreatedAt = Leases->Store().Clock().Now();
		Rev.Uploader = By;
		Rev.Reason = Reason::Restore;
		Rev.RestoredFrom = From.Number;
		Rev.GameBuild = From.GameBuild;
		Rev.ModVersion = From.ModVersion;
		auto C = Leases->CommitRevision(Token, Rev);
		if (!C) return C.Err();
		Leases->Store().Log().Info("RevisionRestored", {{"world", Token.WorldId}, {"revision", std::to_string(Rev.Number)}, {"restored_from", std::to_string(From.Number)}});
		return Rev;
	}

	Result<std::vector<RevisionMeta>> SyncEngine::History(const std::string& CommitId, size_t Max)
	{
		IWorldRepository& Repo = Leases->Store().Repository();
		std::vector<std::string> Shards;
		SW_ASSIGN(Shards, Repo.ListDirectory(CommitId, Paths::RevisionsDir));
		// Shard and file names are zero-padded: descending sort = newest first.
		std::sort(Shards.begin(), Shards.end(), std::greater<>());
		std::vector<RevisionMeta> Out;
		for (const std::string& Shard : Shards)
		{
			if (Out.size() >= Max) break;
			const std::string ShardDir = std::string(Paths::RevisionsDir) + "/" + Shard;
			std::vector<std::string> Names;
			SW_ASSIGN(Names, Repo.ListDirectory(CommitId, ShardDir));
			std::sort(Names.begin(), Names.end(), std::greater<>());
			for (const std::string& N : Names)
			{
				if (Out.size() >= Max) break;
				std::string Text;
				SW_ASSIGN(Text, Repo.ReadFile(CommitId, ShardDir + "/" + N));
				json::Value V;
				SW_ASSIGN(V, json::Parse(Text));
				RevisionMeta R;
				SW_ASSIGN(R, RevisionMeta::FromJson(V));
				if (R.Path() != ShardDir + "/" + N) return MakeError(ErrorCode::Invalid, "revision file name does not match its content: " + N);
				Out.push_back(std::move(R));
			}
		}
		return Out;
	}
}
