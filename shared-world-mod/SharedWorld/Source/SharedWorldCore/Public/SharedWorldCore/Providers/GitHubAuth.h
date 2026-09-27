#pragma once
// GitHub sign-in with the OAuth 2.0 device flow: the mod shows a short code,
// the player enters it at github.com/login/device, the mod polls. Only the
// app's public client id ships with the mod (no client secret). The token is
// handed to an ICredentialStore; it is never logged or shown.

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "SharedWorldCore/Providers/Http.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	class ICredentialStore
	{
	public:
		virtual ~ICredentialStore() = default;
		virtual Result<std::string> Read(const std::string& Key) = 0; // NotFound if absent
		virtual Status Write(const std::string& Key, const std::string& Secret) = 0;
		virtual Status Remove(const std::string& Key) = 0;
	};

	/** Tests only. */
	class MemoryCredentialStore final : public ICredentialStore
	{
	public:
		Result<std::string> Read(const std::string& Key) override;
		Status Write(const std::string& Key, const std::string& Secret) override;
		Status Remove(const std::string& Key) override;

	private:
		std::mutex Mutex;
		std::map<std::string, std::string> Secrets;
	};

	struct DeviceCode
	{
		std::string Code;            // device_code (secret-ish; never shown)
		std::string UserCode;        // shown to the player, e.g. WDJB-MJHT
		std::string VerificationUri; // https://github.com/login/device
		int IntervalSeconds = 5;
		TimeMs ExpiresAt = 0;
	};

	enum class DevicePoll { Pending, SlowDown, Authorized, Denied, Expired };

	struct DevicePollResult
	{
		DevicePoll State = DevicePoll::Pending;
		std::string AccessToken; // only when Authorized
		int IntervalSeconds = 5; // updated on SlowDown
	};

	class GitHubDeviceFlow
	{
	public:
		GitHubDeviceFlow(std::shared_ptr<IHttpClient> InHttp, std::string InClientId, const IClock& InClock, std::string InScope = "repo",
			std::string InWebBase = "https://github.com", std::string InApiBase = "https://api.github.com");

		Result<DeviceCode> Start();
		/** One poll; call again after IntervalSeconds while Pending / SlowDown. */
		Result<DevicePollResult> Poll(const DeviceCode& Code);
		/** GitHub login of the token's owner (GET /user). */
		Result<std::string> FetchLogin(const std::string& Token);

	private:
		std::shared_ptr<IHttpClient> Http;
		std::string ClientId;
		const IClock& Clock;
		std::string Scope;
		std::string WebBase;
		std::string ApiBase;
	};
}
