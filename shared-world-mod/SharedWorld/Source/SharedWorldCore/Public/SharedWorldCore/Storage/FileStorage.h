#pragma once
// Filesystem providers: a single PC or a real network share (SMB/NFS with
// working exclusive-create). NOT safe on folders replicated by sync clients
// (Google Drive / OneDrive / Dropbox desktop apps): those replicate
// eventually, so two machines could both win a commit.
//
// Repository layout (a miniature Git):
//   <root>/repo/HEAD                  current commit id
//   <root>/repo/commits/<id>.json     {parent, message, time, files:{path: blob}}
//   <root>/repo/blobs/<sha256>        file contents
// Objects: <root>/objects/sha256/<ab>/<sha256>

#include <mutex>

#include "SharedWorldCore/Storage/Storage.h"
#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	class FileRepository final : public IWorldRepository
	{
	public:
		/** Root directory of the shared world (created if missing). */
		explicit FileRepository(std::string InRoot);

		Result<std::string> Head() override;
		Result<std::string> ReadFile(const std::string& CommitId, const std::string& Path) override;
		Result<std::vector<std::string>> ListDirectory(const std::string& CommitId, const std::string& Dir) override;
		Result<std::string> Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message) override;
		Result<std::vector<CommitInfo>> Log(const std::string& FromCommit, int MaxCount) override;
		std::string Describe() const override { return "folder " + Root; }

		/** A lock file older than this is presumed abandoned by a crashed process. */
		double StaleLockSeconds = 30.0;

	private:
		struct LoadedCommit;
		Result<LoadedCommit> LoadCommit(const std::string& Id);
		Status Lock();
		void Unlock();

		std::string Root;
		std::string RepoDir;
		std::mutex ProcessMutex; // serialises commits within this process
		SystemRandom Rng;
	};

	class FileObjectStore final : public IObjectStore
	{
	public:
		explicit FileObjectStore(std::string InRoot);

		Result<bool> Has(const std::string& Sha256) override;
		Status Put(const std::string& Sha256, const std::string& LocalPath) override;
		Status Get(const std::string& Sha256, const std::string& DestPath) override;
		Status Remove(const std::string& Sha256) override;
		Result<std::vector<std::string>> List() override;
		std::string Describe() const override { return "folder " + Root; }

	private:
		Result<std::string> PathFor(const std::string& Sha256) const;
		std::string Root;
	};
}
