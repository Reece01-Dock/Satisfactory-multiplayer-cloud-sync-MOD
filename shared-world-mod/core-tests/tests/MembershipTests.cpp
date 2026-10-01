// Player list / settings edits, the open flag and GitHub device-flow sign-in.
#include <atomic>
#include <deque>
#include <thread>

#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/World/Membership.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	struct Fixture
	{
		std::shared_ptr<FakeClock> Clock = std::make_shared<FakeClock>(StartTime);
		std::shared_ptr<MemoryRepository> Repo = std::make_shared<MemoryRepository>();
		WorldStore Store{Repo, TestWorldId, Clock, Logger(), 200};

		explicit Fixture(bool bWithOwner = true)
		{
			NewWorld W = TestWorld();
			if (bWithOwner)
			{
				W.Players.bOpen = false;
				W.Players.Members.push_back(Member{Player(1).PlayerId, "P1", Role::Owner});
			}
			auto R = Store.Create(W);
			if (!R) swtest::ReportFailure(__FILE__, __LINE__, "create failed: " + R.Err().Describe());
		}
		PlayerList Players()
		{
			auto S = Store.Load();
			return Store.LoadPlayers(S->CommitId).Value();
		}
	};

	Member As(int N, Role R) { return Member{Player(N).PlayerId, Player(N).DisplayName, R}; }
}

SW_TEST(Membership_OpenFlagRoundTripAndLegacyDefault)
{
	PlayerList L;
	L.bOpen = false;
	L.Members.push_back(As(1, Role::Owner));
	auto Back = PlayerList::FromJson(L.ToJson());
	ASSERT_OK(Back);
	EXPECT_TRUE(!Back->bOpen);
	// Files without "open": a member list meant restricted, an empty one open.
	auto Legacy = PlayerList::FromJson(json::Parse(R"({"members":[{"playerId":"a","displayName":"A","role":"owner"}]})").Value());
	ASSERT_OK(Legacy);
	EXPECT_TRUE(!Legacy->bOpen);
	auto Empty = PlayerList::FromJson(json::Parse(R"({"members":[]})").Value());
	ASSERT_OK(Empty);
	EXPECT_TRUE(Empty->bOpen);
	EXPECT_ERR(PlayerList::FromJson(json::Parse(R"({"open":"yes","members":[]})").Value()), ErrorCode::Invalid);
}

SW_TEST(Membership_RolesGateEdits)
{
	Fixture F;
	ASSERT_OK(AddMember(F.Store, Player(1), As(2, Role::Admin)));
	ASSERT_OK(AddMember(F.Store, Player(2), As(3, Role::Member)));   // admin may invite members
	EXPECT_ERR(AddMember(F.Store, Player(2), As(4, Role::Admin)), ErrorCode::Unauthorized);  // but not admins
	EXPECT_ERR(AddMember(F.Store, Player(3), As(4, Role::Member)), ErrorCode::Unauthorized); // members cannot invite
	EXPECT_ERR(AddMember(F.Store, Player(9), As(4, Role::Member)), ErrorCode::Unauthorized); // strangers cannot
	EXPECT_ERR(SetMemberRole(F.Store, Player(2), Player(1).PlayerId, Role::Member), ErrorCode::Unauthorized);
	EXPECT_ERR(RemoveMember(F.Store, Player(2), Player(1).PlayerId), ErrorCode::Unauthorized);
	ASSERT_OK(SetMemberRole(F.Store, Player(2), Player(3).PlayerId, Role::Viewer));
	EXPECT_ERR(SetMemberRole(F.Store, Player(1), Player(8).PlayerId, Role::Member), ErrorCode::NotFound);

	// The last owner can be neither demoted nor removed, even by themselves.
	EXPECT_ERR(SetMemberRole(F.Store, Player(1), Player(1).PlayerId, Role::Admin), ErrorCode::Invalid);
	EXPECT_ERR(RemoveMember(F.Store, Player(1), Player(1).PlayerId), ErrorCode::Invalid);
	ASSERT_OK(SetMemberRole(F.Store, Player(1), Player(2).PlayerId, Role::Owner));
	ASSERT_OK(RemoveMember(F.Store, Player(1), Player(1).PlayerId)); // now allowed: P2 owns it

	// Anyone may leave.
	ASSERT_OK(RemoveMember(F.Store, Player(3), Player(3).PlayerId));
	PlayerList L = F.Players();
	ASSERT_EQ(L.Members.size(), size_t(1));
	EXPECT_EQ(L.Members[0].PlayerId, Player(2).PlayerId);
	EXPECT_TRUE(L.Members[0].MemberRole == Role::Owner);
}

