// Port of shared-world-helper/internal/syncer tests + restore/dedup/cache.
#include <functional>

#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Sync/Sync.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	// Object store wrapper that runs a callback right after a successful Put.
	struct HookObjects final : IObjectStore
	{
		std::shared_ptr<MemoryObjectStore> Inner;
		std::function<void()> AfterPut;
		Result<bool> Has(const std::string& S) override { return Inner->Has(S); }
		Status Put(const std::string& S, const std::string& P) override
		{
			Status R = Inner->Put(S, P);
			if (R.Ok() && AfterPut)
			{
				auto F = std::move(AfterPut);
				AfterPut = nullptr;
				F();
			}
			return R;
		}
		Status Get(const std::string& S, const std::string& D) override { return Inner->Get(S, D); }
		Status Remove(const std::string& S) override { return Inner->Remove(S); }
		Result<std::vector<std::string>> List() override { return Inner->List(); }
		std::string Describe() const override { return "hook"; }
	};

	struct Env
	{
		std::shared_ptr<FakeClock> Clock = std::make_shared<FakeClock>(StartTime);
		std::shared_ptr<MemoryRepository> Repo = std::make_shared<MemoryRepository>();
		std::shared_ptr<MemoryObjectStore> Objects = std::make_shared<MemoryObjectStore>();
		std::shared_ptr<HookObjects> Hooked = std::make_shared<HookObjects>();
		std::string SaveDir = TempDir();

		Env()
		{
			Hooked->Inner = Objects;
			(void)CreateTestWorld(Repo, Clock);
		}

		// One player's machine: its own lease manager + data dir, shared cloud.
		struct Peer
		{
			std::shared_ptr<LeaseManager> Leases;
			std::shared_ptr<SyncEngine> Sync;
		};
		Peer MakePeer(std::shared_ptr<ILogSink> Sink = nullptr)
		{
			Peer P;
			P.Leases = MakeLeases(Repo, Clock, std::move(Sink));
			P.Sync = std::make_shared<SyncEngine>(Hooked, P.Leases, SyncConfig{TempDir(), 5});
			return P;
		}

		std::string WriteSave(const std::string& Name, const std::string& Body)
		{
			const std::string P = file::Join(SaveDir, Name + ".sav");
			(void)file::Remove(P);
			(void)file::CreateExclusive(P, save::BuildSynthetic("S", Body));
			return P;
		}
	};

	LeaseToken Acquire(Env::Peer& P, int N)
	{
		auto R = P.Leases->Acquire(Player(N), "nonce-" + std::to_string(N) + "-" + std::to_string(P.Leases->Store().Clock().Now()));
		return R.Ok() && R->Token ? *R->Token : LeaseToken{};
	}

	UploadOptions Fast()
	{
		UploadOptions O;
		O.StableQuiet = 20;
		O.StableTimeout = 5000;
		return O;
	}

	WorldState LoadState(Env::Peer& P) { return P.Leases->Store().Load().Value().State; }
}

SW_TEST(Sync_UploadThenDownloadRoundTrip)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	const std::string Src = E.WriteSave("host", "revision one");
	auto Up = A.Sync->Upload(Tok, Src, Fast());
	ASSERT_OK(Up);
	EXPECT_EQ(Up->Revision.Number, int64_t(1));
	EXPECT_EQ(Tok.BaseRevision, int64_t(1));
	auto Same = A.Sync->Upload(Tok, Src, Fast());
	ASSERT_OK(Same);
	EXPECT_TRUE(Same->bUnchanged);

	auto B = E.MakePeer();
	const std::string Target = file::Join(TempDir(), "SharedWorld_our-factory.sav");
	ASSERT_OK(file::CreateExclusive(Target, "old local junk"));
	auto Dl = B.Sync->Download(LoadState(B), Target);
	ASSERT_OK(Dl);
	EXPECT_EQ(file::ReadAll(Target).Value(), file::ReadAll(Src).Value());
	ASSERT_TRUE(!Dl->BackupPath.empty());
	EXPECT_EQ(file::ReadAll(Dl->BackupPath).Value(), std::string("old local junk"));
	auto Again = B.Sync->Download(LoadState(B), Target);
	EXPECT_TRUE(Again->bAlreadyLocal);
}

