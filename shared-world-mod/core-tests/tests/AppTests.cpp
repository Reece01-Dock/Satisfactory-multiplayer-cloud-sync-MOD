// Local world list, provider factory and GitHub collaborator invites.
#include "FakeGitHub.h"
#include "SharedWorldCore/App/LocalSettings.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Providers/GitHub.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	WorldEntry GitHubWorld(const std::string& Id = "our-factory")
	{
		WorldEntry W;
		W.WorldId = Id;
		W.DisplayName = "Our Factory";
		W.Provider.Kind = ProviderKind::GitHub;
		W.Provider.Owner = "owner";
		W.Provider.Repo = "repo";
		W.AddedAt = StartTime;
		W.LastPlayedAt = StartTime + Seconds(60);
		return W;
	}
}

SW_TEST(App_LocalSettingsRoundTripHoldsNoSecrets)
{
	const std::string Path = file::Join(TempDir(), "nested/settings.json");
	auto Empty = LoadLocalSettings(Path);
	ASSERT_OK(Empty);
	EXPECT_TRUE(Empty->Worlds.empty());

	LocalSettings S;
	S.GitHubLogin = "octo-player";
	ASSERT_OK(S.Upsert(GitHubWorld()));
	WorldEntry Folder;
	Folder.WorldId = "lan-world";
	Folder.DisplayName = "LAN";
	Folder.Provider.Kind = ProviderKind::Folder;
	Folder.Provider.FolderPath = "\\\\nas\\games\\shared";
	ASSERT_OK(S.Upsert(Folder));
	WorldEntry Renamed = GitHubWorld();
	Renamed.DisplayName = "Renamed";
	ASSERT_OK(S.Upsert(Renamed)); // replaces, does not duplicate
	ASSERT_OK(SaveLocalSettings(Path, S));

	auto Back = LoadLocalSettings(Path);
	ASSERT_OK(Back);
	ASSERT_EQ(Back->Worlds.size(), size_t(2));
	EXPECT_EQ(Back->Find("our-factory")->DisplayName, std::string("Renamed"));
	EXPECT_EQ(Back->Find("our-factory")->LastPlayedAt, StartTime + Seconds(60));
	EXPECT_EQ(Back->Find("lan-world")->Provider.FolderPath, std::string("\\\\nas\\games\\shared"));
	EXPECT_EQ(Back->GitHubLogin, std::string("octo-player"));
	const std::string Raw = file::ReadAll(Path).Value();
	EXPECT_TRUE(Raw.find("token") == std::string::npos);
	EXPECT_TRUE(Back->Remove("lan-world"));
	EXPECT_TRUE(!Back->Remove("lan-world"));
}

SW_TEST(App_LocalSettingsRejectsHostileEntries)
{
	LocalSettings S;
	WorldEntry W = GitHubWorld("../escape");
	EXPECT_ERR(S.Upsert(W), ErrorCode::Invalid);
	W = GitHubWorld();
	W.Provider.Repo = "..";
	EXPECT_ERR(S.Upsert(W), ErrorCode::Invalid);
	W.Provider.Repo = "repo/../../other";
	EXPECT_ERR(S.Upsert(W), ErrorCode::Invalid);
	W.Provider.Kind = ProviderKind::Folder;
	for (const char* Bad : {"relative/path", "C:\\games\\..\\Windows", "/srv/../etc", ""})
	{
		W.Provider.FolderPath = Bad;
		EXPECT_ERR(S.Upsert(W), ErrorCode::Invalid);
	}
	W.Provider.FolderPath = "C:\\Users\\me\\Shared Worlds";
	ASSERT_OK(S.Upsert(W));

	// A damaged file is reported, never silently replaced by empty settings.
	const std::string Path = file::Join(TempDir(), "settings.json");
	ASSERT_OK(file::WriteAtomic(Path, "{not json"));
	EXPECT_ERR(LoadLocalSettings(Path), ErrorCode::Corrupt);
	ASSERT_OK(file::WriteAtomic(Path, R"({"version":1,"githubLogin":"","worlds":[{"worldId":"a..b/c","name":"x","provider":{"kind":"github","owner":"o","repo":"r"},"addedAt":"2026-09-27T12:00:00Z","lastPlayedAt":"2026-09-27T12:00:00Z"}]})"));
	EXPECT_ERR(LoadLocalSettings(Path), ErrorCode::Corrupt);
	ASSERT_OK(file::WriteAtomic(Path, R"({"version":2,"worlds":[]})"));
	EXPECT_ERR(LoadLocalSettings(Path), ErrorCode::Unsupported);
	ASSERT_OK(file::WriteAtomic(Path, R"({"version":1,"githubLogin":"","worlds":[{"worldId":"w","name":"x","provider":{"kind":"ftp"},"addedAt":"2026-09-27T12:00:00Z","lastPlayedAt":"2026-09-27T12:00:00Z"}]})"));
	EXPECT_ERR(LoadLocalSettings(Path), ErrorCode::Unsupported); // written by a newer mod
}

