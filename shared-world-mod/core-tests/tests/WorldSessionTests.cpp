// Port of shared-world-helper/internal/world tests + migration, host-loss
// recovery, restore, compatibility and membership.
#include <chrono>
#include <memory>
#include <thread>

#include "SharedWorldCore/Migration/Migration.h"
#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "SharedWorldCore/World/Creation.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	struct Cluster
	{
		std::shared_ptr<FakeClock> Clock = std::make_shared<FakeClock>(StartTime);
		std::shared_ptr<MemoryRepository> Repo = std::make_shared<MemoryRepository>();
		std::shared_ptr<MemoryObjectStore> Objects = std::make_shared<MemoryObjectStore>();
		Cluster()
		{
			Repo->BeforeCommit = []() -> Status
			{
				std::this_thread::sleep_for(std::chrono::microseconds(200));
				return {};
			};
		}
	};

	/** One player's PC: its own session, data dir and save dir; shared cloud. */
	struct Peer
	{
		Cluster* C = nullptr;
		Identity Me;
		std::string DataDir = TempDir();
		std::string SaveDir = TempDir();
		std::shared_ptr<LeaseManager> Leases;
		std::shared_ptr<SyncEngine> Sync;
		std::unique_ptr<WorldSession> Session;
		std::string GameBuild = "491125";

		void Boot()
		{
			Leases = MakeLeases(C->Repo, C->Clock);
			Sync = std::make_shared<SyncEngine>(C->Objects, Leases, SyncConfig{DataDir, 10});
			SessionConfig Cfg;
			Cfg.Me = Me;
			Cfg.SaveDirectory = SaveDir;
			Cfg.SaveName = "SharedWorld_our-factory";
			Cfg.Versions.GameBuild = GameBuild;
			Cfg.Versions.ModVersion = "0.2.0";
			Cfg.Upload.StableQuiet = 20;
			Cfg.Upload.StableTimeout = 5000;
			Session = std::make_unique<WorldSession>(Leases, Sync, Cfg);
		}
		/** The game process dies: no release, no final upload. */
		void Crash() { Session.reset(); }
		SessionView V() const { return Session->View(); }
		SessionState St() const { return Session->View().State; }
		std::string SavePath() const { return file::Join(SaveDir, "SharedWorld_our-factory.sav"); }
		/** The game writes its save (what SaveGame does before OnSaveCompleted). */
		void GameWritesSave(const std::string& Body)
		{
			(void)file::WriteAtomic(SavePath(), save::BuildSynthetic("S", Body));
		}
		void Settle() { if (Session) Session->WaitIdle(); }
	};

	Peer MakePeer(Cluster& C, int N)
	{
		Peer P;
		P.C = &C;
		P.Me = Player(N);
		P.Boot();
		return P;
	}

	void Settle(std::initializer_list<Peer*> Peers)
	{
		for (int Round = 0; Round < 3; ++Round)
			for (Peer* P : Peers) P->Settle();
	}

	/** Advances cloud time and ticks everyone (heartbeats, polls). */
	void Step(Cluster& C, std::initializer_list<Peer*> Peers, TimeMs Advance)
	{
		C.Clock->Advance(Advance);
		for (Peer* P : Peers)
			if (P->Session) P->Session->Tick(C.Clock->Now());
		Settle(Peers);
	}

	void Seed(Cluster& C, const std::string& Body, bool bRestrict = false)
	{
		Peer Creator = MakePeer(C, 1);
		const std::string Src = file::Join(TempDir(), "MyOldSave.sav");
		(void)file::CreateExclusive(Src, save::BuildSynthetic("MyOldSave", Body));
		CreateWorldParams P;
		P.Name = "Our Factory";
		P.Creator = Player(1);
		P.SourceSavePath = Src;
		P.OriginalSaveName = "MyOldSave";
		P.Versions.GameBuild = "491125";
		P.Versions.ModVersion = "0.2.0";
		P.Settings.Name = "Our Factory";
		P.bRestrictToMembers = bRestrict;
		auto R = CreateSharedWorld(*Creator.Leases, *Creator.Sync, P, 20);
		if (!R) swtest::ReportFailure(__FILE__, __LINE__, "seed failed: " + R.Err().Describe());
	}

	JoinInfo Session(const std::string& Id) { return JoinInfo{"online-session-id", Id, "EOS"}; }

	/** Host reaches HOSTING with published join data. */
	void HostUp(Peer& P, const std::string& SessionId)
	{
		P.Session->Play();
		P.Settle();
		P.Session->OnHostingStarted(Session(SessionId));
		P.Settle();
	}

	std::string Describe(const SessionView& V)
	{
		return std::string(ToString(V.State)) + " (" + V.Message + ")" + (V.Error ? " error=" + V.Error->Code + ": " + V.Error->Detail : "");
	}
}

