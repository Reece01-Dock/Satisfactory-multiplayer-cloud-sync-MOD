#include "MultiplayerSimulator.h"
#include "SharedWorldCore/HostElection/HostElection.h"
#include "SharedWorldCore/HostMigration/HostMigration.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;
using namespace swsim;

namespace
{
	Identity Named(const char* Id, const char* Name)
	{
		return Identity{Id, Name, "EOS", std::string("install-") + Id};
	}

	void ExpectOrder(const std::vector<RankedHost>& Ranked, std::initializer_list<const char*> Ids)
	{
		ASSERT_EQ(Ranked.size(), Ids.size());
		size_t i = 0;
		for (const char* Id : Ids)
		{
			EXPECT_EQ(Ranked[i].Who.PlayerId, std::string(Id));
			++i;
		}
	}

	LinkSample Link(int Rtt, int Jitter = 2, int LossBp = 0, int Upload = 8000)
	{
		LinkSample S;
		S.RttMs = Rtt;
		S.JitterMs = Jitter;
		S.LossBp = LossBp;
		S.bReachable = true;
		S.UploadKbps = Upload;
		return S;
	}
}

// ---- Peer matrix + scoring -------------------------------------------------

SW_TEST(HostElection_PeerMatrixRanksAlexThenJackThenReeceThenSam)
{
	//            Reece  Alex  Sam  Jack
	// Reece         -    18    70    31
	// Alex         19     -    24    22
	// Sam          72    25     -    48
	// Jack         30    21    47     -
	SimulatedNetworkQualityProvider Net;
	Net.SetSymmetric("reece", "alex", Link(18));
	Net.SetSymmetric("reece", "sam", Link(70));
	Net.SetSymmetric("reece", "jack", Link(31));
	Net.SetSymmetric("alex", "sam", Link(24));
	Net.SetSymmetric("alex", "jack", Link(22));
	Net.SetSymmetric("sam", "jack", Link(48));
	// slight asymmetry matching the example
	Net.SetLink("alex", "reece", Link(19));
	Net.SetLink("sam", "reece", Link(72));
	Net.SetLink("jack", "reece", Link(30));
	Net.SetLink("sam", "alex", Link(25));
	Net.SetLink("jack", "alex", Link(21));
	Net.SetLink("jack", "sam", Link(47));

	PeerQualityMatrix M;
	const std::vector<std::string> Ids = {"reece", "alex", "sam", "jack"};
	M.SetPlayers(Ids);
	Net.ApplyTo(M, StartTime);

	std::vector<HostCandidate> Cs;
	for (const std::string& Id : Ids)
	{
		Identity Who = Named(Id.c_str(), Id.c_str());
		Cs.push_back(MakeCandidate(Who, M, Ids, true, true, true, true));
	}
	auto Ranked = RankHosts(Cs, Identity{}, {});
	ASSERT_TRUE(Ranked.size() >= 4);
	ExpectOrder(Ranked, {"alex", "jack", "reece", "sam"});

	// Alex degrades: loss + jitter → Jack should rise above Alex.
	Net.SetSymmetric("alex", "reece", Link(19, 90, 800));
	Net.SetSymmetric("alex", "sam", Link(24, 90, 800));
	Net.SetSymmetric("alex", "jack", Link(22, 90, 800));
	M = PeerQualityMatrix{};
	M.SetPlayers(Ids);
	Net.ApplyTo(M, StartTime);
	Cs.clear();
	for (const std::string& Id : Ids)
	{
		Cs.push_back(MakeCandidate(Named(Id.c_str(), Id.c_str()), M, Ids, true, true, true, true));
	}
	Ranked = RankHosts(Cs, Identity{}, {});
	ASSERT_TRUE(!Ranked.empty());
	EXPECT_EQ(Ranked.front().Who.PlayerId, std::string("jack"));
}

