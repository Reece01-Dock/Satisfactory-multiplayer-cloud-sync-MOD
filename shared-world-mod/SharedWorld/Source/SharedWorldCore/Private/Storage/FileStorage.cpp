#include "SharedWorldCore/Storage/FileStorage.h"

#include <chrono>
#include <map>
#include <thread>

#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	struct FileRepository::LoadedCommit
	{
		CommitInfo Info;
		std::map<std::string, std::string> Files; // path -> blob sha
	};

	FileRepository::FileRepository(std::string InRoot)
		: Root(std::move(InRoot)), RepoDir(file::Join(Root, "repo"))
	{
		(void)file::CreateDirectories(file::Join(RepoDir, "commits"));
		(void)file::CreateDirectories(file::Join(RepoDir, "blobs"));
	}

	static bool IsCommitId(const std::string& Id) { return Sha256::IsValidHex(Id); }

	Result<std::string> FileRepository::Head()
	{
		auto H = file::ReadAll(file::Join(RepoDir, "HEAD"), 128);
		if (!H)
		{
			if (H.Is(ErrorCode::NotFound)) return MakeError(ErrorCode::NotFound, "repository is empty");
			return H.Err();
		}
		std::string Id = H.Value();
		while (!Id.empty() && (Id.back() == '\n' || Id.back() == '\r')) Id.pop_back();
		if (!IsCommitId(Id)) return MakeError(ErrorCode::Corrupt, "HEAD is corrupt");
		return Id;
	}

	Result<FileRepository::LoadedCommit> FileRepository::LoadCommit(const std::string& Id)
	{
		if (!IsCommitId(Id)) return MakeError(ErrorCode::NotFound, "unknown commit");
		std::string Text;
		SW_ASSIGN(Text, file::ReadAll(file::Join(file::Join(RepoDir, "commits"), Id + ".json"), 16 << 20));
		// Commits are content-addressed: detect corruption / tampering.
		if (Sha256::HexOf(Text) != Id) return MakeError(ErrorCode::Corrupt, "commit " + Id + " is corrupt");
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text));
		LoadedCommit C;
		C.Info.Id = Id;
		SW_ASSIGN(C.Info.Parent, json::GetString(V, "parent", 64));
		SW_ASSIGN(C.Info.Message, json::GetString(V, "message", 8192));
		SW_ASSIGN(C.Info.Time, json::GetInt(V, "time"));
		const json::Value* Files = V.Find("files");
		if (!Files || !Files->IsObject()) return MakeError(ErrorCode::Corrupt, "commit files missing");
		for (const auto& [P, B] : Files->AsObject())
		{
			if (!B.IsString() || !Sha256::IsValidHex(B.AsString())) return MakeError(ErrorCode::Corrupt, "commit entry invalid");
			C.Files[P] = B.AsString();
		}
		return C;
	}

	Result<std::string> FileRepository::ReadFile(const std::string& CommitId, const std::string& Path)
	{
		SW_TRY(ValidateRepoPath(Path));
		LoadedCommit C;
		SW_ASSIGN(C, LoadCommit(CommitId));
		auto It = C.Files.find(Path);
		if (It == C.Files.end()) return MakeError(ErrorCode::NotFound, Path + " not found");
		std::string Data;
		SW_ASSIGN(Data, file::ReadAll(file::Join(file::Join(RepoDir, "blobs"), It->second), 64 << 20));
		if (Sha256::HexOf(Data) != It->second) return MakeError(ErrorCode::Corrupt, "blob for " + Path + " is corrupt");
		return Data;
	}

	Result<std::vector<std::string>> FileRepository::ListDirectory(const std::string& CommitId, const std::string& Dir)
	{
		SW_TRY(ValidateRepoPath(Dir));
		LoadedCommit C;
		SW_ASSIGN(C, LoadCommit(CommitId));
		std::vector<std::string> Out;
		const std::string Prefix = Dir + "/";
		for (const auto& [P, _] : C.Files)
		{
			if (P.compare(0, Prefix.size(), Prefix) == 0 && P.find('/', Prefix.size()) == std::string::npos)
			{
				Out.push_back(P.substr(Prefix.size()));
			}
		}
		return Out;
	}

	Status FileRepository::Lock()
	{
		const std::string LockPath = file::Join(RepoDir, "HEAD.lock");
		auto Delay = std::chrono::milliseconds(2);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (true)
		{
			Status S = file::CreateExclusive(LockPath, "lock");
			if (S.Ok()) return {};
			if (!S.Is(ErrorCode::AlreadyExists) && !S.Is(ErrorCode::Contention)) return S;
			auto Age = file::AgeSeconds(LockPath);
			if (Age.Ok() && Age.Value() > StaleLockSeconds)
			{
				(void)file::Remove(LockPath); // abandoned by a crashed process
				continue;
			}
			if (std::chrono::steady_clock::now() > Deadline) return MakeError(ErrorCode::Contention, "repository is locked");
			std::this_thread::sleep_for(Delay);
			if (Delay < std::chrono::milliseconds(50)) Delay *= 2;
		}
	}

	void FileRepository::Unlock() { (void)file::Remove(file::Join(RepoDir, "HEAD.lock")); }

	Result<std::string> FileRepository::Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message)
	{
		for (const FileChange& C : Changes) SW_TRY(ValidateRepoPath(C.Path));
		std::lock_guard<std::mutex> ProcessLock(ProcessMutex);
		SW_TRY(Lock());
		struct Unlocker { FileRepository* R; ~Unlocker() { R->Unlock(); } } Guard{this};

		auto Current = Head();
		const std::string CurrentId = Current.Ok() ? Current.Value() : std::string();
		if (!Current.Ok() && !Current.Is(ErrorCode::NotFound)) return Current.Err();
		if (CurrentId != ExpectedHead) return MakeError(ErrorCode::Conflict, "head moved");

		std::map<std::string, std::string> Files;
		if (!CurrentId.empty())
		{
			LoadedCommit Parent;
			SW_ASSIGN(Parent, LoadCommit(CurrentId));
			Files = std::move(Parent.Files);
		}
		for (const FileChange& C : Changes)
		{
			if (!C.Content)
			{
				Files.erase(C.Path);
				continue;
			}
			const std::string Blob = Sha256::HexOf(*C.Content);
			const std::string BlobPath = file::Join(file::Join(RepoDir, "blobs"), Blob);
			if (!file::Exists(BlobPath)) SW_TRY(file::WriteAtomic(BlobPath, *C.Content));
			Files[C.Path] = Blob;
		}
		json::Value V;
		V.Set("parent", CurrentId);
		V.Set("message", Message);
		V.Set("time", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
		V.Set("nonce", Rng.Hex(8)); // identical changes still get distinct ids
		json::Object FO;
		for (auto& [P, B] : Files) FO.emplace(P, json::Value(B));
		V.Set("files", json::Value(std::move(FO)));
		const std::string Text = json::Serialize(V);
		const std::string Id = Sha256::HexOf(Text);
		SW_TRY(file::WriteAtomic(file::Join(file::Join(RepoDir, "commits"), Id + ".json"), Text));
		SW_TRY(file::WriteAtomic(file::Join(RepoDir, "HEAD"), Id + "\n"));
		return Id;
	}

	Result<std::vector<CommitInfo>> FileRepository::Log(const std::string& FromCommit, int MaxCount)
	{
		std::vector<CommitInfo> Out;
		std::string Id = FromCommit;
		while (!Id.empty() && static_cast<int>(Out.size()) < MaxCount)
		{
			LoadedCommit C;
			SW_ASSIGN(C, LoadCommit(Id));
			Out.push_back(C.Info);
			Id = C.Info.Parent;
		}
		return Out;
	}

	// ------------------------------------------------------------ objects

	FileObjectStore::FileObjectStore(std::string InRoot) : Root(file::Join(InRoot, "objects/sha256"))
	{
		(void)file::CreateDirectories(Root);
	}

	Result<std::string> FileObjectStore::PathFor(const std::string& Sha) const
	{
		if (!Sha256::IsValidHex(Sha)) return MakeError(ErrorCode::Invalid, "invalid object id");
		return file::Join(file::Join(Root, Sha.substr(0, 2)), Sha);
	}

	Result<bool> FileObjectStore::Has(const std::string& Sha)
	{
		std::string P;
		SW_ASSIGN(P, PathFor(Sha));
		return file::Exists(P);
	}

	Status FileObjectStore::Put(const std::string& Sha, const std::string& LocalPath)
	{
		std::string Final;
		SW_ASSIGN(Final, PathFor(Sha));
		if (file::Exists(Final)) return {}; // content-addressed: same id, same bytes
		SW_TRY(file::CreateDirectories(file::Parent(Final)));
		const std::string Tmp = file::TempSibling(Final, "partial");
		SW_TRY(file::CopyExclusive(LocalPath, Tmp));
		auto H = file::Hash(Tmp);
		if (!H.Ok() || H->Sha256 != Sha)
		{
			(void)file::Remove(Tmp);
			return MakeError(ErrorCode::Corrupt, "object content does not match its id");
		}
		Status S = file::LinkExclusive(Tmp, Final); // atomic create-only publish
		(void)file::Remove(Tmp);
		if (!S.Ok() && !S.Is(ErrorCode::AlreadyExists)) return S;
		return {};
	}

	Status FileObjectStore::Get(const std::string& Sha, const std::string& DestPath)
	{
		std::string P;
		SW_ASSIGN(P, PathFor(Sha));
		if (!file::Exists(P)) return MakeError(ErrorCode::NotFound, "object " + Sha + " not found");
		(void)file::Remove(DestPath);
		return file::CopyExclusive(P, DestPath);
	}

	Status FileObjectStore::Remove(const std::string& Sha)
	{
		std::string P;
		SW_ASSIGN(P, PathFor(Sha));
		return file::Remove(P);
	}

	Result<std::vector<std::string>> FileObjectStore::List()
	{
		std::vector<std::string> Out;
		std::vector<std::string> Dirs;
		SW_ASSIGN(Dirs, file::ListNames(Root));
		for (const std::string& D : Dirs)
		{
			std::vector<std::string> Names;
			SW_ASSIGN(Names, file::ListNames(file::Join(Root, D)));
			for (const std::string& N : Names)
			{
				if (Sha256::IsValidHex(N)) Out.push_back(N);
			}
		}
		return Out;
	}
}
