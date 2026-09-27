#include "SharedWorldCore/Storage/MemoryStorage.h"

#include "SharedWorldCore/Util/FileUtil.h"

namespace sw
{
	const MemoryRepository::CommitData* MemoryRepository::FindCommit(const std::string& Id) const
	{
		auto It = Commits.find(Id);
		return It == Commits.end() ? nullptr : &It->second;
	}

	Result<std::string> MemoryRepository::Head()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (HeadId.empty())
		{
			return MakeError(ErrorCode::NotFound, "repository is empty");
		}
		return HeadId;
	}

	Result<std::string> MemoryRepository::ReadFile(const std::string& CommitId, const std::string& Path)
	{
		SW_TRY(ValidateRepoPath(Path));
		std::lock_guard<std::mutex> Lock(Mutex);
		const CommitData* C = FindCommit(CommitId);
		if (!C)
		{
			return MakeError(ErrorCode::NotFound, "unknown commit " + CommitId);
		}
		auto It = C->Files.find(Path);
		if (It == C->Files.end())
		{
			return MakeError(ErrorCode::NotFound, Path + " not found");
		}
		return It->second;
	}

	Result<std::vector<std::string>> MemoryRepository::ListDirectory(const std::string& CommitId, const std::string& Dir)
	{
		SW_TRY(ValidateRepoPath(Dir));
		std::lock_guard<std::mutex> Lock(Mutex);
		const CommitData* C = FindCommit(CommitId);
		if (!C)
		{
			return MakeError(ErrorCode::NotFound, "unknown commit " + CommitId);
		}
		std::vector<std::string> Out;
		const std::string Prefix = Dir + "/";
		for (const auto& [P, _] : C->Files)
		{
			if (P.compare(0, Prefix.size(), Prefix) == 0 && P.find('/', Prefix.size()) == std::string::npos)
			{
				Out.push_back(P.substr(Prefix.size()));
			}
		}
		return Out;
	}

	Result<std::string> MemoryRepository::Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message)
	{
		for (const FileChange& C : Changes)
		{
			SW_TRY(ValidateRepoPath(C.Path));
		}
		if (BeforeCommit)
		{
			SW_TRY(BeforeCommit());
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		if (ExpectedHead != HeadId)
		{
			return MakeError(ErrorCode::Conflict, "head moved");
		}
		CommitData New;
		if (const CommitData* Parent = FindCommit(HeadId))
		{
			New.Files = Parent->Files;
		}
		for (const FileChange& C : Changes)
		{
			if (C.Content)
			{
				New.Files[C.Path] = *C.Content;
			}
			else
			{
				New.Files.erase(C.Path);
			}
		}
		New.Info.Id = "c" + std::to_string(++Counter);
		New.Info.Parent = HeadId;
		New.Info.Message = Message;
		HeadId = New.Info.Id;
		Commits.emplace(HeadId, std::move(New));
		if (LoseNextCommitResponse)
		{
			LoseNextCommitResponse = false;
			return MakeError(ErrorCode::Ambiguous, "connection reset after commit was sent");
		}
		return HeadId;
	}

	Result<std::vector<CommitInfo>> MemoryRepository::Log(const std::string& FromCommit, int MaxCount)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<CommitInfo> Out;
		std::string Id = FromCommit;
		while (!Id.empty() && static_cast<int>(Out.size()) < MaxCount)
		{
			const CommitData* C = FindCommit(Id);
			if (!C) break;
			Out.push_back(C->Info);
			Id = C->Info.Parent;
		}
		return Out;
	}

	void MemoryRepository::ForceWrite(const std::string& Path, const std::string& Content)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		CommitData New;
		if (const CommitData* Parent = FindCommit(HeadId)) New.Files = Parent->Files;
		New.Files[Path] = Content;
		New.Info.Id = "c" + std::to_string(++Counter);
		New.Info.Parent = HeadId;
		New.Info.Message = "forced";
		HeadId = New.Info.Id;
		Commits.emplace(HeadId, std::move(New));
	}

	size_t MemoryRepository::CommitCount() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Commits.size();
	}

	// ------------------------------------------------------------ objects

	Result<bool> MemoryObjectStore::Has(const std::string& Sha256)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Offline) return MakeError(ErrorCode::Network, "object store unreachable");
		return Objects.count(Sha256) > 0;
	}

	Status MemoryObjectStore::Put(const std::string& Sha256, const std::string& LocalPath)
	{
		std::string Bytes;
		SW_ASSIGN(Bytes, file::ReadAll(LocalPath, int64_t(4) << 30));
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Offline) return MakeError(ErrorCode::Network, "object store unreachable");
		++Puts;
		if (FailPutAfterBytes > 0)
		{
			return MakeError(ErrorCode::Network, "connection dropped after " + std::to_string(FailPutAfterBytes) + " bytes");
		}
		if (Objects.count(Sha256))
		{
			return {};
		}
		Objects[Sha256] = std::move(Bytes);
		return {};
	}

	Status MemoryObjectStore::Get(const std::string& Sha256, const std::string& DestPath)
	{
		std::string Bytes;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (Offline) return MakeError(ErrorCode::Network, "object store unreachable");
			auto It = Objects.find(Sha256);
			if (It == Objects.end()) return MakeError(ErrorCode::NotFound, "object " + Sha256 + " not found");
			Bytes = It->second;
		}
		if (CorruptOnGet)
		{
			CorruptOnGet(Bytes);
		}
		(void)file::Remove(DestPath);
		return file::CreateExclusive(DestPath, Bytes);
	}

	Status MemoryObjectStore::Remove(const std::string& Sha256)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Objects.erase(Sha256);
		return {};
	}

	Result<std::vector<std::string>> MemoryObjectStore::List()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<std::string> Out;
		for (const auto& [K, _] : Objects) Out.push_back(K);
		return Out;
	}

	size_t MemoryObjectStore::PutCount() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Puts;
	}
}
