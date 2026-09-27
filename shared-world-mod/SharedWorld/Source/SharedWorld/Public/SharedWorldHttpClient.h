#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/Providers/Http.h"

/**
 * sw::IHttpClient over FHttpModule. Called only on SharedWorldCore's worker
 * threads (never the game thread: Send() blocks). FHttpModule itself is
 * ticked on the game thread by the engine, so blocking a worker thread here
 * does not stall requests in flight.
 *
 * Redirects: the providers do not depend on whether the backend follows
 * them (see the .cpp). Whether the game's libcurl forwards Authorization
 * across hosts is a runtime-validation item in STATUS.md.
 */
class FSharedWorldHttpClient final : public sw::IHttpClient
{
public:
	sw::Result<sw::HttpResponse> Send(const sw::HttpRequest& Request) override;
};
