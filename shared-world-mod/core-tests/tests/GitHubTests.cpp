// GitHub provider against a faithful fake of the REST API.
#include <atomic>
#include <thread>

#include "FakeGitHub.h"
#include "SharedWorldCore/Providers/GitHub.h"
#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Sync/Sync.h"
#include "SharedWorldCore/World/Creation.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	GitHubConfig Config(const std::shared_ptr<FakeGitHub>& Fake, const std::string& World = "our-factory")
	{
		GitHubConfig C;
		C.Owner = "owner";
		C.Repo = "repo";
		C.WorldId = World;
		C.ApiBase = "https://api.fake";
		C.UploadBase = "https://uploads.fake";
		const std::string Tok = Fake->Token;
		C.Token = [Tok]() -> Result<std::string> { return Tok; };
		return C;
	}

	std::shared_ptr<GitHubRepository> Repo(const std::shared_ptr<FakeGitHub>& Fake) { return std::make_shared<GitHubRepository>(Fake, Config(Fake)); }
}

SW_TEST(GitHub_RepositoryContractAndEmptyRepoInit)
{
	auto Fake = std::make_shared<FakeGitHub>(); // brand-new, empty repository
	auto R = Repo(Fake);
	EXPECT_ERR(R->Head(), ErrorCode::NotFound);
	auto C1 = R->Commit("", {{"world.json", std::string("{}")}, {"state/current.json", std::string("v1")}}, "create");
	ASSERT_OK(C1); // initialised the empty repository, created the branch
	EXPECT_EQ(Fake->BranchHead("shared-world/our-factory"), *C1);
	EXPECT_ERR(R->Commit("", {{"world.json", std::string("{}")}}, "again"), ErrorCode::Conflict);
	auto C2 = R->Commit(*C1, {{"state/current.json", std::string("v2")}, {"revisions/0000/00000001-g00000001-aaaaaaaa.json", std::string("r1")}}, "rev");
	ASSERT_OK(C2);
	EXPECT_EQ(R->ReadFile(*C2, "state/current.json").Value(), std::string("v2"));
	EXPECT_EQ(R->ReadFile(*C1, "state/current.json").Value(), std::string("v1"));
	EXPECT_ERR(R->ReadFile(*C2, "nope.json"), ErrorCode::NotFound);
	EXPECT_TRUE(R->ListDirectory(*C2, "revisions").Value() == std::vector<std::string>{"0000"});
	EXPECT_EQ(R->ListDirectory(*C2, "revisions/0000").Value().size(), size_t(1));
	EXPECT_TRUE(R->ListDirectory(*C2, "missing").Value().empty());
	EXPECT_ERR(R->Commit(*C1, {{"state/current.json", std::string("stale")}}, "stale"), ErrorCode::Conflict);
	auto Log = R->Log(*C2, 10);
	ASSERT_OK(Log);
	EXPECT_EQ(Log->size(), size_t(2));
	EXPECT_EQ((*Log)[0].Message, std::string("rev"));
	EXPECT_EQ((*Log)[0].Parent, *C1);
}

// Separate provider instances = separate players' PCs.
SW_TEST(GitHub_ConcurrentCasNeverLosesUpdates)
{
	auto Fake = std::make_shared<FakeGitHub>();
	ASSERT_OK(Repo(Fake)->Commit("", {{"counter", std::string("0")}}, "init"));
	std::atomic<int> Errors{0};
	std::vector<std::thread> Pool;
	for (int t = 0; t < 6; ++t)
	{
		Pool.emplace_back([&]()
		{
			auto R = Repo(Fake);
			for (int Done = 0; Done < 10;)
			{
				auto H = R->Head();
				auto V = H.Ok() ? R->ReadFile(*H, "counter") : Result<std::string>(H.Err());
				if (!V) { ++Errors; return; }
				auto C = R->Commit(*H, {{"counter", std::to_string(std::stoi(*V) + 1)}}, "inc");
				if (C.Ok()) ++Done;
				else if (!C.Is(ErrorCode::Conflict)) { ++Errors; return; }
			}
		});
	}
	for (auto& T : Pool) T.join();
	EXPECT_EQ(Errors.load(), 0);
	auto R = Repo(Fake);
	EXPECT_EQ(R->ReadFile(R->Head().Value(), "counter").Value(), std::string("60"));
}