SW_TEST(HostElection_HighLossLosesToHigherPing)
{
	HostCandidate Alex;
	Alex.Who = Named("alex", "Alex");
	Alex.bConnected = Alex.bCompatible = Alex.bStorageReachable = Alex.bSessionCapable = Alex.bHostEligible = true;
	Alex.Network = HostNetworkSummary{16, 16, 20, 80, 900, 900, 0, 3, 8000, true};

	HostCandidate Jack;
	Jack.Who = Named("jack", "Jack");
	Jack.bConnected = Jack.bCompatible = Jack.bStorageReachable = Jack.bSessionCapable = Jack.bHostEligible = true;
	Jack.Network = HostNetworkSummary{24, 24, 28, 3, 0, 0, 0, 3, 8000, true};

	auto Ranked = RankHosts({Alex, Jack}, Named("reece", "Reece"), {});
	ASSERT_EQ(Ranked.size(), size_t(2));
	EXPECT_EQ(Ranked[0].Who.PlayerId, std::string("jack"));
	EXPECT_EQ(Ranked[1].Who.PlayerId, std::string("alex"));
}

SW_TEST(HostElection_HardGatesDisqualifyExcellentPing)
{
	HostCandidate Alex;
	Alex.Who = Named("alex", "Alex");
	Alex.bConnected = Alex.bCompatible = Alex.bHostEligible = Alex.bSessionCapable = true;
	Alex.bStorageReachable = false;
	Alex.Network = HostNetworkSummary{10, 10, 12, 1, 0, 0, 0, 2, 8000, true};

	HostCandidate Jack;
	Jack.Who = Named("jack", "Jack");
	Jack.bConnected = Jack.bCompatible = Jack.bStorageReachable = Jack.bSessionCapable = Jack.bHostEligible = true;
	Jack.Network = HostNetworkSummary{40, 40, 50, 3, 0, 0, 0, 2, 8000, true};

	auto Ranked = RankHosts({Alex, Jack}, Named("reece", "Reece"), {});
	ASSERT_EQ(Ranked.size(), size_t(1));
	EXPECT_EQ(Ranked[0].Who.PlayerId, std::string("jack"));
	EXPECT_TRUE(EvaluateSuccessor(Alex).Readiness == SuccessorReadiness::NotReady);
}

SW_TEST(HostElection_DeterministicTiesBreakOnPlayerId)
{
	HostCandidate A, B;
	A.Who = Named("aaa", "A");
	B.Who = Named("bbb", "B");
	for (HostCandidate* C : {&A, &B})
	{
		C->bConnected = C->bCompatible = C->bStorageReachable = C->bSessionCapable = C->bHostEligible = true;
		C->Network = HostNetworkSummary{25, 25, 25, 2, 0, 0, 0, 2, 8000, true};
	}
	auto R1 = RankHosts({A, B}, Named("host", "H"), {});
	auto R2 = RankHosts({B, A}, Named("host", "H"), {});
	ASSERT_EQ(R1.size(), size_t(2));
	ASSERT_EQ(R2.size(), size_t(2));
	EXPECT_EQ(R1[0].Who.PlayerId, R2[0].Who.PlayerId);
	EXPECT_EQ(R1[0].Who.PlayerId, std::string("aaa"));
	EXPECT_EQ(R1[0].Score.Total, R2[0].Score.Total);
}

SW_TEST(HostElection_HysteresisIgnoresSmallAdvantage)
{
	HysteresisConfig H;
	H.AllowProactiveMigration = true;
	H.MinScoreDelta = 15000;
	H.PersistDuration = Seconds(30);
	EXPECT_TRUE(!ShouldProactivelyMigrate(88000, 90000, StartTime, StartTime + Seconds(60), H));
	EXPECT_TRUE(ShouldProactivelyMigrate(55000, 94000, StartTime, StartTime + Seconds(30), H));
	EXPECT_TRUE(!ShouldProactivelyMigrate(55000, 94000, StartTime, StartTime + Seconds(10), H));
	H.AllowProactiveMigration = false;
	EXPECT_TRUE(!ShouldProactivelyMigrate(55000, 94000, StartTime, StartTime + Seconds(60), H));
}