#define EXPECT_STATE(peer, state) \
	do { const SessionView v_ = (peer).V(); if (v_.State != (state)) swtest::ReportFailure(__FILE__, __LINE__, std::string("expected ") + ToString(state) + ", got " + Describe(v_)); } while (0)

SW_TEST(World_CreateConvertsExistingSave)
{
	Cluster C;
	Seed(C, "the factory");
	Peer A = MakePeer(C, 1);
	auto Snap = A.Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_EQ(Snap->State.HeadNumber(), int64_t(1));
	EXPECT_EQ(Snap->State.Head->Reason, std::string(Reason::Import));
	EXPECT_TRUE(!Snap->State.CurrentLease.has_value());
	auto Info = A.Leases->Store().LoadInfo(Snap->CommitId);
	ASSERT_OK(Info);
	EXPECT_EQ(Info->Name, std::string("Our Factory"));
	EXPECT_EQ(Info->OriginalSaveName, std::string("MyOldSave"));
	// Converting an invalid save is refused before anything is created.
	Cluster C2;
	Peer B = MakePeer(C2, 2);
	const std::string Bad = file::Join(TempDir(), "bad.sav");
	ASSERT_OK(file::CreateExclusive(Bad, "not a save"));
	CreateWorldParams P;
	P.Name = "X";
	P.Creator = Player(2);
	P.SourceSavePath = Bad;
	P.Settings.Name = "X";
	EXPECT_ERR(CreateSharedWorld(*B.Leases, *B.Sync, P, 20), ErrorCode::Corrupt);
	EXPECT_TRUE(!C2.Repo->Head().Ok());
}

// The vertical slice: first PLAY hosts, second PLAY joins the published session.
SW_TEST(World_PlayDecidesHostThenJoin)
{
	Cluster C;
	Seed(C, "the factory");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	A.Session->Play();
	A.Settle();
	EXPECT_STATE(A, SessionState::ReadyToHost);
	EXPECT_TRUE(A.V().TheDecision == Decision::Host);
	EXPECT_EQ(A.V().Revision, int64_t(1));
	ASSERT_OK(save::ValidateFile(A.V().SavePath));
	A.Session->OnHostingStarted(Session("EOS:abc"));
	A.Settle();
	EXPECT_STATE(A, SessionState::Hosting);

	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::JoinReady);
	EXPECT_TRUE(B.V().TheDecision == Decision::Join);
	EXPECT_EQ(B.V().HostName, std::string("P1"));
	ASSERT_TRUE(B.V().Join.has_value());
	EXPECT_EQ(B.V().Join->Data, std::string("EOS:abc"));
	EXPECT_TRUE(!file::Exists(B.SavePath())); // the joiner downloaded nothing
	const WorldSummary S = Summarize(*B.Leases);
	EXPECT_TRUE(S.Status == WorldStatus::Online);
	EXPECT_EQ(S.HostName, std::string("P1"));
	EXPECT_EQ(S.Name, std::string("Our Factory"));
}

