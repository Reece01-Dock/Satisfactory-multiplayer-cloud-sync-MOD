#pragma once
// Player list and settings edits (in-game Players / Settings screens).
//
// These are cooperative rules enforced by every copy of the mod, NOT a
// security boundary: anyone with write access to the storage can edit the
// files directly. Real access control is the provider's (e.g. who is a
// collaborator on the GitHub repository). Every edit is a CAS commit that
// never touches the lease/state document, so it cannot disturb a host.

#include <string>

#include "SharedWorldCore/Lease/Lease.h"

namespace sw
{
	/** Adds (or re-adds) a player. Actor needs Invite; only an owner may grant Admin/Owner. */
	Result<PlayerList> AddMember(WorldStore& Store, const Identity& Actor, const Member& NewMember);
	/** Actor needs Invite. Only an owner may change an admin/owner or grant Admin/Owner. The last owner cannot be demoted. */
	Result<PlayerList> SetMemberRole(WorldStore& Store, const Identity& Actor, const std::string& PlayerId, Role NewRole);
	/** Actor needs RemovePlayers (or is removing themselves). The last owner cannot be removed. */
	Result<PlayerList> RemoveMember(WorldStore& Store, const Identity& Actor, const std::string& PlayerId);
	/** Open = anyone with storage access may play. Actor needs ChangeSettings. */
	Result<PlayerList> SetOpenMembership(WorldStore& Store, const Identity& Actor, bool bOpen);
	/** Replaces state/settings.json. Actor needs ChangeSettings. */
	Result<WorldSettings> UpdateSettings(WorldStore& Store, const Identity& Actor, const WorldSettings& Settings);
}