// Race 4: the shared world changes while an upload is in flight.
SW_TEST(Sync_Race4_CloudChangedDuringUpload)
{
	Env E;
	auto A = E.MakePeer();
	auto B = E.MakePeer();
	LeaseToken TA = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(TA, E.WriteSave("a1", "A rev1"), Fast()));
	int64_t BRev = 0;
	E.Hooked->AfterPut = [&]()
	{
		// While A uploads, A's lease expires and B takes over and commits.
		E.Clock->Advance(Minutes(10));
		LeaseToken TB = Acquire(B, 2);
		auto R = B.Sync->Upload(TB, E.WriteSave("b", "B rev2"), Fast());
		BRev = R.Ok() ? R->Revision.Number : -1;
	};
	const std::string ASrc = E.WriteSave("a2", "A rev2 attempt");
	const std::string Before = file::ReadAll(ASrc).Value();
	ConflictInfo C;
	auto R = A.Sync->Upload(TA, ASrc, Fast(), &C);
	EXPECT_ERR(R, ErrorCode::Fenced);
	const WorldState S = LoadState(A);
	EXPECT_EQ(S.HeadNumber(), BRev);
	EXPECT_EQ(S.Head->Uploader.PlayerId, std::string("player-2"));
	EXPECT_EQ(file::ReadAll(ASrc).Value(), Before); // local save untouched
	ASSERT_TRUE(!C.BackupPath.empty());
	EXPECT_EQ(file::ReadAll(C.BackupPath).Value(), Before); // A's progress preserved
	EXPECT_TRUE(C.BackupPath.find("_conflict-g1-r1") != std::string::npos);
}

// A client holding revision N can never publish over N+1.
SW_TEST(Sync_StaleClientUploadRefused)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("one", "rev1"), Fast()));
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("two", "rev2"), Fast()));
	LeaseToken Stale = Tok;
	Stale.BaseRevision = 1;
	ConflictInfo C;
	EXPECT_ERR(A.Sync->Upload(Stale, E.WriteSave("old", "stale edit"), Fast(), &C), ErrorCode::StaleRevision);
	EXPECT_EQ(C.CloudRevision, int64_t(2));
	EXPECT_EQ(C.LocalBaseRevision, int64_t(1));
	EXPECT_EQ(LoadState(A).HeadNumber(), int64_t(2));
}

// Race 5: corrupted download. The local save must be byte-identical afterwards.
SW_TEST(Sync_Race5_CorruptDownloadLeavesLocalUntouched)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("src", "good data"), Fast()));
	auto B = E.MakePeer();
	const std::string Dir = TempDir();
	const std::string Target = file::Join(Dir, "SharedWorld_our-factory.sav");
	ASSERT_OK(file::CreateExclusive(Target, save::BuildSynthetic("S", "my local copy")));
	const std::string Before = file::ReadAll(Target).Value();
	std::vector<std::function<void(std::string&)>> Corruptions = {
		[](std::string& D) { D[D.size() / 2] ^= 1; },
		[](std::string& D) { D.resize(D.size() - 100); },
		[](std::string& D) { D += '\0'; },
	};
	for (auto& Corrupt : Corruptions)
	{
		E.Objects->CorruptOnGet = Corrupt;
		EXPECT_ERR(B.Sync->Download(LoadState(B), Target), ErrorCode::Corrupt);
		EXPECT_EQ(file::ReadAll(Target).Value(), Before);
	}
	EXPECT_EQ(file::ListNames(Dir).Value().size(), size_t(1)); // no temp files left
	EXPECT_TRUE(!B.Sync->Cache().Find(LoadState(B).Head->ObjectSha256).has_value()); // cache not polluted
}

