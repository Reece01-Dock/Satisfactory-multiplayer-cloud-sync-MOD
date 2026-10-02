#pragma once
// IWorldRepository on plain file storage (any rclone provider), built as an append-only log.
//
// Nothing is ever overwritten. Commit N+1 is a new immutable file log/<N+1> that names its parent and carries the
// whole (small) tree. Who owns entry N+1 decides the race, so this provides the same compare-and-swap that the
// lease, fencing and handoff rules (LeaseManager / WorldStore) are built on, without GitHub:
//
//   * Exclusive mode (store can "create only if absent": Dropbox, OneDrive, Box, S3/R2, GCS, Azure, WebDAV, SFTP,
//     local/SMB): the second create of log/<N+1> fails, so exactly one writer wins. Exact, like a git ref update.
//   * Duplicate mode (store allows two files with the same name: Google Drive, MEGA, ...): both entries exist; every
//     reader applies the same rule (earliest server time, then id). A writer waits SettleMs before trusting its win
//     so a competitor's earlier entry has time to become visible. Safe while listings show new files within
//     SettleMs. Entries must name the winning parent, so a loser that briefly thought it won is ignored by everyone.

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	/** One stored log file. Duplicate-mode stores may return several entries with the same Name. */
	struct LogEntryInfo
	{
		std::string Name;     // "000000000084"
		std::string Id;       // store-unique id (file id, or Name when names are unique)
		TimeMs ServerTime = 0; // time assigned by the storage service, not by any player's clock
	};

	/** Minimal file storage a LogRepository needs. Implemented over rclone in the game, in memory for tests. */
	class ILogStore
	{
	public:
		virtual ~ILogStore() = default;
		/** True if Create fails with AlreadyExists when the name is taken (atomic, server-side). */
		virtual bool ExclusiveCreate() const = 0;
		/** Every entry in Dir (duplicates included). Missing Dir = empty. */
		virtual Result<std::vector<LogEntryInfo>> List(const std::string& Dir) = 0;
		virtual Result<std::string> Read(const std::string& Dir, const LogEntryInfo& Entry) = 0;
		/** Writes a new file. Exclusive stores: AlreadyExists if Name exists. Network failure after sending: Ambiguous. */
		virtual Result<LogEntryInfo> Create(const std::string& Dir, const std::string& Name, const std::string& Data) = 0;
		virtual Status Delete(const std::string& Dir, const LogEntryInfo& Entry) = 0;
		virtual std::string Describe() const = 0;
	};

	struct LogRepositoryConfig
	{
		/** Duplicate mode only: wait before trusting a commit (must exceed the store's listing delay). */
		TimeMs SettleMs = Seconds(8);
		/** Entries older than this many commits behind head are deleted by committers. */
		int KeepEntries = 64;
		/** Injectable for tests. Default: real sleep. */
		std::function<void(TimeMs)> Sleep;
		/** Random writer tag source. Default: SystemRandom. */
		std::function<std::string()> NewWriterTag;
	};

	class LogRepository final : public IWorldRepository
	{
	public:
		LogRepository(std::shared_ptr<ILogStore> Store, LogRepositoryConfig Config = {});

		Result<std::string> Head() override;
		Result<std::string> ReadFile(const std::string& CommitId, const std::string& Path) override;
		Result<std::vector<std::string>> ListDirectory(const std::string& CommitId, const std::string& Dir) override;
		Result<std::string> Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message) override;
		Result<std::vector<CommitInfo>> Log(const std::string& FromCommit, int MaxCount) override;
		std::string Describe() const override;

		/** Parsed, immutable log entry (cached by commit id). */
		struct Entry
		{
			int64_t Seq = 0;
			std::string Id;      // "<seq>.<writer>" : unique per candidate
			std::string Parent;  // commit id of the entry this builds on ("" for the first)
			std::string Message;
			TimeMs Time = 0;
			std::map<std::string, std::string> Tree;
			LogEntryInfo Stored;
		};

	private:
		/** The winning chain's head entry, or NotFound if the log is empty. */
		Result<std::shared_ptr<const Entry>> ResolveHead();
		Result<std::shared_ptr<const Entry>> Load(const LogEntryInfo& Info);
		Result<std::shared_ptr<const Entry>> FindCommit(const std::string& CommitId);
		void Compact(int64_t HeadSeq);

		std::shared_ptr<ILogStore> Store;
		LogRepositoryConfig Cfg;
		std::mutex CacheMutex;
		std::map<std::string, std::shared_ptr<const Entry>> CacheByStoredId; // entries never change
	};

	/** In-memory ILogStore for tests: exclusive or duplicate mode, with optional listing delay. Thread-safe. */
	class MemoryLogStore final : public ILogStore
	{
	public:
		MemoryLogStore(bool bExclusive, TimeMs ListingDelayMs = 0) : bExclusiveMode(bExclusive), DelayMs(ListingDelayMs) {}
		bool ExclusiveCreate() const override { return bExclusiveMode; }
		Result<std::vector<LogEntryInfo>> List(const std::string& Dir) override;
		Result<std::string> Read(const std::string& Dir, const LogEntryInfo& Entry) override;
		Result<LogEntryInfo> Create(const std::string& Dir, const std::string& Name, const std::string& Data) override;
		Status Delete(const std::string& Dir, const LogEntryInfo& Entry) override;
		std::string Describe() const override { return bExclusiveMode ? "memory log (exclusive)" : "memory log (duplicates)"; }

	private:
		struct Stored { LogEntryInfo Info; std::string Data; };
		bool bExclusiveMode;
		TimeMs DelayMs;
		std::mutex Mutex;
		std::map<std::string, std::vector<Stored>> Dirs;
		int64_t NextId = 1;
	};
}
