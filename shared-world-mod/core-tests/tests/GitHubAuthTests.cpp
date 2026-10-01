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
	const char* TokenWithRefresh =
		R"({"access_token":"gho_ACCESS1","token_type":"bearer","scope":"repo","expires_in":28800,"refresh_token":"ghr_REFRESH1","refresh_token_expires_in":15897600})";
	const char* RefreshRotated =
		R"({"access_token":"gho_ACCESS2","token_type":"bearer","scope":"repo","expires_in":28800,"refresh_token":"ghr_REFRESH2","refresh_token_expires_in":15897600})";
	const char* RefreshRotatedAgain =
		R"({"access_token":"gho_ACCESS3","token_type":"bearer","scope":"repo","expires_in":28800,"refresh_token":"ghr_REFRESH3","refresh_token_expires_in":15897600})";

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

	GitHubOAuthTokens Tokens = std::move(P3->Tokens);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Tokens)));
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth->Snapshot().User.Login, std::string("Reece01-Dock"));
	EXPECT_EQ(Auth->Snapshot().User.Id, std::string("42"));
	EXPECT_EQ(Auth->Snapshot().User.AvatarUrl, std::string("https://avatars.githubusercontent.com/u/42"));
	EXPECT_TRUE(Auth->Snapshot().bHasToken);
	auto Stored = ParseGitHubCredentialBlob(Store->Read("test/github").Value());
	ASSERT_OK(Stored);
	EXPECT_EQ(Stored->AccessToken, std::string("gho_SECRET"));
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
		{401, "{}"},      // invalid stored token, no refresh → clear
	};
	ASSERT_OK(Auth->BeginLink());
	auto Poll = Auth->PollOnce();
	ASSERT_OK(Poll);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->Tokens)));
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);

	auto Auth2 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth2->RestoreSession());
	EXPECT_TRUE(Auth2->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth2->Snapshot().User.Login, std::string("Reece01-Dock"));

	auto Auth3 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth3->RestoreSession()); // 401 clears credential when refresh impossible
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
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->Tokens)));
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
	EXPECT_EQ(GitHubAuthPlayerMessage(GitHubAuthState::Disconnected, GitHubAuthErrorKind::TokenRevoked),
		std::string("GitHub authorization was revoked. Reconnect GitHub to continue."));
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

// authenticate → save → restart auth subsystem → load → still authenticated
SW_TEST(GitHubAuth_PersistenceAcrossRestart)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock);
	Http->Replies = {{200, StartReply}, {200, TokenWithRefresh}, {200, UserReply}, {200, UserReply}};
	ASSERT_OK(Auth->BeginLink());
	auto Poll = Auth->PollOnce();
	ASSERT_OK(Poll);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->Tokens)));

	auto Auth2 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth2->RestoreSession());
	EXPECT_TRUE(Auth2->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth2->Snapshot().User.Login, std::string("Reece01-Dock"));
	EXPECT_TRUE(Auth2->Snapshot().Diagnostics.bRefreshTokenPresent);
	EXPECT_TRUE(Auth2->Snapshot().Diagnostics.bRefreshPossible);
	EXPECT_TRUE(Auth2->Snapshot().Diagnostics.ExpiresInSeconds > 0);
}

// expired access + valid refresh → automatic refresh → no login prompt
SW_TEST(GitHubAuth_AccessTokenExpiryAutoRefresh)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	auto Auth = MakeService(Http, Store, Clock);
	Http->Replies = {{200, StartReply}, {200, TokenWithRefresh}, {200, UserReply}};
	ASSERT_OK(Auth->BeginLink());
	auto Poll = Auth->PollOnce();
	ASSERT_OK(Poll);
	ASSERT_OK(Auth->FinalizeAuthorized(std::move(Poll->Tokens)));

	Clock.Advance(Seconds(28800)); // access token expired
	Http->Replies = {{200, RefreshRotated}, {200, UserReply}};
	auto Auth2 = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth2->RestoreSession());
	EXPECT_TRUE(Auth2->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth2->Snapshot().User.Login, std::string("Reece01-Dock"));
	auto Stored = ParseGitHubCredentialBlob(Store->Read("test/github").Value());
	ASSERT_OK(Stored);
	EXPECT_EQ(Stored->AccessToken, std::string("gho_ACCESS2"));
	EXPECT_EQ(Stored->RefreshToken, std::string("ghr_REFRESH2"));
	EXPECT_TRUE(Http->Seen.size() >= 2);
	EXPECT_TRUE(Http->Seen[Http->Seen.size() - 2].Body.find("grant_type=refresh_token") != std::string::npos);
}

// refresh rotates refresh token; second refresh uses the NEW one
SW_TEST(GitHubAuth_RefreshTokenRotationPersisted)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	GitHubOAuthTokens Initial;
	Initial.AccessToken = "gho_OLD";
	Initial.RefreshToken = "ghr_REFRESH1";
	Initial.AccessExpiresAt = StartTime + Seconds(60);
	Initial.RefreshExpiresAt = StartTime + Seconds(15897600);
	ASSERT_OK(Store->Write("test/github", SerializeGitHubCredentialBlob(Initial)));

	Http->Replies = {{200, RefreshRotated}};
	auto T1 = ResolveGitHubAccessToken(Http, Store, Clock, "Iv1.client", "test/github", true);
	ASSERT_OK(T1);
	EXPECT_EQ(*T1, std::string("gho_ACCESS2"));
	auto After1 = ParseGitHubCredentialBlob(Store->Read("test/github").Value());
	ASSERT_OK(After1);
	EXPECT_EQ(After1->RefreshToken, std::string("ghr_REFRESH2"));

	Http->Replies = {{200, RefreshRotatedAgain}};
	auto T2 = ResolveGitHubAccessToken(Http, Store, Clock, "Iv1.client", "test/github", true);
	ASSERT_OK(T2);
	EXPECT_EQ(*T2, std::string("gho_ACCESS3"));
	EXPECT_TRUE(Http->Seen.back().Body.find("ghr_REFRESH2") != std::string::npos);
	auto After2 = ParseGitHubCredentialBlob(Store->Read("test/github").Value());
	ASSERT_OK(After2);
	EXPECT_EQ(After2->RefreshToken, std::string("ghr_REFRESH3"));
}

