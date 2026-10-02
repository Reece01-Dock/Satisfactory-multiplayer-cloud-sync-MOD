// Port of shared-world-helper/internal/lease/lease_test.go (+ handoff tests).
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Storage/FileStorage.h"
#include "SharedWorldCore/Storage/LogRepository.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	RevisionMeta FakeRev(const LeaseToken& Tok, int N)
	{
		RevisionMeta R;
		R.Number = Tok.BaseRevision + 1;
		R.Generation = Tok.Generation;
		R.PreviousRevision = Tok.BaseRevision;
		R.ObjectSha256 = Sha256::HexOf("rev" + std::to_string(N));
		R.Size = 1234;
		R.CreatedAt = 1790000000000;
		R.Uploader = Tok.Holder;
		R.Reason = Reason::Checkpoint;
		return R;
	}

	// Runs Fn with a factory producing independent LeaseManagers on one shared world.
	void ForEachBackend(const std::function<void(const char*, std::function<std::shared_ptr<LeaseManager>()>, FakeClock&)>& Fn)
	{
		{
			auto Clock = std::make_shared<FakeClock>(StartTime);
			auto Repo = std::make_shared<MemoryRepository>();
			// Simulated network latency between read and conditional write,
			// so concurrent read-modify-CAS cycles genuinely interleave.
			Repo->BeforeCommit = []() -> Status
			{
				std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 1500));
				return {};
			};
			ASSERT_OK(CreateTestWorld(Repo, Clock));
			Fn("memory", [Repo, Clock]() { return MakeLeases(Repo, Clock); }, *Clock);
		}
		{
			auto Clock = std::make_shared<FakeClock>(StartTime);
			const std::string Dir = TempDir();
			ASSERT_OK(CreateTestWorld(std::make_shared<FileRepository>(Dir), Clock));
			// A new FileRepository per manager behaves like a separate process.
			Fn("filesystem", [Dir, Clock]() { return MakeLeases(std::make_shared<FileRepository>(Dir), Clock); }, *Clock);
		}
		// The same lease races on plain file storage: append-only log, exclusive-create and duplicate-name stores.
		for (bool bExclusive : {true, false})
		{
			auto Clock = std::make_shared<FakeClock>(StartTime);
			auto Store = std::make_shared<MemoryLogStore>(bExclusive, bExclusive ? 0 : 5);
			LogRepositoryConfig C;
			C.SettleMs = 30;
			ASSERT_OK(CreateTestWorld(std::make_shared<LogRepository>(Store, C), Clock));
			Fn(bExclusive ? "log-exclusive" : "log-duplicates",
				[Store, C, Clock]() { return MakeLeases(std::make_shared<LogRepository>(Store, C), Clock); }, *Clock);
		}
	}
}

// Race 1: many players press Play at the same instant. Exactly one host.
SW_TEST(Lease_Race1_SimultaneousAcquireYieldsExactlyOneHost)
{
	ForEachBackend([](const char* Name, std::function<std::shared_ptr<LeaseManager>()> Make, FakeClock&)
	{
		constexpr int N = 16;
		std::vector<std::optional<Result<AcquireResult>>> Results(N);
		std::atomic<bool> Go{false};
		std::vector<std::thread> Threads;
		for (int i = 0; i < N; ++i)
		{
			Threads.emplace_back([&, i]()
			{
				auto M = Make();
				while (!Go.load()) std::this_thread::yield();
				Results[i].emplace(M->Acquire(Player(i), "nonce-" + std::to_string(i)));
			});
		}
		Go = true;
		for (auto& T : Threads) T.join();
		int Hosts = 0, Joiners = 0;
		for (int i = 0; i < N; ++i)
		{
			ASSERT_OK(*Results[i]);
			const AcquireOutcome O = (*Results[i])->Outcome;
			if (O == AcquireOutcome::Acquired) ++Hosts;
			else if (O == AcquireOutcome::HeldByOther) ++Joiners;
			else swtest::ReportFailure(__FILE__, __LINE__, std::string("unexpected outcome ") + ToString(O));
		}
		if (Hosts != 1 || Joiners != N - 1)
		{
			swtest::ReportFailure(__FILE__, __LINE__, std::string(Name) + ": hosts=" + std::to_string(Hosts) + " joiners=" + std::to_string(Joiners));
		}
		auto Snap = Make()->Store().Load();
		ASSERT_OK(Snap);
		EXPECT_EQ(Snap->State.Generation, int64_t(1)); // only one acquisition may succeed
	});
}

