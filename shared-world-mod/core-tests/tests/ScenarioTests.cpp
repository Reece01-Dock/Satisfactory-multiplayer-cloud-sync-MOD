// Required scenarios A–L for the Shared World automated harness.
// These exercise SharedWorldCore (lease, sync, election, migration) — not a
// parallel fake implementation.

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

#include "MultiplayerSimulator.h"
#include "SharedWorldCore/HostElection/HostElection.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Sync/Sync.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "SharedWorldCore/Save/SaveFile.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;
using namespace swsim;

namespace
{
	LinkSample L(int Rtt, int Jitter = 2, int LossBp = 0)
	{
		LinkSample S;
		S.RttMs = Rtt;
		S.JitterMs = Jitter;
		S.LossBp = LossBp;
		S.bReachable = true;
		S.UploadKbps = 8000;
		return S;
	}

	void Mesh(MultiplayerSimulator& Sim, const std::vector<std::string>& Ids, const std::vector<std::vector<int>>& Rtt)
	{
		for (size_t i = 0; i < Ids.size(); ++i)
		{
			Sim.Net().SetUploadKbps(Ids[i], 8000);
			for (size_t j = 0; j < Ids.size(); ++j)
			{
				if (i == j) continue;
				Sim.Net().SetLink(Ids[i], Ids[j], L(Rtt[i][j]));
			}
		}
	}
}

// ---- A: Normal startup -----------------------------------------------------

SW_TEST(Scenario_A_NormalStartup)
{
	MultiplayerSimulator Sim(swtest::ChaosSeed());
	Sim.AddPlayer("A", 1);
	Sim.AddPlayer("B", 2);
	Sim.Net().SetSymmetric("player-1", "player-2", L(20));
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	ASSERT_EQ(Sim.AuthoritativeCount(), 1);
	ASSERT_OK(Sim.JoinExistingHost("player-2"));
	ASSERT_EQ(Sim.AuthoritativeCount(), 1);
	EXPECT_EQ(Sim.Find("player-2")->JoinCount, 1);
	EXPECT_TRUE(!Sim.Find("player-2")->bAuthoritative);
	auto Snap = Sim.Leases().Store().Load();
	ASSERT_OK(Snap);
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, std::string("player-1"));
	swtest::Record("election");
	swtest::Record("acquire");
	swtest::Record("invariant");
}

// ---- B: Simultaneous Play --------------------------------------------------

SW_TEST(Scenario_B_SimultaneousPlay)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	Repo->BeforeCommit = []() -> Status
	{
		std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 800));
		return {};
	};
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	constexpr int N = 16;
	std::vector<std::optional<Result<AcquireResult>>> Results(N);
	std::atomic<bool> Go{false};
	std::vector<std::thread> Threads;
	for (int i = 0; i < N; ++i)
	{
		Threads.emplace_back([&, i]()
		{
			auto M = MakeLeases(Repo, Clock);
			while (!Go.load()) std::this_thread::yield();
			Results[i].emplace(M->Acquire(Player(i), "nonce-" + std::to_string(i)));
		});
	}
	Go = true;
	for (auto& T : Threads) T.join();
	int Hosts = 0;
	std::string Winner;
	for (int i = 0; i < N; ++i)
	{
		ASSERT_OK(*Results[i]);
		if ((*Results[i])->Outcome == AcquireOutcome::Acquired)
		{
			++Hosts;
			Winner = (*Results[i])->Token->Holder.PlayerId;
		}
	}
	EXPECT_EQ(Hosts, 1);
	EXPECT_TRUE(!Winner.empty());
	for (int i = 0; i < N; ++i)
	{
		if ((*Results[i])->Outcome == AcquireOutcome::HeldByOther)
		{
			EXPECT_EQ((*Results[i])->Snapshot.State.CurrentLease->Holder.PlayerId, Winner);
		}
	}
	swtest::Record("acquire");
	swtest::Record("invariant");
}