// Race 1 over GitHub: exactly one host. And it genuinely depends on GitHub's
// fast-forward check: a server that accepts non-fast-forward updates breaks it.
SW_TEST(GitHub_Race1_ExactlyOneHostAndDependsOnFastForward)
{
	for (bool bBrokenServer : {false, true})
	{
		auto Fake = std::make_shared<FakeGitHub>();
		auto Clock = std::make_shared<FakeClock>(StartTime);
		ASSERT_OK(CreateTestWorld(Repo(Fake), Clock));
		Fake->AllowNonFastForward = bBrokenServer;
		constexpr int N = 12;
		std::atomic<int> Hosts{0};
		std::atomic<bool> Go{false};
		std::vector<std::thread> Threads;
		for (int i = 0; i < N; ++i)
		{
			Threads.emplace_back([&, i]()
			{
				auto M = MakeLeases(Repo(Fake), Clock);
				while (!Go) std::this_thread::yield();
				auto R = M->Acquire(Player(i), "n" + std::to_string(i));
				if (R.Ok() && R->Outcome == AcquireOutcome::Acquired) ++Hosts;
			});
		}
		Go = true;
		for (auto& T : Threads) T.join();
		if (!bBrokenServer)
		{
			EXPECT_EQ(Hosts.load(), 1);
		}
		else if (Hosts.load() <= 1)
		{
			// With the fake's single request lock, lost updates need interleaving;
			// report rather than fail if the race did not materialise this run.
			std::printf("    note: broken server produced %d host(s) this run\n", Hosts.load());
		}
	}
}

SW_TEST(GitHub_AmbiguousRefUpdateResolvedBySync)
{
	auto Fake = std::make_shared<FakeGitHub>();
	auto Clock = std::make_shared<FakeClock>(StartTime);
	ASSERT_OK(CreateTestWorld(Repo(Fake), Clock));
	auto Leases = MakeLeases(Repo(Fake), Clock);
	auto Objects = std::make_shared<GitHubReleaseObjectStore>(Fake, Config(Fake));
	SyncEngine Sync(Objects, Leases, SyncConfig{TempDir(), 5});
	LeaseToken Tok = *Leases->Acquire(Player(1), "a")->Token;
	const std::string Src = file::Join(TempDir(), "s.sav");
	ASSERT_OK(file::CreateExclusive(Src, save::BuildSynthetic("S", "payload")));
	Fake->LoseNextRefUpdateResponse = true; // the revision commit lands, the response is lost
	UploadOptions O;
	O.StableQuiet = 20;
	auto Up = Sync.Upload(Tok, Src, O);
	ASSERT_OK(Up);
	EXPECT_EQ(Up->Revision.Number, int64_t(1));
	EXPECT_EQ(Leases->Store().Load()->State.HeadNumber(), int64_t(1));
}