SW_TEST(HostElection_SelectSuccessorUsesScoring)
{
	auto S = SelectSuccessor({{Named("p2", "Jack"), true, true, true, false, 40},
			{Named("p3", "Alex"), true, true, true, false, 20}},
		{}, Named("p1", "Reece"));
	ASSERT_TRUE(S.has_value());
	EXPECT_EQ(S->PlayerId, std::string("p3"));
	S = SelectSuccessor({{Named("p2", "Jack"), true, true, true, true, 40},
			{Named("p3", "Alex"), true, true, true, false, 20}},
		{}, Named("p1", "Reece"));
	ASSERT_TRUE(S.has_value());
	EXPECT_EQ(S->PlayerId, std::string("p2")); // cached head bonus
}

// ---- Migration scenarios ---------------------------------------------------

static void WireGoodMesh(MultiplayerSimulator& Sim, const std::vector<std::string>& Ids, const std::vector<std::vector<int>>& Rtt)
{
	for (size_t i = 0; i < Ids.size(); ++i)
	{
		Sim.Net().SetUploadKbps(Ids[i], 8000);
		for (size_t j = 0; j < Ids.size(); ++j)
		{
			if (i == j) continue;
			Sim.Net().SetLink(Ids[i], Ids[j], Link(Rtt[i][j]));
		}
	}
}

SW_TEST(Migration1_BestSuccessorTakesOver)
{
	MultiplayerSimulator Sim(42);
	Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	Sim.AddPlayer("Sam", 4);
	const std::vector<std::string> Ids = {"player-1", "player-2", "player-3", "player-4"};
	// Prefer Alex (low RTT to all), then Jack, then Sam
	WireGoodMesh(Sim, Ids, {
		{0, 18, 31, 70},
		{19, 0, 22, 24},
		{30, 21, 0, 47},
		{72, 25, 48, 0},
	});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	Sim.RefreshAllRankings();
	auto Ranked = Sim.GlobalRanking();
	ASSERT_TRUE(!Ranked.empty());
	EXPECT_EQ(Ranked[0].Who.PlayerId, std::string("player-2")); // Alex
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-2"));
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	EXPECT_TRUE(Sim.CheckInvariants().empty());
}

SW_TEST(Migration2_PreferredSuccessorDisconnectsFallsBackToJack)
{
	MultiplayerSimulator Sim(7);
	Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	Sim.AddPlayer("Sam", 4);
	const std::vector<std::string> Ids = {"player-1", "player-2", "player-3", "player-4"};
	WireGoodMesh(Sim, Ids, {
		{0, 18, 31, 70},
		{19, 0, 22, 24},
		{30, 21, 0, 47},
		{72, 25, 48, 0},
	});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	Sim.RefreshAllRankings();
	EXPECT_EQ(Sim.GlobalRanking()[0].Who.PlayerId, std::string("player-2"));

	// Migration starts conceptually; Alex drops before acquire.
	Sim.SetOnline("player-2", false);
	Sim.Find("player-2")->bHasHeadCached = false;
	Sim.RefreshAllRankings();
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-3")); // Jack
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	EXPECT_TRUE(Sim.CheckInvariants().empty());
}

SW_TEST(Migration3_BestPingHighLossLoses)
{
	MultiplayerSimulator Sim(3);
	Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	Sim.Net().SetSymmetric("player-1", "player-2", Link(15, 80, 1000));
	Sim.Net().SetSymmetric("player-1", "player-3", Link(25, 3, 0));
	Sim.Net().SetSymmetric("player-2", "player-3", Link(20, 80, 1000));
	Sim.Net().SetUploadKbps("player-2", 8000);
	Sim.Net().SetUploadKbps("player-3", 8000);
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	Sim.RefreshAllRankings();
	EXPECT_EQ(Sim.GlobalRanking()[0].Who.PlayerId, std::string("player-3"));
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-3"));
}

