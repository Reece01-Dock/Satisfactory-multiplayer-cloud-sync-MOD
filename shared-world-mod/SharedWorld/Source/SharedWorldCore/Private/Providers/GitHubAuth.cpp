#include "SharedWorldCore/Providers/GitHubAuth.h"

#include "SharedWorldCore/Util/Json.h"

namespace sw
{
	Result<std::string> MemoryCredentialStore::Read(const std::string& Key)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Secrets.find(Key);
		if (It == Secrets.end()) return MakeError(ErrorCode::NotFound, "no stored credential");
		return It->second;
	}

	Status MemoryCredentialStore::Write(const std::string& Key, const std::string& Secret)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Secrets[Key] = Secret;
		return {};
	}

	Status MemoryCredentialStore::Remove(const std::string& Key)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Secrets.erase(Key);
		return {};
	}

	namespace
	{
		std::string FormEncode(const std::string& S)
		{
			static const char* Hex = "0123456789ABCDEF";
			std::string Out;
			for (unsigned char C : S)
			{
				if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '-' || C == '_' || C == '.' || C == '~') Out += static_cast<char>(C);
				else { Out += '%'; Out += Hex[C >> 4]; Out += Hex[C & 15]; }
			}
			return Out;
		}

		HttpRequest FormPost(const std::string& Url, const std::string& Body)
		{
			HttpRequest R;
			R.Method = "POST";
			R.Url = Url;
			R.Body = Body;
			R.Headers = {{"Accept", "application/json"}, {"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", "SatisfactorySharedWorld"}};
			return R;
		}

		Result<GitHubUserInfo> ParseUser(const json::Value& V)
		{
			GitHubUserInfo U;
			SW_ASSIGN(U.Login, json::GetString(V, "login", 64));
			if (auto Id = json::GetOptionalInt(V, "id"); Id.Ok() && Id->has_value())
			{
				U.Id = std::to_string(**Id);
			}
			else if (auto IdS = json::GetOptionalString(V, "id", 32); IdS.Ok() && IdS->has_value())
			{
				U.Id = **IdS;
			}
			if (auto Name = json::GetOptionalString(V, "name", 128); Name.Ok() && Name->has_value()) U.Name = **Name;
			if (auto Av = json::GetOptionalString(V, "avatar_url", 512); Av.Ok() && Av->has_value())
			{
				if (Av->value().compare(0, 8, "https://") == 0) U.AvatarUrl = **Av;
			}
			return U;
		}
	}

	std::string ResolveGitHubOAuthClientId(const char* Embedded, const char* EnvValue)
	{
		if (EnvValue && EnvValue[0] != '\0') return EnvValue;
		if (Embedded && Embedded[0] != '\0') return Embedded;
		return {};
	}

	std::string GitHubAuthPlayerMessage(GitHubAuthState State, GitHubAuthErrorKind Error, const GitHubUserInfo& User)
	{
		switch (State)
		{
		case GitHubAuthState::Disconnected:
			if (Error == GitHubAuthErrorKind::ClientIdMissing) return "GitHub integration is not configured in this build.";
			return "Not connected";
		case GitHubAuthState::Starting: return "Opening GitHub...";
		case GitHubAuthState::WaitingForUser: return "Waiting for GitHub approval...";
		case GitHubAuthState::Authorizing: return "Finishing GitHub connection...";
		case GitHubAuthState::Connected:
			return User.Login.empty() ? "GitHub connected." : ("Connected as " + User.Login);
		case GitHubAuthState::Expired: return "The code expired. Try again.";
		case GitHubAuthState::Denied: return "GitHub access was denied.";
		case GitHubAuthState::Error:
			switch (Error)
			{
			case GitHubAuthErrorKind::ClientIdMissing: return "GitHub integration is not configured in this build.";
			case GitHubAuthErrorKind::NoInternet: return "Could not reach GitHub. Check your internet connection.";
			case GitHubAuthErrorKind::GitHubUnreachable: return "Could not reach GitHub.";
			case GitHubAuthErrorKind::DeviceCodeExpired: return "The code expired. Try again.";
			case GitHubAuthErrorKind::AuthorizationDenied: return "GitHub access was denied.";
			case GitHubAuthErrorKind::TokenInvalid:
			case GitHubAuthErrorKind::TokenRevoked: return "GitHub connection expired. Link GitHub again.";
			case GitHubAuthErrorKind::RateLimited: return "GitHub is rate limiting requests. Try again shortly.";
			case GitHubAuthErrorKind::Cancelled: return "GitHub linking cancelled.";
			case GitHubAuthErrorKind::CredentialStoreFailed: return "Could not store the GitHub credential securely.";
			case GitHubAuthErrorKind::MalformedResponse: return "GitHub returned an unexpected response.";
			default: return "Could not connect to GitHub.";
			}
		}
		return "Could not connect to GitHub.";
	}

	GitHubDeviceFlow::GitHubDeviceFlow(std::shared_ptr<IHttpClient> InHttp, std::string InClientId, const IClock& InClock, std::string InScope, std::string InWebBase, std::string InApiBase)
		: Http(std::move(InHttp)), ClientId(std::move(InClientId)), Clock(InClock), Scope(std::move(InScope)), WebBase(std::move(InWebBase)), ApiBase(std::move(InApiBase))
	{
	}

	Result<DeviceCode> GitHubDeviceFlow::Start()
	{
		if (ClientId.empty()) return MakeError(ErrorCode::Unsupported, "GitHub integration is not configured in this build");
		HttpResponse R;
		SW_ASSIGN(R, Http->Send(FormPost(WebBase + "/login/device/code", "client_id=" + FormEncode(ClientId) + "&scope=" + FormEncode(Scope))));
		if (R.Status == 0) return MakeError(ErrorCode::Network, "no internet");
		if (R.Status == 429) return MakeError(ErrorCode::RateLimited, "GitHub rate limited device authorization");
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub sign-in could not start (" + std::to_string(R.Status) + ")");
		json::Value V;
		auto Parsed = json::Parse(R.Body);
		if (!Parsed) return MakeError(ErrorCode::Invalid, "malformed device authorization response");
		V = *Parsed;
		DeviceCode C;
		SW_ASSIGN(C.Code, json::GetString(V, "device_code", 200));
		SW_ASSIGN(C.UserCode, json::GetString(V, "user_code", 32));
		SW_ASSIGN(C.VerificationUri, json::GetString(V, "verification_uri", 200));
		int64_t Interval, ExpiresIn;
		SW_ASSIGN(Interval, json::GetInt(V, "interval"));
		SW_ASSIGN(ExpiresIn, json::GetInt(V, "expires_in"));
		if (C.VerificationUri.compare(0, 8, "https://") != 0) return MakeError(ErrorCode::Invalid, "unexpected verification URL");
		C.IntervalSeconds = static_cast<int>(Interval < 1 ? 5 : Interval);
		C.ExpiresAt = Clock.Now() + Seconds(ExpiresIn);
		return C;
	}

	Result<DevicePollResult> GitHubDeviceFlow::Poll(const DeviceCode& Code)
	{
		DevicePollResult Out;
		Out.IntervalSeconds = Code.IntervalSeconds;
		if (Clock.Now() > Code.ExpiresAt)
		{
			Out.State = DevicePoll::Expired;
			return Out;
		}
		HttpResponse R;
		SW_ASSIGN(R, Http->Send(FormPost(WebBase + "/login/oauth/access_token",
			"client_id=" + FormEncode(ClientId) + "&device_code=" + FormEncode(Code.Code) + "&grant_type=" + FormEncode("urn:ietf:params:oauth:grant-type:device_code"))));
		if (R.Status == 0) return MakeError(ErrorCode::Network, "no internet");
		if (R.Status == 429) return MakeError(ErrorCode::RateLimited, "GitHub rate limited token poll");
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub sign-in poll failed (" + std::to_string(R.Status) + ")");
		json::Value V;
		auto Parsed = json::Parse(R.Body);
		if (!Parsed) return MakeError(ErrorCode::Invalid, "malformed token response");
		V = *Parsed;
		if (auto Tok = json::GetOptionalString(V, "access_token", 512); Tok.Ok() && Tok->has_value())
		{
			Out.State = DevicePoll::Authorized;
			Out.AccessToken = **Tok;
			return Out;
		}
		std::string Err;
		SW_ASSIGN(Err, json::GetString(V, "error", 64));
		if (Err == "authorization_pending") Out.State = DevicePoll::Pending;
		else if (Err == "slow_down")
		{
			Out.State = DevicePoll::SlowDown;
			if (auto I = json::GetOptionalInt(V, "interval"); I.Ok() && I->has_value()) Out.IntervalSeconds = static_cast<int>(**I);
			else Out.IntervalSeconds = Code.IntervalSeconds + 5;
		}
		else if (Err == "expired_token") Out.State = DevicePoll::Expired;
		else if (Err == "access_denied") Out.State = DevicePoll::Denied;
		else return MakeError(ErrorCode::Unauthorized, "GitHub sign-in failed: " + Err);
		return Out;
	}

	Result<std::string> GitHubDeviceFlow::FetchLogin(const std::string& Token)
	{
		auto U = FetchUser(Token);
		if (!U) return U.Err();
		return U->Login;
	}

	Result<GitHubUserInfo> GitHubDeviceFlow::FetchUser(const std::string& Token)
	{
		HttpRequest Req;
		Req.Url = ApiBase + "/user";
		Req.Headers = {{"Accept", "application/vnd.github+json"}, {"Authorization", "Bearer " + Token}, {"User-Agent", "SatisfactorySharedWorld"}};
		HttpResponse R;
		SW_ASSIGN(R, Http->Send(Req));
		if (R.Status == 0) return MakeError(ErrorCode::Network, "no internet");
		if (R.Status == 401) return MakeError(ErrorCode::Unauthorized, "GitHub rejected the credentials");
		if (R.Status == 403) return MakeError(ErrorCode::Unauthorized, "GitHub revoked or forbade the credentials");
		if (R.Status == 429) return MakeError(ErrorCode::RateLimited, "GitHub rate limited user lookup");
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub user lookup failed (" + std::to_string(R.Status) + ")");
		json::Value V;
		auto Parsed = json::Parse(R.Body);
		if (!Parsed) return MakeError(ErrorCode::Invalid, "malformed user response");
		return ParseUser(*Parsed);
	}

	GitHubAuthErrorKind GitHubAuthService::ClassifyError(const Error& Err)
	{
		if (Err.Code == ErrorCode::Unsupported) return GitHubAuthErrorKind::ClientIdMissing;
		if (Err.Code == ErrorCode::RateLimited) return GitHubAuthErrorKind::RateLimited;
		if (Err.Code == ErrorCode::Unauthorized)
		{
			if (Err.Message.find("revok") != std::string::npos) return GitHubAuthErrorKind::TokenRevoked;
			return GitHubAuthErrorKind::TokenInvalid;
		}
		if (Err.Code == ErrorCode::Invalid) return GitHubAuthErrorKind::MalformedResponse;
		if (Err.Code == ErrorCode::Network)
		{
			if (Err.Message.find("no internet") != std::string::npos) return GitHubAuthErrorKind::NoInternet;
			return GitHubAuthErrorKind::GitHubUnreachable;
		}
		if (Err.Code == ErrorCode::Io) return GitHubAuthErrorKind::CredentialStoreFailed;
		return GitHubAuthErrorKind::Other;
	}

	GitHubAuthService::GitHubAuthService(std::shared_ptr<IHttpClient> InHttp, std::shared_ptr<ICredentialStore> InStore, const IClock& InClock,
		std::string InClientId, std::string InCredentialKey, Logger InLog)
		: Http(std::move(InHttp)), Store(std::move(InStore)), Clock(InClock), ClientIdValue(std::move(InClientId)), CredentialKey(std::move(InCredentialKey)), Log(std::move(InLog))
	{
	}

	bool GitHubAuthService::IsConfigured() const
	{
		return !ClientIdValue.empty();
	}

	void GitHubAuthService::SetState(GitHubAuthState NewState, GitHubAuthErrorKind NewError)
	{
		State = NewState;
		ErrorKind = NewError;
	}

	GitHubAuthSnapshot GitHubAuthService::Snapshot() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		GitHubAuthSnapshot S;
		S.State = State;
		S.Error = ErrorKind;
		S.UserCode = ActiveCode.UserCode;
		S.VerificationUri = ActiveCode.VerificationUri;
		S.IntervalSeconds = ActiveCode.IntervalSeconds > 0 ? ActiveCode.IntervalSeconds : 5;
		S.User = User;
		S.bHasToken = bHasToken;
		S.PlayerMessage = GitHubAuthPlayerMessage(State, ErrorKind, User);
		return S;
	}

	Status GitHubAuthService::BeginLink()
	{
		std::string ClientIdCopy;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (!IsConfigured())
			{
				SetState(GitHubAuthState::Error, GitHubAuthErrorKind::ClientIdMissing);
				Log.Warn("github_auth_unconfigured", {{"reason", "client_id_missing"}});
				return MakeError(ErrorCode::Unsupported, "GitHub integration is not configured in this build");
			}
			LinkGeneration.fetch_add(1);
			SetState(GitHubAuthState::Starting, GitHubAuthErrorKind::None);
			ActiveCode = {};
			ClientIdCopy = ClientIdValue;
			Log.Info("github_device_auth_started", {});
		}
		GitHubDeviceFlow Flow(Http, ClientIdCopy, Clock);
		auto Code = Flow.Start();
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!Code)
		{
			const GitHubAuthErrorKind Kind = ClassifyError(Code.Err());
			SetState(GitHubAuthState::Error, Kind);
			Log.Warn("github_device_auth_start_failed", {{"category", std::to_string(static_cast<int>(Kind))}});
			return Code.Err();
		}
		ActiveCode = *Code;
		SetState(GitHubAuthState::WaitingForUser, GitHubAuthErrorKind::None);
		Log.Info("github_device_code_ready", {{"has_user_code", ActiveCode.UserCode.empty() ? "no" : "yes"}});
		return {};
	}

	Result<DevicePollResult> GitHubAuthService::PollOnce()
	{
		DeviceCode Code;
		std::string ClientIdCopy;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (State != GitHubAuthState::WaitingForUser && State != GitHubAuthState::Authorizing)
			{
				return MakeError(ErrorCode::Invalid, "no active GitHub link");
			}
			Code = ActiveCode;
			ClientIdCopy = ClientIdValue;
		}
		GitHubDeviceFlow Flow(Http, ClientIdCopy, Clock);
		auto Poll = Flow.Poll(Code);
		if (!Poll)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			SetState(GitHubAuthState::Error, ClassifyError(Poll.Err()));
			Log.Warn("github_device_poll_failed", {{"category", std::to_string(static_cast<int>(ErrorKind))}});
			return Poll.Err();
		}
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ActiveCode.IntervalSeconds = Poll->IntervalSeconds;
			switch (Poll->State)
			{
			case DevicePoll::Pending:
			case DevicePoll::SlowDown:
				SetState(GitHubAuthState::WaitingForUser, GitHubAuthErrorKind::None);
				break;
			case DevicePoll::Authorized:
				SetState(GitHubAuthState::Authorizing, GitHubAuthErrorKind::None);
				break;
			case DevicePoll::Denied:
				SetState(GitHubAuthState::Denied, GitHubAuthErrorKind::AuthorizationDenied);
				Log.Info("github_auth_denied", {});
				break;
			case DevicePoll::Expired:
				SetState(GitHubAuthState::Expired, GitHubAuthErrorKind::DeviceCodeExpired);
				Log.Info("github_auth_expired", {});
				break;
			}
		}
		return Poll;
	}

	Status GitHubAuthService::PersistAndConnect(const std::string& Token, GitHubUserInfo InUser)
	{
		if (Status W = Store->Write(CredentialKey, Token); !W)
		{
			SetState(GitHubAuthState::Error, GitHubAuthErrorKind::CredentialStoreFailed);
			Log.Error("github_credential_store_failed", {{"op", "write"}});
			return W.Err();
		}
		User = std::move(InUser);
		bHasToken = true;
		ActiveCode = {};
		SetState(GitHubAuthState::Connected, GitHubAuthErrorKind::None);
		Log.Info("github_auth_succeeded", {{"user", User.Login}, {"token_present", "yes"}});
		return {};
	}

	Status GitHubAuthService::FinalizeAuthorized(std::string AccessToken)
	{
		if (AccessToken.empty())
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			SetState(GitHubAuthState::Error, GitHubAuthErrorKind::TokenInvalid);
			return MakeError(ErrorCode::Unauthorized, "empty access token");
		}
		std::string ClientIdCopy;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ClientIdCopy = ClientIdValue;
			SetState(GitHubAuthState::Authorizing, GitHubAuthErrorKind::None);
		}
		GitHubDeviceFlow Flow(Http, ClientIdCopy, Clock);
		auto Profile = Flow.FetchUser(AccessToken);
		// Wipe the local copy after use regardless of outcome.
		const std::string TokenCopy = std::move(AccessToken);
		AccessToken.clear();
		if (!Profile)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			SetState(GitHubAuthState::Error, ClassifyError(Profile.Err()));
			Log.Warn("github_auth_validate_failed", {{"category", std::to_string(static_cast<int>(ErrorKind))}});
			return Profile.Err();
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		return PersistAndConnect(TokenCopy, *Profile);
	}

	void GitHubAuthService::CancelLink()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		LinkGeneration.fetch_add(1);
		ActiveCode = {};
		if (State == GitHubAuthState::Connected) return;
		SetState(GitHubAuthState::Disconnected, GitHubAuthErrorKind::Cancelled);
		Log.Info("github_auth_cancelled", {});
	}

	Status GitHubAuthService::Disconnect()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		LinkGeneration.fetch_add(1);
		ActiveCode = {};
		User = {};
		bHasToken = false;
		Status R = Store->Remove(CredentialKey);
		SetState(GitHubAuthState::Disconnected, GitHubAuthErrorKind::None);
		Log.Info("github_auth_disconnected", {{"token_present", "no"}});
		return R;
	}

	Status GitHubAuthService::RestoreSession()
	{
		std::string Token;
		std::string ClientIdCopy;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			auto Tok = Store->Read(CredentialKey);
			if (!Tok)
			{
				if (Tok.Is(ErrorCode::NotFound))
				{
					SetState(GitHubAuthState::Disconnected, GitHubAuthErrorKind::None);
					bHasToken = false;
					User = {};
					return {};
				}
				SetState(GitHubAuthState::Error, ClassifyError(Tok.Err()));
				return Tok.Err();
			}
			Token = *Tok;
			ClientIdCopy = ClientIdValue.empty() ? std::string("x") : ClientIdValue;
			bHasToken = true;
		}
		GitHubDeviceFlow Flow(Http, ClientIdCopy, Clock);
		auto Profile = Flow.FetchUser(Token);
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!Profile)
		{
			const GitHubAuthErrorKind Kind = ClassifyError(Profile.Err());
			if (Kind == GitHubAuthErrorKind::NoInternet || Kind == GitHubAuthErrorKind::GitHubUnreachable || Kind == GitHubAuthErrorKind::RateLimited)
			{
				// Keep the credential; player can retry when the network recovers.
				bHasToken = true;
				SetState(GitHubAuthState::Error, Kind);
				Log.Warn("github_auth_restore_network", {{"token_present", "yes"}});
				return Profile.Err();
			}
			(void)Store->Remove(CredentialKey);
			bHasToken = false;
			User = {};
			SetState(GitHubAuthState::Disconnected, GitHubAuthErrorKind::None);
			Log.Info("github_auth_restore_cleared", {{"token_present", "no"}});
			return {};
		}
		User = *Profile;
		bHasToken = true;
		SetState(GitHubAuthState::Connected, GitHubAuthErrorKind::None);
		Log.Info("github_auth_restored", {{"user", User.Login}, {"token_present", "yes"}});
		return {};
	}

	Result<GitHubUserInfo> GitHubAuthService::TestAccess()
	{
		auto Tok = Store->Read(CredentialKey);
		if (!Tok)
		{
			if (Tok.Is(ErrorCode::NotFound)) return MakeError(ErrorCode::Unauthorized, "no GitHub account connected");
			return Tok.Err();
		}
		GitHubDeviceFlow Flow(Http, ClientIdValue.empty() ? std::string("x") : ClientIdValue, Clock);
		auto Profile = Flow.FetchUser(*Tok);
		if (!Profile) return Profile.Err();
		std::lock_guard<std::mutex> Lock(Mutex);
		User = *Profile;
		bHasToken = true;
		SetState(GitHubAuthState::Connected, GitHubAuthErrorKind::None);
		Log.Info("github_auth_test_ok", {{"user", User.Login}, {"token_present", "yes"}});
		return Profile;
	}
}