// expired access + invalid refresh → ask user to authenticate
SW_TEST(GitHubAuth_FailedRefreshPromptsRelink)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	GitHubOAuthTokens Initial;
	Initial.AccessToken = "gho_OLD";
	Initial.RefreshToken = "ghr_DEAD";
	Initial.AccessExpiresAt = StartTime; // already expired
	Initial.RefreshExpiresAt = StartTime + Seconds(15897600);
	ASSERT_OK(Store->Write("test/github", SerializeGitHubCredentialBlob(Initial)));

	Http->Replies = {{200, R"({"error":"bad_refresh_token","error_description":"The refresh token is invalid."})"}};
	auto Auth = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth->RestoreSession());
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Disconnected);
	EXPECT_TRUE(Auth->Snapshot().Error == GitHubAuthErrorKind::TokenRevoked
		|| Auth->Snapshot().Error == GitHubAuthErrorKind::TokenInvalid);
	EXPECT_ERR(Store->Read("test/github"), ErrorCode::NotFound);
	EXPECT_EQ(GitHubAuthPlayerMessage(Auth->Snapshot().State, Auth->Snapshot().Error),
		std::string("GitHub authorization was revoked. Reconnect GitHub to continue."));
}

// 403 permissions/rate limit must NOT delete credentials
SW_TEST(GitHubAuth_403DoesNotClearCredentials)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	ASSERT_OK(Store->Write("test/github", SerializeGitHubCredentialBlob(GitHubOAuthTokens{"gho_OK", "ghr_OK", StartTime + Seconds(3600), StartTime + Seconds(999999)})));

	Http->Replies = {{403, R"({"message":"You need at least the `repo` scope"})"}};
	auto Auth = MakeService(Http, Store, Clock);
	EXPECT_ERR(Auth->RestoreSession(), ErrorCode::BadState);
	EXPECT_TRUE(Auth->Snapshot().bHasToken);
	EXPECT_TRUE(Store->Read("test/github").Ok());

	Http->Replies = {{403, R"({"message":"API rate limit exceeded"})"}};
	Http->Seen.clear();
	// Attach rate-limit signal via a custom reply: FetchUser checks body + would need headers.
	// Use 429 which is unambiguously RateLimited.
	Http->Replies = {{429, R"({"message":"API rate limit exceeded"})"}};
	auto Auth2 = MakeService(Http, Store, Clock);
	EXPECT_ERR(Auth2->RestoreSession(), ErrorCode::RateLimited);
	EXPECT_TRUE(Auth2->Snapshot().bHasToken);
	EXPECT_TRUE(Store->Read("test/github").Ok());
}

// temporary network outage must NOT unlink GitHub
SW_TEST(GitHubAuth_NetworkFailureKeepsCredentials)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	ASSERT_OK(Store->Write("test/github", "gho_STILL_HERE"));
	Http->Replies = {{0, ""}};
	auto Auth = MakeService(Http, Store, Clock);
	EXPECT_ERR(Auth->RestoreSession(), ErrorCode::Network);
	EXPECT_TRUE(Auth->Snapshot().bHasToken);
	EXPECT_EQ(Store->Read("test/github").Value(), std::string("gho_STILL_HERE"));

	Http->Replies = {{200, UserReply}};
	ASSERT_OK(Auth->RestoreSession());
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);
}

SW_TEST(GitHubAuth_LegacyBareTokenStillLoads)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	ASSERT_OK(Store->Write("test/github", "gho_LEGACY"));
	Http->Replies = {{200, UserReply}};
	auto Auth = MakeService(Http, Store, Clock);
	ASSERT_OK(Auth->RestoreSession());
	EXPECT_TRUE(Auth->Snapshot().State == GitHubAuthState::Connected);
	EXPECT_EQ(Auth->EnsureValidAccessToken().Value(), std::string("gho_LEGACY"));
}

SW_TEST(GitHubAuth_ProactiveRefreshBeforeExpiry)
{
	auto Http = std::make_shared<ScriptedHttp>();
	auto Store = std::make_shared<MemoryCredentialStore>();
	FakeClock Clock(StartTime);
	GitHubOAuthTokens Initial;
	Initial.AccessToken = "gho_SOON";
	Initial.RefreshToken = "ghr_REFRESH1";
	Initial.AccessExpiresAt = StartTime + Minutes(4); // inside 5-minute skew
	Initial.RefreshExpiresAt = StartTime + Seconds(15897600);
	ASSERT_OK(Store->Write("test/github", SerializeGitHubCredentialBlob(Initial)));
	Http->Replies = {{200, RefreshRotated}};
	auto Tok = ResolveGitHubAccessToken(Http, Store, Clock, "Iv1.client", "test/github", false);
	ASSERT_OK(Tok);
	EXPECT_EQ(*Tok, std::string("gho_ACCESS2"));
}