// Test 10: two clients press PLAY at the same moment -> one hosts, the other joins.
SW_TEST(World_SimultaneousPlayOneHostsOneJoins)
{
	for (int Round = 0; Round < 5; ++Round)
	{
		Cluster C;
		Seed(C, "x");
		Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
		A.Session->Play();
		B.Session->Play();
		Settle({&A, &B});
		Peer* Host = A.St() == SessionState::ReadyToHost ? &A : B.St() == SessionState::ReadyToHost ? &B : nullptr;
		ASSERT_TRUE(Host != nullptr);
		Peer* Other = Host == &A ? &B : &A;
		EXPECT_STATE(*Other, SessionState::WaitingForHost); // never a second host
		EXPECT_TRUE(Other->V().TheDecision == Decision::Join);
		Host->Session->OnHostingStarted(Session("s1"));
		Host->Settle();
		Step(C, {&A, &B}, Seconds(3));
		EXPECT_STATE(*Other, SessionState::JoinReady);
		EXPECT_EQ(Other->V().Join->Data, std::string("s1"));
	}
}

SW_TEST(World_HostStopUploadsAndReleases)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	HostUp(A, "s1");
	A.GameWritesSave("rev2 progress");
	A.Session->OnSaveCompleted(SaveKind::Final);
	A.Settle();
	EXPECT_STATE(A, SessionState::Idle);
	WorldSummary S = Summarize(*A.Leases);
	EXPECT_TRUE(S.Status == WorldStatus::Available);
	EXPECT_EQ(S.Revision, int64_t(2));
	EXPECT_EQ(S.LastHostName, std::string("P1"));
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::ReadyToHost);
	EXPECT_EQ(B.V().Revision, int64_t(2));
	EXPECT_EQ(file::ReadAll(B.SavePath()).Value(), file::ReadAll(A.SavePath()).Value());
}

SW_TEST(World_CheckpointsAdvanceRevisionWhileHosting)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	A.GameWritesSave("checkpoint 1");
	A.Session->OnSaveCompleted(SaveKind::Checkpoint);
	A.Settle();
	EXPECT_STATE(A, SessionState::Hosting);
	A.GameWritesSave("checkpoint 2");
	A.Session->OnSaveCompleted(SaveKind::Checkpoint);
	A.Settle();
	EXPECT_EQ(A.V().Revision, int64_t(3));
	// Heartbeats keep the lease alive over time.
	for (int i = 0; i < 10; ++i) Step(C, {&A}, Seconds(25));
	EXPECT_STATE(A, SessionState::Hosting);
	EXPECT_TRUE(Summarize(*A.Leases).Status == WorldStatus::Online);
}

// Test 7 + 8: host crash -> next host gets a higher generation; the crashed
// host's stale progress never overwrites the newer revision.
SW_TEST(World_HostCrashRecoveryAndStaleReturn)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	HostUp(A, "s1");
	const int64_t GenA = A.V().Generation;
	A.GameWritesSave("A unsynced progress");
	A.Crash();
	EXPECT_TRUE(Summarize(*B.Leases).Status == WorldStatus::Online); // until TTL + grace
	C.Clock->Advance(Minutes(3));
	EXPECT_TRUE(Summarize(*B.Leases).Status == WorldStatus::Recoverable);
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::ReadyToHost);
	EXPECT_EQ(B.V().Generation, GenA + 1);
	B.Session->OnHostingStarted(Session("s2"));
	B.Settle();
	B.GameWritesSave("B progress");
	B.Session->OnSaveCompleted(SaveKind::Final);
	B.Settle();

	A.Boot(); // A restarts the game
	A.Session->Play();
	A.Settle();
	EXPECT_STATE(A, SessionState::ReadyToHost);
	EXPECT_EQ(A.V().Revision, int64_t(2));
	EXPECT_EQ(file::ReadAll(A.SavePath()).Value(), file::ReadAll(B.SavePath()).Value());
	bool bPreserved = false;
	for (const LocalBackup& Bk : A.Sync->Backups().List("our-factory").Value())
	{
		if (Bk.bProtected && file::ReadAll(Bk.Path).Value() == save::BuildSynthetic("S", "A unsynced progress")) bPreserved = true;
	}
	EXPECT_TRUE(bPreserved);
	bool bNoted = false;
	for (const std::string& Step : A.V().Steps) bNoted |= Step.find("Unsynchronised progress found") != std::string::npos;
	EXPECT_TRUE(bNoted);
}