SW_TEST(Migration4_SaveCacheEmergencyBias)
{
	HostScoreWeights W;
	W.PreferCachedSaveInEmergency = true;
	W.EmergencySaveCacheBonus = 50000;

	HostCandidate Alex = {};
	Alex.Who = Named("alex", "Alex");
	Alex.bConnected = Alex.bCompatible = Alex.bStorageReachable = Alex.bSessionCapable = Alex.bHostEligible = true;
	Alex.bHasHeadCached = false;
	Alex.Network = HostNetworkSummary{12, 12, 15, 2, 0, 0, 0, 2, 8000, true};

	HostCandidate Jack = {};
	Jack.Who = Named("jack", "Jack");
	Jack.bConnected = Jack.bCompatible = Jack.bStorageReachable = Jack.bSessionCapable = Jack.bHostEligible = true;
	Jack.bHasHeadCached = true;
	Jack.Network = HostNetworkSummary{55, 55, 90, 3, 0, 0, 0, 2, 8000, true};

	auto Normal = RankHosts({Alex, Jack}, Named("reece", "Reece"), W, false);
	ASSERT_TRUE(!Normal.empty());
	EXPECT_EQ(Normal[0].Who.PlayerId, std::string("alex")); // prep time: network wins

	auto Emergency = RankHosts({Alex, Jack}, Named("reece", "Reece"), W, true);
	ASSERT_TRUE(!Emergency.empty());
	EXPECT_EQ(Emergency[0].Who.PlayerId, std::string("jack")); // emergency: cached save wins
}

SW_TEST(Migration5_HostCrashSuccessorTakesOver)
{
	MultiplayerSimulator Sim(99);
	auto& Reece = Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	const std::vector<std::string> Ids = {"player-1", "player-2", "player-3"};
	WireGoodMesh(Sim, Ids, {{0, 20, 40}, {20, 0, 22}, {40, 22, 0}});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const int64_t Gen = Reece.Token->Generation;
	const LeaseToken Stale = *Reece.Token;
	ASSERT_OK(Sim.CrashHost("player-1"));
	ASSERT_OK(Sim.AdvanceRecovery());
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-2"));
	EXPECT_EQ(Sim.CurrentHost()->Token->Generation, Gen + 1);
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
	ASSERT_OK(Sim.ExpectStaleHostFenced("player-1", Stale));
	EXPECT_TRUE(Sim.CheckInvariants().empty());
}

SW_TEST(Migration6_StaleHostReturnsFenced)
{
	MultiplayerSimulator Sim(11);
	auto& Reece = Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	WireGoodMesh(Sim, {"player-1", "player-2"}, {{0, 20}, {20, 0}});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	const LeaseToken Stale = *Reece.Token;
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_OK(Sim.ExpectStaleHostFenced("player-1", Stale));
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
}

SW_TEST(Migration7_SessionPublishFailureFallsBack)
{
	MultiplayerSimulator Sim(13);
	Sim.AddPlayer("Reece", 1);
	auto& Alex = Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	WireGoodMesh(Sim, {"player-1", "player-2", "player-3"}, {{0, 15, 40}, {15, 0, 20}, {40, 20, 0}});
	Alex.bCanPublishSession = false;
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	ASSERT_OK(Sim.PlannedLeave("player-1"));
	ASSERT_TRUE(Sim.CurrentHost() != nullptr);
	EXPECT_EQ(Sim.CurrentHost()->Id.PlayerId, std::string("player-3"));
	EXPECT_EQ(Sim.AuthoritativeCount(), 1);
}

SW_TEST(Migration8_StorageOfflineBlocksPrematureRelease)
{
	auto Clock = std::make_shared<SimulatedClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto Leases = MakeLeases(Repo, Clock);
	Identity Host = Player(1);
	auto Acq = Leases->Acquire(Host, "n");
	ASSERT_OK(Acq);
	HostMigrationEngine Eng(Host, Clock, Leases, {});
	Eng.SetCurrentHost(Host, Acq->Token->Generation, 0);
	Eng.SetLeaseToken(*Acq->Token);
	Eng.UpdateCandidates({
		MakeCandidate(Player(2), PeerQualityMatrix{}, {"player-1", "player-2"}, true, true, true, true),
	});
	// Force empty matrix summary → still eligible via ping fallback
	HostCandidate C;
	C.Who = Player(2);
	C.bConnected = C.bCompatible = C.bStorageReachable = C.bSessionCapable = C.bHostEligible = true;
	C.PingMs = 20;
	Eng.UpdateCandidates({C});
	Eng.Config().FreezeRankingOnMigrationStart = true;
	ASSERT_OK(Eng.BeginPlannedMigration());
	Eng.SetHooks({
		{},
		[](const std::string&) -> Result<RevisionMeta> { return MakeError(ErrorCode::Network, "storage offline"); },
	});
	auto R = Eng.OnFinalSaveCompleted("save.sav");
	EXPECT_TRUE(!R.Ok());
	EXPECT_TRUE(Eng.Phase() == MigrationPhase::Running); // stayed host
	auto Snap = Leases->Store().Load();
	ASSERT_OK(Snap);
	ASSERT_TRUE(Snap->State.CurrentLease.has_value());
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, Host.PlayerId);
}