// Race 2: host crashes (no heartbeats). Lease expires, next player gets generation + 1.
SW_TEST(Lease_Race2_CrashedHostLeaseExpires)
{
	ForEachBackend([](const char*, std::function<std::shared_ptr<LeaseManager>()> Make, FakeClock& Clock)
	{
		auto M = Make();
		auto A = M->Acquire(Player(1), "a");
		ASSERT_OK(A);
		ASSERT_TRUE(A->Outcome == AcquireOutcome::Acquired);
		// Default LeaseConfig: TTL=45s, SkewGrace=15s → observer expiry at 60s.
		Clock.Advance(Seconds(30));
		EXPECT_TRUE(M->Acquire(Player(2), "b")->Outcome == AcquireOutcome::HeldByOther);
		Clock.Advance(Seconds(20)); // 50s: past TTL, inside skew grace
		EXPECT_TRUE(M->Acquire(Player(2), "b")->Outcome == AcquireOutcome::HeldByOther);
		Clock.Advance(Seconds(15)); // 65s: past observer expiry
		auto B = M->Acquire(Player(2), "b");
		ASSERT_OK(B);
		ASSERT_TRUE(B->Outcome == AcquireOutcome::Acquired);
		EXPECT_EQ(B->Token->Generation, A->Token->Generation + 1);
		ASSERT_TRUE(B->TookOverExpired.has_value());
		EXPECT_EQ(B->TookOverExpired->Holder.PlayerId, std::string("player-1"));
		EXPECT_EQ(B->Snapshot.State.LastSession->Reason, std::string("expired"));
	});
}

// Race 3: the old host returns after takeover. Every write is fenced.
SW_TEST(Lease_Race3_OldHostIsFenced)
{
	ForEachBackend([](const char*, std::function<std::shared_ptr<LeaseManager>()> Make, FakeClock& Clock)
	{
		auto M = Make();
		auto A = M->Acquire(Player(1), "a");
		ASSERT_OK(A);
		LeaseToken Old = *A->Token;
		Clock.Advance(Minutes(5));
		auto B = M->Acquire(Player(2), "b");
		ASSERT_OK(B);
		ASSERT_TRUE(B->Outcome == AcquireOutcome::Acquired);
		EXPECT_ERR(M->Renew(Old, LeaseUpdate{LeasePhase::Hosting}), ErrorCode::Fenced);
		EXPECT_ERR(M->CommitRevision(Old, FakeRev(Old, 1)), ErrorCode::Fenced);
		EXPECT_ERR(M->Release(Old), ErrorCode::Fenced);
		LeaseUpdate Publish;
		Publish.Join = JoinInfo{"address", "10.0.0.1:7777", ""};
		EXPECT_ERR(M->Renew(Old, Publish), ErrorCode::Fenced); // cannot publish a session either
		auto S = M->Store().Load();
		ASSERT_OK(S);
		EXPECT_EQ(S->State.CurrentLease->Holder.PlayerId, std::string("player-2"));
		EXPECT_EQ(S->State.HeadNumber(), int64_t(0));
		EXPECT_TRUE(!S->State.CurrentLease->Join.has_value());
	});
}

SW_TEST(Lease_CommitRequiresCurrentBaseRevision)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	auto A = M->Acquire(Player(1), "a");
	LeaseToken Tok = *A->Token;
	ASSERT_OK(M->CommitRevision(Tok, FakeRev(Tok, 1)));
	EXPECT_EQ(Tok.BaseRevision, int64_t(1));
	LeaseToken Stale = Tok;
	Stale.BaseRevision = 0;
	EXPECT_ERR(M->CommitRevision(Stale, FakeRev(Stale, 2)), ErrorCode::StaleRevision);
	auto S = M->Store().Load();
	EXPECT_EQ(S->State.HeadNumber(), int64_t(1));
	EXPECT_EQ(S->State.Head->ObjectSha256, Sha256::HexOf("rev1"));
	// The revision metadata file was written in the same commit as the state.
	auto Meta = Repo->ReadFile(S->CommitId, S->State.Head->Path());
	ASSERT_OK(Meta);
	auto Log = Repo->Log(S->CommitId, 1);
	EXPECT_TRUE(Log->at(0).Message.find("Shared World revision 1") == 0);
	EXPECT_TRUE(Log->at(0).Message.find("Save SHA256: " + Sha256::HexOf("rev1")) != std::string::npos);
}