// Crashed host comes back before anyone else played: its own lease is resumed
// immediately (no wait for expiry) and its unsynced progress is published.
SW_TEST(World_CrashedHostResumesOwnLeaseAndRecoversProgress)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	const int64_t Gen = A.V().Generation;
	const std::string Progress = save::BuildSynthetic("S", "progress made before the crash");
	(void)file::WriteAtomic(A.SavePath(), Progress);
	A.Crash();
	A.Boot();
	A.Session->Play();
	A.Settle();
	EXPECT_STATE(A, SessionState::ReadyToHost);
	EXPECT_EQ(A.V().Generation, Gen); // same lease, resumed
	EXPECT_EQ(A.V().Revision, int64_t(2));
	EXPECT_EQ(file::ReadAll(A.SavePath()).Value(), Progress);
	auto Snap = A.Leases->Store().Load();
	EXPECT_EQ(Snap->State.Head->Reason, std::string(Reason::Recovered));
}

// Race 3 end-to-end: the replaced host's heartbeat detects fencing and stops.
SW_TEST(World_ReplacedHostDetectsLeaseLoss)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	HostUp(A, "s1");
	// A's network is down for 5 minutes: no ticks reach it.
	C.Clock->Advance(Minutes(5));
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::ReadyToHost);
	Step(C, {&A}, Seconds(1)); // A's network returns: heartbeat is fenced
	EXPECT_STATE(A, SessionState::LeaseLost);
	ASSERT_TRUE(A.V().Error.has_value());
	EXPECT_EQ(A.V().Error->Code, std::string("LEASE_LOST"));
	A.GameWritesSave("stale");
	A.Session->OnSaveCompleted(SaveKind::Final); // ignored: no authority
	A.Settle();
	EXPECT_EQ(Summarize(*B.Leases).Revision, int64_t(1));
}

SW_TEST(World_SameUserSecondInstanceBlocked)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	Peer A2 = MakePeer(C, 1);
	A2.Me.InstallId = "other-pc";
	A2.Boot();
	A2.Session->Play();
	A2.Settle();
	EXPECT_STATE(A2, SessionState::Error);
	EXPECT_EQ(A2.V().Error->Code, std::string("ALREADY_HOSTING_ELSEWHERE"));
}

SW_TEST(World_CorruptCloudSaveReleasesLease)
{
	Cluster C;
	Seed(C, "rev1");
	Peer B = MakePeer(C, 2);
	C.Objects->CorruptOnGet = [](std::string& D) { D[100] = static_cast<char>(~D[100]); };
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::Error);
	EXPECT_EQ(B.V().Error->Code, std::string("CORRUPT_DOWNLOAD"));
	EXPECT_TRUE(B.V().Error->bLocalSaveUnchanged);
	EXPECT_TRUE(Summarize(*B.Leases).Status == WorldStatus::Available); // lease released
}

SW_TEST(World_HostWithoutJoinInfoStillMeansJoin)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	A.Session->Play();
	A.Settle();
	A.Session->OnHostingStarted(std::nullopt);
	A.Settle();
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::JoinReady);
	EXPECT_TRUE(!B.V().Join.has_value());
	EXPECT_TRUE(B.V().Message.find("friends list") != std::string::npos);
}

