#pragma once
// Creating a Shared World from an existing local save ("convert existing
// save"; a brand-new game is saved by the game first, then converted).

#include <string>

#include "SharedWorldCore/Sync/Sync.h"
#include "SharedWorldCore/World/Compatibility.h"

namespace sw
{
	struct CreateWorldParams
	{
		std::string Name;
		Identity Creator;
		/** The local save to convert (validated before anything is created). */
		std::string SourceSavePath;
		std::string OriginalSaveName;
		LocalVersions Versions;
		WorldSettings Settings;
		/** Empty: anyone with storage access may play. Otherwise creator becomes owner. */
		bool bRestrictToMembers = true;
	};

	/**
	 * Initialises the repository (world.json, state, players, settings),
	 * acquires the lease, uploads the save as revision 1 and releases.
	 * Retry-safe: if a previous attempt created the world but did not upload
	 * revision 1, calling again continues from there.
	 */
	Result<WorldInfo> CreateSharedWorld(LeaseManager& Leases, SyncEngine& Sync, const CreateWorldParams& Params, TimeMs StableQuiet = Seconds(1));
}
