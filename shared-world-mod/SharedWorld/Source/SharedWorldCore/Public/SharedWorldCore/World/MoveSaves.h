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

namespace sw
{
	/**
	 * Moves a whole world (record, state, players, settings, revision history, locks and save files) from its current
	 * repository + object store to new ones, e.g. GitHub -> Dropbox.
	 *
	 *  1. Takes the lease on the old copy (nobody can host or publish while moving).
	 *  2. Copies every save object to ToObjects (verified).
	 *  3. Copies every record file into ToRepo as its first commit, with the lease cleared and the generation kept, so
	 *     fencing continues where it left off. Fails with AlreadyExists if ToRepo already holds a world.
	 *  4. Marks the old copy's world.json movedTo=Target (it then refuses to host) and releases the lease.
	 * Nothing is deleted from the old location.
	 */
	Result<MoveSavesResult> MoveWholeWorld(LeaseManager& FromLeases, IObjectStore& FromObjects,
		IWorldRepository& ToRepo, IObjectStore& ToObjects, const WorldInfo::SaveStorageInfo& Target,
		const Identity& Me, const std::string& TempDir,
		const std::function<void(size_t Done, size_t Total)>& Progress = nullptr);
}
