#include "SharedWorldCore/App/LocalSettings.h"

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Providers/GitHub.h"
#include "SharedWorldCore/Storage/FileStorage.h"
#include "SharedWorldCore/Util/FileUtil.h"

namespace sw
{
	using json::Value;

	const char* ToString(ProviderKind K)
	{
		switch (K)
		{
		case ProviderKind::GitHub: return "github";
		case ProviderKind::Folder: return "folder";
		}
		return "unknown";
	}

	namespace
	{
		bool IsAbsolute(const std::string& P)
		{
			if (!P.empty() && P[0] == '/') return true;
			if (P.size() >= 3 && ((P[0] >= 'A' && P[0] <= 'Z') || (P[0] >= 'a' && P[0] <= 'z')) && P[1] == ':' && (P[2] == '\\' || P[2] == '/')) return true;
			return P.size() >= 3 && P[0] == '\\' && P[1] == '\\'; // UNC share
		}

		Status ValidateFolder(const std::string& P)
		{
			if (P.empty() || P.size() > 1024 || !IsAbsolute(P)) return MakeError(ErrorCode::Invalid, "storage folder must be an absolute path");
			size_t Start = 0;
			for (size_t i = 0; i <= P.size(); ++i)
			{
				if (i < P.size() && static_cast<unsigned char>(P[i]) < 0x20) return MakeError(ErrorCode::Invalid, "storage folder contains control characters");
				if (i == P.size() || P[i] == '/' || P[i] == '\\')
				{
					if (P.compare(Start, i - Start, "..") == 0 && i - Start == 2) return MakeError(ErrorCode::Invalid, "storage folder must not contain '..'");
					Start = i + 1;
				}
			}
			return {};
		}

		Result<std::string> Text(const Value& V, const char* Key, size_t Max)
		{
			std::string S;
			SW_ASSIGN(S, json::GetString(V, Key, Max));
			for (unsigned char C : S)
			{
				if (C < 0x20) return MakeError(ErrorCode::Invalid, std::string(Key) + " contains control characters");
			}
			return S;
		}
	}

	Status ProviderConfig::Validate() const
	{
		switch (Kind)
		{
		case ProviderKind::GitHub:
			if (!ValidGitHubName(Owner) || !ValidGitHubName(Repo)) return MakeError(ErrorCode::Invalid, "invalid GitHub owner/repository name");
			return {};
		case ProviderKind::Folder:
			return ValidateFolder(FolderPath);
		}
		return MakeError(ErrorCode::Invalid, "unknown storage provider");
	}

	Value ProviderConfig::ToJson() const
	{
		Value V;
		V.Set("kind", ToString(Kind));
		if (Kind == ProviderKind::GitHub)
		{
			V.Set("owner", Owner);
			V.Set("repo", Repo);
		}
		else
		{
			V.Set("path", FolderPath);
		}
		return V;
	}

	Result<ProviderConfig> ProviderConfig::FromJson(const Value& V)
	{
		ProviderConfig C;
		std::string Kind;
		SW_ASSIGN(Kind, json::GetString(V, "kind", 16));
		if (Kind == "github")
		{
			C.Kind = ProviderKind::GitHub;
			SW_ASSIGN(C.Owner, Text(V, "owner", 100));
			SW_ASSIGN(C.Repo, Text(V, "repo", 100));
		}
		else if (Kind == "folder")
		{
			C.Kind = ProviderKind::Folder;
			SW_ASSIGN(C.FolderPath, Text(V, "path", 1024));
		}
		else
		{
			return MakeError(ErrorCode::Unsupported, "unknown storage provider '" + Kind + "'");
		}
		SW_TRY(C.Validate());
		return C;
	}

	const WorldEntry* LocalSettings::Find(const std::string& WorldId) const
	{
		for (const WorldEntry& W : Worlds)
		{
			if (W.WorldId == WorldId) return &W;
		}
		return nullptr;
	}

	Status LocalSettings::Upsert(const WorldEntry& Entry)
	{
		SW_TRY(ValidateWorldId(Entry.WorldId));
		SW_TRY(Entry.Provider.Validate());
		if (Entry.DisplayName.size() > 128) return MakeError(ErrorCode::Invalid, "world name too long");
		for (WorldEntry& W : Worlds)
		{
			if (W.WorldId == Entry.WorldId)
			{
				W = Entry;
				return {};
			}
		}
		if (Worlds.size() >= 256) return MakeError(ErrorCode::Invalid, "too many worlds");
		Worlds.push_back(Entry);
		return {};
	}