// Test 9: intentional migration -> successor starts exactly the committed revision,
// other clients reconnect to the successor.
SW_TEST(World_PlannedMigrationHandsOverExactRevision)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2), Cc = MakePeer(C, 3);
	HostUp(A, "sessionA");
	A.Session->SetPlayers({{"P1", "player-1"}, {"P2", "player-2"}, {"P3", "player-3"}});
	for (Peer* P : {&B, &Cc})
	{
		P->Session->Play();
		P->Settle();
		EXPECT_STATE(*P, SessionState::JoinReady);
		P->Session->OnJoinedAsClient();
		EXPECT_STATE(*P, SessionState::Joined);
	}
	const int64_t GenA = A.V().Generation;
	auto Successor = SelectSuccessor({{Player(2), true, true, true, false, 40}, {Player(3), true, true, true, false, 20}}, {}, Player(1));
	ASSERT_TRUE(Successor.has_value());
	EXPECT_EQ(Successor->PlayerId, std::string("player-3")); // lower ping wins
	Successor = SelectSuccessor({{Player(2), true, true, true, true, 40}, {Player(3), true, true, true, false, 20}}, {}, Player(1));
	EXPECT_EQ(Successor->PlayerId, std::string("player-2")); // cached head beats ping

	A.Session->RequestMigration(*Successor);
	EXPECT_STATE(A, SessionState::Migrating);
	A.GameWritesSave("state at migration");
	A.Session->OnSaveCompleted(SaveKind::Migration);
	A.Settle();
	EXPECT_STATE(A, SessionState::Idle);
	const WorldState Mid = A.Leases->Store().Load()->State;
	ASSERT_TRUE(Mid.PendingHandoff.has_value());
	const RevisionMeta Committed = *Mid.Head;
	EXPECT_EQ(Committed.Reason, std::string(Reason::Migration));
	EXPECT_TRUE(Summarize(*Cc.Leases).Status == WorldStatus::Migrating);

	Step(C, {&B, &Cc}, Seconds(3));
	EXPECT_STATE(B, SessionState::ReadyToHost);  // successor takes over
	EXPECT_STATE(Cc, SessionState::Reconnecting); // waits for the new host
	EXPECT_EQ(B.V().Generation, GenA + 1);
	EXPECT_EQ(B.V().Revision, Committed.Number);
	EXPECT_EQ(file::Hash(B.SavePath())->Sha256, Committed.ObjectSha256); // exactly the committed revision
	B.Session->OnHostingStarted(Session("sessionB"));
	B.Settle();
	Step(C, {&B, &Cc}, Seconds(3));
	EXPECT_STATE(Cc, SessionState::JoinReady);
	EXPECT_EQ(Cc.V().Join->Data, std::string("sessionB"));
	EXPECT_EQ(Cc.V().HostName, std::string("P2"));
}

// Host crash while clients are connected: they wait for the lease to expire,
// one of them takes over (deterministic order, CAS decides), the rest reconnect.
SW_TEST(World_HostCrashClientsRecover)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2), Cc = MakePeer(C, 3);
	HostUp(A, "sessionA");
	A.Session->SetPlayers({{"P1", "player-1"}, {"P2", "player-2"}, {"P3", "player-3"}});
	Step(C, {&A}, Seconds(25)); // heartbeat publishes the player list
	for (Peer* P : {&B, &Cc})
	{
		P->Session->Play();
		P->Settle();
		P->Session->OnJoinedAsClient();
	}
	A.Crash();
	B.Session->OnHostConnectionLost();
	Cc.Session->OnHostConnectionLost();
	EXPECT_STATE(B, SessionState::Reconnecting);
	// Last heartbeat at t=25s: lease expires at 115s, +30s skew grace = 145s.
	// Takeover slots: rank 1 (P2) at 150s, rank 2 (P3) at 155s.
	Step(C, {&B, &Cc}, Seconds(60));             // t=85
	EXPECT_STATE(B, SessionState::Reconnecting); // lease still valid: wait
	Step(C, {&B, &Cc}, Seconds(62));             // t=147: expired, no slot open yet
	EXPECT_STATE(B, SessionState::Reconnecting);
	EXPECT_STATE(Cc, SessionState::Reconnecting);
	Step(C, {&B, &Cc}, Seconds(4));              // t=151: only B's slot is open
	EXPECT_STATE(B, SessionState::ReadyToHost);
	EXPECT_STATE(Cc, SessionState::Reconnecting);
	B.Session->OnHostingStarted(Session("sessionB"));
	B.Settle();
	Step(C, {&B, &Cc}, Seconds(3));
	EXPECT_STATE(Cc, SessionState::JoinReady);
	EXPECT_EQ(Cc.V().Join->Data, std::string("sessionB"));
	EXPECT_EQ(B.V().Revision, int64_t(1)); // newest trustworthy revision
}