SW_TEST(Migration9_MultipleCandidatesRaceExactlyOneWins)
{
	MultiplayerSimulator Sim(21);
	Sim.AddPlayer("Reece", 1);
	Sim.AddPlayer("Alex", 2);
	Sim.AddPlayer("Jack", 3);
	Sim.AddPlayer("Sam", 4);
	WireGoodMesh(Sim, {"player-1", "player-2", "player-3", "player-4"}, {
		{0, 20, 20, 20}, {20, 0, 20, 20}, {20, 20, 0, 20}, {20, 20, 20, 0},
	});
	ASSERT_OK(Sim.BootstrapHost("player-1"));
	ASSERT_OK(Sim.CrashHost("player-1"));
	Sim.Clock().Advance(Sim.Leases().Config().TTL + Sim.Leases().Config().SkewGrace + Seconds(1));
	int Wins = 0;
	std::string Winner;
	for (int i = 2; i <= 4; ++i)
	{
		auto Acq = Sim.Leases().Acquire(Player(i), "race-" + std::to_string(i));
		ASSERT_OK(Acq);
		if (Acq->Outcome == AcquireOutcome::Acquired)
		{
			++Wins;
			Winner = Acq->Token->Holder.PlayerId;
		}
	}
	EXPECT_EQ(Wins, 1);
	EXPECT_TRUE(!Winner.empty());
}

SW_TEST(Migration10_RankingFrozenDuringPlannedMigration)
{
	auto Clock = std::make_shared<SimulatedClock>(StartTime);
	auto Repo = std::make_shared<MemoryRepository>();
	ASSERT_OK(CreateTestWorld(Repo, Clock));
	auto Leases = MakeLeases(Repo, Clock);
	Identity Host = Player(1);
	auto Acq = Leases->Acquire(Host, "n");
	ASSERT_OK(Acq);
	HostMigrationEngine Eng(Host, Clock, Leases, {});
	Eng.SetCurrentHost(Host, Acq->Token->Generation, 0);
	Eng.SetLeaseToken(*Acq->Token);
	HostCandidate Alex, Jack;
	Alex.Who = Player(2); Alex.bConnected = Alex.bCompatible = Alex.bStorageReachable = Alex.bSessionCapable = true; Alex.PingMs = 15;
	Jack.Who = Player(3); Jack.bConnected = Jack.bCompatible = Jack.bStorageReachable = Jack.bSessionCapable = true; Jack.PingMs = 40;
	Eng.UpdateCandidates({Alex, Jack});
	ASSERT_OK(Eng.BeginPlannedMigration());
	auto Chosen = Eng.PreferredSuccessor(true);
	ASSERT_TRUE(Chosen.has_value());
	EXPECT_EQ(Chosen->Who.PlayerId, std::string("player-2"));

	Alex.PingMs = 200;
	Alex.Network = HostNetworkSummary{200, 200, 250, 50, 0, 0, 0, 2, 8000, true};
	Jack.PingMs = 20;
	Eng.UpdateCandidates({Alex, Jack}); // ignored: frozen
	Chosen = Eng.PreferredSuccessor(true);
	ASSERT_TRUE(Chosen.has_value());
	EXPECT_EQ(Chosen->Who.PlayerId, std::string("player-2"));
}