// Race 6: the connection drops halfway through an upload.
SW_TEST(Sync_Race6_InterruptedUploadKeepsAuthoritativeSave)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("v1", "authoritative"), Fast()));
	const RevisionMeta HeadBefore = *LoadState(A).Head;
	E.Objects->FailPutAfterBytes = 100;
	std::string Big;
	for (int i = 0; i < 5000; ++i) Big += "new progress ";
	const std::string V2 = E.WriteSave("v2", Big);
	EXPECT_ERR(A.Sync->Upload(Tok, V2, Fast()), ErrorCode::Network);
	E.Objects->FailPutAfterBytes = 0;
	const WorldState S = LoadState(A);
	EXPECT_EQ(S.Head->ObjectSha256, HeadBefore.ObjectSha256);
	auto B = E.MakePeer();
	ASSERT_OK(B.Sync->Download(S, file::Join(TempDir(), "check.sav")));
	auto Retry = A.Sync->Upload(Tok, V2, Fast());
	ASSERT_OK(Retry);
	EXPECT_EQ(Retry->Revision.Number, int64_t(2));
}

SW_TEST(Sync_AmbiguousCommitIsResolved)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	E.Repo->LoseNextCommitResponse = true;
	auto R = A.Sync->Upload(Tok, E.WriteSave("x", "data"), Fast());
	ASSERT_OK(R);
	EXPECT_EQ(R->Revision.Number, int64_t(1));
	EXPECT_EQ(Tok.BaseRevision, int64_t(1));
}

SW_TEST(Sync_TruncatedLocalSaveNeverUploaded)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	std::string Body;
	for (int i = 0; i < 300000; ++i) Body += 'x';
	const std::string Full = save::BuildSynthetic("S", Body);
	const std::string P = file::Join(E.SaveDir, "crash.sav");
	ASSERT_OK(file::CreateExclusive(P, Full.substr(0, Full.size() * 2 / 3)));
	EXPECT_ERR(A.Sync->Upload(Tok, P, Fast()), ErrorCode::Corrupt);
	EXPECT_TRUE(!LoadState(A).Head.has_value());
	EXPECT_EQ(E.Objects->PutCount(), size_t(0));
}

SW_TEST(Sync_InspectLocalDetectsUnsyncedChanges)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	const std::string P = E.WriteSave("SharedWorld_our-factory", "v1");
	EXPECT_TRUE(A.Sync->InspectLocal(P)->Status == LocalStatus::Unknown);
	ASSERT_OK(A.Sync->Upload(Tok, P, Fast()));
	EXPECT_TRUE(A.Sync->InspectLocal(P)->Status == LocalStatus::InSync);
	E.WriteSave("SharedWorld_our-factory", "v1 + unsynced progress");
	auto I = A.Sync->InspectLocal(P);
	EXPECT_TRUE(I->Status == LocalStatus::Modified);
	EXPECT_EQ(I->State.SyncedRevision, int64_t(1));
	EXPECT_TRUE(A.Sync->InspectLocal(file::Join(E.SaveDir, "none.sav"))->Status == LocalStatus::Missing);
}

// Test 11: restoring an old revision appends a NEW revision (history is linear).
SW_TEST(Sync_RestoreCreatesNewRevision)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("r1", "first"), Fast()));
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("r2", "second"), Fast()));
	const size_t PutsBefore = E.Objects->PutCount();
	auto History = A.Sync->History(A.Leases->Store().Load()->CommitId);
	ASSERT_OK(History);
	ASSERT_EQ(History->size(), size_t(2));
	EXPECT_EQ((*History)[0].Number, int64_t(2)); // newest first
	auto R = A.Sync->Restore(Tok, (*History)[1], Player(1));
	ASSERT_OK(R);
	EXPECT_EQ(R->Number, int64_t(3));
	EXPECT_EQ(R->RestoredFrom, int64_t(1));
	EXPECT_EQ(R->ObjectSha256, (*History)[1].ObjectSha256);
	EXPECT_EQ(E.Objects->PutCount(), PutsBefore); // no re-upload
	auto After = A.Sync->History(A.Leases->Store().Load()->CommitId);
	EXPECT_EQ(After->size(), size_t(3)); // nothing removed
	// Restore also respects fencing.
	LeaseToken Stale = Tok;
	Stale.Generation = 99;
	EXPECT_ERR(A.Sync->Restore(Stale, (*History)[0], Player(1)), ErrorCode::Fenced);
}

