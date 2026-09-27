#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/Providers/Http.h"

/**
 * sw::IHttpClient over FHttpModule. Called only on SharedWorldCore's worker
 * threads (never the game thread: Send() blocks). FHttpModule itself is
 * ticked on the game thread by the engine, so blocking a worker thread here
 * does not stall requests in flight.
 *
 * Never follows redirects (SharedWorldCore::IHttpClient contract): the
 * provider layer resolves them explicitly so the Authorization header is
 * never forwarded to another host (e.g. a release-asset storage redirect).
 */
class FSharedWorldHttpClient final : public sw::IHttpClient
{
public:
	sw::Result<sw::HttpResponse> Send(const sw::HttpRequest& Request) override;
};
