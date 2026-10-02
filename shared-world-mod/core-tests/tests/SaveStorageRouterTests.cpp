// SaveStorageRouter: a world's save files go where world.json says, without touching the repository.

#include <memory>
#include <string>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Storage/SaveStorageRouter.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	std::shared_ptr<MemoryRepository> MakeWorld(const WorldInfo::SaveStorageInfo& Saves)
	{
		auto Repo = std::make_shared<MemoryRepository>();
		WorldStore Store(Repo, TestWorldId, std::make_shared<FakeClock>(StartTime), Logger());
		NewWorld W = TestWorld();
		W.Info.SaveStorage = Saves;
		auto R = Store.Create(W);
		return R ? Repo : nullptr;
	}

	/** Puts one object into Store and returns its id. */
	std::string PutObject(IObjectStore& Store, const std::string& Content)
	{
		const std::string Path = file::Join(swtest::TempDir(), "obj.bin");
		if (!file::WriteAtomic(Path, Content)) return {};
		const std::string Id = Sha256::HexOf(Content);
		return Store.Put(Id, Path) ? Id : std::string();
	}

	struct Fixture
	{
		std::shared_ptr<MemoryObjectStore> Default = std::make_shared<MemoryObjectStore>();
		std::shared_ptr<MemoryObjectStore> Remote = std::make_shared<MemoryObjectStore>();
		std::string OpenedFs;
		std::string SeenBackend, SeenLabel;
		int Opens = 0;

		SaveStorageRouterConfig Config(std::shared_ptr<IWorldRepository> Repo, const std::string& Link)
		{
			SaveStorageRouterConfig C;
			C.WorldId = TestWorldId;
			C.Repository = std::move(Repo);
			C.Default = Default;
			C.SaveRemote = Link;
			C.OpenRemote = [this](const std::string& Fs) -> Result<std::shared_ptr<IObjectStore>>
			{
				++Opens;
				OpenedFs = Fs;
				return std::shared_ptr<IObjectStore>(Remote);
			};
			C.OnSeen = [this](const std::string& B, const std::string& L) { SeenBackend = B; SeenLabel = L; };
			return C;
		}
	};
}

SW_TEST(SaveRouter_DefaultWorldUsesRepositoryStore)
{
	auto Repo = MakeWorld({});
	ASSERT_TRUE(Repo != nullptr);
	Fixture F;
	const std::string Id = PutObject(*F.Default, "default save");
	ASSERT_TRUE(!Id.empty());
	SaveStorageRouter Router(F.Config(Repo, "sw-dropbox-1:SharedWorlds")); // a stray link must not matter
	auto Has = Router.Has(Id);
	ASSERT_OK(Has);
	EXPECT_TRUE(*Has);
	EXPECT_EQ(F.Opens, 0);
	EXPECT_EQ(F.SeenBackend, std::string());
}

SW_TEST(SaveRouter_LinkedWorldUsesProviderFolder)
{
	auto Repo = MakeWorld({"dropbox", "Dropbox"});
	ASSERT_TRUE(Repo != nullptr);
	Fixture F;
	const std::string Id = PutObject(*F.Remote, "save on dropbox");
	ASSERT_TRUE(!Id.empty());
	SaveStorageRouter Router(F.Config(Repo, "sw-dropbox-1:SharedWorlds"));
	auto Has = Router.Has(Id);
	ASSERT_OK(Has);
	EXPECT_TRUE(*Has);
	EXPECT_EQ(F.OpenedFs, std::string("sw-dropbox-1:SharedWorlds/") + TestWorldId);
	EXPECT_EQ(F.SeenBackend, std::string("dropbox"));
	EXPECT_EQ(F.SeenLabel, std::string("Dropbox"));
	// Resolved once, then cached.
	ASSERT_OK(Router.Has(Id));
	EXPECT_EQ(F.Opens, 1);
	// Nothing leaked into the repository's own store.
	auto DefaultList = F.Default->List();
	ASSERT_OK(DefaultList);
	EXPECT_EQ(DefaultList->size(), static_cast<size_t>(0));
}

SW_TEST(SaveRouter_TopLevelLinkHasNoDoubleSeparator)
{
	auto Repo = MakeWorld({"drive", "Google Drive"});
	ASSERT_TRUE(Repo != nullptr);
	Fixture F;
	SaveStorageRouter Router(F.Config(Repo, "sw-drive-1:"));
	ASSERT_OK(Router.List());
	EXPECT_EQ(F.OpenedFs, std::string("sw-drive-1:") + TestWorldId);
}

SW_TEST(SaveRouter_UnlinkedWorldAsksToLinkInsteadOfGuessing)
{
	auto Repo = MakeWorld({"dropbox", "Dropbox"});
	ASSERT_TRUE(Repo != nullptr);
	Fixture F;
	PutObject(*F.Default, "must not be used");
	SaveStorageRouter Router(F.Config(Repo, ""));
	auto Has = Router.Has(Sha256::HexOf("anything"));
	EXPECT_ERR(Has, ErrorCode::Unsupported);
	EXPECT_TRUE(Has.Err().Message.find(SaveStorageNotLinkedPhrase) != std::string::npos);
	EXPECT_TRUE(Has.Err().Message.find("Dropbox") != std::string::npos);
	EXPECT_EQ(F.Opens, 0);
}

SW_TEST(SaveRouter_WorldInfoSaveStorageRoundTrip)
{
	WorldInfo W = TestWorld().Info;
	EXPECT_TRUE(W.ToJson().Find("saveStorage") == nullptr); // default worlds write nothing new
	W.SaveStorage = {"onedrive", "Microsoft OneDrive"};
	auto Back = WorldInfo::FromJson(W.ToJson());
	ASSERT_OK(Back);
	EXPECT_EQ(Back->SaveStorage.Backend, std::string("onedrive"));
	EXPECT_EQ(Back->SaveStorage.Label, std::string("Microsoft OneDrive"));
}
