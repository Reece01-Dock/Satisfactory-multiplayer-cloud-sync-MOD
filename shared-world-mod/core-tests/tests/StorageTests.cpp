#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Storage/FileStorage.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "TestFramework.h"

using namespace sw;

namespace
{
	Identity Player(int N)
	{
		return Identity{"player-" + std::to_string(N), "P" + std::to_string(N), "EOS", "install-" + std::to_string(N)};
	}

	// Runs Fn against every repository implementation.
	void ForEachRepository(const std::function<void(const char*, std::function<std::unique_ptr<IWorldRepository>()>)>& Fn)
	{
		Fn("memory", [Shared = std::make_shared<MemoryRepository>()]() mutable
		{
			// Memory "instances" must share state; wrap the shared one.
			struct View final : IWorldRepository
			{
				std::shared_ptr<MemoryRepository> R;
				Result<std::string> Head() override { return R->Head(); }
				Result<std::string> ReadFile(const std::string& C, const std::string& P) override { return R->ReadFile(C, P); }
				Result<std::vector<std::string>> ListDirectory(const std::string& C, const std::string& D) override { return R->ListDirectory(C, D); }
				Result<std::string> Commit(const std::string& E, const std::vector<FileChange>& Ch, const std::string& M) override { return R->Commit(E, Ch, M); }
				Result<std::vector<CommitInfo>> Log(const std::string& F, int N) override { return R->Log(F, N); }
				std::string Describe() const override { return "memory view"; }
			};
			auto V = std::make_unique<View>();
			V->R = Shared;
			return std::unique_ptr<IWorldRepository>(std::move(V));
		});
		const std::string Dir = swtest::TempDir();
		// Separate FileRepository instances on one directory behave like separate processes.
		Fn("filesystem", [Dir]() { return std::unique_ptr<IWorldRepository>(std::make_unique<FileRepository>(Dir)); });
	}
}

SW_TEST(Model_StateRoundTripAndValidation)
{
	WorldState S;
	S.WorldId = "our-factory";
	S.StateVersion = 7;
	S.Generation = 3;
	RevisionMeta R;
	R.Number = 5;
	R.Generation = 3;
	R.PreviousRevision = 4;
	R.ObjectSha256 = Sha256::HexOf("save");
	R.Size = 1234;
	R.CreatedAt = 1790000000000;
	R.Uploader = Player(1);
	R.Reason = Reason::Checkpoint;
	S.Head = R;
	Lease L;
	L.Generation = 3;
	L.Holder = Player(1);
	L.Nonce = "n1";
	L.AcquiredAt = 1790000000000;
	L.RenewedAt = L.AcquiredAt;
	L.ExpiresAt = L.AcquiredAt + 90000;
	L.BaseRevision = 5;
	L.Phase = LeasePhase::Hosting;
	L.Join = JoinInfo{"online-session-id", "EOS:abc", "EOS"};
	L.Players = {{"Reece", "player-1"}};
	S.CurrentLease = L;
	S.UpdatedAt = L.AcquiredAt;
	ASSERT_OK(S.Validate("our-factory"));
	auto Back = DecodeState(EncodeState(S), "our-factory");
	ASSERT_OK(Back);
	EXPECT_EQ(EncodeState(*Back), EncodeState(S));
	EXPECT_EQ(Back->CurrentLease->Join->Data, std::string("EOS:abc"));
	EXPECT_EQ(R.Path(), std::string("revisions/0000/00000005-g00000003-") + R.ObjectSha256.substr(0, 8) + ".json");
	EXPECT_ERR(DecodeState(EncodeState(S), "other-world"), ErrorCode::Invalid);
}