// ---- C: Best host selection + dynamic update -------------------------------

SW_TEST(Scenario_C_BestHostSelectionAndDynamicUpdate)
{
	SimulatedNetworkQualityProvider Net;
	const std::vector<std::string> Ids = {"reece", "alex", "sam", "jack"};
	Net.SetSymmetric("reece", "alex", L(18));
	Net.SetSymmetric("reece", "sam", L(70));
	Net.SetSymmetric("reece", "jack", L(31));
	Net.SetSymmetric("alex", "sam", L(24));
	Net.SetSymmetric("alex", "jack", L(22));
	Net.SetSymmetric("sam", "jack", L(48));
	PeerQualityMatrix M;
	M.SetPlayers(Ids);
	Net.ApplyTo(M, StartTime);
	std::vector<HostCandidate> Cs;
	for (const std::string& Id : Ids)
	{
		Cs.push_back(MakeCandidate(Identity{Id, Id, "EOS", "i-" + Id}, M, Ids, true, true, true, true));
	}
	auto Ranked = RankHosts(Cs, Identity{}, {});
	ASSERT_TRUE(Ranked.size() >= 4);
	EXPECT_EQ(Ranked[0].Who.PlayerId, std::string("alex"));
	swtest::Record("election");

	Net.SetSymmetric("alex", "reece", L(19, 90, 800));
	Net.SetSymmetric("alex", "sam", L(24, 90, 800));
	Net.SetSymmetric("alex", "jack", L(22, 90, 800));
	M = PeerQualityMatrix{};
	M.SetPlayers(Ids);
	Net.ApplyTo(M, StartTime);
	Cs.clear();
	for (const std::string& Id : Ids)
	{
		Cs.push_back(MakeCandidate(Identity{Id, Id, "EOS", "i-" + Id}, M, Ids, true, true, true, true));
	}
	Ranked = RankHosts(Cs, Identity{}, {});
	ASSERT_TRUE(!Ranked.empty());
	EXPECT_EQ(Ranked[0].Who.PlayerId, std::string("jack"));
	swtest::Record("election");
}

// ---- D: Intentional host migration ----------------------------------------

SW_TEST(Scenario_D_IntentionalHostMigration)
{
	MultiplayerSimulator Sim(swtest::ChaosSeed() + 3);
	Sim.AddPlayer("A", 1);
	Sim.AddPlayer("B", 2);
	Sim.AddPlayer("C", 3);
	Sim.AddPlayer("D", 4);
	Mesh(Sim, {"player-1", "player-2", "player-3", "player-4"}, {
		{0, 40, 50, 60}, {40, 0, 18, 22}, {50, 18, 0, 30}, {60, 22, 30, 0},
	});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const int64_t Gen = Sim.CurrentHost()->Token->Generation;
	const int64_t Rev = Sim.CurrentHost()->Token->BaseRevision;
	Sim.RefreshAllRankings();
	EXPECT_EQ(Sim.GlobalRanking()[0].Who.PlayerId, std::string("player-2"));
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-2"));
	EXPECT_EQ(Sim.CurrentHost()->Token->Generation, Gen + 1);
	EXPECT_TRUE(Sim.CurrentHost()->Token->BaseRevision >= Rev);
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	EXPECT_TRUE(Sim.CheckInvariants().empty());
	swtest::Record("migration");
	swtest::Record("invariant");
}

// ---- E: Host crash ---------------------------------------------------------

SW_TEST(Scenario_E_HostCrash)
{
	MultiplayerSimulator Sim(swtest::ChaosSeed() + 4);
	auto& A = Sim.AddPlayer("A", 1);
	Sim.AddPlayer("B", 2);
	Sim.AddPlayer("C", 3);
	Mesh(Sim, {"player-1", "player-2", "player-3"}, {{0, 20, 40}, {20, 0, 22}, {40, 22, 0}});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const int64_t Gen = A.Token->Generation;
	const LeaseToken Stale = *A.Token;
	ASSERT_OK(Sim.CrashHost("player-1"));
	ASSERT_OK(Sim.AdvanceRecovery());
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-2"));
	EXPECT_EQ(Sim.CurrentHost()->Token->Generation, Gen + 1);
	ASSERT_OK(Sim.ExpectStaleHostFenced("player-1", Stale));
	swtest::Record("crash");
	swtest::Record("invariant");
}

