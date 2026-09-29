#include "SharedWorldCore/HostHandshake/HostHandshake.h"
#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/World/Creation.h"
#include "SharedWorldCore/World/WorldSession.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	Identity HostId() { return Player(1); }
	Identity ClientId() { return Player(2); }

	Lease SampleLease()
	{
		Lease L;
		L.Generation = 7;
		L.Holder = HostId();
		L.Nonce = "nonce-abc";
		L.AcquiredAt = StartTime;
		L.RenewedAt = StartTime;
		L.ExpiresAt = StartTime + Seconds(90);
		L.BaseRevision = 3;
		L.Phase = LeasePhase::Hosting;
		L.bHostReady = true;
		L.Join = JoinInfo{"online-session-id", "EOS:session-xyz", "EOS"};
		return L;
	}

	struct Fixture
	{
		std::shared_ptr<FakeClock> Clock = std::make_shared<FakeClock>(StartTime);
		std::shared_ptr<MemoryRepository> Repo = std::make_shared<MemoryRepository>();
		std::shared_ptr<MemoryObjectStore> Objects = std::make_shared<MemoryObjectStore>();
		std::shared_ptr<LeaseManager> Leases;
		std::shared_ptr<SyncEngine> Sync;

		Fixture()
		{
			Leases = MakeLeases(Repo, Clock);
			Sync = std::make_shared<SyncEngine>(Objects, Leases, SyncConfig{TempDir(), 5});
			const std::string Src = file::Join(TempDir(), "seed.sav");
			(void)file::CreateExclusive(Src, save::BuildSynthetic("seed", "body"));
			CreateWorldParams P;
			P.Name = "Factory";
			P.Creator = HostId();
			P.SourceSavePath = Src;
			P.OriginalSaveName = "seed";
			P.Versions.GameBuild = "491125";
			P.Versions.ModVersion = "0.2.0";
			P.bRestrictToMembers = false;
			auto R = CreateSharedWorld(*Leases, *Sync, P, 20);
			if (!R) ReportFailure(__FILE__, __LINE__, R.Err().Describe());
		}

		SessionConfig CfgFor(const Identity& Me)
		{
			SessionConfig Cfg;
			Cfg.Me = Me;
			Cfg.SaveDirectory = TempDir();
			Cfg.SaveName = "SharedWorld_our-factory";
			Cfg.Versions.GameBuild = "491125";
			Cfg.Versions.ModVersion = "0.2.0";
			Cfg.Upload.StableQuiet = 20;
			return Cfg;
		}
	};
}

SW_TEST(Handshake_ValidateAck_AcceptsMatchingHost)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	Ack.Timestamp = StartTime;
	ASSERT_OK(ValidateHelloAck(Hello, Ack, L, 3, L.Join));
}

SW_TEST(Handshake_ValidateAck_RejectsNonceMismatch)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "wrong";
	Ack.Timestamp = StartTime;
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Invalid);
}

SW_TEST(Handshake_ValidateAck_RejectsWrongHostIdentity)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = "impostor";
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Invalid);
}

SW_TEST(Handshake_ValidateAck_RejectsStaleSessionId)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:stale";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Invalid);
}

SW_TEST(Session_WaitsUntilHostReadyBeforeJoin)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	EXPECT_EQ(std::string(ToString(Host.View().State)), "READY_TO_HOST");

	WorldSession Client(F.Leases, F.Sync, F.CfgFor(ClientId()));
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "WAITING_FOR_HOST");

	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();
	EXPECT_EQ(std::string(ToString(Host.View().State)), "HOSTING");

	F.Clock->Advance(Seconds(3));
	Client.Tick(F.Clock->Now());
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_READY");
}

SW_TEST(Session_UnreachableHostRetriesWithoutStealingLease)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();

	auto Fake = std::make_shared<FakeHostVerifier>();
	Fake->NextOutcome = HostVerifyOutcome::Unreachable;
	Fake->NextDetail = "timeout";

	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Fake;
	ClientCfg.HostVerifyTimeout = Seconds(30);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_RETRY");
	EXPECT_TRUE(Fake->CallCount >= 1);

	auto Snap = F.Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_TRUE(Snap->State.CurrentLease.has_value());
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, HostId().PlayerId);
	EXPECT_EQ(Snap->State.CurrentLease->Generation, Host.View().Generation);
}

SW_TEST(Handshake_ValidateAck_RejectsProtocolMismatch)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion + 1;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Unsupported);
}

SW_TEST(Handshake_ValidateAck_RejectsRevisionMismatch)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 99;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::StaleRevision);
}

SW_TEST(Handshake_ValidateAck_RejectsGenerationMismatch)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "our-factory";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 51; // Hello expects 7
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Fenced);
}

SW_TEST(Handshake_ValidateAck_RejectsWorldMismatch)
{
	const Lease L = SampleLease();
	SharedWorldHello Hello = MakeHello(ClientId(), "our-factory", L, 3, "probe-nonce", StartTime);
	SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = HostHandshakeProtocolVersion;
	Ack.WorldId = "other-world";
	Ack.HostPlayerId = L.Holder.PlayerId;
	Ack.CurrentRevision = 3;
	Ack.LeaseGeneration = 7;
	Ack.SessionId = "EOS:session-xyz";
	Ack.Nonce = "probe-nonce";
	EXPECT_ERR(ValidateHelloAck(Hello, Ack, L, 3, L.Join), ErrorCode::Invalid);
}