SW_TEST(Model_RejectsInvalidRemoteState)
{
	const std::string Good = R"({"schemaVersion":2,"worldId":"w","stateVersion":1,"generation":0,"head":null,"lease":null,"lastSession":null,"handoff":null,"updatedAt":"2026-09-27T12:00:00.000Z"})";
	ASSERT_OK(DecodeState(Good, "w"));
	struct Case { const char* Name; std::string Text; ErrorCode Code; };
	std::vector<Case> Cases = {
		{"newer schema", R"({"schemaVersion":3,"worldId":"w","stateVersion":1,"generation":0,"updatedAt":"2026-09-27T12:00:00Z"})", ErrorCode::Unsupported},
		{"bad world id", R"({"schemaVersion":2,"worldId":"../x","stateVersion":1,"generation":0,"updatedAt":"2026-09-27T12:00:00Z"})", ErrorCode::Invalid},
		{"negative gen", R"({"schemaVersion":2,"worldId":"w","stateVersion":1,"generation":-1,"updatedAt":"2026-09-27T12:00:00Z"})", ErrorCode::Invalid},
		{"lease gen mismatch", R"({"schemaVersion":2,"worldId":"w","stateVersion":1,"generation":2,"lease":{"generation":1,"holder":{"playerId":"p","displayName":"P","platform":"x","installId":"i"},"nonce":"n","acquiredAt":"2026-09-27T12:00:00Z","renewedAt":"2026-09-27T12:00:00Z","expiresAt":"2026-09-27T12:01:00Z","baseRevision":0,"phase":"HOSTING","join":null,"players":[]},"updatedAt":"2026-09-27T12:00:00Z"})", ErrorCode::Invalid},
		{"bad object hash", R"({"schemaVersion":2,"worldId":"w","stateVersion":1,"generation":1,"head":{"number":1,"generation":1,"previousRevision":0,"object":"xyz","size":5,"createdAt":"2026-09-27T12:00:00Z","uploader":{"playerId":"p","displayName":"P","platform":"x","installId":"i"},"reason":"import"},"updatedAt":"2026-09-27T12:00:00Z"})", ErrorCode::Invalid},
		{"control chars", std::string(R"({"schemaVersion":2,"worldId":"w","stateVersion":1,"generation":1,"lastSession":{"host":{"playerId":"p","displayName":"a\u0007b","platform":"x","installId":"i"},"generation":1,"endedAt":"2026-09-27T12:00:00Z","reason":"released"},"updatedAt":"2026-09-27T12:00:00Z"})"), ErrorCode::Invalid},
		{"not json", "{", ErrorCode::Invalid},
	};
	for (const Case& C : Cases)
	{
		auto R = DecodeState(C.Text, "w");
		if (R.Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string("accepted: ") + C.Name);
		else if (R.Err().Code != C.Code) swtest::ReportFailure(__FILE__, __LINE__, std::string(C.Name) + ": " + R.Err().Describe());
	}
}

SW_TEST(Model_PlayersSettingsWorldInfo)
{
	PlayerList P;
	P.Members = {{"p1", "Reece", Role::Owner}, {"p2", "Alex", Role::Member}};
	auto PB = PlayerList::FromJson(P.ToJson());
	ASSERT_OK(PB);
	EXPECT_EQ(PB->Members.size(), size_t(2));
	EXPECT_TRUE(HasPermission(Role::Owner, Permission::DeleteWorld));
	EXPECT_TRUE(!HasPermission(Role::Member, Permission::RestoreRevision));
	EXPECT_TRUE(!HasPermission(Role::Viewer, Permission::Play));
	PlayerList NoOwner;
	NoOwner.Members = {{"p2", "Alex", Role::Member}};
	EXPECT_ERR(PlayerList::FromJson(NoOwner.ToJson()), ErrorCode::Invalid);

	WorldSettings S;
	S.Name = "My Factory";
	ASSERT_OK(WorldSettings::FromJson(S.ToJson()));
	S.SyncIntervalMinutes = 0;
	EXPECT_ERR(WorldSettings::FromJson(S.ToJson()), ErrorCode::Invalid);

	WorldInfo W;
	W.WorldId = "0b6f9f2e-5b1d-4c7e-9d7a-2f0c3e4a5b6c";
	W.Name = "My Factory";
	W.CreatedBy = Player(1);
	W.CreatedAt = 1790000000000;
	W.RequiredMods = {{"SML", "3.12.0"}};
	auto WB = WorldInfo::FromJson(W.ToJson());
	ASSERT_OK(WB);
	EXPECT_EQ(WB->RequiredMods[0].Version, std::string("3.12.0"));
}

