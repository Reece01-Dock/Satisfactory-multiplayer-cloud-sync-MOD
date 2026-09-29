#pragma once
// Storage provider interfaces.
//
// IWorldRepository is a versioned tree of small files with ONE atomic
// primitive: Commit(ExpectedHead, Changes) succeeds only if the head is still
// ExpectedHead. It is a Git branch (GitHub provider), a local mini-Git
// (filesystem provider) or an in-memory model (tests). All lease, fencing
// and revision rules are built on this single compare-and-swap.
//
// IObjectStore holds the .sav bytes, content-addressed by SHA-256. Objects
// are immutable; Put is idempotent and never exposes a partial object.

#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Util/Result.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	struct SaveObjectEncoding;
	struct CompressStats;

	struct FileChange
	{
		std::string Path;
		std::optional<std::string> Content; // nullopt deletes the file
	};

	struct CommitInfo
	{
		std::string Id;
		std::string Parent;
		std::string Message;
		TimeMs Time = 0;
	};

	/** Repository file paths: lowercase [a-z0-9._-] segments separated by '/', no "." or "..". */
	Status ValidateRepoPath(const std::string& Path);

	class IWorldRepository
	{
	public:
		virtual ~IWorldRepository() = default;

		/** Current head commit id, or NotFound when no Shared World exists there yet. */
		virtual Result<std::string> Head() = 0;
		/** File content at CommitId; NotFound if the file does not exist there. */
		virtual Result<std::string> ReadFile(const std::string& CommitId, const std::string& Path) = 0;
		/** Names of files AND subdirectories directly inside Dir at CommitId (empty if Dir is missing). */
		virtual Result<std::vector<std::string>> ListDirectory(const std::string& CommitId, const std::string& Dir) = 0;
		/**
		 * Atomically applies Changes as one commit on top of ExpectedHead
		 * (empty = create the first commit) and returns the new head.
		 * Conflict: the head moved (lost the race). Ambiguous: outcome
		 * unknown, re-read Head() to find out.
		 */
		virtual Result<std::string> Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message) = 0;
		/** Commits from FromCommit backwards (newest first). */
		virtual Result<std::vector<CommitInfo>> Log(const std::string& FromCommit, int MaxCount) = 0;
		/** Human-readable location for diagnostics (never contains credentials). */
		virtual std::string Describe() const = 0;
	};

	class IObjectStore
	{
	public:
		virtual ~IObjectStore() = default;

		virtual Result<bool> Has(const std::string& Sha256) = 0;
		/**
		 * Stores LocalPath under ObjectId when content hash(LocalPath)==ObjectId.
		 * Prefer PutBlob when storing opaque packages whose bytes differ from ObjectId.
		 */
		virtual Status Put(const std::string& Sha256, const std::string& LocalPath) = 0;
		/**
		 * Store opaque bytes under ObjectId without requiring hash(LocalPath)==ObjectId.
		 * Default implementation falls back to Put (hash-checked).
		 */
		virtual Status PutBlob(const std::string& ObjectId, const std::string& LocalPath)
		{
			return Put(ObjectId, LocalPath);
		}
		/** Writes the object's bytes to DestPath (overwritten). Callers verify the logical save hash. */
		virtual Status Get(const std::string& Sha256, const std::string& DestPath) = 0;
		virtual Status Remove(const std::string& Sha256) = 0;
		virtual Result<std::vector<std::string>> List() = 0;
		virtual std::string Describe() const = 0;
		/** When Put stores a packaged object, implementations may expose encoding metadata. */
		virtual const SaveObjectEncoding* PeekLastPutEncoding() const { return nullptr; }
		virtual const CompressStats* PeekLastPutStats() const { return nullptr; }
	};
}