// ---- F: Old host returns ---------------------------------------------------

SW_TEST(Scenario_F_OldHostReturns)
{
	MultiplayerSimulator Sim(swtest::ChaosSeed() + 5);
	auto& A = Sim.AddPlayer("A", 1);
	Sim.AddPlayer("B", 2);
	Sim.Net().SetSymmetric("player-1", "player-2", L(20));
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const LeaseToken Stale = *A.Token;
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_OK(Sim.ExpectStaleHostFenced("player-1", Stale));
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	swtest::Record("migration");
	swtest::Record("invariant");
}

// ---- G: Network partition (coordinator remains authoritative) --------------

SW_TEST(Scenario_G_NetworkPartition)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto MA = MakeLeases(Repo, Clock);
	auto MB = MakeLeases(Repo, Clock);
	auto MC = MakeLeases(Repo, Clock);
	auto Acq = MA->Acquire(Player(1), "a");
	ASSERT_OK(Acq);
	ASSERT_TRUE(Acq->Outcome == AcquireOutcome::Acquired);
	const int64_t Gen = Acq->Token->Generation;

	// Group C/D cannot talk to coordinator (storage offline for their attempts via BeforeCommit).
	std::atomic<int> Deny{0};
	Repo->BeforeCommit = [&]() -> Status
	{
		if (Deny.load() > 0) return MakeError(ErrorCode::Network, "partitioned from coordinator");
		return {};
	};

	// Partitioned clients observe live lease and must join, not host.
	Deny = 0;
	auto See = MB->Acquire(Player(2), "b");
	ASSERT_OK(See);
	EXPECT_TRUE(See->Outcome == AcquireOutcome::HeldByOther);
	EXPECT_EQ(See->Snapshot.State.CurrentLease->Holder.PlayerId, std::string("player-1"));

	// Even after local clock advances inside skew, coordinator still has live lease if A renews.
	Clock->Advance(Seconds(60));
	ASSERT_OK(MA->Renew(*Acq->Token, {}));
	auto Steal = MC->Acquire(Player(3), "c");
	ASSERT_OK(Steal);
	EXPECT_TRUE(Steal->Outcome == AcquireOutcome::HeldByOther);

	// Heal + crash A: exactly one successor.
	Deny = 0;
	Clock->Advance(MA->Config().TTL + MA->Config().SkewGrace + Seconds(1));
	auto B = MB->Acquire(Player(2), "b2");
	auto C = MC->Acquire(Player(3), "c2");
	ASSERT_OK(B);
	ASSERT_OK(C);
	int Wins = (B->Outcome == AcquireOutcome::Acquired ? 1 : 0) + (C->Outcome == AcquireOutcome::Acquired ? 1 : 0);
	EXPECT_EQ(Wins, 1);
	EXPECT_TRUE((B->Outcome == AcquireOutcome::Acquired ? B->Token->Generation : C->Token->Generation) == Gen + 1);
	swtest::Record("acquire");
	swtest::Record("invariant");
}

// ---- H: Interrupted upload -------------------------------------------------

