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

namespace sw
{
	namespace
	{
		/** Every file in the tree at Commit (recursive). */
		Status CollectTree(IWorldRepository& Repo, const std::string& Commit, const std::string& Dir, std::vector<FileChange>& Out)
		{
			std::vector<std::string> Names;
			SW_ASSIGN(Names, Repo.ListDirectory(Commit, Dir));
			for (const std::string& Name : Names)
			{
				const std::string Path = Dir.empty() ? Name : Dir + "/" + Name;
				auto Text = Repo.ReadFile(Commit, Path);
				if (Text)
				{
					Out.push_back(FileChange{Path, *Text});
				}
				else
				{
					// Not readable as a file: it is a directory (providers report that differently), or a real error.
					auto Sub = Repo.ListDirectory(Commit, Path);
					if (!Sub || Sub->empty()) return Text.Err();
					SW_TRY(CollectTree(Repo, Commit, Path, Out));
				}
			}
			return {};
		}
	}

	Result<MoveSavesResult> MoveWholeWorld(LeaseManager& FromLeases, IObjectStore& FromObjects,
		IWorldRepository& ToRepo, IObjectStore& ToObjects, const WorldInfo::SaveStorageInfo& Target,
		const Identity& Me, const std::string& TempDir, const std::function<void(size_t Done, size_t Total)>& Progress)
	{
		SW_TRY(Me.Validate());
		SW_TRY(file::CreateDirectories(TempDir));
		if (auto Existing = ToRepo.Head(); Existing)
		{
			return MakeError(ErrorCode::AlreadyExists, "A Shared World already exists in that folder.");
		}
		else if (!Existing.Is(ErrorCode::NotFound))
		{
			return Existing.Err().Wrap("check the new storage");
		}

		SystemRandom Rng;
		auto Acq = FromLeases.Acquire(Me, Rng.Hex(16));
		if (!Acq) return Acq.Err();
		if (!Acq->Token)
		{
			return MakeError(ErrorCode::Conflict, "Someone is playing this world right now. Move it when nobody is hosting it.");
		}
		const LeaseToken Tok = *Acq->Token;
		auto Finish = [&](Result<MoveSavesResult> R) -> Result<MoveSavesResult>
		{
			const Status Rel = FromLeases.Release(Tok);
			if (!Rel && !Rel.Is(ErrorCode::Fenced)) FromLeases.Store().Log().Warn("MoveWorldReleaseFailed", {{"error", Rel.Err().Describe()}});
			return R;
		};

		// 1. Save objects.
		MoveSavesResult Out;
		auto Ids = FromObjects.List();
		if (!Ids) return Finish(Ids.Err().Wrap("list the save files"));
		const size_t Total = Ids->size();
		size_t Done = 0;
		for (const std::string& Id : *Ids)
		{
			auto There = ToObjects.Has(Id);
			if (!There) return Finish(There.Err().Wrap("check the new storage"));
			if (*There) ++Out.Present;
			else
			{
				const std::string Tmp = file::Join(TempDir, Id + ".move");
				if (Status G = FromObjects.Get(Id, Tmp); !G) { (void)file::Remove(Tmp); return Finish(G.Err().Wrap("download a save file")); }
				const Status P = ToObjects.Put(Id, Tmp);
				(void)file::Remove(Tmp);
				if (!P) return Finish(P.Err().Wrap("upload a save file"));
				auto Check = ToObjects.Has(Id);
				if (!Check || !*Check) return Finish(MakeError(ErrorCode::Corrupt, "a save file did not arrive in the new storage"));
				++Out.Copied;
			}
			++Done;
			if (Progress) Progress(Done, Total);
		}

		// 2. Record files, read at the state we hold the lease on.
		auto Snap = FromLeases.Store().Load();
		if (!Snap) return Finish(Snap.Err().Wrap("read the world"));
		// The record layout is fixed (see Paths): world.json plus the state, revisions and recovery folders.
		std::vector<FileChange> Files;
		IWorldRepository& FromRepo = FromLeases.Store().Repository();
		auto InfoText = FromRepo.ReadFile(Snap->CommitId, Paths::WorldInfo);
		if (!InfoText) return Finish(InfoText.Err().Wrap("read the world"));
		Files.push_back(FileChange{Paths::WorldInfo, *InfoText});
		for (const char* Dir : {"state", Paths::RevisionsDir, Paths::RecoveryDir})
		{
			if (Status C = CollectTree(FromRepo, Snap->CommitId, Dir, Files); !C) return Finish(C.Err().Wrap("read the world"));
		}
		for (FileChange& F : Files)
		{
			if (F.Path == Paths::State)
			{
				// The new copy starts unhosted; the generation carries over so fencing continues.
				WorldState S = Snap->State;
				S.CurrentLease.reset();
				S.PendingHandoff.reset();
				S.StateVersion += 1;
				F.Content = EncodeState(S);
			}
			else if (F.Path == Paths::WorldInfo)
			{
				auto V = json::Parse(*F.Content);
				if (!V) return Finish(V.Err());
				auto Info = WorldInfo::FromJson(*V);
				if (!Info) return Finish(Info.Err());
				Info->SaveStorage = {}; // saves now live with the world
				Info->MovedTo = {};
				F.Content = json::Serialize(Info->ToJson(), 2);
			}
		}
		auto Created = ToRepo.Commit("", Files, "moved from " + FromLeases.Store().Repository().Describe());
		if (!Created) return Finish(Created.Err().Wrap("write the world to the new storage"));

		// 3. Freeze the old copy so nobody can continue it there.
		auto Marked = FromLeases.Store().UpdateDocument(Paths::WorldInfo,
			[Target](const std::string& Current) -> Result<std::string>
			{
				json::Value V;
				SW_ASSIGN(V, json::Parse(Current));
				WorldInfo Info;
				SW_ASSIGN(Info, WorldInfo::FromJson(V));
				Info.MovedTo = Target;
				return json::Serialize(Info.ToJson(), 2);
			},
			"moved to " + Target.Label);
		if (!Marked) return Finish(Marked.Err().Wrap("mark the old copy as moved"));
		return Finish(Out);
	}
}