SW_TEST(App_FolderProviderOpensWorkingStorage)
{
	WorldEntry W;
	W.WorldId = "our-factory";
	W.Provider.Kind = ProviderKind::Folder;
	W.Provider.FolderPath = TempDir();
	auto S = OpenWorldStorage(W, ProviderEnvironment{});
	ASSERT_OK(S);
	auto Clock = std::make_shared<FakeClock>(StartTime);
	ASSERT_OK(CreateTestWorld(S->Repository, Clock));
	// A second PC opening the same folder sees the same world.
	auto Again = OpenWorldStorage(W, ProviderEnvironment{});
	ASSERT_OK(Again);
	WorldStore Store(Again->Repository, TestWorldId, Clock, Logger());
	ASSERT_OK(Store.Load());
	EXPECT_TRUE(file::Exists(file::Join(W.Provider.FolderPath, "our-factory/repo")));
}

SW_TEST(App_GitHubProviderUsesCredentialStoreLive)
{
	auto Fake = std::make_shared<FakeGitHub>();
	auto Creds = std::make_shared<MemoryCredentialStore>();
	ProviderEnvironment Env;
	Env.Http = Fake;
	Env.Credentials = Creds;
	Env.GitHubApiBase = "https://api.fake";
	Env.GitHubUploadBase = "https://uploads.fake";
	EXPECT_ERR(OpenWorldStorage(GitHubWorld(), ProviderEnvironment{}), ErrorCode::Unsupported);
	auto S = OpenWorldStorage(GitHubWorld(), Env);
	ASSERT_OK(S);
	EXPECT_ERR(S->Repository->Head(), ErrorCode::Unauthorized); // not signed in
	ASSERT_OK(Creds->Write(GitHubCredentialKey, Fake->Token));
	auto Clock = std::make_shared<FakeClock>(StartTime);
	ASSERT_OK(CreateTestWorld(S->Repository, Clock));
	ASSERT_OK(Creds->Remove(GitHubCredentialKey)); // sign out takes effect immediately
	EXPECT_ERR(S->Repository->Head(), ErrorCode::Unauthorized);
}

SW_TEST(App_GitHubInviteCollaborator)
{
	auto Fake = std::make_shared<FakeGitHub>();
	GitHubConfig C;
	C.Owner = "owner";
	C.Repo = "repo";
	C.WorldId = "our-factory";
	C.ApiBase = "https://api.fake";
	C.UploadBase = "https://uploads.fake";
	const std::string Tok = Fake->Token;
	C.Token = [Tok]() -> Result<std::string> { return Tok; };
	auto First = InviteGitHubCollaborator(Fake, C, "friend-1");
	ASSERT_OK(First);
	EXPECT_TRUE(*First);
	auto Second = InviteGitHubCollaborator(Fake, C, "friend-1");
	ASSERT_OK(Second);
	EXPECT_TRUE(!*Second);
	EXPECT_ERR(InviteGitHubCollaborator(Fake, C, "../admin"), ErrorCode::Invalid);
	EXPECT_ERR(InviteGitHubCollaborator(Fake, C, "a.b"), ErrorCode::Invalid);
}

SW_TEST(App_CheckpointIntervalIsClampedAndRoundTrips)
{
	EXPECT_EQ(LocalSettings::ClampCheckpointSeconds(0), LocalSettings::MinCheckpointSeconds);
	EXPECT_EQ(LocalSettings::ClampCheckpointSeconds(-5), LocalSettings::MinCheckpointSeconds);
	EXPECT_EQ(LocalSettings::ClampCheckpointSeconds(1000000), LocalSettings::MaxCheckpointSeconds);
	EXPECT_EQ(LocalSettings::ClampCheckpointSeconds(600), 600);

	LocalSettings S;
	EXPECT_EQ(S.CheckpointIntervalSeconds, LocalSettings::DefaultCheckpointSeconds);
	S.CheckpointIntervalSeconds = 900;
	auto Back = LocalSettings::FromJson(S.ToJson());
	ASSERT_OK(Back);
	EXPECT_EQ(Back->CheckpointIntervalSeconds, 900);

	// A hand-edited file cannot turn checkpoints off or make them spam.
	json::Value V = S.ToJson();
	V.Set("checkpointIntervalSeconds", int64_t(1));
	auto Low = LocalSettings::FromJson(V);
	ASSERT_OK(Low);
	EXPECT_EQ(Low->CheckpointIntervalSeconds, LocalSettings::MinCheckpointSeconds);

	// Files written before the field existed keep the default.
	std::string Text = json::Serialize(S.ToJson());
	const size_t At = Text.find("\"checkpointIntervalSeconds\":900,");
	ASSERT_TRUE(At != std::string::npos);
	Text.erase(At, std::string("\"checkpointIntervalSeconds\":900,").size());
	auto Parsed = json::Parse(Text);
	ASSERT_OK(Parsed);
	auto Legacy = LocalSettings::FromJson(*Parsed);
	ASSERT_OK(Legacy);
	EXPECT_EQ(Legacy->CheckpointIntervalSeconds, LocalSettings::DefaultCheckpointSeconds);
}