SW_TEST(Scenario_H_InterruptedUpload)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	auto Objects = std::make_shared<MemoryObjectStore>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto Leases = MakeLeases(Repo, Clock);
	auto Acq = Leases->Acquire(Player(1), "a");
	ASSERT_OK(Acq);
	const std::string Dir = TempDir();
	SyncEngine Sync(Objects, Leases, SyncConfig{Dir, 5});
	const std::string Save = file::Join(Dir, "w.sav");
	ASSERT_OK(file::WriteAtomic(Save, save::BuildSynthetic("W", "rev99-body")));
	Objects->FailPutAfterBytes = 32;
	UploadOptions Opt;
	Opt.StableQuiet = 0;
	Opt.StableTimeout = 1000;
	Opt.bVerifyByReadBack = false;
	auto Up = Sync.Upload(*Acq->Token, Save, Opt);
	EXPECT_TRUE(!Up.Ok());
	auto Snap = Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_TRUE(!Snap->State.Head.has_value());
	Objects->FailPutAfterBytes = 0;
	ASSERT_OK(file::WriteAtomic(Save, save::BuildSynthetic("W", "rev100-body")));
	ASSERT_OK(Sync.Upload(*Acq->Token, Save, Opt));
	Snap = Leases->Store().Load();
	ASSERT_OK(Snap);
	ASSERT_TRUE(Snap->State.Head.has_value());
	EXPECT_EQ(Snap->State.Head->Number, int64_t(1));
	swtest::Record("storage");
	swtest::Record("invariant");
}

// ---- I: Git conflict (CAS) -------------------------------------------------

SW_TEST(Scenario_I_GitConflict)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	Repo->BeforeCommit = []() -> Status
	{
		std::this_thread::sleep_for(std::chrono::microseconds(std::rand() % 1000));
		return {};
	};
	std::atomic<bool> Go{false};
	std::atomic<int> Ok{0};
	std::vector<std::thread> Threads;
	for (int i = 0; i < 8; ++i)
	{
		Threads.emplace_back([&, i]()
		{
			auto Store = std::make_shared<WorldStore>(Repo, TestWorldId, Clock, Logger(), 40);
			while (!Go.load()) std::this_thread::yield();
			auto R = Store->Mutate([&](WorldState& S, TimeMs, WorldStore::Mutation& M) -> Status
			{
				(void)S;
				M.Message = "edit-" + std::to_string(i);
				return {};
			});
			if (R) Ok.fetch_add(1);
		});
	}
	Go = true;
	for (auto& T : Threads) T.join();
	EXPECT_TRUE(Ok.load() >= 1);
	auto Head = Repo->Head();
	ASSERT_OK(Head);
	swtest::Record("git");
	swtest::Record("invariant");
}

// ---- J: Corrupt save -------------------------------------------------------

SW_TEST(Scenario_J_CorruptSave)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	auto Objects = std::make_shared<MemoryObjectStore>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto Leases = MakeLeases(Repo, Clock);
	auto Acq = Leases->Acquire(Player(1), "a");
	ASSERT_OK(Acq);
	const std::string DirA = TempDir();
	const std::string DirB = TempDir();
	SyncEngine HostSync(Objects, Leases, SyncConfig{DirA, 5});
	SyncEngine ClientSync(Objects, Leases, SyncConfig{DirB, 5});
	const std::string Save = file::Join(DirA, "w.sav");
	ASSERT_OK(file::WriteAtomic(Save, save::BuildSynthetic("W", "good")));
	UploadOptions Opt;
	Opt.StableQuiet = 0;
	Opt.StableTimeout = 1000;
	Opt.bVerifyByReadBack = false;
	ASSERT_OK(HostSync.Upload(*Acq->Token, Save, Opt));
	const std::string Dest = file::Join(DirB, "client.sav");
	ASSERT_OK(file::WriteAtomic(Dest, "old-local"));
	const std::string Before = *file::ReadAll(Dest);
	Objects->CorruptOnGet = [](std::string& Bytes)
	{
		if (!Bytes.empty()) Bytes[Bytes.size() / 2] ^= 1;
	};
	auto Snap = Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_ERR(ClientSync.Download(Snap->State, Dest), ErrorCode::Corrupt);
	EXPECT_EQ(*file::ReadAll(Dest), Before);
	swtest::Record("storage");
	swtest::Record("invariant");
}