SW_TEST(Membership_EditsNeverTouchTheLeaseState)
{
	Fixture F;
	LeaseManager Leases(std::shared_ptr<WorldStore>(&F.Store, [](WorldStore*) {}), LeaseConfig{});
	auto A = Leases.Acquire(Player(1), "nonce-1");
	ASSERT_OK(A);
	ASSERT_TRUE(A->Outcome == AcquireOutcome::Acquired);
	const std::string StateBefore = F.Repo->ReadFile(F.Repo->Head().Value(), Paths::State).Value();
	ASSERT_OK(SetOpenMembership(F.Store, Player(1), true));
	ASSERT_OK(AddMember(F.Store, Player(1), As(2, Role::Member)));
	EXPECT_EQ(F.Repo->ReadFile(F.Repo->Head().Value(), Paths::State).Value(), StateBefore);
	// The host's fenced heartbeat still works after the unrelated commits.
	ASSERT_OK(Leases.Renew(*A->Token, LeaseUpdate{}));
	EXPECT_TRUE(F.Players().bOpen);
	EXPECT_ERR(F.Store.UpdateDocument(Paths::State, [](const std::string&) -> Result<std::string> { return std::string("{}"); }, "x"), ErrorCode::Invalid);
	EXPECT_ERR(F.Store.UpdateDocument("../escape.json", [](const std::string&) -> Result<std::string> { return std::string("{}"); }, "x"), ErrorCode::Invalid);
}

SW_TEST(Membership_ConcurrentEditsAllLand)
{
	Fixture F;
	F.Repo->BeforeCommit = []() -> Status
	{
		std::this_thread::sleep_for(std::chrono::microseconds(300));
		return {};
	};
	std::vector<std::thread> Threads;
	std::atomic<int> Failures{0};
	for (int N = 2; N <= 9; ++N)
	{
		Threads.emplace_back([&F, &Failures, N]()
		{
			WorldStore Mine(F.Repo, TestWorldId, F.Clock, Logger(), 200); // each PC has its own store
			if (!AddMember(Mine, Player(1), As(N, Role::Member))) ++Failures;
		});
	}
	for (auto& T : Threads) T.join();
	EXPECT_EQ(Failures.load(), 0);
	EXPECT_EQ(F.Players().Members.size(), size_t(9)); // no lost update
}

SW_TEST(Membership_OwnerlessWorldIsClaimedByFirstManager)
{
	Fixture F(/*bWithOwner*/ false);
	EXPECT_TRUE(F.Players().bOpen);
	ASSERT_OK(SetOpenMembership(F.Store, Player(4), false));
	PlayerList L = F.Players();
	EXPECT_TRUE(!L.bOpen);
	ASSERT_EQ(L.Members.size(), size_t(1));
	EXPECT_EQ(L.Members[0].PlayerId, Player(4).PlayerId);
	EXPECT_TRUE(L.Members[0].MemberRole == Role::Owner);
	EXPECT_ERR(SetOpenMembership(F.Store, Player(5), true), ErrorCode::Unauthorized);
}

SW_TEST(Membership_SettingsRequireRole)
{
	Fixture F;
	ASSERT_OK(AddMember(F.Store, Player(1), As(2, Role::Member)));
	WorldSettings S;
	S.Name = "Renamed";
	S.MaxPlayers = 8;
	EXPECT_ERR(UpdateSettings(F.Store, Player(2), S), ErrorCode::Unauthorized);
	ASSERT_OK(UpdateSettings(F.Store, Player(1), S));
	auto Snap = F.Store.Load();
	auto Stored = F.Store.LoadSettings(Snap->CommitId);
	ASSERT_OK(Stored);
	EXPECT_EQ(Stored->Name, std::string("Renamed"));
	EXPECT_EQ(Stored->MaxPlayers, int64_t(8));
	S.MaxPlayers = 0;
	EXPECT_ERR(UpdateSettings(F.Store, Player(1), S), ErrorCode::Invalid);
}

// ------------------------------------------------------------ device flow

namespace
{
	/** Scripted github.com: answers the device-flow endpoints in order. */
	class ScriptedHttp final : public IHttpClient
	{
	public:
		std::deque<std::pair<int, std::string>> Replies;
		std::vector<HttpRequest> Seen;

		Result<HttpResponse> Send(const HttpRequest& Req) override
		{
			Seen.push_back(Req);
			if (Replies.empty()) return MakeError(ErrorCode::Network, "no scripted reply");
			HttpResponse R;
			R.Status = Replies.front().first;
			R.Body = Replies.front().second;
			Replies.pop_front();
			return R;
		}
	};

