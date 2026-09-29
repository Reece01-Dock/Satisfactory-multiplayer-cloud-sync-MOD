// GitHubAuthService + device-flow coverage for the in-mod Link GitHub UX.
#include <algorithm>
#include <deque>
#include <string>

#include "SharedWorldCore/Providers/GitHubAuth.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
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

	const char* StartReply = R"({"device_code":"dc-secret","user_code":"ABCD-EFGH","verification_uri":"https://github.com/login/device","expires_in":900,"interval":5})";
	const char* UserReply = R"({"login":"Reece01-Dock","id":42,"name":"Reece","avatar_url":"https://avatars.githubusercontent.com/u/42"})";

	std::shared_ptr<GitHubAuthService> MakeService(std::shared_ptr<ScriptedHttp> Http, std::shared_ptr<MemoryCredentialStore> Store, FakeClock& Clock, const char* ClientId = "Iv1.client")
	{
		return std::make_shared<GitHubAuthService>(Http, Store, Clock, ClientId, "test/github", Logger{});
	}
}

SW_TEST(GitHubAuth_ResolveClientId)
{
	EXPECT_EQ(ResolveGitHubOAuthClientId("", ""), std::string());
	EXPECT_EQ(ResolveGitHubOAuthClientId("embedded", ""), std::string("embedded"));
	EXPECT_EQ(ResolveGitHubOAuthClientId("embedded", "env"), std::string("env"));
	EXPECT_EQ(ResolveGitHubOAuthClientId("", "env"), std::string("env"));
}

SW_TEST(GitHubAuth_MissingClientId)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock, "");
	EXPECT_TRUE(!Auth->IsConfigured());
	EXPECT_ERR(Auth->BeginLink(), ErrorCode::Unsupported);
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Error);
	EXPECT_TRUE(Auth->Snapshot().Error == GitHubAuthErrorKind::ClientIdMissing);
	EXPECT_EQ(Auth->Snapshot().PlayerMessage, std::string("GitHub integration is not configured in this build."));
	EXPECT_TRUE(Http->Seen.empty());
}

SW_TEST(GitHubAuth_DeviceStartUserCodeAndPendingSlowDownSuccess)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock);
	Http->Replies = {
		{200, StartReply},
		{200, R"({"error":"authorization_pending"})"},
		{200, R"({"error":"slow_down","interval":10})"},
		{200, R"({"access_token":"gho_SECRET","token_type":"bearer","scope":"repo"})"},
		{200, UserReply},
	};
	ASSERT_OK(Auth->BeginLink());
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::WaitingForUser);
	EXPECT_EQ(Auth->Snapshot().UserCode, std::string("ABCD-EFGH"));
	EXPECT_EQ(Auth->Snapshot().VerificationUri, std::string("https://github.com/login/device"));

	auto P1 = Auth->PollOnce();
	ASSERT_OK(P1);
	EXPECT_TRUE(P1->State == DevicePoll::Pending);
	auto P2 = Auth->PollOnce();
	ASSERT_OK(P2);
	EXPECT_TRUE(P2->State == DevicePoll::SlowDown);
	EXPECT_EQ(P2->IntervalSeconds, 10);
	auto P3 = Auth->PollOnce();
	ASSERT_OK(P3);
	ASSERT_TRUE(P3->State == DevicePoll::Authorized);
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Authorizing);

	std::string Token = std::move(P3->AccessToken);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Token)));
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth->Snapshot().User.Login, std::string("Reece01-Dock"));
	EXPECT_EQ(Auth->Snapshot().User.Id, std::string("42"));
	EXPECT_EQ(Auth->Snapshot().User.AvatarUrl, std::string("https://avatars.githubusercontent.com/u/42"));
	EXPECT_TRUE(Auth->Snapshot().bHasToken);
	EXPECT_EQ(Store->Read("test/github").Value(), std::string("gho_SECRET"));
}

SW_TEST(GitHubAuth_ExpiredDeniedNetworkMalformed)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock);

	Http->Replies = {{200, StartReply}, {200, R"({"error":"expired_token"})"}};
	ASSERT_OK(Auth->BeginLink());
	EXPECT_TRUE(Auth->PollOnce()->State == DevicePoll::Expired);
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Expired);

	Auth = MakeService(Http, Store, Clock);
	Http->Replies = {{200, StartReply}, {200, R"({"error":"access_denied"})"}};
	ASSERT_OK(Auth->BeginLink());
	EXPECT_TRUE(Auth->PollOnce()->State == DevicePoll::Denied);
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Denied);

	Auth = MakeService(Http, Store, Clock);
	Http->Replies = {{0, ""}};
	EXPECT_ERR(Auth->BeginLink(), ErrorCode::Network);
	EXPECT_TRUE(Auth->Snapshot().Error == GitHubAuthErrorKind::NoInternet);

	Auth = MakeService(Http, Store, Clock);
	Http->Replies = {{200, "not-json"}};
	EXPECT_ERR(Auth->BeginLink(), ErrorCode::Invalid);
	EXPECT_TRUE(Auth->Snapshot().Error == GitHubAuthErrorKind::MalformedResponse);
}