SW_TEST(Migration_RollingStatsTrackDegradation)
{
	RollingLinkStats S;
	S.Observe(Link(20), StartTime);
	EXPECT_EQ(S.RttMs, 20);
	S.Observe(Link(40, 10, 100), StartTime + Seconds(5));
	EXPECT_TRUE(S.RttMs > 20);
	EXPECT_TRUE(S.RttMs < 40);
	S.Observe(LinkSample{0, 0, 0, false, 0}, StartTime + Seconds(10));
	EXPECT_TRUE(!S.bReachable);
	EXPECT_TRUE(S.ReachFailures >= 1);
}

SW_TEST(Chaos_SmallSuiteReproducible)
{
	constexpr int Sessions = 40;
	const uint64_t BaseSeed = swtest::ChaosSeed();
	for (int Session = 0; Session < Sessions; ++Session)
	{
		const uint64_t Seed = BaseSeed + static_cast<uint64_t>(Session) * 17;
		MultiplayerSimulator Sim(Seed);
		std::mt19937_64 Rng(Seed);
		const int N = 4 + static_cast<int>(Rng() % 5); // 4..8
		for (int i = 1; i <= N; ++i) Sim.AddPlayer("P" + std::to_string(i), i);
		std::vector<std::string> Ids;
		for (int i = 1; i <= N; ++i) Ids.push_back("player-" + std::to_string(i));
		for (int i = 0; i < N; ++i)
		{
			Sim.Net().SetUploadKbps(Ids[i], 4000 + static_cast<int>(Rng() % 6000));
			for (int j = 0; j < N; ++j)
			{
				if (i == j) continue;
				Sim.Net().SetLink(Ids[i], Ids[j], Link(10 + static_cast<int>(Rng() % 80), static_cast<int>(Rng() % 20), static_cast<int>(Rng() % 200)));
			}
		}
		ASSERT_OK(Sim.BootstrapHost("player-1"));
		const int Events = 8 + static_cast<int>(Rng() % 12);
		for (int E = 0; E < Events; ++E)
		{
			const int Kind = static_cast<int>(Rng() % 6);
			SimPlayer* H = Sim.CurrentHost();
			if (!H) break;
			if (Kind == 0)
			{
				// latency spike
				const int A = 1 + static_cast<int>(Rng() % N);
				const int B = 1 + static_cast<int>(Rng() % N);
				if (A != B) Sim.Net().SetSymmetric("player-" + std::to_string(A), "player-" + std::to_string(B), Link(5 + static_cast<int>(Rng() % 120), static_cast<int>(Rng() % 40), static_cast<int>(Rng() % 500)));
				Sim.RefreshAllRankings();
			}
			else if (Kind == 1 && N > 2)
			{
				// Toggle storage on at most one non-host player; keep ≥1 storage-ready client.
				const int Vic = 2 + static_cast<int>(Rng() % (N - 1));
				SimPlayer* HostNow = Sim.CurrentHost();
				for (auto& P : Sim.Players())
				{
					if (HostNow && P->Id.PlayerId == HostNow->Id.PlayerId) continue;
					P->bStorageReachable = true;
				}
				if (SimPlayer* V = Sim.Find("player-" + std::to_string(Vic)))
				{
					V->bStorageReachable = (Rng() % 3) != 0; // usually leave reachable
				}
				Sim.RefreshAllRankings();
			}
			else if (Kind == 2)
			{
				auto Stale = H->Token;
				const std::string CrashedId = H->Id.PlayerId;
				ASSERT_OK(Sim.CrashHost(CrashedId));
				auto Rec = Sim.AdvanceRecovery();
				if (!Rec.Ok())
				{
					swtest::ReportFailure(__FILE__, __LINE__, "chaos crash recovery failed seed=" + std::to_string(Seed) + ": " + Rec.Err().Describe() + "\n" + Sim.DumpAllTraces());
					return;
				}
				if (Stale) ASSERT_OK(Sim.ExpectStaleHostFenced(CrashedId, *Stale));
				Sim.SetOnline(CrashedId, true);
				if (SimPlayer* Back = Sim.Find(CrashedId)) Back->bHostEligible = true;
			}
			else if (Kind == 3 && Sim.AuthoritativeCount() == 1)
			{
				const std::string Leaving = H->Id.PlayerId;
				auto Rec = Sim.PlannedLeave(Leaving);
				if (!Rec.Ok())
				{
					swtest::ReportFailure(__FILE__, __LINE__, "chaos planned leave failed seed=" + std::to_string(Seed) + ": " + Rec.Err().Describe() + "\n" + Sim.DumpAllTraces());
					return;
				}
				if (SimPlayer* Left = Sim.Find(Leaving)) Left->bHostEligible = true;
			}
			else if (Kind == 4)
			{
				Sim.Objects().Offline = (Rng() % 5) == 0;
			}
			else
			{
				Sim.Clock().Advance(Seconds(5 + static_cast<int>(Rng() % 30)));
			}
			auto Viol = Sim.CheckInvariants();
			if (!Viol.empty())
			{
				swtest::ReportFailure(__FILE__, __LINE__, "chaos invariant seed=" + std::to_string(Seed) + " event=" + std::to_string(E) + ": " + Viol[0].What + "\n" + Sim.DumpAllTraces());
				return;
			}
		}
		EXPECT_TRUE(Sim.AuthoritativeCount() <= 1);
		swtest::Record("invariant");
	}
}

