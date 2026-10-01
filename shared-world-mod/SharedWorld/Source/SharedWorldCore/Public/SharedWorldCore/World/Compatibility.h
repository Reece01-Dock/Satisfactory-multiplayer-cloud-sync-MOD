#pragma once
// Version compatibility between this installation and a Shared World.

#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Model/Model.h"

namespace sw
{
	struct LocalVersions
	{
		std::string GameBuild;  // numeric changelist, e.g. "491125"
		std::string ModVersion; // semver, e.g. "0.2.0"
		std::vector<RequiredMod> InstalledMods;
	};

	/**
	 * Refuses when continuing could damage the world: the head revision was
	 * written by a newer game build, the local mod is older than the world's
	 * required version, or a required mod is missing / too old.
	 * Newer mods may open older worlds once (world.json is then bumped so
	 * friends must update to the same version). Multiplayer peers are also
	 * locked to an exact RemoteVersionRange in the .uplugin.
	 * Returns Unsupported with a player-facing message.
	 */
	Status CheckCompatibility(const LocalVersions& Local, const WorldInfo& Info, const std::optional<RevisionMeta>& Head);

	/** Compares dotted numeric versions: -1, 0, 1. Non-numeric parts compare as 0. */
	int CompareVersions(const std::string& A, const std::string& B);
}
