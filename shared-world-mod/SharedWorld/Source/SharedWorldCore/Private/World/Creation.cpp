#include "SharedWorldCore/World/Creation.h"

#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	Result<WorldInfo> CreateSharedWorld(LeaseManager& Leases, SyncEngine& Sync, const CreateWorldParams& P, TimeMs StableQuiet)
	{
		WorldStore& Store = Leases.Store();
		SW_TRY(P.Creator.Validate());
		if (auto V = save::ValidateFile(P.SourceSavePath); !V)
		{
			return V.Err().Wrap("the selected save cannot be converted");
		}
		NewWorld W;
		W.Info.WorldId = Store.WorldId();
		W.Info.Name = P.Name;
		W.Info.CreatedBy = P.Creator;
		W.Info.CreatedAt = Store.Clock().Now();
		W.Info.OriginalSaveName = P.OriginalSaveName;
		W.Info.GameBuild = P.Versions.GameBuild;
		W.Info.ModVersion = P.Versions.ModVersion;
		W.Info.RequiredMods = P.Versions.InstalledMods;
		if (P.bRestrictToMembers)
		{
			W.Players.Members.push_back(Member{P.Creator.PlayerId, P.Creator.DisplayName, Role::Owner});
		}
		W.Settings = P.Settings;
		if (W.Settings.Name.empty()) W.Settings.Name = P.Name;

		auto Created = Store.Create(W);
		if (!Created && !Created.Is(ErrorCode::AlreadyExists))
		{
			return Created.Err().Wrap("create Shared World");
		}
		StateSnapshot Snap;
		SW_ASSIGN(Snap, Store.Load());
		if (Snap.State.Head)
		{
			if (Created.Ok()) return MakeError(ErrorCode::Invalid, "new world unexpectedly has a save");
			return MakeError(ErrorCode::AlreadyExists, "this Shared World already exists");
		}
		SystemRandom Rng;
		auto Acq = Leases.Acquire(P.Creator, Rng.Hex(16));
		if (!Acq) return Acq.Err();
		if (!Acq->Token) return MakeError(ErrorCode::Conflict, "someone else is initialising this Shared World");
		LeaseToken Tok = *Acq->Token;
		UploadOptions O;
		O.Reason = Reason::Import;
		O.GameBuild = P.Versions.GameBuild;
		O.ModVersion = P.Versions.ModVersion;
		O.StableQuiet = StableQuiet;
		auto Up = Sync.Upload(Tok, P.SourceSavePath, O);
		const Status Rel = Leases.Release(Tok);
		if (!Up) return Up.Err().Wrap("upload the first revision");
		if (!Rel && !Rel.Is(ErrorCode::Fenced)) Store.Log().Warn("ReleaseFailed", {{"world", Store.WorldId()}, {"error", Rel.Err().Describe()}});
		auto Info = Store.LoadInfo(Store.Load().Ok() ? Store.Load()->CommitId : Snap.CommitId);
		return Info;
	}
}
