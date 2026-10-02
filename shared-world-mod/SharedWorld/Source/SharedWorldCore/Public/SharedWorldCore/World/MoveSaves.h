#pragma once
// Moving a world's save files to another object store (e.g. GitHub -> Dropbox).
//
// Safe against a host uploading at the same time: the lease is taken first, so nobody can publish a revision while
// files are copied, and world.json is only switched (through the normal compare-and-swap commit) once every file is
// confirmed at the destination. Nothing is deleted from the old store, so a failed or abandoned move changes nothing.

#include <functional>
#include <string>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	struct MoveSavesResult
	{
		int Copied = 0;   // files uploaded to the destination
		int Present = 0;  // already there (e.g. a previous attempt)
	};

	/**
	 * Copies every object in From to To, then records Target in world.json. TempDir is a writable local folder.
	 * Progress(done, total) is called after each file. Fails with Conflict when someone else holds the lease.
	 */
	Result<MoveSavesResult> MoveWorldSaves(LeaseManager& Leases, IObjectStore& From, IObjectStore& To,
		const WorldInfo::SaveStorageInfo& Target, const Identity& Me, const std::string& TempDir,
		const std::function<void(size_t Done, size_t Total)>& Progress = nullptr);
}