SW_TEST(Lease_RenewAfterExpiryWithoutTakeover)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	LeaseToken Tok = *M->Acquire(Player(1), "a")->Token;
	Clock->Advance(Minutes(10));
	ASSERT_OK(M->Renew(Tok, {}));
	ASSERT_OK(M->CommitRevision(Tok, FakeRev(Tok, 1)));
}

SW_TEST(Lease_SameUserSecondInstanceIsNotAHost)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	EXPECT_TRUE(M->Acquire(Player(1), "first")->Outcome == AcquireOutcome::Acquired);
	auto B = M->Acquire(Player(1), "second");
	EXPECT_TRUE(B->Outcome == AcquireOutcome::HeldBySelfElsewhere);
	EXPECT_TRUE(!B->Token.has_value());
	auto Again = M->Acquire(Player(1), "first");
	EXPECT_TRUE(Again->Outcome == AcquireOutcome::AlreadyHeld);
	EXPECT_TRUE(Again->Token.has_value());
}

SW_TEST(Lease_ReleaseMakesWorldAvailable)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	LeaseToken Tok = *M->Acquire(Player(1), "a")->Token;
	ASSERT_OK(M->Release(Tok));
	auto B = M->Acquire(Player(2), "b");
	EXPECT_TRUE(B->Outcome == AcquireOutcome::Acquired);
	EXPECT_EQ(B->Token->Generation, int64_t(2));
	EXPECT_EQ(B->Snapshot.State.LastSession->Reason, std::string("released"));
}

// Planned migration: the released world is reserved for the chosen
// successor for a short window; everyone else waits (and then joins).
SW_TEST(Lease_HandoffReservesWorldForSuccessor)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	LeaseToken A = *M->Acquire(Player(1), "a")->Token;
	ASSERT_OK(M->CommitRevision(A, FakeRev(A, 1)));
	ASSERT_OK(M->Release(A, Player(2)));
	auto S = M->Store().Load();
	ASSERT_TRUE(S->State.PendingHandoff.has_value());
	EXPECT_EQ(S->State.PendingHandoff->Revision, int64_t(1));
	EXPECT_EQ(S->State.LastSession->Reason, std::string("migrated"));
	// Player 3 cannot jump the queue.
	EXPECT_TRUE(M->Acquire(Player(3), "c")->Outcome == AcquireOutcome::ReservedForSuccessor);
	// The successor acquires normally (fresh generation, exactly revision 1).
	auto B = M->Acquire(Player(2), "b");
	ASSERT_OK(B);
	ASSERT_TRUE(B->Outcome == AcquireOutcome::Acquired);
	EXPECT_TRUE(B->bFromHandoff);
	EXPECT_EQ(B->Token->Generation, A.Generation + 1);
	EXPECT_EQ(B->Token->BaseRevision, int64_t(1));
	EXPECT_TRUE(!B->Snapshot.State.PendingHandoff.has_value());
}

SW_TEST(Lease_ExpiredHandoffOpensWorldToEveryone)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	LeaseToken A = *M->Acquire(Player(1), "a")->Token;
	ASSERT_OK(M->Release(A, Player(2)));
	Clock->Advance(Minutes(3)); // successor never showed up
	auto C = M->Acquire(Player(3), "c");
	ASSERT_OK(C);
	EXPECT_TRUE(C->Outcome == AcquireOutcome::Acquired);
	EXPECT_TRUE(!C->bFromHandoff);
}

SW_TEST(Lease_InvalidRemoteStateRejected)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	for (const char* Bad : {
			 R"({"schemaVersion":99,"worldId":"our-factory","stateVersion":1,"generation":0,"updatedAt":"2026-09-27T12:00:00Z"})",
			 R"({"schemaVersion":2,"worldId":"other","stateVersion":1,"generation":0,"updatedAt":"2026-09-27T12:00:00Z"})",
			 "{"})
	{
		Repo->ForceWrite(Paths::State, Bad);
		if (M->Store().Load().Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string("accepted: ") + Bad);
		if (M->Acquire(Player(1), "n").Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string("acquired on invalid state: ") + Bad);
	}
}

SW_TEST(Lease_AmbiguousCommitReportedNotRetriedBlindly)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	Repo->LoseNextCommitResponse = true;
	auto A = M->Acquire(Player(1), "a");
	EXPECT_ERR(A, ErrorCode::Ambiguous);
	// Retrying with the same nonce is idempotent: the caller learns it holds the lease.
	auto Again = M->Acquire(Player(1), "a");
	ASSERT_OK(Again);
	EXPECT_TRUE(Again->Outcome == AcquireOutcome::AlreadyHeld);
	EXPECT_EQ(Again->Token->Generation, int64_t(1));
}