SW_TEST(Session_SessionResolveVerifier_SuccessThenJoin)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();

	auto Resolver = std::make_shared<SessionResolveHostVerifier>();
	Resolver->PresentSessionId = "EOS:live";
	Resolver->HostPlayerId = HostId().PlayerId;

	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Resolver;
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_READY");
	EXPECT_TRUE(Resolver->CallCount >= 1);
}

SW_TEST(Session_SessionResolveVerifier_MissingSessionRetries)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();

	auto Resolver = std::make_shared<SessionResolveHostVerifier>();
	Resolver->PresentSessionId = "EOS:other"; // published id will not resolve
	Resolver->HostPlayerId = HostId().PlayerId;

	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Resolver;
	ClientCfg.HostVerifyTimeout = Seconds(30);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_RETRY");

	auto Snap = F.Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, HostId().PlayerId);
}

SW_TEST(Session_SessionResolveVerifier_StaleGenerationRejected)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();
	const int64_t LiveGen = Host.View().Generation;

	auto Resolver = std::make_shared<SessionResolveHostVerifier>();
	Resolver->PresentSessionId = "EOS:live";
	Resolver->HostPlayerId = HostId().PlayerId;
	Resolver->HostLeaseGeneration = LiveGen - 1; // old host

	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Resolver;
	ClientCfg.HostVerifyTimeout = Seconds(30);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_RETRY");
}

SW_TEST(Session_VerifyFailureDoesNotAcquireLease)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();
	const int64_t Gen = Host.View().Generation;

	auto Fake = std::make_shared<FakeHostVerifier>();
	Fake->NextOutcome = HostVerifyOutcome::Timeout;
	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Fake;
	ClientCfg.HostVerifyTimeout = Seconds(5);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	// Advance past verify window into recovering — still must not hold lease.
	F.Clock->Advance(Seconds(10));
	Client.Tick(F.Clock->Now());
	Client.WaitIdle();
	auto Snap = F.Leases->Store().Load();
	ASSERT_OK(Snap);
	EXPECT_TRUE(Snap->State.CurrentLease.has_value());
	EXPECT_EQ(Snap->State.CurrentLease->Generation, Gen);
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, HostId().PlayerId);
	EXPECT_TRUE(Client.View().State == SessionState::JoinRetry || Client.View().State == SessionState::RecoveringHost);
}

SW_TEST(Session_DroppedProbeThenRetrySucceeds)
{
	Fixture F;
	WorldSession Host(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host.Play();
	Host.WaitIdle();
	Host.OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host.WaitIdle();

	auto Fake = std::make_shared<FakeHostVerifier>();
	Fake->FailCount = 1;
	Fake->FailOutcome = HostVerifyOutcome::Unreachable;
	Fake->NextOutcome = HostVerifyOutcome::Verified;
	Fake->NextDetail = "recovered";

	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Fake;
	ClientCfg.HostVerifyTimeout = Seconds(60);
	ClientCfg.PollInterval = Seconds(1);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_RETRY");

	F.Clock->Advance(Seconds(3));
	Client.Tick(F.Clock->Now());
	Client.WaitIdle();
	EXPECT_EQ(std::string(ToString(Client.View().State)), "JOIN_READY");
	EXPECT_TRUE(Fake->CallCount >= 2);
}

SW_TEST(Session_LeaseExpiryAfterVerifyFailuresAllowsAcquire)
{
	Fixture F;
	auto Host = std::make_unique<WorldSession>(F.Leases, F.Sync, F.CfgFor(HostId()));
	Host->Play();
	Host->WaitIdle();
	Host->OnHostingStarted(JoinInfo{"online-session-id", "EOS:live", "EOS"});
	Host->WaitIdle();
	// Crash: destroy host session without Release (heartbeats stop).
	Host.reset();

	auto Fake = std::make_shared<FakeHostVerifier>();
	Fake->NextOutcome = HostVerifyOutcome::Unreachable;
	SessionConfig ClientCfg = F.CfgFor(ClientId());
	ClientCfg.HostVerifier = Fake;
	ClientCfg.HostVerifyTimeout = Seconds(5);
	WorldSession Client(F.Leases, F.Sync, ClientCfg);
	Client.Play();
	Client.WaitIdle();
	Client.Cancel(); // leave retry/recovery without taking over early

	// Expire the host lease (TTL 90 + skew 30).
	F.Clock->Advance(Seconds(130));
	Client.Play();
	Client.WaitIdle();
	EXPECT_TRUE(Client.View().State == SessionState::ReadyToHost || Client.View().State == SessionState::Downloading ||
		Client.View().State == SessionState::Recovering);
	auto Snap = F.Leases->Store().Load();
	ASSERT_OK(Snap);
	ASSERT_TRUE(Snap->State.CurrentLease.has_value());
	EXPECT_EQ(Snap->State.CurrentLease->Holder.PlayerId, ClientId().PlayerId);
}
