#pragma once
// Moving saves between the local save directory, the local object cache and
// the shared object store. Port of shared-world-helper/internal/syncer,
// adapted to content-addressed objects.
//
// Invariants:
//  * The local save is replaced only by bytes whose SHA-256 and size match
//    the committed revision AND that pass full structural validation, after
//    the previous local file was backed up; replacement is an atomic rename.
//  * An uploaded object only becomes authoritative through a fenced
//    LeaseManager::CommitRevision. Objects are never deleted on conflict
//    (another revision may reference the same content).
//  * A refused upload is preserved locally as a conflict backup; the
//    original save file is never modified by an upload.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	/** What this machine knows about its copy of a world. */
	struct LocalWorldState
	{
		int64_t SyncedRevision = 0;
		std::string SyncedSha256; // local save hash at last download/upload
		TimeMs SyncedAt = 0;
		/** Persisted while hosting, so a restarted game can tell it crashed while host. */
		std::optional<LeaseToken> ActiveLease;
	};

	class LocalStateStore
	{
	public:
		explicit LocalStateStore(std::string InDir) : Dir(std::move(InDir)) {}
		/** Corrupt or missing state reads as "nothing known" (forces backups). */
		LocalWorldState Load(const std::string& WorldId) const;
		Status Save(const std::string& WorldId, const LocalWorldState& State) const;

	private:
		std::string Dir;
	};

	struct LocalBackup
	{
		std::string Path;
		std::string Name;
		int64_t Size = 0;
		bool bProtected = false; // conflict/recovery backups are never pruned
	};

	/** Local copies of saves before replacement and of refused uploads. */
	class BackupManager
	{
	public:
		BackupManager(std::string InDir, int InKeep, std::shared_ptr<IClock> InClock) : Dir(std::move(InDir)), Keep(InKeep), Clock(std::move(InClock)) {}
		/** Label: short tag such as "pre-download-r184" or "conflict-g591-r184". */
		Result<std::string> Preserve(const std::string& WorldId, const std::string& Src, const std::string& Label);
		Result<std::vector<LocalBackup>> List(const std::string& WorldId) const; // newest first

	private:
		void Prune(const std::string& WorldId);
		std::string Dir;
		int Keep;
		std::shared_ptr<IClock> Clock;
	};

	/** Verified objects kept locally (fast migration/restore, less bandwidth). Never authoritative. */
	class ObjectCache
	{
	public:
		explicit ObjectCache(std::string InDir) : Dir(std::move(InDir)) {}
		/** Path of a cached object whose content still hashes to Sha, else nullopt. */
		std::optional<std::string> Find(const std::string& Sha);
		/** Adds a verified file (copied) to the cache. */
		Status Add(const std::string& Sha, const std::string& VerifiedFile);
		std::string PathFor(const std::string& Sha) const;

	private:
		std::string Dir;
	};

	enum class LocalStatus
	{
		Missing,  // no local save
		InSync,   // identical to the last synced revision
		Modified, // changed since the last sync: progress that never reached the shared world
		Unknown,  // exists but this machine has no sync record
	};
	const char* ToString(LocalStatus S);

	struct LocalInspection
	{
		LocalStatus Status = LocalStatus::Missing;
		std::string Sha256;
		LocalWorldState State;
	};

	struct DownloadResult
	{
		int64_t Revision = 0;
		bool bAlreadyLocal = false;
		bool bFromCache = false;
		std::string BackupPath;
	};

	struct UploadOptions
	{
		std::string Reason = Reason::Checkpoint;
		std::string GameBuild;
		std::string ModVersion;
		/** Read the stored object back and hash it (doubles transfer; on by default). */
		bool bVerifyByReadBack = true;
		TimeMs StableQuiet = Seconds(3);
		TimeMs StableTimeout = Minutes(2);
	};

	struct UploadResult
	{
		RevisionMeta Revision;
		bool bUnchanged = false;   // content equals head: no new revision
		bool bDeduplicated = false; // object already existed in the store
	};

	/** Details of a refused upload (Fenced / StaleRevision). */
	struct ConflictInfo
	{
		std::string BackupPath;
		int64_t CloudRevision = 0;
		int64_t LocalBaseRevision = 0;
	};

	struct SyncConfig
	{
		std::string DataDir; // <data>/worlds/<id>/..., <data>/cache/...
		int KeepLocalBackups = 20;
		/** Cloud revisions + unreferenced save objects retained after each upload. Min 5. */
		int KeepCloudRevisions = 5;
	};

	class SyncEngine
	{
	public:
		SyncEngine(std::shared_ptr<IObjectStore> InObjects, std::shared_ptr<LeaseManager> InLeases, SyncConfig InConfig);

		LocalStateStore& LocalState() { return States; }
		BackupManager& Backups() { return BackupMgr; }
		ObjectCache& Cache() { return CacheMgr; }
		IObjectStore& Objects() { return *ObjectStore; }

		Result<LocalInspection> InspectLocal(const std::string& SavePath);

		/** Installs State.Head into Target (see header comment). */
		Result<DownloadResult> Download(const WorldState& State, const std::string& Target);

		/**
		 * Publishes the save at Src as revision Token.BaseRevision + 1. On a
		 * refused upload (Fenced / StaleRevision) the snapshot is preserved and
		 * Conflict (if non-null) describes it.
		 */
		Result<UploadResult> Upload(LeaseToken& Token, const std::string& Src, const UploadOptions& Options, ConflictInfo* Conflict = nullptr);

		/**
		 * Makes an older revision's content the new head as revision N+1
		 * (history is never rewritten). Requires the lease.
		 */
		Result<RevisionMeta> Restore(LeaseToken& Token, const RevisionMeta& From, const Identity& By);

		/** Accepted revisions at CommitId, newest first. */
		Result<std::vector<RevisionMeta>> History(const std::string& CommitId, size_t Max = 200);

		Status MarkSynced(int64_t Revision, const std::string& Sha);

	private:
		std::string WorldDir() const;
		Result<std::string> FetchVerified(const RevisionMeta& Head, bool& bFromCache);
		/** Best-effort: drop revision metadata + orphaned objects beyond KeepCloudRevisions. */
		void PruneOldRevisions(LeaseToken& Token);

		std::shared_ptr<IObjectStore> ObjectStore;
		std::shared_ptr<LeaseManager> Leases;
		SyncConfig Cfg;
		LocalStateStore States;
		BackupManager BackupMgr;
		ObjectCache CacheMgr;
	};
}