SW_TEST(Repository_ContractBasics)
{
	ForEachRepository([](const char* Name, std::function<std::unique_ptr<IWorldRepository>()> Make)
	{
		auto R = Make();
		if (!R->Head().Is(ErrorCode::NotFound)) swtest::ReportFailure(__FILE__, __LINE__, std::string(Name) + ": empty repo has a head");
		auto C1 = R->Commit("", {{"world.json", std::string("{}")}, {"state/current.json", std::string("v1")}}, "create");
		ASSERT_OK(C1);
		// Creating again (expected empty) must fail: someone already created it.
		EXPECT_ERR(R->Commit("", {{"world.json", std::string("{}")}}, "create again"), ErrorCode::Conflict);
		auto C2 = R->Commit(*C1, {{"state/current.json", std::string("v2")}, {"revisions/00000001-g00000001-aaaaaaaa.json", std::string("r1")}}, "rev 1");
		ASSERT_OK(C2);
		EXPECT_EQ(R->ReadFile(*C2, "state/current.json").Value(), std::string("v2"));
		EXPECT_EQ(R->ReadFile(*C1, "state/current.json").Value(), std::string("v1")); // history is immutable
		EXPECT_ERR(R->ReadFile(*C1, "revisions/00000001-g00000001-aaaaaaaa.json"), ErrorCode::NotFound);
		auto L = R->ListDirectory(*C2, "revisions");
		ASSERT_OK(L);
		EXPECT_EQ(L->size(), size_t(1));
		// Subdirectories are listed too.
		auto C2b = R->Commit(*C2, {{"revisions/0001/x.json", std::string("r")}, {"revisions/0001/y.json", std::string("r")}}, "nested");
		ASSERT_OK(C2b);
		auto L2 = R->ListDirectory(*C2b, "revisions");
		ASSERT_OK(L2);
		EXPECT_EQ(L2->size(), size_t(2)); // the file and the "0001" directory
		auto L3 = R->ListDirectory(*C2b, "revisions/0001");
		EXPECT_EQ(L3->size(), size_t(2));
		C2 = C2b;
		// Stale expected head -> Conflict, nothing written.
		EXPECT_ERR(R->Commit(*C1, {{"state/current.json", std::string("stale")}}, "stale"), ErrorCode::Conflict);
		EXPECT_EQ(R->ReadFile(R->Head().Value(), "state/current.json").Value(), std::string("v2"));
		// Delete.
		auto C3 = R->Commit(*C2, {{"state/current.json", std::nullopt}}, "delete");
		ASSERT_OK(C3);
		EXPECT_ERR(R->ReadFile(*C3, "state/current.json"), ErrorCode::NotFound);
		auto Log = R->Log(*C3, 10);
		ASSERT_OK(Log);
		EXPECT_EQ(Log->size(), size_t(4));
		EXPECT_EQ((*Log)[0].Message, std::string("delete"));
		// Hostile paths.
		EXPECT_ERR(R->Commit(*C3, {{"../escape", std::string("x")}}, "x"), ErrorCode::Invalid);
		EXPECT_ERR(R->ReadFile(*C3, "a//b"), ErrorCode::Invalid);
		EXPECT_ERR(R->ReadFile(*C3, "State/Current.json"), ErrorCode::Invalid);
	});
}