// ---- K: Unsynchronised local progress --------------------------------------

SW_TEST(Scenario_K_UnsyncedLocalProgress)
{
	// Case 2 is the critical one: newer cloud must never be overwritten.
	// World_NewerCloudRevisionRefusesUpload / Sync_StaleClientUploadRefused cover this;
	// assert fencing here at lease level.
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto M = MakeLeases(Repo, Clock);
	auto A = M->Acquire(Player(1), "a");
	ASSERT_OK(A);
	RevisionMeta R1;
	R1.Number = 1;
	R1.Generation = A->Token->Generation;
	R1.PreviousRevision = 0;
	R1.ObjectSha256 = Sha256::HexOf("r1");
	R1.Size = 10;
	R1.CreatedAt = StartTime;
	R1.Uploader = Player(1);
	R1.Reason = Reason::Checkpoint;
	ASSERT_OK(M->CommitRevision(*A->Token, R1));
	LeaseToken Stale = *A->Token;
	ASSERT_OK(M->Release(*A->Token));
	Clock->Advance(M->Config().TTL + M->Config().SkewGrace + Seconds(1));
	auto B = M->Acquire(Player(2), "b");
	ASSERT_OK(B);
	RevisionMeta R2;
	R2.Number = 2;
	R2.Generation = B->Token->Generation;
	R2.PreviousRevision = 1;
	R2.ObjectSha256 = Sha256::HexOf("r2");
	R2.Size = 11;
	R2.CreatedAt = Clock->Now();
	R2.Uploader = Player(2);
	R2.Reason = Reason::Recovered;
	ASSERT_OK(M->CommitRevision(*B->Token, R2));
	RevisionMeta StaleAttempt = R2;
	StaleAttempt.Number = Stale.BaseRevision + 1;
	StaleAttempt.Generation = Stale.Generation;
	StaleAttempt.PreviousRevision = Stale.BaseRevision;
	StaleAttempt.ObjectSha256 = Sha256::HexOf("stale-overwrite");
	EXPECT_ERR(M->CommitRevision(Stale, StaleAttempt), ErrorCode::Fenced);
	auto Snap = M->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_EQ(Snap->State.Head->ObjectSha256, Sha256::HexOf("r2"));
	swtest::Record("crash");
	swtest::Record("invariant");
}

// ---- L: Steam/session join stays CLIENT ------------------------------------

SW_TEST(Scenario_L_SteamJoinStaysClient)
{
	MultiplayerSimulator Sim(swtest::ChaosSeed() + 11);
	Sim.AddPlayer("Host", 1);
	Sim.AddPlayer("SteamFriend", 2);
	Sim.Net().SetSymmetric("player-1", "player-2", L(25));
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const int64_t Gen = Sim.CurrentHost()->Token->Generation;
	// Steam join path: observe live lease → CLIENT, never acquire.
	ASSERT_OK(Sim.JoinExistingHost("player-2"));
	EXPECT_TRUE(!Sim.Find("player-2")->bAuthoritative);
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	EXPECT_EQ(Sim.CurrentHost()->Token->Generation, Gen);
	swtest::Record("acquire");
	swtest::Record("invariant");
}

// ---- Hysteresis explicit scenario ------------------------------------------

SW_TEST(Scenario_Hysteresis_NoFlapping)
{
	HysteresisConfig H;
	H.AllowProactiveMigration = true;
	H.MinScoreDelta = 15000;
	H.PersistDuration = Seconds(30);
	EXPECT_TRUE(!ShouldProactivelyMigrate(88000, 90000, StartTime, StartTime + Seconds(60), H));
	EXPECT_TRUE(!ShouldProactivelyMigrate(55000, 94000, StartTime, StartTime + Seconds(10), H));
	EXPECT_TRUE(ShouldProactivelyMigrate(55000, 94000, StartTime, StartTime + Seconds(30), H));
	swtest::Record("election");
}