// A player who quit to the menu must never take over in the background.
SW_TEST(World_ClientThatLeftNeverTakesOver)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1), B = MakePeer(C, 2);
	HostUp(A, "sessionA");
	A.Session->SetPlayers({{"P1", "player-1"}, {"P2", "player-2"}});
	Step(C, {&A}, Seconds(25));
	B.Session->Play();
	B.Settle();
	B.Session->OnJoinedAsClient();
	EXPECT_STATE(B, SessionState::Joined);
	B.Session->OnLeftAsClient();
	EXPECT_STATE(B, SessionState::Idle);
	A.Crash();
	Step(C, {&B}, Seconds(200)); // far past expiry and every takeover slot
	Step(C, {&B}, Seconds(5));
	EXPECT_STATE(B, SessionState::Idle);
	EXPECT_TRUE(!file::Exists(B.SavePath())); // nothing downloaded, nothing hosted
	B.Session->OnLeftAsClient(); // no-op outside JOINED
	EXPECT_STATE(B, SessionState::Idle);
}

SW_TEST(World_NewerCloudRevisionRefusesUpload)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	// Revision 2 appears behind the session's back (another process of the same lease).
	auto Snap = A.Leases->Store().Load();
	LeaseToken Tok{"our-factory", Snap->State.Generation, Snap->State.CurrentLease->Nonce, 1, Player(1)};
	const std::string Other = file::Join(TempDir(), "other.sav");
	ASSERT_OK(file::CreateExclusive(Other, save::BuildSynthetic("S", "committed elsewhere")));
	UploadOptions O;
	O.StableQuiet = 20;
	ASSERT_OK(A.Sync->Upload(Tok, Other, O));
	A.GameWritesSave("local progress on rev1");
	A.Session->OnSaveCompleted(SaveKind::Checkpoint);
	A.Settle();
	EXPECT_STATE(A, SessionState::Error);
	EXPECT_EQ(A.V().Error->Code, std::string("NEWER_SAVE_EXISTS"));
	EXPECT_EQ(A.V().Error->CloudRevision, int64_t(2));
	EXPECT_EQ(A.V().Error->LocalRevision, int64_t(1));
	ASSERT_TRUE(!A.V().Error->BackupPath.empty());
	EXPECT_EQ(file::ReadAll(A.V().Error->BackupPath).Value(), save::BuildSynthetic("S", "local progress on rev1"));
}

// Test 11 end-to-end: restore from History appends a new revision.
SW_TEST(World_RestoreFromHistory)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	A.GameWritesSave("rev2");
	A.Session->OnSaveCompleted(SaveKind::Final);
	A.Settle();
	A.Session->Restore(1);
	A.Settle();
	EXPECT_STATE(A, SessionState::Idle);
	auto Snap = A.Leases->Store().Load();
	EXPECT_EQ(Snap->State.HeadNumber(), int64_t(3));
	EXPECT_EQ(Snap->State.Head->RestoredFrom, int64_t(1));
	auto H = A.Sync->History(Snap->CommitId);
	EXPECT_EQ(H->size(), size_t(3));
	EXPECT_EQ((*H)[0].ObjectSha256, (*H)[2].ObjectSha256);
	// The next PLAY hosts the restored content.
	Peer B = MakePeer(C, 2);
	B.Session->Play();
	B.Settle();
	EXPECT_EQ(file::ReadAll(B.SavePath()).Value(), save::BuildSynthetic("MyOldSave", "rev1"));
}

SW_TEST(World_IncompatibleGameVersionRefused)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	HostUp(A, "s1");
	A.GameWritesSave("saved by a newer build");
	// Simulate a newer game build having written the head revision.
	auto Snap = A.Leases->Store().Load();
	A.Crash();
	A.GameBuild = "500000";
	A.Boot();
	A.Session->Play();
	A.Settle(); // resumes the lease and uploads with GameBuild 500000
	Peer B = MakePeer(C, 2); // still on 491125
	C.Clock->Advance(Minutes(10));
	B.Session->Play();
	B.Settle();
	EXPECT_STATE(B, SessionState::Error);
	EXPECT_EQ(B.V().Error->Code, std::string("INCOMPATIBLE"));
	EXPECT_TRUE(B.V().Error->Message.find("newer Satisfactory version") != std::string::npos);
	EXPECT_TRUE(!file::Exists(B.SavePath()));
}

