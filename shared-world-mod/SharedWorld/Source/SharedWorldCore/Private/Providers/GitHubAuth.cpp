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
	}

	GitHubDeviceFlow::GitHubDeviceFlow(std::shared_ptr<IHttpClient> InHttp, std::string InClientId, const IClock& InClock, std::string InScope, std::string InWebBase, std::string InApiBase)
		: Http(std::move(InHttp)), ClientId(std::move(InClientId)), Clock(InClock), Scope(std::move(InScope)), WebBase(std::move(InWebBase)), ApiBase(std::move(InApiBase))
	{
	}

	Result<DeviceCode> GitHubDeviceFlow::Start()
	{
		if (ClientId.empty()) return MakeError(ErrorCode::Unsupported, "GitHub sign-in is not configured for this build");
		HttpResponse R;
		SW_ASSIGN(R, Http->Send(FormPost(WebBase + "/login/device/code", "client_id=" + FormEncode(ClientId) + "&scope=" + FormEncode(Scope))));
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub sign-in could not start (" + std::to_string(R.Status) + ")");
		json::Value V;
		SW_ASSIGN(V, json::Parse(R.Body));
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
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub sign-in poll failed (" + std::to_string(R.Status) + ")");
		json::Value V;
		SW_ASSIGN(V, json::Parse(R.Body));
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
		HttpRequest Req;
		Req.Url = ApiBase + "/user";
		Req.Headers = {{"Accept", "application/vnd.github+json"}, {"Authorization", "Bearer " + Token}, {"User-Agent", "SatisfactorySharedWorld"}};
		HttpResponse R;
		SW_ASSIGN(R, Http->Send(Req));
		if (R.Status == 401) return MakeError(ErrorCode::Unauthorized, "GitHub rejected the credentials");
		if (R.Status != 200) return MakeError(ErrorCode::Network, "GitHub user lookup failed (" + std::to_string(R.Status) + ")");
		json::Value V;
		SW_ASSIGN(V, json::Parse(R.Body));
		return json::GetString(V, "login", 64);
	}
}
