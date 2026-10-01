#pragma once

#include "CoreMinimal.h"
#include "SharedWorldCore/Providers/Http.h"

#include <atomic>
#include <memory>

/**
 * sw::IHttpClient over FHttpModule. Called only on SharedWorldCore's worker
 * threads (never the game thread: Send() blocks). FHttpModule itself is
 * ticked on the game thread by the engine, so blocking a worker thread here
 * does not stall requests in flight — but Deinitialize must be able to abort
 * those waits or the game thread deadlocks joining the workers.
 *
 * Redirects: the providers do not depend on whether the backend follows
 * them (see the .cpp). Whether the game's libcurl forwards Authorization
 * across hosts is a runtime-validation item in STATUS.md.
 */
class FSharedWorldHttpClient final : public sw::IHttpClient
{
public:
	explicit FSharedWorldHttpClient(std::shared_ptr<std::atomic<bool>> InShutdownFlag = nullptr);

	sw::Result<sw::HttpResponse> Send(const sw::HttpRequest& Request) override;

private:
	std::shared_ptr<std::atomic<bool>> ShutdownFlag;
};