	const char* StartReply = R"({"device_code":"dc-secret","user_code":"WDJB-MJHT","verification_uri":"https://github.com/login/device","expires_in":900,"interval":5})";
}

SW_TEST(DeviceFlow_HappyPathWithSlowDown)
{
	auto Http = std::make_shared<ScriptedHttp>();
	FakeClock Clock(StartTime);
	GitHubDeviceFlow Flow(Http, "Iv1.client", Clock);
	Http->Replies = {
		{200, StartReply},
		{200, R"({"error":"authorization_pending"})"},
		{200, R"({"error":"slow_down","interval":10})"},
		{200, R"({"access_token":"gho_SECRET","token_type":"bearer","scope":"repo"})"},
		{200, R"({"login":"octo-player","id":1})"},
	};
	auto Code = Flow.Start();
	ASSERT_OK(Code);
	EXPECT_EQ(Code->UserCode, std::string("WDJB-MJHT"));
	EXPECT_EQ(Code->ExpiresAt, StartTime + Seconds(900));
	EXPECT_EQ(Http->Seen[0].Url, std::string("https://github.com/login/device/code"));
	EXPECT_TRUE(Http->Seen[0].Body.find("client_id=Iv1.client") != std::string::npos);
	EXPECT_TRUE(Http->Seen[0].Body.find("secret") == std::string::npos); // public client: no secret

	auto P1 = Flow.Poll(*Code);
	ASSERT_OK(P1);
	EXPECT_TRUE(P1->State == DevicePoll::Pending);
	auto P2 = Flow.Poll(*Code);
	ASSERT_OK(P2);
	EXPECT_TRUE(P2->State == DevicePoll::SlowDown);
	EXPECT_EQ(P2->IntervalSeconds, 10);
	auto P3 = Flow.Poll(*Code);
	ASSERT_OK(P3);
	ASSERT_TRUE(P3->State == DevicePoll::Authorized);
	EXPECT_EQ(P3->AccessToken(), std::string("gho_SECRET"));
	EXPECT_TRUE(Http->Seen[1].Body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code") != std::string::npos);

	auto Login = Flow.FetchLogin(P3->AccessToken());
	ASSERT_OK(Login);
	EXPECT_EQ(*Login, std::string("octo-player"));
	EXPECT_EQ(Http->Seen[4].Url, std::string("https://api.github.com/user"));

	MemoryCredentialStore Creds;
	EXPECT_ERR(Creds.Read("github"), ErrorCode::NotFound);
	ASSERT_OK(Creds.Write("github", P3->AccessToken()));
	EXPECT_EQ(Creds.Read("github").Value(), std::string("gho_SECRET"));
	ASSERT_OK(Creds.Remove("github"));
	EXPECT_ERR(Creds.Read("github"), ErrorCode::NotFound);
}

SW_TEST(DeviceFlow_DeniedExpiredAndHostileReplies)
{
	auto Http = std::make_shared<ScriptedHttp>();
	FakeClock Clock(StartTime);
	GitHubDeviceFlow Flow(Http, "Iv1.client", Clock);
	Http->Replies = {{200, StartReply}, {200, R"({"error":"access_denied"})"}, {200, R"({"error":"expired_token"})"},
		{200, R"({"error":"incorrect_client_credentials"})"}};
	auto Code = Flow.Start();
	ASSERT_OK(Code);
	EXPECT_TRUE(Flow.Poll(*Code)->State == DevicePoll::Denied);
	EXPECT_TRUE(Flow.Poll(*Code)->State == DevicePoll::Expired);
	EXPECT_ERR(Flow.Poll(*Code), ErrorCode::Unauthorized);

	// Past expiry the mod stops polling without a request.
	const size_t Before = Http->Seen.size();
	Clock.Advance(Seconds(901));
	EXPECT_TRUE(Flow.Poll(*Code)->State == DevicePoll::Expired);
	EXPECT_EQ(Http->Seen.size(), Before);

	// Unconfigured build, non-https verification URL, garbage, server errors.
	GitHubDeviceFlow Unconfigured(Http, "", Clock);
	EXPECT_ERR(Unconfigured.Start(), ErrorCode::Unsupported);
	Http->Replies = {{200, R"({"device_code":"d","user_code":"U","verification_uri":"http://evil.example/device","expires_in":900,"interval":5})"},
		{200, "not json"}, {500, "{}"}, {401, "{}"}};
	EXPECT_ERR(Flow.Start(), ErrorCode::Invalid);
	EXPECT_TRUE(!Flow.Start().Ok());
	EXPECT_ERR(Flow.Start(), ErrorCode::Network);
	EXPECT_ERR(Flow.FetchLogin("bad"), ErrorCode::Unauthorized);
}
