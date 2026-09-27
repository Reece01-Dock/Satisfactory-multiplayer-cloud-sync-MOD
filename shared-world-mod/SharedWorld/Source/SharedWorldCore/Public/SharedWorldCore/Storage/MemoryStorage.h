#pragma once
// In-memory providers for tests, with fault injection.

#include <functional>
#include <map>
#include <mutex>

#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	class MemoryRepository final : public IWorldRepository
	{
	public:
		Result<std::string> Head() override;
		Result<std::string> ReadFile(const std::string& CommitId, const std::string& Path) override;
		Result<std::vector<std::string>> ListDirectory(const std::string& CommitId, const std::string& Dir) override;
		Result<std::string> Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message) override;
		Result<std::vector<CommitInfo>> Log(const std::string& FromCommit, int MaxCount) override;
		std::string Describe() const override { return "memory"; }

		/** Runs before each commit, outside the lock (latency / injected failures). */
		std::function<Status()> BeforeCommit;
		/** Next commit is applied but reported as Ambiguous (lost response). */
		bool LoseNextCommitResponse = false;
		/** Rewrites a file at head without CAS (tests: tampering / foreign writer). */
		void ForceWrite(const std::string& Path, const std::string& Content);
		size_t CommitCount() const;

	private:
		struct CommitData
		{
			CommitInfo Info;
			std::map<std::string, std::string> Files;
		};
		mutable std::mutex Mutex;
		std::map<std::string, CommitData> Commits;
		std::string HeadId;
		uint64_t Counter = 0;
		const CommitData* FindCommit(const std::string& Id) const;
	};

	class MemoryObjectStore final : public IObjectStore
	{
	public:
		Result<bool> Has(const std::string& Sha256) override;
		Status Put(const std::string& Sha256, const std::string& LocalPath) override;
		Status Get(const std::string& Sha256, const std::string& DestPath) override;
		Status Remove(const std::string& Sha256) override;
		Result<std::vector<std::string>> List() override;
		std::string Describe() const override { return "memory objects"; }

		/** >0: Put reads this many bytes then fails like a dropped connection. */
		int64_t FailPutAfterBytes = 0;
		/** May alter bytes returned by Get. */
		std::function<void(std::string& Bytes)> CorruptOnGet;
		/** Makes the store unreachable. */
		bool Offline = false;
		size_t PutCount() const;

	private:
		mutable std::mutex Mutex;
		std::map<std::string, std::string> Objects;
		size_t Puts = 0;
	};
}