SW_TEST(GitHub_ReleaseObjectsPutGetDedupAndRecoverPartialUpload)
{
	auto Fake = std::make_shared<FakeGitHub>();
	(void)Repo(Fake)->Commit("", {{"x", std::string("1")}}, "init"); // non-empty repo
	GitHubReleaseObjectStore Store(Fake, Config(Fake));
	const std::string Src = file::Join(TempDir(), "a.sav");
	ASSERT_OK(file::CreateExclusive(Src, "save bytes"));
	const std::string Sha = Sha256::HexOf("save bytes");
	EXPECT_TRUE(!Store.Has(Sha).Value());
	// An upload that dies mid-way leaves a "starter" asset; the next Put cleans it up.
	Fake->FailNextUploadMidway = true;
	EXPECT_ERR(Store.Put(Sha, Src), ErrorCode::Network);
	EXPECT_TRUE(!Store.Has(Sha).Value()); // incomplete assets never count
	ASSERT_OK(Store.Put(Sha, Src));
	EXPECT_TRUE(Store.Has(Sha).Value());
	const int Before = Fake->ContentCreatingRequests;
	ASSERT_OK(Store.Put(Sha, Src)); // dedup: no upload
	EXPECT_EQ(Fake->ContentCreatingRequests.load(), Before);
	const std::string Out = file::Join(TempDir(), "out.sav");
	ASSERT_OK(Store.Get(Sha, Out));
	EXPECT_EQ(file::ReadAll(Out).Value(), std::string("save bytes"));
	EXPECT_TRUE(!Fake->AuthLeakedToObjectHost.load()); // redirect followed without the token
	// A second store instance (another PC) sees the object.
	GitHubReleaseObjectStore Other(Fake, Config(Fake));
	EXPECT_TRUE(Other.Has(Sha).Value());
	EXPECT_EQ(Other.List().Value().size(), size_t(1));
	// Wrong content for the id is refused.
	const std::string Bad = file::Join(TempDir(), "b.sav");
	ASSERT_OK(file::CreateExclusive(Bad, "other"));
	EXPECT_ERR(Store.Put(Sha256::HexOf("not other"), Bad), ErrorCode::Corrupt);
}

SW_TEST(GitHub_ErrorsAreMappedAndTokenNeverLeaks)
{
	auto Fake = std::make_shared<FakeGitHub>();
	(void)Repo(Fake)->Commit("", {{"x", std::string("1")}}, "init");
	auto R = Repo(Fake);
	Fake->RateLimitNextRequests = 1;
	auto H = R->Head();
	EXPECT_ERR(H, ErrorCode::RateLimited);
	EXPECT_TRUE(H.Err().Message.find("60") != std::string::npos);
	GitHubConfig BadAuth = Config(Fake);
	BadAuth.Token = []() -> Result<std::string> { return std::string("gho_WRONG"); };
	GitHubRepository Wrong(Fake, BadAuth);
	auto W = Wrong.Head();
	EXPECT_ERR(W, ErrorCode::Unauthorized);
	EXPECT_TRUE(W.Err().Message.find("gho_") == std::string::npos);
	GitHubConfig NoAccount = Config(Fake);
	NoAccount.Token = nullptr;
	EXPECT_ERR(GitHubRepository(Fake, NoAccount).Head(), ErrorCode::Unauthorized);
	GitHubConfig Evil = Config(Fake);
	Evil.Repo = "../../evil";
	EXPECT_ERR(GitHubRepository(Fake, Evil).Head(), ErrorCode::Invalid);
}

