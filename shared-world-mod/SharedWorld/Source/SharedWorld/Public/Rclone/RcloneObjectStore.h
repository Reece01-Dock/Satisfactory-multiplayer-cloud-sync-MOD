#pragma once
// sw::IObjectStore on top of any rclone remote (Google Drive, OneDrive, Dropbox, S3, WebDAV, SFTP, local folder...).
//
// This is exactly the part of the storage design rclone can own safely: .sav objects are immutable and
// content-addressed, so no compare-and-swap is needed. The repository that holds leases, revisions and fencing is a
// different interface (sw::IWorldRepository) and is NOT implemented here.
//
// All calls block on network I/O: use from the background queue, never the game thread.

#include <string>
#include <vector>

#include "CoreMinimal.h"
#include "SharedWorldCore/Storage/Storage.h"

class SHAREDWORLD_API FRcloneObjectStore final : public sw::IObjectStore
{
public:
	/**
	 * Fs is any rclone path: "mydrive:SharedWorlds", "s3remote:bucket/prefix" or a plain local directory.
	 * Objects are stored as <Fs>/objects/<first two chars>/<id>.
	 */
	explicit FRcloneObjectStore(FString InFs);

	sw::Result<bool> Has(const std::string& Sha256) override;
	sw::Status Put(const std::string& Sha256, const std::string& LocalPath) override;
	sw::Status PutBlob(const std::string& ObjectId, const std::string& LocalPath) override;
	sw::Status Get(const std::string& Sha256, const std::string& DestPath) override;
	sw::Status Remove(const std::string& Sha256) override;
	sw::Result<std::vector<std::string>> List() override;
	std::string Describe() const override;

private:
	FString Fs;
};

#include "SharedWorldCore/Storage/LogRepository.h"

/**
 * sw::ILogStore on an rclone path: the world record of a world stored on any rclone provider (see sw::LogRepository).
 * Entries are small JSON files with unique names; rclone overwrites but never needs to here, and it reports no
 * trustworthy server time, so the repository decides races by name with a settle wait.
 */
class SHAREDWORLD_API FRcloneLogStore final : public sw::ILogStore
{
public:
	explicit FRcloneLogStore(FString InFs);
	bool ExclusiveCreate() const override { return false; }
	sw::Result<std::vector<sw::LogEntryInfo>> List(const std::string& Dir) override;
	sw::Result<std::string> Read(const std::string& Dir, const sw::LogEntryInfo& Entry) override;
	sw::Result<sw::LogEntryInfo> Create(const std::string& Dir, const std::string& Name, const std::string& Data) override;
	sw::Status Delete(const std::string& Dir, const sw::LogEntryInfo& Entry) override;
	std::string Describe() const override;

private:
	FString Fs;
};
