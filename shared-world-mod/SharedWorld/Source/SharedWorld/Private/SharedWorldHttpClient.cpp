#include "SharedWorldHttpClient.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "SharedWorldTypes.h"

namespace
{
	/** Shared between the waiting worker and the completion callback, which may outlive a timed-out wait. */
	struct FCompletion
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
		TAtomic<bool> bConnectionFailed{false};
		~FCompletion() { FPlatformProcess::ReturnSynchEventToPool(Done); }
	};
}

sw::Result<sw::HttpResponse> FSharedWorldHttpClient::Send(const sw::HttpRequest& Request)
{
	// Blocks a SharedWorldCore worker thread; never the game thread, which
	// ticks the HTTP module and must keep rendering.
	check(!IsInGameThread());

	// Redirects: whatever the backend does is safe. GitHubReleaseObjectStore
	// accepts a 302 (and follows it itself without credentials) or an
	// already-followed 200. libcurl >= 7.58 does not forward a custom
	// Authorization header to a different host when it follows a redirect.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(UTF8_TO_TCHAR(Request.Method.c_str()));
	Req->SetURL(UTF8_TO_TCHAR(Request.Url.c_str()));
	for (const auto& [Key, Value] : Request.Headers)
	{
		Req->SetHeader(UTF8_TO_TCHAR(Key.c_str()), UTF8_TO_TCHAR(Value.c_str()));
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
	Req->OnProcessRequestComplete().BindLambda([State](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		State->bConnectionFailed = !bOk || !Response.IsValid();
		State->Done->Trigger();
	});
	if (!Req->ProcessRequest())
	{
		return sw::MakeError(sw::ErrorCode::Network, "could not start the HTTP request");
	}
	// The request has its own timeout; this outer bound only protects the
	// worker if completion never arrives (e.g. the engine is shutting down).
	if (!State->Done->Wait(FTimespan::FromSeconds(TimeoutSeconds + 30)))
	{
		Req->OnProcessRequestComplete().Unbind();
		Req->CancelRequest();
		return sw::MakeError(sw::ErrorCode::Network, "network request timed out");
	}
	FHttpResponsePtr Response = Req->GetResponse();
	if (State->bConnectionFailed || !Response.IsValid())
	{
		return sw::MakeError(sw::ErrorCode::Network, "network request failed");
	}

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
		// Written in one piece; the core re-hashes the file before trusting it.
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