// End to end over GitHub: create world, HOST, JOIN, checkpoint, stop.
SW_TEST(GitHub_EndToEndHostJoinStop)
{
	auto Fake = std::make_shared<FakeGitHub>();
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto MakeSession = [&](int N, std::string& SaveDir) -> std::unique_ptr<WorldSession>
	{
		auto Leases = MakeLeases(Repo(Fake), Clock);
		auto Sync = std::make_shared<SyncEngine>(std::make_shared<GitHubReleaseObjectStore>(Fake, Config(Fake)), Leases, SyncConfig{TempDir(), 5});
		SessionConfig Cfg;
		Cfg.Me = Player(N);
		Cfg.SaveDirectory = SaveDir = TempDir();
		Cfg.SaveName = "SharedWorld_our-factory";
		Cfg.Upload.StableQuiet = 20;
		return std::make_unique<WorldSession>(Leases, Sync, Cfg);
	};
	{
		auto Leases = MakeLeases(Repo(Fake), Clock);
		SyncEngine Sync(std::make_shared<GitHubReleaseObjectStore>(Fake, Config(Fake)), Leases, SyncConfig{TempDir(), 5});
		const std::string Src = file::Join(TempDir(), "Old.sav");
		ASSERT_OK(file::CreateExclusive(Src, save::BuildSynthetic("Old", "factory")));
		CreateWorldParams P;
		P.Name = "Our Factory";
		P.Creator = Player(1);
		P.SourceSavePath = Src;
		P.Settings.Name = "Our Factory";
		P.bRestrictToMembers = false;
		ASSERT_OK(CreateSharedWorld(*Leases, Sync, P, 20));
	}
	std::string DirA, DirB;
	auto A = MakeSession(1, DirA);
	auto B = MakeSession(2, DirB);
	A->Play();
	A->WaitIdle();
	ASSERT_TRUE(A->View().State == SessionState::ReadyToHost);
	A->OnHostingStarted(JoinInfo{"online-session-id", "EOS:1", "EOS"});
	A->WaitIdle();
	B->Play();
	B->WaitIdle();
	ASSERT_TRUE(B->View().State == SessionState::JoinReady);
	EXPECT_EQ(B->View().Join->Data, std::string("EOS:1"));
	// Budget check: a heartbeat costs exactly 3 content-creating requests.
	const int Before = Fake->ContentCreatingRequests;
	Clock->Advance(Seconds(21));
	A->Tick(Clock->Now());
	A->WaitIdle();
	EXPECT_EQ(Fake->ContentCreatingRequests.load() - Before, 3);
	(void)file::WriteAtomic(file::Join(DirA, "SharedWorld_our-factory.sav"), save::BuildSynthetic("S", "final"));
	A->OnSaveCompleted(SaveKind::Final);
	A->WaitIdle();
	EXPECT_TRUE(A->View().State == SessionState::Idle);
	auto Leases = MakeLeases(Repo(Fake), Clock);
	const WorldSummary S = Summarize(*Leases);
	EXPECT_TRUE(S.Status == WorldStatus::Available);
	EXPECT_EQ(S.Revision, int64_t(2));
	EXPECT_EQ(Fake->AssetCount(), size_t(2));
	EXPECT_TRUE(!Fake->AuthLeakedToObjectHost.load());
	// Human-readable history on the branch.
	auto Log = Repo(Fake)->Log(Repo(Fake)->Head().Value(), 50);
	bool bFound = false;
	for (const CommitInfo& C : *Log) bFound |= C.Message.find("Shared World revision 2") == 0 && C.Message.find("Reason: final") != std::string::npos;
	EXPECT_TRUE(bFound);
}

// Deterministic proof that safety comes from GitHub's fast-forward check:
// two PCs read the same head and both commit on top of it.
SW_TEST(GitHub_SecondCommitOnSameHeadIsRejectedOnlyBecauseOfFastForward)
{
	for (bool bBrokenServer : {false, true})
	{
		auto Fake = std::make_shared<FakeGitHub>();
		auto Base = Repo(Fake)->Commit("", {{"state/current.json", std::string("v1")}}, "init");
		ASSERT_OK(Base);
		Fake->AllowNonFastForward = bBrokenServer;
		auto PcA = Repo(Fake), PcB = Repo(Fake);
		const std::string Seen = PcA->Head().Value();
		EXPECT_EQ(PcB->Head().Value(), Seen);
		ASSERT_OK(PcA->Commit(Seen, {{"state/current.json", std::string("A")}}, "A"));
		auto B = PcB->Commit(Seen, {{"state/current.json", std::string("B")}}, "B");
		if (!bBrokenServer)
		{
			EXPECT_ERR(B, ErrorCode::Conflict);
			EXPECT_EQ(PcA->ReadFile(PcA->Head().Value(), "state/current.json").Value(), std::string("A"));
		}
		else
		{
			EXPECT_TRUE(B.Ok()); // lost update: exactly what the fast-forward rule prevents
		}
	}
}
