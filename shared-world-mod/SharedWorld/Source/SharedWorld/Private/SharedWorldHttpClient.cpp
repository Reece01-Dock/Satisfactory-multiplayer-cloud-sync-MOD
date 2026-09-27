#include "SharedWorldHttpClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "SharedWorldTypes.h"

sw::Result<sw::HttpResponse> FSharedWorldHttpClient::Send(const sw::HttpRequest& Request)
{
	// Blocks on a worker queue thread; must never run on the game thread
	// (that would stall heartbeats/session polling behind network I/O).
	check(!IsInGameThread());

	// UNVERIFIED (see STATUS.md): EHttpRequestRedirectPolicy::Never is the
	// documented way to stop the HTTP backend from auto-following redirects,
	// but its behaviour has not been confirmed against this game's build of
	// libcurl. If it turns out to still follow redirects, an Authorization
	// header could leak to a release-asset storage host; SW_TEST GitHubTests
	// covers this at the provider layer with FakeGitHub, but that cannot
	// exercise the real HTTP backend.
	TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest(FString(), EHttpRequestRedirectPolicy::Never);
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
		Bytes.Append(reinterpret_cast<const uint8*>(Request.Body.data()), Request.Body.size());
		Req->SetContent(MoveTemp(Bytes));
	}
	if (Request.TimeoutSeconds > 0)
	{
		Req->SetTimeout(static_cast<float>(Request.TimeoutSeconds));
	}

	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
	bool bConnectionFailed = false;
	Req->OnProcessRequestComplete().BindLambda([Done, &bConnectionFailed](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
	{
		bConnectionFailed = !bOk || !Response.IsValid();
		Done->Trigger();
	});
	if (!Req->ProcessRequest())
	{
		FPlatformProcess::ReturnSynchEventToPool(Done);
		return sw::MakeError(sw::ErrorCode::Network, "could not start the HTTP request");
	}
	Done->Wait();
	FPlatformProcess::ReturnSynchEventToPool(Done);

	if (bConnectionFailed)
	{
		return sw::MakeError(sw::ErrorCode::Network, "network request failed");
	}
	FHttpResponsePtr Response = Req->GetResponse();
	sw::HttpResponse Out;
	Out.Status = Response->GetResponseCode();
	for (const FString& Header : Response->GetAllHeaders())
	{
		FString Key, Value;
		if (Header.Split(TEXT(": "), &Key, &Value))
		{
			Out.Headers.emplace_back(TCHAR_TO_UTF8(*Key), TCHAR_TO_UTF8(*Value));
		}
	}
	const TArray<uint8>& Content = Response->GetContent();
	if (!Request.ResponseFile.empty())
	{
		if (Out.Status >= 200 && Out.Status < 300 &&
			!FFileHelper::SaveArrayToFile(Content, UTF8_TO_TCHAR(Request.ResponseFile.c_str())))
		{
			return sw::MakeError(sw::ErrorCode::Io, "could not write the downloaded file");
		}
	}
	else
	{
		Out.Body.assign(reinterpret_cast<const char*>(Content.GetData()), Content.Num());
	}
	return Out;
}