SW_TEST(GitHubAuth_PersistRestoreInvalidDisconnect)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock);
	Http->Replies = {
		{200, StartReply},
		{200, R"({"access_token":"gho_KEEP","token_type":"bearer","scope":"repo"})"},
		{200, UserReply},
		{200, UserReply}, // restore
		{401, "{}"},      // invalid stored token
	};
	ASSERT_OK(Auth->BeginLink());
	auto Poll = Auth->PollOnce();
	ASSERT_OK(Poll);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->AccessToken)));
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);

	auto Auth2 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth2->RestoreSession());
	EXPECT_TRUE(Auth2->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth2->Snapshot().User.Login, std::string("Reece01-Dock"));

	auto Auth3 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth3->RestoreSession()); // 401 clears credential
	EXPECT_TRUE(Auth3->Snapshot().State == GitHubAuthState::Disconnected);
	EXPECT_ERR(Store->Read("test/github"), ErrorCode::NotFound);

	ASSERT_OK(Store->Write("test/github", "gho_AGAIN"));
	Http->Replies = {{200, UserReply}};
	auto Auth4 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth4->RestoreSession());
	ASSERT_OK(Auth4->Disconnect());
	EXPECT_TRUE(Auth4->Snapshot().State == GitHubAuthState::Disconnected);
	EXPECT_TRUE(!Auth4->Snapshot().bHasToken);
	EXPECT_ERR(Store->Read("test/github"), ErrorCode::NotFound);
}

SW_TEST(GitHubAuth_CancelStopsLinkAndTokenNeverInLogs)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Sink = std::make_shared<MemoryLogSink>();
	auto Auth = std::make_shared<GitHubAuthService>(Http, Store, Clock, "Iv1.client", "test/github", Logger{Sink});
	Http->Replies = {
		{200, StartReply},
		{200, R"({"access_token":"gho_SECRET_SHOULD_NOT_LOG","token_type":"bearer","scope":"repo"})"},
		{200, UserReply},
	};
	ASSERT_OK(Auth->BeginLink());
	Auth->CancelLink();
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Disconnected);
	EXPECT_ERR(Auth->PollOnce(), ErrorCode::Invalid);

	Http->Replies = {
		{200, StartReply},
		{200, R"({"access_token":"gho_SECRET_SHOULD_NOT_LOG","token_type":"bearer","scope":"repo"})"},
		{200, UserReply},
	};
	Auth = std::make_shared<GitHubAuthService>(Http, Store, Clock, "Iv1.client", "test/github", Logger{Sink});
	ASSERT_OK(Auth->BeginLink());
	auto Poll = Auth->PollOnce();
	ASSERT_OK(Poll);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->AccessToken)));
	EXPECT_TRUE(!Sink->Contains("gho_"));
	EXPECT_TRUE(Sink->Contains("github_auth_succeeded"));
	EXPECT_TRUE(Sink->Contains("user=Reece01-Dock"));
}

SW_TEST(GitHubAuth_TestAccessAndPlayerMessages)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	ASSERT_OK(Store->Write("test/github", "gho_T"));
	Http->Replies = {{200, UserReply}};
	auto Auth = MakeService(Http, Store, Clock);
	auto U = Auth->TestAccess();
	ASSERT_OK(U);
	EXPECT_EQ(U->Login, std::string("Reece01-Dock"));
	EXPECT_EQ(GitHubAuthPlayerMessage(GitHubAuthState::WaitingForUser, GitHubAuthErrorKind::None), std::string("Waiting for GitHub approval..."));
	EXPECT_EQ(GitHubAuthPlayerMessage(GitHubAuthState::Expired, GitHubAuthErrorKind::DeviceCodeExpired), std::string("The code expired. Try again."));
	EXPECT_EQ(GitHubAuthPlayerMessage(GitHubAuthState::Denied, GitHubAuthErrorKind::AuthorizationDenied), std::string("GitHub access was denied."));
}

SW_TEST(GitHubAuth_FetchUserProfile)
{
	auto Http = std::make_shared<ScriptedHttp>();
	FakeClock Clock(StartTime);
	GitHubDeviceFlow Flow(Http, "Iv1.client", Clock);
	Http->Replies = {{200, UserReply}};
	auto U = Flow.FetchUser("gho_x");
	ASSERT_OK(U);
	EXPECT_EQ(U->Login, std::string("Reece01-Dock"));
	EXPECT_EQ(U->Name, std::string("Reece"));
	EXPECT_TRUE(Http->Seen[0].Headers.end() != std::find_if(Http->Seen[0].Headers.begin(), Http->Seen[0].Headers.end(),
		[](const auto& H) { return H.first == "Authorization"; }));
}