SW_TEST(World_MembershipRequiredWhenRestricted)
{
	Cluster C;
	Seed(C, "rev1", /*bRestrict*/ true);
	Peer Stranger = MakePeer(C, 7);
	Stranger.Session->Play();
	Stranger.Settle();
	EXPECT_STATE(Stranger, SessionState::Error);
	EXPECT_EQ(Stranger.V().Error->Code, std::string("NOT_A_MEMBER"));
	Peer Owner = MakePeer(C, 1);
	Owner.Session->Play();
	Owner.Settle();
	EXPECT_STATE(Owner, SessionState::ReadyToHost);
}

SW_TEST(World_StorageUnavailableGivesFriendlyError)
{
	Cluster C;
	Seed(C, "rev1");
	Peer A = MakePeer(C, 1);
	C.Repo->BeforeCommit = []() -> Status { return MakeError(ErrorCode::Network, "connection refused"); };
	A.Session->Play();
	A.Settle();
	EXPECT_STATE(A, SessionState::Error);
	EXPECT_TRUE(A.V().Error->Message.find("could not be reached") != std::string::npos);
	EXPECT_TRUE(A.V().Error->bRetryable);
	EXPECT_TRUE(!file::Exists(A.SavePath()));
	C.Repo->BeforeCommit = nullptr;
	A.Session->Play(); // Retry from ERROR
	A.Settle();
	EXPECT_STATE(A, SessionState::ReadyToHost);
}

SW_TEST(Migration_RecoveryCandidateRules)
{
	WorldState S;
	S.WorldId = "w";
	S.Generation = 5;
	RevisionMeta H;
	H.Number = 10;
	H.ObjectSha256 = Sha256::HexOf("head");
	S.Head = H;
	auto Cand = [&](const char* Src, int64_t Gen, int64_t Base, const char* Content, TimeMs At, bool bValid = true, bool bAvail = true)
	{
		RecoveryCandidate C;
		C.Source = Src;
		C.Generation = Gen;
		C.BaseRevision = Base;
		C.ObjectSha256 = Sha256::HexOf(Content);
		C.Size = 100;
		C.SavedAt = At;
		C.bValidated = bValid;
		C.bObjectAvailable = bAvail;
		return C;
	};
	// Newest timestamp alone never wins: wrong generation / base / unvalidated are rejected.
	auto R = SelectRecoveryCandidate({Cand("a", 4, 10, "old gen", 9000), Cand("b", 5, 9, "old base", 9000), Cand("c", 5, 10, "unvalidated", 9000, false),
		Cand("d", 5, 10, "missing", 9000, true, false), Cand("e", 5, 10, "good", 1000)}, 5, S);
	ASSERT_TRUE(R.has_value());
	EXPECT_EQ(R->Source, std::string("e"));
	// Same content as head is not "newer progress".
	EXPECT_TRUE(!SelectRecoveryCandidate({Cand("h", 5, 10, "head", 5000)}, 5, S).has_value());
	// Among valid candidates, deterministic: newest, then size, then hash.
	auto R2 = SelectRecoveryCandidate({Cand("x", 5, 10, "one", 2000), Cand("y", 5, 10, "two", 3000)}, 5, S);
	EXPECT_EQ(R2->Source, std::string("y"));
	EXPECT_TRUE(!SelectRecoveryCandidate({Cand("x", 5, 10, "one", 2000)}, 0, S).has_value());
	// Takeover rank is deterministic and honours preferred hosts.
	std::vector<SessionPlayer> Ps = {{"C", "player-3"}, {"A", "player-1"}, {"B", "player-2"}};
	EXPECT_EQ(TakeoverRank(Ps, {}, "player-2"), 1);
	EXPECT_EQ(TakeoverRank(Ps, {"player-3"}, "player-3"), 0);
	EXPECT_EQ(TakeoverRank(Ps, {}, "stranger"), 3);
}