	bool LocalSettings::Remove(const std::string& WorldId)
	{
		for (size_t i = 0; i < Worlds.size(); ++i)
		{
			if (Worlds[i].WorldId == WorldId)
			{
				Worlds.erase(Worlds.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	Value LocalSettings::ToJson() const
	{
		Value V;
		V.Set("version", int64_t(CurrentVersion));
		V.Set("githubLogin", GitHubLogin);
		json::Array A;
		for (const WorldEntry& W : Worlds)
		{
			Value E;
			E.Set("worldId", W.WorldId);
			E.Set("name", W.DisplayName);
			E.Set("provider", W.Provider.ToJson());
			E.Set("addedAt", FormatTime(W.AddedAt));
			E.Set("lastPlayedAt", FormatTime(W.LastPlayedAt));
			A.push_back(std::move(E));
		}
		V.Set("worlds", Value(std::move(A)));
		return V;
	}

	Result<LocalSettings> LocalSettings::FromJson(const Value& V)
	{
		int64_t Version;
		SW_ASSIGN(Version, json::GetInt(V, "version"));
		if (Version != CurrentVersion) return MakeError(ErrorCode::Unsupported, "settings were written by a newer version of the mod");
		LocalSettings S;
		SW_ASSIGN(S.GitHubLogin, Text(V, "githubLogin", 64));
		if (!S.GitHubLogin.empty() && !ValidGitHubName(S.GitHubLogin)) return MakeError(ErrorCode::Invalid, "invalid GitHub login");
		const Value* A = V.Find("worlds");
		if (!A || !A->IsArray()) return MakeError(ErrorCode::Invalid, "worlds missing");
		for (const Value& E : A->AsArray())
		{
			WorldEntry W;
			SW_ASSIGN(W.WorldId, json::GetString(E, "worldId", 64));
			SW_ASSIGN(W.DisplayName, Text(E, "name", 128));
			const Value* P = E.Find("provider");
			if (!P) return MakeError(ErrorCode::Invalid, "world without provider");
			SW_ASSIGN(W.Provider, ProviderConfig::FromJson(*P));
			std::string Added, Played;
			SW_ASSIGN(Added, json::GetString(E, "addedAt", 64));
			SW_ASSIGN(Played, json::GetString(E, "lastPlayedAt", 64));
			SW_ASSIGN(W.AddedAt, ParseTime(Added));
			SW_ASSIGN(W.LastPlayedAt, ParseTime(Played));
			if (S.Find(W.WorldId)) return MakeError(ErrorCode::Invalid, "duplicate world id in settings");
			SW_TRY(S.Upsert(W));
		}
		return S;
	}

	Result<LocalSettings> LoadLocalSettings(const std::string& Path)
	{
		auto Raw = file::ReadAll(Path, int64_t(4) << 20);
		if (!Raw)
		{
			if (Raw.Is(ErrorCode::NotFound)) return LocalSettings{};
			return Raw.Err();
		}
		auto V = json::Parse(*Raw);
		if (!V) return MakeError(ErrorCode::Corrupt, "settings file is damaged: " + V.Err().Message);
		auto S = LocalSettings::FromJson(*V);
		if (!S && !S.Is(ErrorCode::Unsupported)) return MakeError(ErrorCode::Corrupt, "settings file is damaged: " + S.Err().Message);
		return S;
	}

	Status SaveLocalSettings(const std::string& Path, const LocalSettings& Settings)
	{
		SW_TRY(file::CreateDirectories(file::Parent(Path)));
		return file::WriteAtomic(Path, json::Serialize(Settings.ToJson(), 2));
	}

	Result<WorldStorage> OpenWorldStorage(const WorldEntry& Entry, const ProviderEnvironment& Env)
	{
		SW_TRY(ValidateWorldId(Entry.WorldId));
		SW_TRY(Entry.Provider.Validate());
		WorldStorage Out;
		switch (Entry.Provider.Kind)
		{
		case ProviderKind::GitHub:
		{
			if (!Env.Http || !Env.Credentials) return MakeError(ErrorCode::Unsupported, "GitHub storage is not available");
			GitHubConfig C;
			C.Owner = Entry.Provider.Owner;
			C.Repo = Entry.Provider.Repo;
			C.WorldId = Entry.WorldId;
			C.ApiBase = Env.GitHubApiBase;
			C.UploadBase = Env.GitHubUploadBase;
			// Read on every request so signing out takes effect immediately.
			std::shared_ptr<ICredentialStore> Creds = Env.Credentials;
			C.Token = [Creds]() -> Result<std::string>
			{
				auto T = Creds->Read(GitHubCredentialKey);
				if (!T && T.Is(ErrorCode::NotFound)) return MakeError(ErrorCode::Unauthorized, "no GitHub account connected");
				return T;
			};
			Out.Repository = std::make_shared<GitHubRepository>(Env.Http, C);
			Out.Objects = std::make_shared<GitHubReleaseObjectStore>(Env.Http, C);
			return Out;
		}
		case ProviderKind::Folder:
		{
			const std::string Root = file::Join(Entry.Provider.FolderPath, Entry.WorldId);
			Out.Repository = std::make_shared<FileRepository>(file::Join(Root, "repo"));
			Out.Objects = std::make_shared<FileObjectStore>(file::Join(Root, "objects"));
			return Out;
		}
		}
		return MakeError(ErrorCode::Invalid, "unknown storage provider");
	}
}
