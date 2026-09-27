#include "SharedWorldCore/Model/Model.h"

#include <cstdio>

#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	using json::Value;

	Status ValidateWorldId(const std::string& Id)
	{
		if (Id.empty() || Id.size() > 64)
		{
			return MakeError(ErrorCode::Invalid, "invalid world id (length)");
		}
		for (size_t i = 0; i < Id.size(); ++i)
		{
			const char C = Id[i];
			const bool bOk = (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || (C == '-' && i > 0);
			if (!bOk)
			{
				return MakeError(ErrorCode::Invalid, "invalid world id '" + Id + "'");
			}
		}
		return {};
	}

	Status ValidateText(const char* Field, const std::string& S, size_t MaxLen)
	{
		if (S.size() > MaxLen)
		{
			return MakeError(ErrorCode::Invalid, std::string(Field) + " is too long");
		}
		for (unsigned char C : S)
		{
			if (C < 0x20 && C != '\t')
			{
				return MakeError(ErrorCode::Invalid, std::string(Field) + " contains control characters");
			}
		}
		// JSON decoding already guarantees valid UTF-8 for remote data.
		return {};
	}

	namespace
	{
		Value TimeValue(TimeMs T) { return Value(FormatTime(T)); }

		Result<TimeMs> GetTime(const Value& V, const char* Key)
		{
			std::string S;
			SW_ASSIGN(S, json::GetString(V, Key, 64));
			auto T = ParseTime(S);
			if (!T)
			{
				return T.Err().Wrap(Key);
			}
			return T.Value();
		}

		Result<std::string> GetText(const Value& V, const char* Key, size_t MaxLen)
		{
			std::string S;
			SW_ASSIGN(S, json::GetString(V, Key, MaxLen));
			SW_TRY(ValidateText(Key, S, MaxLen));
			return S;
		}

		Result<std::string> GetOptionalText(const Value& V, const char* Key, size_t MaxLen)
		{
			std::optional<std::string> S;
			SW_ASSIGN(S, json::GetOptionalString(V, Key, MaxLen));
			if (!S)
			{
				return std::string();
			}
			SW_TRY(ValidateText(Key, *S, MaxLen));
			return *S;
		}

		Result<int64_t> GetNonNegative(const Value& V, const char* Key)
		{
			int64_t I;
			SW_ASSIGN(I, json::GetInt(V, Key));
			if (I < 0)
			{
				return MakeError(ErrorCode::Invalid, std::string(Key) + " must not be negative");
			}
			return I;
		}

		const Value* ObjectField(const Value& V, const char* Key) { return json::GetOptional(V, Key); }
	}

	// ------------------------------------------------------------ Identity

	Value Identity::ToJson() const
	{
		Value V;
		V.Set("playerId", PlayerId);
		V.Set("displayName", DisplayName);
		V.Set("platform", Platform);
		V.Set("installId", InstallId);
		return V;
	}

	Result<Identity> Identity::FromJson(const Value& V)
	{
		if (!V.IsObject()) return MakeError(ErrorCode::Invalid, "identity must be an object");
		Identity I;
		SW_ASSIGN(I.PlayerId, GetText(V, "playerId", 128));
		SW_ASSIGN(I.DisplayName, GetText(V, "displayName", 128));
		SW_ASSIGN(I.Platform, GetText(V, "platform", 32));
		SW_ASSIGN(I.InstallId, GetText(V, "installId", 128));
		SW_TRY(I.Validate());
		return I;
	}

	Status Identity::Validate() const
	{
		if (PlayerId.empty() || InstallId.empty())
		{
			return MakeError(ErrorCode::Invalid, "identity requires playerId and installId");
		}
		SW_TRY(ValidateText("playerId", PlayerId, 128));
		SW_TRY(ValidateText("displayName", DisplayName, 128));
		SW_TRY(ValidateText("platform", Platform, 32));
		return ValidateText("installId", InstallId, 128);
	}

	// ------------------------------------------------------------ RevisionMeta

	std::string RevisionMeta::Path() const
	{
		char Buf[96];
		// Sharded by thousands: GitHub recommends <= 3000 entries per directory.
		std::snprintf(Buf, sizeof(Buf), "revisions/%04lld/%08lld-g%08lld-%.8s.json", static_cast<long long>(Number / 1000), static_cast<long long>(Number),
			static_cast<long long>(Generation), ObjectSha256.c_str());
		return Buf;
	}

	Value RevisionMeta::ToJson() const
	{
		Value V;
		V.Set("number", Number);
		V.Set("generation", Generation);
		V.Set("previousRevision", PreviousRevision);
		V.Set("object", ObjectSha256);
		V.Set("size", Size);
		V.Set("createdAt", TimeValue(CreatedAt));
		V.Set("uploader", Uploader.ToJson());
		V.Set("reason", Reason);
		if (RestoredFrom)
		{
			V.Set("restoredFrom", RestoredFrom);
		}
		V.Set("gameBuild", GameBuild);
		V.Set("modVersion", ModVersion);
		return V;
	}

	Result<RevisionMeta> RevisionMeta::FromJson(const Value& V)
	{
		if (!V.IsObject()) return MakeError(ErrorCode::Invalid, "revision must be an object");
		RevisionMeta R;
		SW_ASSIGN(R.Number, GetNonNegative(V, "number"));
		SW_ASSIGN(R.Generation, GetNonNegative(V, "generation"));
		SW_ASSIGN(R.PreviousRevision, GetNonNegative(V, "previousRevision"));
		SW_ASSIGN(R.ObjectSha256, json::GetString(V, "object", 64));
		SW_ASSIGN(R.Size, GetNonNegative(V, "size"));
		SW_ASSIGN(R.CreatedAt, GetTime(V, "createdAt"));
		const Value* U = V.Find("uploader");
		if (!U) return MakeError(ErrorCode::Invalid, "revision uploader missing");
		SW_ASSIGN(R.Uploader, Identity::FromJson(*U));
		SW_ASSIGN(R.Reason, GetText(V, "reason", 32));
		std::optional<int64_t> Restored;
		SW_ASSIGN(Restored, json::GetOptionalInt(V, "restoredFrom"));
		R.RestoredFrom = Restored.value_or(0);
		SW_ASSIGN(R.GameBuild, GetOptionalText(V, "gameBuild", 64));
		SW_ASSIGN(R.ModVersion, GetOptionalText(V, "modVersion", 64));
		SW_TRY(R.Validate());
		return R;
	}

	Status RevisionMeta::Validate() const
	{
		if (Number < 1) return MakeError(ErrorCode::Invalid, "revision number must be >= 1");
		if (Generation < 1) return MakeError(ErrorCode::Invalid, "revision generation must be >= 1");
		if (PreviousRevision != Number - 1) return MakeError(ErrorCode::Invalid, "revision must follow its previous revision");
		if (!Sha256::IsValidHex(ObjectSha256)) return MakeError(ErrorCode::Invalid, "revision object hash invalid");
		if (Size <= 0) return MakeError(ErrorCode::Invalid, "revision size invalid");
		if (RestoredFrom < 0 || RestoredFrom >= Number) return MakeError(ErrorCode::Invalid, "restoredFrom invalid");
		SW_TRY(Uploader.Validate());
		return ValidateText("reason", Reason, 32);
	}

	// ------------------------------------------------------------ enums

	const char* ToString(LeasePhase P)
	{
		switch (P)
		{
		case LeasePhase::Preparing: return "PREPARING";
		case LeasePhase::Hosting: return "HOSTING";
		case LeasePhase::Saving: return "SAVING";
		case LeasePhase::Stopping: return "STOPPING";
		case LeasePhase::Migrating: return "MIGRATING";
		}
		return "?";
	}

	Result<LeasePhase> ParseLeasePhase(const std::string& S)
	{
		for (LeasePhase P : {LeasePhase::Preparing, LeasePhase::Hosting, LeasePhase::Saving, LeasePhase::Stopping, LeasePhase::Migrating})
		{
			if (S == ToString(P)) return P;
		}
		return MakeError(ErrorCode::Invalid, "unknown lease phase '" + S + "'");
	}

	const char* ToString(Role R)
	{
		switch (R)
		{
		case Role::Owner: return "owner";
		case Role::Admin: return "admin";
		case Role::Member: return "member";
		case Role::Viewer: return "viewer";
		}
		return "?";
	}

	Result<Role> ParseRole(const std::string& S)
	{
		for (Role R : {Role::Owner, Role::Admin, Role::Member, Role::Viewer})
		{
			if (S == ToString(R)) return R;
		}
		return MakeError(ErrorCode::Invalid, "unknown role '" + S + "'");
	}

	bool HasPermission(Role R, Permission P)
	{
		switch (P)
		{
		case Permission::Play: return R != Role::Viewer;
		case Permission::Invite: return R == Role::Owner || R == Role::Admin;
		case Permission::RemovePlayers: return R == Role::Owner || R == Role::Admin;
		case Permission::RestoreRevision: return R == Role::Owner || R == Role::Admin;
		case Permission::ChangeSettings: return R == Role::Owner || R == Role::Admin;
		case Permission::DeleteWorld: return R == Role::Owner;
		case Permission::ModifyStorage: return R == Role::Owner;
		}
		return false;
	}

	// ------------------------------------------------------------ JoinInfo

	Value JoinInfo::ToJson() const
	{
		Value V;
		V.Set("kind", Kind);
		V.Set("value", Data);
		V.Set("backend", Backend);
		return V;
	}

	Result<JoinInfo> JoinInfo::FromJson(const json::Value& V)
	{
		JoinInfo J;
		SW_ASSIGN(J.Kind, GetText(V, "kind", 32));
		SW_ASSIGN(J.Data, GetText(V, "value", 512));
		SW_ASSIGN(J.Backend, GetOptionalText(V, "backend", 64));
		SW_TRY(J.Validate());
		return J;
	}

	Status JoinInfo::Validate() const
	{
		if (Kind != "online-session-id" && Kind != "address") return MakeError(ErrorCode::Invalid, "unknown join kind '" + Kind + "'");
		if (Data.empty()) return MakeError(ErrorCode::Invalid, "join value empty");
		SW_TRY(ValidateText("join.value", Data, 512));
		return ValidateText("join.backend", Backend, 64);
	}

	// ------------------------------------------------------------ WorldState

	Value WorldState::ToJson() const
	{
		Value V;
		V.Set("schemaVersion", SchemaVersion);
		V.Set("worldId", WorldId);
		V.Set("stateVersion", StateVersion);
		V.Set("generation", Generation);
		V.Set("head", Head ? Head->ToJson() : Value());
		if (CurrentLease)
		{
			const Lease& L = *CurrentLease;
			Value LV;
			LV.Set("generation", L.Generation);
			LV.Set("holder", L.Holder.ToJson());
			LV.Set("nonce", L.Nonce);
			LV.Set("acquiredAt", TimeValue(L.AcquiredAt));
			LV.Set("renewedAt", TimeValue(L.RenewedAt));
			LV.Set("expiresAt", TimeValue(L.ExpiresAt));
			LV.Set("baseRevision", L.BaseRevision);
			LV.Set("phase", ToString(L.Phase));
			LV.Set("join", L.Join ? L.Join->ToJson() : Value());
			json::Array Players;
			for (const SessionPlayer& P : L.Players)
			{
				Value PV;
				PV.Set("displayName", P.DisplayName);
				PV.Set("playerId", P.PlayerId);
				PV.Set("installId", P.InstallId);
				Players.push_back(std::move(PV));
			}
			LV.Set("players", Value(std::move(Players)));
			V.Set("lease", std::move(LV));
		}
		else
		{
			V.Set("lease", Value());
		}
		if (LastSession)
		{
			Value SV;
			SV.Set("host", LastSession->Host.ToJson());
			SV.Set("generation", LastSession->Generation);
			SV.Set("endedAt", TimeValue(LastSession->EndedAt));
			SV.Set("reason", LastSession->Reason);
			V.Set("lastSession", std::move(SV));
		}
		else
		{
			V.Set("lastSession", Value());
		}
		if (PendingHandoff)
		{
			Value HV;
			HV.Set("successor", PendingHandoff->Successor.ToJson());
			HV.Set("revision", PendingHandoff->Revision);
			HV.Set("expiresAt", TimeValue(PendingHandoff->ExpiresAt));
			V.Set("handoff", std::move(HV));
		}
		else
		{
			V.Set("handoff", Value());
		}
		V.Set("updatedAt", TimeValue(UpdatedAt));
		return V;
	}

	Result<WorldState> WorldState::FromJson(const Value& V, const std::string& ExpectedWorldId)
	{
		if (!V.IsObject()) return MakeError(ErrorCode::Invalid, "state must be an object");
		int64_t Schema;
		SW_ASSIGN(Schema, json::GetInt(V, "schemaVersion"));
		if (Schema != SchemaVersion)
		{
			return MakeError(ErrorCode::Unsupported, "state schemaVersion " + std::to_string(Schema) + " is not supported by this mod version (expects " + std::to_string(SchemaVersion) + "); update the mod");
		}
		WorldState S;
		SW_ASSIGN(S.WorldId, json::GetString(V, "worldId", 64));
		SW_ASSIGN(S.StateVersion, GetNonNegative(V, "stateVersion"));
		SW_ASSIGN(S.Generation, GetNonNegative(V, "generation"));
		if (const Value* H = ObjectField(V, "head"))
		{
			RevisionMeta R;
			SW_ASSIGN(R, RevisionMeta::FromJson(*H));
			S.Head = std::move(R);
		}
		if (const Value* LV = ObjectField(V, "lease"))
		{
			Lease L;
			SW_ASSIGN(L.Generation, GetNonNegative(*LV, "generation"));
			const Value* Holder = LV->Find("holder");
			if (!Holder) return MakeError(ErrorCode::Invalid, "lease holder missing");
			SW_ASSIGN(L.Holder, Identity::FromJson(*Holder));
			SW_ASSIGN(L.Nonce, GetText(*LV, "nonce", 64));
			SW_ASSIGN(L.AcquiredAt, GetTime(*LV, "acquiredAt"));
			SW_ASSIGN(L.RenewedAt, GetTime(*LV, "renewedAt"));
			SW_ASSIGN(L.ExpiresAt, GetTime(*LV, "expiresAt"));
			SW_ASSIGN(L.BaseRevision, GetNonNegative(*LV, "baseRevision"));
			std::string Phase;
			SW_ASSIGN(Phase, json::GetString(*LV, "phase", 32));
			SW_ASSIGN(L.Phase, ParseLeasePhase(Phase));
			if (const Value* J = ObjectField(*LV, "join"))
			{
				JoinInfo Join;
				SW_ASSIGN(Join, JoinInfo::FromJson(*J));
				L.Join = std::move(Join);
			}
			if (const Value* P = ObjectField(*LV, "players"))
			{
				if (!P->IsArray() || P->AsArray().size() > 128) return MakeError(ErrorCode::Invalid, "lease players invalid");
				for (const Value& PV : P->AsArray())
				{
					SessionPlayer SP;
					SW_ASSIGN(SP.DisplayName, GetText(PV, "displayName", 128));
					SW_ASSIGN(SP.PlayerId, GetOptionalText(PV, "playerId", 128));
					SW_ASSIGN(SP.InstallId, GetOptionalText(PV, "installId", 128));
					L.Players.push_back(std::move(SP));
				}
			}
			S.CurrentLease = std::move(L);
		}
		if (const Value* SV = ObjectField(V, "lastSession"))
		{
			SessionEnd E;
			const Value* Host = SV->Find("host");
			if (!Host) return MakeError(ErrorCode::Invalid, "lastSession host missing");
			SW_ASSIGN(E.Host, Identity::FromJson(*Host));
			SW_ASSIGN(E.Generation, GetNonNegative(*SV, "generation"));
			SW_ASSIGN(E.EndedAt, GetTime(*SV, "endedAt"));
			SW_ASSIGN(E.Reason, GetText(*SV, "reason", 32));
			S.LastSession = std::move(E);
		}
		if (const Value* HV = ObjectField(V, "handoff"))
		{
			Handoff H;
			const Value* Succ = HV->Find("successor");
			if (!Succ) return MakeError(ErrorCode::Invalid, "handoff successor missing");
			SW_ASSIGN(H.Successor, Identity::FromJson(*Succ));
			SW_ASSIGN(H.Revision, GetNonNegative(*HV, "revision"));
			SW_ASSIGN(H.ExpiresAt, GetTime(*HV, "expiresAt"));
			S.PendingHandoff = std::move(H);
		}
		SW_ASSIGN(S.UpdatedAt, GetTime(V, "updatedAt"));
		SW_TRY(S.Validate(ExpectedWorldId));
		return S;
	}

	Status WorldState::Validate(const std::string& ExpectedWorldId) const
	{
		SW_TRY(ValidateWorldId(WorldId));
		if (WorldId != ExpectedWorldId)
		{
			return MakeError(ErrorCode::Invalid, "state belongs to world '" + WorldId + "', expected '" + ExpectedWorldId + "'");
		}
		if (Head)
		{
			SW_TRY(Head->Validate());
			if (Head->Generation > Generation) return MakeError(ErrorCode::Invalid, "head generation is newer than state generation");
		}
		if (CurrentLease)
		{
			const Lease& L = *CurrentLease;
			if (L.Generation < 1 || L.Generation != Generation)
			{
				return MakeError(ErrorCode::Invalid, "lease generation does not match state generation");
			}
			SW_TRY(L.Holder.Validate());
			if (L.Nonce.empty()) return MakeError(ErrorCode::Invalid, "lease nonce missing");
			if (L.ExpiresAt <= L.AcquiredAt) return MakeError(ErrorCode::Invalid, "lease expiry precedes acquisition");
			if (L.BaseRevision > HeadNumber()) return MakeError(ErrorCode::Invalid, "lease base revision is ahead of head");
			if (L.Join) SW_TRY(L.Join->Validate());
			if (L.Players.size() > 128) return MakeError(ErrorCode::Invalid, "too many players");
		}
		if (PendingHandoff)
		{
			SW_TRY(PendingHandoff->Successor.Validate());
			if (PendingHandoff->Revision != HeadNumber()) return MakeError(ErrorCode::Invalid, "handoff revision must equal head");
		}
		return {};
	}

	Result<WorldState> DecodeState(const std::string& Text, const std::string& WorldId)
	{
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text));
		return WorldState::FromJson(V, WorldId);
	}

	std::string EncodeState(const WorldState& S) { return json::Serialize(S.ToJson(), 2); }

	// ------------------------------------------------------------ WorldInfo

	Value WorldInfo::ToJson() const
	{
		Value V;
		V.Set("schemaVersion", SchemaVersion);
		V.Set("worldId", WorldId);
		V.Set("name", Name);
		V.Set("createdBy", CreatedBy.ToJson());
		V.Set("createdAt", TimeValue(CreatedAt));
		V.Set("originalSaveName", OriginalSaveName);
		V.Set("gameBuild", GameBuild);
		V.Set("modVersion", ModVersion);
		json::Array Mods;
		for (const RequiredMod& M : RequiredMods)
		{
			Value MV;
			MV.Set("modReference", M.ModReference);
			MV.Set("version", M.Version);
			Mods.push_back(std::move(MV));
		}
		V.Set("requiredMods", Value(std::move(Mods)));
		return V;
	}

	Result<WorldInfo> WorldInfo::FromJson(const Value& V)
	{
		if (!V.IsObject()) return MakeError(ErrorCode::Invalid, "world.json must be an object");
		int64_t Schema;
		SW_ASSIGN(Schema, json::GetInt(V, "schemaVersion"));
		if (Schema != SchemaVersion) return MakeError(ErrorCode::Unsupported, "world.json schema not supported; update the mod");
		WorldInfo W;
		SW_ASSIGN(W.WorldId, json::GetString(V, "worldId", 64));
		SW_ASSIGN(W.Name, GetText(V, "name", 128));
		const Value* C = V.Find("createdBy");
		if (!C) return MakeError(ErrorCode::Invalid, "createdBy missing");
		SW_ASSIGN(W.CreatedBy, Identity::FromJson(*C));
		SW_ASSIGN(W.CreatedAt, GetTime(V, "createdAt"));
		SW_ASSIGN(W.OriginalSaveName, GetOptionalText(V, "originalSaveName", 128));
		SW_ASSIGN(W.GameBuild, GetOptionalText(V, "gameBuild", 64));
		SW_ASSIGN(W.ModVersion, GetOptionalText(V, "modVersion", 64));
		if (const Value* Mods = ObjectField(V, "requiredMods"))
		{
			if (!Mods->IsArray() || Mods->AsArray().size() > 256) return MakeError(ErrorCode::Invalid, "requiredMods invalid");
			for (const Value& MV : Mods->AsArray())
			{
				RequiredMod M;
				SW_ASSIGN(M.ModReference, GetText(MV, "modReference", 128));
				SW_ASSIGN(M.Version, GetText(MV, "version", 64));
				W.RequiredMods.push_back(std::move(M));
			}
		}
		SW_TRY(W.Validate());
		return W;
	}

	Status WorldInfo::Validate() const
	{
		SW_TRY(ValidateWorldId(WorldId));
		if (Name.empty()) return MakeError(ErrorCode::Invalid, "world name empty");
		SW_TRY(ValidateText("name", Name, 128));
		return CreatedBy.Validate();
	}

	// ------------------------------------------------------------ Players

	const Member* PlayerList::Find(const std::string& PlayerId) const
	{
		for (const Member& M : Members)
		{
			if (M.PlayerId == PlayerId) return &M;
		}
		return nullptr;
	}

	Value PlayerList::ToJson() const
	{
		json::Array A;
		for (const Member& M : Members)
		{
			Value MV;
			MV.Set("playerId", M.PlayerId);
			MV.Set("displayName", M.DisplayName);
			MV.Set("role", ToString(M.MemberRole));
			A.push_back(std::move(MV));
		}
		Value V;
		V.Set("open", bOpen);
		V.Set("members", Value(std::move(A)));
		return V;
	}

	Result<PlayerList> PlayerList::FromJson(const Value& V)
	{
		const Value* Arr = V.Find("members");
		if (!Arr || !Arr->IsArray() || Arr->AsArray().size() > 256) return MakeError(ErrorCode::Invalid, "players.members invalid");
		PlayerList L;
		const Value* Open = json::GetOptional(V, "open");
		if (Open && !Open->IsBool()) return MakeError(ErrorCode::Invalid, "players.open must be a boolean");
		for (const Value& MV : Arr->AsArray())
		{
			Member M;
			SW_ASSIGN(M.PlayerId, GetText(MV, "playerId", 128));
			SW_ASSIGN(M.DisplayName, GetText(MV, "displayName", 128));
			std::string RoleName;
			SW_ASSIGN(RoleName, json::GetString(MV, "role", 16));
			SW_ASSIGN(M.MemberRole, ParseRole(RoleName));
			L.Members.push_back(std::move(M));
		}
		// Files written before "open" existed: a member list meant restricted.
		L.bOpen = Open ? Open->AsBool() : L.Members.empty();
		SW_TRY(L.Validate());
		return L;
	}

	Status PlayerList::Validate() const
	{
		int Owners = 0;
		for (size_t i = 0; i < Members.size(); ++i)
		{
			if (Members[i].PlayerId.empty()) return MakeError(ErrorCode::Invalid, "member without player id");
			if (Members[i].MemberRole == Role::Owner) ++Owners;
			for (size_t j = i + 1; j < Members.size(); ++j)
			{
				if (Members[i].PlayerId == Members[j].PlayerId) return MakeError(ErrorCode::Invalid, "duplicate member");
			}
		}
		if (!Members.empty() && Owners == 0) return MakeError(ErrorCode::Invalid, "a world needs an owner");
		return {};
	}

	// ------------------------------------------------------------ Settings

	Value WorldSettings::ToJson() const
	{
		Value V;
		V.Set("name", Name);
		V.Set("syncIntervalMinutes", SyncIntervalMinutes);
		V.Set("hostMigration", HostMigration);
		V.Set("peerRecovery", PeerRecovery);
		V.Set("keepRevisions", KeepRevisions);
		V.Set("maxPlayers", MaxPlayers);
		json::Array P;
		for (const std::string& Id : PreferredHosts) P.push_back(Value(Id));
		V.Set("preferredHosts", Value(std::move(P)));
		return V;
	}

	Result<WorldSettings> WorldSettings::FromJson(const Value& V)
	{
		WorldSettings S;
		SW_ASSIGN(S.Name, GetText(V, "name", 128));
		SW_ASSIGN(S.SyncIntervalMinutes, GetNonNegative(V, "syncIntervalMinutes"));
		SW_ASSIGN(S.HostMigration, json::GetBool(V, "hostMigration"));
		SW_ASSIGN(S.PeerRecovery, json::GetBool(V, "peerRecovery"));
		SW_ASSIGN(S.KeepRevisions, GetNonNegative(V, "keepRevisions"));
		SW_ASSIGN(S.MaxPlayers, GetNonNegative(V, "maxPlayers"));
		if (const Value* P = ObjectField(V, "preferredHosts"))
		{
			if (!P->IsArray() || P->AsArray().size() > 64) return MakeError(ErrorCode::Invalid, "preferredHosts invalid");
			for (const Value& Id : P->AsArray())
			{
				if (!Id.IsString()) return MakeError(ErrorCode::Invalid, "preferredHosts entry invalid");
				S.PreferredHosts.push_back(Id.AsString());
			}
		}
		SW_TRY(S.Validate());
		return S;
	}

	Status WorldSettings::Validate() const
	{
		if (SyncIntervalMinutes < 1 || SyncIntervalMinutes > 240) return MakeError(ErrorCode::Invalid, "sync interval must be 1..240 minutes");
		if (KeepRevisions < 5) return MakeError(ErrorCode::Invalid, "keep at least 5 revisions");
		if (MaxPlayers < 1 || MaxPlayers > 128) return MakeError(ErrorCode::Invalid, "maxPlayers out of range");
		return ValidateText("name", Name, 128);
	}
}