#if SW_CHAOS_FULL
SW_TEST(Chaos_LargeOvernightSuite)
{
	constexpr int Sessions = 2000;
	const uint64_t BaseSeed = swtest::ChaosSeed();
	for (int Session = 0; Session < Sessions; ++Session)
	{
		const uint64_t Seed = BaseSeed + static_cast<uint64_t>(Session) * 131;
		MultiplayerSimulator Sim(Seed);
		std::mt19937_64 Rng(Seed);
		const int N = 4 + static_cast<int>(Rng() % 12); // 4..15
		for (int i = 1; i <= N; ++i) Sim.AddPlayer("P" + std::to_string(i), i);
		std::vector<std::string> Ids;
		for (int i = 1; i <= N; ++i) Ids.push_back("player-" + std::to_string(i));
		for (int i = 0; i < N; ++i)
		{
			Sim.Net().SetUploadKbps(Ids[i], 2000 + static_cast<int>(Rng() % 10000));
			for (int j = 0; j < N; ++j)
			{
				if (i == j) continue;
				Sim.Net().SetLink(Ids[i], Ids[j], Link(8 + static_cast<int>(Rng() % 100), static_cast<int>(Rng() % 30), static_cast<int>(Rng() % 400)));
			}
		}
		ASSERT_OK(Sim.BootstrapHost("player-1"));
		const int Events = 20 + static_cast<int>(Rng() % 40);
		for (int E = 0; E < Events; ++E)
		{
			SimPlayer* H = Sim.CurrentHost();
			if (!H) break;
			switch (Rng() % 7)
			{
			case 0:
				Sim.Net().SetSymmetric(Ids[Rng() % N], Ids[Rng() % N], Link(10 + static_cast<int>(Rng() % 150)));
				Sim.RefreshAllRankings();
				break;
			case 1:
				Sim.Find(Ids[1 + Rng() % (N - 1)])->bStorageReachable = (Rng() % 2) == 0;
				break;
			case 2:
			{
				auto Stale = H->Token;
				ASSERT_OK(Sim.CrashHost(H->Id.PlayerId));
				ASSERT_OK(Sim.AdvanceRecovery());
				if (Stale) ASSERT_OK(Sim.ExpectStaleHostFenced(H->Id.PlayerId, *Stale));
				Sim.SetOnline(H->Id.PlayerId, true);
				break;
			}
			case 3:
				ASSERT_OK(Sim.PlannedLeave(H->Id.PlayerId));
				break;
			case 4:
				Sim.Objects().Offline = (Rng() % 8) == 0;
				break;
			default:
				Sim.Clock().Advance(Seconds(3 + static_cast<int>(Rng() % 40)));
				break;
			}
			auto Viol = Sim.CheckInvariants();
			if (!Viol.empty())
			{
				swtest::ReportFailure(__FILE__, __LINE__, "chaos-full seed=" + std::to_string(Seed) + ": " + Viol[0].What + "\n" + Sim.DumpAllTraces());
				return;
			}
		}
	}
}
#endif