// The central guarantee: concurrent read-modify-CAS loops never lose an
// update. Filesystem variant uses separate instances (= separate processes).
SW_TEST(Repository_ConcurrentCasNeverLosesUpdates)
{
	ForEachRepository([](const char* Name, std::function<std::unique_ptr<IWorldRepository>()> Make)
	{
		auto Init = Make();
		auto First = Init->Commit("", {{"counter", std::string("0")}}, "init");
		ASSERT_OK(First);
		constexpr int Threads = 6, PerThread = 15;
		std::atomic<int> Errors{0};
		std::vector<std::thread> Pool;
		for (int t = 0; t < Threads; ++t)
		{
			Pool.emplace_back([&Make, &Errors]()
			{
				auto R = Make();
				for (int Done = 0; Done < PerThread;)
				{
					auto H = R->Head();
					if (!H) { ++Errors; std::fprintf(stderr, "    head: %s\n", H.Err().Describe().c_str()); return; }
					auto V = R->ReadFile(*H, "counter");
					if (!V) { ++Errors; std::fprintf(stderr, "    read: %s\n", V.Err().Describe().c_str()); return; }
					const int N = std::stoi(*V) + 1;
					auto C = R->Commit(*H, {{"counter", std::to_string(N)}}, "inc");
					if (C.Ok()) ++Done;
					else if (!C.Is(ErrorCode::Conflict)) { ++Errors; std::fprintf(stderr, "    commit: %s\n", C.Err().Describe().c_str()); return; }
				}
			});
		}
		for (auto& T : Pool) T.join();
		EXPECT_EQ(Errors.load(), 0);
		auto R = Make();
		auto Final = R->ReadFile(R->Head().Value(), "counter");
		ASSERT_OK(Final);
		if (std::stoi(*Final) != Threads * PerThread)
		{
			swtest::ReportFailure(__FILE__, __LINE__, std::string(Name) + ": lost updates, counter=" + *Final);
		}
	});
}

SW_TEST(FileRepository_DetectsCorruptionAndStaleLock)
{
	const std::string Dir = swtest::TempDir();
	FileRepository R(Dir);
	auto C = R.Commit("", {{"state/current.json", std::string("hello")}}, "init");
	ASSERT_OK(C);
	// Tamper with the blob: reads must fail, not return garbage.
	const std::string Blob = file::Join(Dir, "repo/blobs/" + Sha256::HexOf("hello"));
	ASSERT_OK(file::WriteAtomic(Blob, "evil"));
	EXPECT_ERR(R.ReadFile(*C, "state/current.json"), ErrorCode::Corrupt);
	// A lock left behind by a crashed process is broken after it goes stale.
	ASSERT_OK(file::CreateExclusive(file::Join(Dir, "repo/HEAD.lock"), "lock"));
	R.StaleLockSeconds = 0.05;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	ASSERT_OK(R.Commit(*C, {{"state/current.json", std::string("next")}}, "after stale lock"));
}

SW_TEST(ObjectStore_ContentAddressedAndIdempotent)
{
	const std::string Dir = swtest::TempDir();
	FileObjectStore FS(Dir);
	MemoryObjectStore Mem;
	for (IObjectStore* S : std::vector<IObjectStore*>{&FS, &Mem})
	{
		const std::string Src = file::Join(swtest::TempDir(), "a.sav");
		ASSERT_OK(file::CreateExclusive(Src, "save bytes"));
		const std::string Sha = Sha256::HexOf("save bytes");
		EXPECT_TRUE(!S->Has(Sha).Value());
		ASSERT_OK(S->Put(Sha, Src));
		ASSERT_OK(S->Put(Sha, Src)); // idempotent
		EXPECT_TRUE(S->Has(Sha).Value());
		const std::string Out = file::Join(swtest::TempDir(), "out.sav");
		ASSERT_OK(S->Get(Sha, Out));
		EXPECT_EQ(file::ReadAll(Out).Value(), std::string("save bytes"));
		EXPECT_EQ(S->List().Value().size(), size_t(1));
		EXPECT_ERR(S->Get(Sha256::HexOf("missing"), Out), ErrorCode::NotFound);
	}
	// Filesystem: an object whose bytes do not match its id is refused and leaves nothing behind.
	const std::string Src = file::Join(swtest::TempDir(), "b.sav");
	ASSERT_OK(file::CreateExclusive(Src, "other"));
	EXPECT_ERR(FS.Put(Sha256::HexOf("not other"), Src), ErrorCode::Corrupt);
	EXPECT_EQ(FS.List().Value().size(), size_t(1));
	EXPECT_ERR(FS.Has("../../etc/passwd"), ErrorCode::Invalid);
}
