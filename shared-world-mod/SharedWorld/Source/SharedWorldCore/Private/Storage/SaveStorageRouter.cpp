#include "SharedWorldCore/Storage/SaveStorageRouter.h"

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Util/Json.h"

namespace sw
{
	SaveStorageRouter::SaveStorageRouter(SaveStorageRouterConfig Config) : Cfg(std::move(Config)) {}

	Result<std::shared_ptr<IObjectStore>> SaveStorageRouter::Resolve()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Resolved) return Resolved;

		// What does the world itself say? (world.json is written before the first save is uploaded.)
		std::string Backend = Cfg.KnownBackend;
		std::string Label = Cfg.KnownLabel;
		bool bFromWorld = false;
		if (Cfg.Repository)
		{
			auto Head = Cfg.Repository->Head();
			if (Head && !Head->empty())
			{
				auto Text = Cfg.Repository->ReadFile(*Head, Paths::WorldInfo);
				if (Text)
				{
					auto V = json::Parse(*Text);
					if (!V) return V.Err().Wrap("read world.json");
					auto Info = WorldInfo::FromJson(*V);
					if (!Info) return Info.Err();
					Backend = Info->SaveStorage.Backend;
					Label = Info->SaveStorage.Label;
					bFromWorld = true;
				}
				else if (!Text.Is(ErrorCode::NotFound))
				{
					return Text.Err(); // offline etc.: try again next time, never guess
				}
			}
			else if (!Head && !Head.Is(ErrorCode::NotFound) && !Head.Is(ErrorCode::NoWorld))
			{
				return Head.Err();
			}
		}
		if (bFromWorld && Cfg.OnSeen) Cfg.OnSeen(Backend, Label);

		std::shared_ptr<IObjectStore> Store;
		std::string Name;
		if (Backend.empty())
		{
			Store = Cfg.Default;
			Name = Cfg.Default ? Cfg.Default->Describe() : std::string("none");
		}
		else
		{
			const std::string Shown = Label.empty() ? Backend : Label;
			if (Cfg.SaveRemote.empty())
			{
				return MakeError(ErrorCode::Unsupported,
					"This world's saves are stored on " + Shown + ". Link " + Shown + " in the world's Storage tab " + SaveStorageNotLinkedPhrase + ".");
			}
			if (!Cfg.OpenRemote)
			{
				return MakeError(ErrorCode::Unsupported, "This world's saves are stored on " + Shown + ", and the storage engine isn't available in this build.");
			}
			std::string Fs = Cfg.SaveRemote;
			if (!Fs.empty() && Fs.back() != ':' && Fs.back() != '/') Fs += '/';
			Fs += Cfg.WorldId;
			auto Opened = Cfg.OpenRemote(Fs);
			if (!Opened) return Opened.Err();
			Store = *Opened;
			Name = Shown + " (" + Fs + ")";
		}
		if (!Store) return MakeError(ErrorCode::Unsupported, "no save storage");
		// Only an answer based on world.json is final; one based on the cached marker is re-checked next time.
		if (bFromWorld)
		{
			Resolved = Store;
			ResolvedName = Name;
		}
		return Store;
	}

	Result<bool> SaveStorageRouter::Has(const std::string& Sha256)
	{
		auto S = Resolve();
		if (!S) return S.Err();
		return (*S)->Has(Sha256);
	}

	Status SaveStorageRouter::Put(const std::string& Sha256, const std::string& LocalPath)
	{
		auto S = Resolve();
		if (!S) return S.Err();
		return (*S)->Put(Sha256, LocalPath);
	}

	Status SaveStorageRouter::Get(const std::string& Sha256, const std::string& DestPath)
	{
		auto S = Resolve();
		if (!S) return S.Err();
		return (*S)->Get(Sha256, DestPath);
	}

	Status SaveStorageRouter::Remove(const std::string& Sha256)
	{
		auto S = Resolve();
		if (!S) return S.Err();
		return (*S)->Remove(Sha256);
	}

	Result<std::vector<std::string>> SaveStorageRouter::List()
	{
		auto S = Resolve();
		if (!S) return S.Err();
		return (*S)->List();
	}

	std::string SaveStorageRouter::Describe() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!ResolvedName.empty()) return ResolvedName;
		return Cfg.Default ? Cfg.Default->Describe() : std::string("save storage (not resolved yet)");
	}
}
