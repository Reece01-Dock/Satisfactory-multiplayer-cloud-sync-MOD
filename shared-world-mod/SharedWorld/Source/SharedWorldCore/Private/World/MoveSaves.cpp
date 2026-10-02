#include "SharedWorldCore/World/MoveSaves.h"

#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	Result<MoveSavesResult> MoveWorldSaves(LeaseManager& Leases, IObjectStore& From, IObjectStore& To,
		const WorldInfo::SaveStorageInfo& Target, const Identity& Me, const std::string& TempDir,
		const std::function<void(size_t Done, size_t Total)>& Progress)
	{
		SW_TRY(Me.Validate());
		SW_TRY(file::CreateDirectories(TempDir));

		// Hold the lease for the whole move: no revision can be published while files are copied.
		SystemRandom Rng;
		auto Acq = Leases.Acquire(Me, Rng.Hex(16));
		if (!Acq) return Acq.Err();
		if (!Acq->Token)
		{
			return MakeError(ErrorCode::Conflict, "Someone is playing this world right now. Move its saves when nobody is hosting it.");
		}
		const LeaseToken Tok = *Acq->Token;
		auto Finish = [&](Result<MoveSavesResult> R) -> Result<MoveSavesResult>
		{
			const Status Rel = Leases.Release(Tok);
			if (!Rel && !Rel.Is(ErrorCode::Fenced)) Leases.Store().Log().Warn("MoveSavesReleaseFailed", {{"error", Rel.Err().Describe()}});
			return R;
		};

		auto Ids = From.List();
		if (!Ids) return Finish(Ids.Err().Wrap("list the current save files"));

		MoveSavesResult Out;
		const size_t Total = Ids->size();
		size_t Done = 0;
		for (const std::string& Id : *Ids)
		{
			auto There = To.Has(Id);
			if (!There) return Finish(There.Err().Wrap("check the new storage"));
			if (*There)
			{
				++Out.Present;
			}
			else
			{
				const std::string Tmp = file::Join(TempDir, Id + ".move");
				if (Status G = From.Get(Id, Tmp); !G)
				{
					(void)file::Remove(Tmp);
					return Finish(G.Err().Wrap("download a save file"));
				}
				const Status P = To.Put(Id, Tmp); // Put verifies the content hash
				(void)file::Remove(Tmp);
				if (!P) return Finish(P.Err().Wrap("upload a save file"));
				auto Check = To.Has(Id);
				if (!Check || !*Check) return Finish(MakeError(ErrorCode::Corrupt, "a save file did not arrive in the new storage"));
				++Out.Copied;
			}
			++Done;
			if (Progress) Progress(Done, Total);
		}

		// Every file is in place: switch world.json (compare-and-swap; the lease is still ours).
		auto Written = Leases.Store().UpdateDocument(Paths::WorldInfo,
			[Target](const std::string& Current) -> Result<std::string>
			{
				json::Value V;
				SW_ASSIGN(V, json::Parse(Current));
				WorldInfo Info;
				SW_ASSIGN(Info, WorldInfo::FromJson(V));
				Info.SaveStorage = Target;
				return json::Serialize(Info.ToJson(), 2);
			},
			Target.IsDefault() ? std::string("move saves back to the world storage") : "move saves to " + Target.Label);
		if (!Written) return Finish(Written.Err().Wrap("record the new save location"));
		return Finish(Out);
	}
}