// Test 12: duplicate content is stored once.
SW_TEST(Sync_DuplicateContentIsDeduplicated)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("a", "state A"), Fast()));
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("b", "state B"), Fast()));
	const size_t Puts = E.Objects->PutCount();
	auto R = A.Sync->Upload(Tok, E.WriteSave("a-again", "state A"), Fast()); // same bytes as revision 1
	ASSERT_OK(R);
	EXPECT_TRUE(R->bDeduplicated);
	EXPECT_EQ(R->Revision.Number, int64_t(3));
	EXPECT_EQ(E.Objects->PutCount(), Puts);
	EXPECT_EQ(E.Objects->List()->size(), size_t(2));
}

// Test 13: repository unavailable -> clean error, nothing modified.
SW_TEST(Sync_RepositoryUnavailableIsSafe)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("v1", "one"), Fast()));
	E.Repo->BeforeCommit = []() -> Status { return MakeError(ErrorCode::Network, "repository unreachable"); };
	const std::string P = E.WriteSave("v2", "two");
	auto R = A.Sync->Upload(Tok, P, Fast());
	EXPECT_ERR(R, ErrorCode::Network);
	E.Repo->BeforeCommit = nullptr;
	EXPECT_EQ(LoadState(A).HeadNumber(), int64_t(1));
	EXPECT_TRUE(A.Sync->InspectLocal(P)->Status == LocalStatus::Modified); // still pending, will be retried
}

SW_TEST(Sync_ObjectCacheServesOfflineAndRejectsDamage)
{
	Env E;
	auto A = E.MakePeer();
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("v1", "cached content"), Fast()));
	auto B = E.MakePeer();
	const WorldState S = LoadState(B);
	ASSERT_OK(B.Sync->Download(S, file::Join(TempDir(), "one.sav")));
	E.Objects->Offline = true; // store unreachable: the verified cache still works
	auto D = B.Sync->Download(S, file::Join(TempDir(), "two.sav"));
	ASSERT_OK(D);
	EXPECT_TRUE(D->bFromCache);
	// Damage the cache entry: it is detected and not used.
	const std::string CachePath = B.Sync->Cache().PathFor(S.Head->ObjectSha256);
	(void)file::Remove(CachePath);
	ASSERT_OK(file::CreateExclusive(CachePath, "tampered"));
	EXPECT_ERR(B.Sync->Download(S, file::Join(TempDir(), "three.sav")), ErrorCode::Network);
	E.Objects->Offline = false;
	auto Fresh = B.Sync->Download(S, file::Join(TempDir(), "four.sav"));
	ASSERT_OK(Fresh);
	EXPECT_TRUE(!Fresh->bFromCache);
}

// Test 14: secrets never reach the logs even when errors carry them.
SW_TEST(Sync_LogsNeverContainSecrets)
{
	Env E;
	auto Sink = std::make_shared<MemoryLogSink>();
	auto A = E.MakePeer(Sink);
	Logger L(Sink);
	L.Error("ProviderAuthFailed", {{"authorization", "Bearer ghp_SECRET123"}, {"access_token", "gho_SECRET456"}, {"world", "our-factory"}});
	LeaseToken Tok = Acquire(A, 1);
	ASSERT_OK(A.Sync->Upload(Tok, E.WriteSave("v", "x"), Fast()));
	for (const std::string& Line : Sink->Lines())
	{
		if (Line.find("SECRET") != std::string::npos) swtest::ReportFailure(__FILE__, __LINE__, "secret logged: " + Line);
	}
	EXPECT_TRUE(Sink->Contains("event=RevisionCommitted"));
	EXPECT_TRUE(Sink->Contains("event=LeaseAcquired"));
}
