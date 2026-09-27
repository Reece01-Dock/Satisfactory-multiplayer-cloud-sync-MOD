#include "SharedWorldCore/World/Membership.h"

#include "SharedWorldCore/Util/Json.h"

namespace sw
{
	namespace
	{
		Result<PlayerList> Decode(const std::string& Text)
		{
			if (Text.empty()) return PlayerList{}; // missing file: open, no members
			json::Value V;
			SW_ASSIGN(V, json::Parse(Text));
			return PlayerList::FromJson(V);
		}

		/**
		 * Permission of Actor in List. A world without members has no owner
		 * yet: whoever manages it first becomes its owner (see Claim below).
		 */
		Status Require(const PlayerList& List, const Identity& Actor, Permission P)
		{
			if (List.Members.empty()) return {};
			const Member* M = List.Find(Actor.PlayerId);
			if (!M) return MakeError(ErrorCode::Unauthorized, "you are not a member of this Shared World");
			if (!HasPermission(M->MemberRole, P)) return MakeError(ErrorCode::Unauthorized, "your role does not allow that");
			return {};
		}

		bool IsOwner(const PlayerList& List, const Identity& Actor)
		{
			const Member* M = List.Find(Actor.PlayerId);
			return List.Members.empty() || (M && M->MemberRole == Role::Owner);
		}

		/** The first managing edit of an owner-less (legacy/open) world makes the actor its owner. */
		void Claim(PlayerList& List, const Identity& Actor)
		{
			if (List.Members.empty()) List.Members.push_back(Member{Actor.PlayerId, Actor.DisplayName, Role::Owner});
		}

		int Owners(const PlayerList& List)
		{
			int N = 0;
			for (const Member& M : List.Members) N += M.MemberRole == Role::Owner ? 1 : 0;
			return N;
		}

		using EditFn = std::function<Status(PlayerList& List)>;

		Result<PlayerList> EditPlayers(WorldStore& Store, const std::string& Message, const EditFn& Fn)
		{
			PlayerList Out;
			auto R = Store.UpdateDocument(Paths::Players, [&](const std::string& Current) -> Result<std::string>
			{
				PlayerList L;
				SW_ASSIGN(L, Decode(Current));
				SW_TRY(Fn(L));
				SW_TRY(L.Validate());
				Out = L;
				return json::Serialize(L.ToJson(), 2);
			}, Message);
			if (!R) return R.Err();
			return Out;
		}

		Status ValidMember(const Member& M)
		{
			if (M.PlayerId.empty() || M.PlayerId.size() > 128) return MakeError(ErrorCode::Invalid, "invalid player id");
			if (M.DisplayName.size() > 128) return MakeError(ErrorCode::Invalid, "display name too long");
			return {};
		}
	}

	Result<PlayerList> AddMember(WorldStore& Store, const Identity& Actor, const Member& NewMember)
	{
		SW_TRY(ValidMember(NewMember));
		return EditPlayers(Store, "Players: " + Actor.DisplayName + " added " + NewMember.DisplayName + " as " + ToString(NewMember.MemberRole), [&](PlayerList& L) -> Status
		{
			SW_TRY(Require(L, Actor, Permission::Invite));
			const bool bPrivileged = NewMember.MemberRole == Role::Owner || NewMember.MemberRole == Role::Admin;
			if (bPrivileged && !IsOwner(L, Actor)) return MakeError(ErrorCode::Unauthorized, "only an owner can add admins or owners");
			Claim(L, Actor);
			for (Member& M : L.Members)
			{
				if (M.PlayerId != NewMember.PlayerId) continue;
				if (M.MemberRole == Role::Owner || M.MemberRole == Role::Admin)
				{
					if (!IsOwner(L, Actor)) return MakeError(ErrorCode::Unauthorized, "only an owner can change an admin or owner");
					if (M.MemberRole == Role::Owner && NewMember.MemberRole != Role::Owner && Owners(L) == 1) return MakeError(ErrorCode::Invalid, "the world needs at least one owner");
				}
				M = NewMember;
				return {};
			}
			if (L.Members.size() >= 256) return MakeError(ErrorCode::Invalid, "too many players");
			L.Members.push_back(NewMember);
			return {};
		});
	}

