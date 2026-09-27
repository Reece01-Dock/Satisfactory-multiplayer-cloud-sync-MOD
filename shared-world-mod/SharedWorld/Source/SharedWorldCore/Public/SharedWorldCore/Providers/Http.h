#pragma once
// Minimal blocking HTTP interface used by network providers. Called only on
// worker threads. The game layer supplies the implementation (FHttpModule);
// tests supply fakes. Implementations must NOT follow redirects on their
// own: providers follow them explicitly so credentials never travel to
// another host.

#include <string>
#include <utility>
#include <vector>

#include "SharedWorldCore/Util/Result.h"

namespace sw
{
	struct HttpRequest
	{
		std::string Method = "GET";
		std::string Url;
		std::vector<std::pair<std::string, std::string>> Headers;
		std::string Body;
		/** If set, the request body is streamed from this file instead of Body. */
		std::string BodyFile;
		/** If set, a 2xx response body is written to this file instead of Body. */
		std::string ResponseFile;
		int TimeoutSeconds = 60;
	};

	struct HttpResponse
	{
		int Status = 0;
		std::vector<std::pair<std::string, std::string>> Headers;
		std::string Body;
		/** Case-insensitive header lookup ("" if absent). */
		std::string Header(const std::string& Name) const;
	};

	class IHttpClient
	{
	public:
		virtual ~IHttpClient() = default;
		/** Network failures (DNS, TLS, timeouts, resets) -> ErrorCode::Network. */
		virtual Result<HttpResponse> Send(const HttpRequest& Request) = 0;
	};
}
