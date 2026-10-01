#include "SharedWorldHttpClient.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "SharedWorldTypes.h"

namespace
{
	/**
	 * Shared between the waiting worker and the completion callback.
	 * Must outlive both: the callback may run after Send() has already returned
	 * (soft-abandon on shutdown) or after a timed-out CancelRequest.
	 */
	struct FCompletion
	{
		FEvent* Done = nullptr;
		TAtomic<bool> bConnectionFailed{false};
		TAtomic<bool> bFinished{false};

		FCompletion()
		{
			Done = FPlatformProcess::GetSynchEventFromPool(true);
		}

		~FCompletion()
		{
			if (Done)
			{
				FPlatformProcess::ReturnSynchEventToPool(Done);
				Done = nullptr;
			}
		}
	};

	bool ShouldAbandonHttp(const std::shared_ptr<std::atomic<bool>>& ShutdownFlag)
	{
		if (IsEngineExitRequested())
		{
			return true;
		}
		return ShutdownFlag && ShutdownFlag->load(std::memory_order_acquire);
	}
}

FSharedWorldHttpClient::FSharedWorldHttpClient(std::shared_ptr<std::atomic<bool>> InShutdownFlag)
	: ShutdownFlag(std::move(InShutdownFlag))
{
}

sw::Result<sw::HttpResponse> FSharedWorldHttpClient::Send(const sw::HttpRequest& Request)
{
	// Blocks a SharedWorldCore worker thread; never the game thread, which
	// ticks the HTTP module and must keep rendering.
	check(!IsInGameThread());

	if (ShouldAbandonHttp(ShutdownFlag))
	{
		return sw::MakeError(sw::ErrorCode::Network, "shutting down");
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(UTF8_TO_TCHAR(Request.Method.c_str()));
	Req->SetURL(UTF8_TO_TCHAR(Request.Url.c_str()));
	bool bHasContentType = false;
	for (const auto& [Key, Value] : Request.Headers)
	{
		if (FCStringAnsi::Stricmp(Key.c_str(), "Content-Type") == 0)
		{
			bHasContentType = true;
		}
		Req->SetHeader(UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(Value.c_str()));
	}
	const bool bHasBody = !Request.BodyFile.empty() || !Request.Body.empty();
	if (bHasBody && !bHasContentType)
	{
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	}
	if (!Request.BodyFile.empty())
	{
		if (!Req->SetContentAsStreamedFile(UTF8_TO_TCHAR(Request.BodyFile.c_str())))
		{
			return sw::MakeError(sw::ErrorCode::Io, "could not open the upload file");
		}
	}
	else if (!Request.Body.empty())
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Request.Body.data()), static_cast<int32>(Request.Body.size()));
		Req->SetContent(MoveTemp(Bytes));
	}
	const int32 TimeoutSeconds = Request.TimeoutSeconds > 0 ? Request.TimeoutSeconds : 60;
	Req->SetTimeout(static_cast<float>(TimeoutSeconds));

	TSharedRef<FCompletion, ESPMode::ThreadSafe> State = MakeShared<FCompletion, ESPMode::ThreadSafe>();
	// Capture Req so the request stays alive if Send() soft-abandons before completion.
	// Never Unbind+destroy the request from a worker during engine exit — that races
	// FHttpModule teardown and has caused hard machine lockups.
	Req->OnProcessRequestComplete().BindLambda([State, Req](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		State->bConnectionFailed = !bOk || !Response.IsValid();
		State->bFinished = true;
		State->Done->Trigger();
	});
	if (!Req->ProcessRequest())
	{
		return sw::MakeError(sw::ErrorCode::Network, "could not start the HTTP request");
	}

	const double Deadline = FPlatformTime::Seconds() + static_cast<double>(TimeoutSeconds) + 30.0;
	for (;;)
	{
		if (State->Done->Wait(FTimespan::FromMilliseconds(100)))
		{
			break;
		}
		if (ShouldAbandonHttp(ShutdownFlag))
		{
			// Soft abandon: leave the request + callback alive (held by the lambda).
			// Do NOT CancelRequest / Unbind here.
			return sw::MakeError(sw::ErrorCode::Network, "shutting down");
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			// Normal timeout (not exit): cancel and wait briefly for the callback so
			// the pooled FEvent is not returned while Trigger may still run.
			Req->CancelRequest();
			State->Done->Wait(FTimespan::FromSeconds(2.0));
			return sw::MakeError(sw::ErrorCode::Network, "network request timed out");
		}
	}

	if (State->bConnectionFailed || !Req->GetResponse().IsValid())
	{
		return sw::MakeError(sw::ErrorCode::Network, "network request failed");
	}

	FHttpResponsePtr Response = Req->GetResponse();
	sw::HttpResponse Out;
	Out.Status = Response->GetResponseCode();
	for (const FString& Header : Response->GetAllHeaders())
	{
		FString Key, Value;
		if (Header.Split(TEXT(":"), &Key, &Value))
		{
			Out.Headers.emplace_back(TCHAR_TO_UTF8(*Key.TrimStartAndEnd()), TCHAR_TO_UTF8(*Value.TrimStartAndEnd()));
		}
	}
	const TArray<uint8>& Content = Response->GetContent();
	if (!Request.ResponseFile.empty() && Out.Status >= 200 && Out.Status < 300)
	{
		if (!FFileHelper::SaveArrayToFile(Content, UTF8_TO_TCHAR(Request.ResponseFile.c_str())))
		{
			return sw::MakeError(sw::ErrorCode::Io, "could not write the downloaded file");
		}
	}
	else
	{
		Out.Body.assign(reinterpret_cast<const char*>(Content.GetData()), static_cast<size_t>(Content.Num()));
	}
	return Out;
}