	Result<PlayerList> SetMemberRole(WorldStore& Store, const Identity& Actor, const std::string& PlayerId, Role NewRole)
	{
		return EditPlayers(Store, "Players: " + Actor.DisplayName + " changed a role to " + ToString(NewRole), [&](PlayerList& L) -> Status
		{
			SW_TRY(Require(L, Actor, Permission::Invite));
			Claim(L, Actor);
			for (Member& M : L.Members)
			{
				if (M.PlayerId != PlayerId) continue;
				const bool bTouchesPrivileged = M.MemberRole == Role::Owner || M.MemberRole == Role::Admin || NewRole == Role::Owner || NewRole == Role::Admin;
				if (bTouchesPrivileged && !IsOwner(L, Actor)) return MakeError(ErrorCode::Unauthorized, "only an owner can change admins or owners");
				if (M.MemberRole == Role::Owner && NewRole != Role::Owner && Owners(L) == 1) return MakeError(ErrorCode::Invalid, "the world needs at least one owner");
				M.MemberRole = NewRole;
				return {};
			}
			return MakeError(ErrorCode::NotFound, "that player is not a member");
		});
	}

	Result<PlayerList> RemoveMember(WorldStore& Store, const Identity& Actor, const std::string& PlayerId)
	{
		return EditPlayers(Store, "Players: " + Actor.DisplayName + " removed a player", [&](PlayerList& L) -> Status
		{
			if (PlayerId != Actor.PlayerId) SW_TRY(Require(L, Actor, Permission::RemovePlayers));
			for (size_t i = 0; i < L.Members.size(); ++i)
			{
				const Member& M = L.Members[i];
				if (M.PlayerId != PlayerId) continue;
				if ((M.MemberRole == Role::Owner || M.MemberRole == Role::Admin) && PlayerId != Actor.PlayerId && !IsOwner(L, Actor))
					return MakeError(ErrorCode::Unauthorized, "only an owner can remove admins or owners");
				if (M.MemberRole == Role::Owner && Owners(L) == 1) return MakeError(ErrorCode::Invalid, "the world needs at least one owner");
				L.Members.erase(L.Members.begin() + static_cast<std::ptrdiff_t>(i));
				return {};
			}
			return MakeError(ErrorCode::NotFound, "that player is not a member");
		});
	}

	Result<PlayerList> SetOpenMembership(WorldStore& Store, const Identity& Actor, bool bOpen)
	{
		return EditPlayers(Store, std::string("Players: ") + Actor.DisplayName + (bOpen ? " opened the world" : " restricted the world to members"), [&](PlayerList& L) -> Status
		{
			SW_TRY(Require(L, Actor, Permission::ChangeSettings));
			Claim(L, Actor);
			L.bOpen = bOpen;
			return {};
		});
	}

	Result<WorldSettings> UpdateSettings(WorldStore& Store, const Identity& Actor, const WorldSettings& Settings)
	{
		SW_TRY(Settings.Validate());
		auto Snap = Store.Load();
		if (!Snap) return Snap.Err();
		auto Players = Store.LoadPlayers(Snap->CommitId);
		if (Players.Ok()) SW_TRY(Require(*Players, Actor, Permission::ChangeSettings));
		else if (!Players.Is(ErrorCode::NotFound)) return Players.Err();
		auto R = Store.UpdateDocument(Paths::Settings, [&](const std::string&) -> Result<std::string>
		{
			return json::Serialize(Settings.ToJson(), 2);
		}, "Settings: " + Actor.DisplayName + " updated the world settings");
		if (!R) return R.Err();
		return Settings;
	}
}
