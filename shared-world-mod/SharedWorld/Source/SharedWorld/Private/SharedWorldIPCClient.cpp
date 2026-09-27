#include "SharedWorldIPCClient.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SharedWorldTypes.h"

namespace
{
	constexpr int32 MaxConnectAttempts = 20;
	constexpr float RetryDelaySeconds = 1.0f;
	constexpr float RequestTimeoutSeconds = 30.0f;
}

FString USharedWorldIPCClient::GetHelperDataDir()
{
	// On Windows UserSettingsDir() is %LOCALAPPDATA%, matching the helper's
	// default data directory (config.DefaultDataDir).
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"));
}

void USharedWorldIPCClient::Connect()
{
	if (bConnected || bConnecting)
	{
		return;
	}
	bConnecting = true;
	Attempts = 0;
	CheckHealth();
}

bool USharedWorldIPCClient::LoadDiscovery()
{
	const FString Path = FPaths::Combine(GetHelperDataDir(), TEXT("discovery.json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		LastError = TEXT("The Shared World helper is not running.");
		return false;
	}
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	int32 Port = 0;
	FString NewToken;
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()
		|| !Json->TryGetNumberField(TEXT("port"), Port) || !Json->TryGetStringField(TEXT("token"), NewToken)
		|| Port <= 0 || Port > 65535 || NewToken.Len() < 32)
	{
		LastError = TEXT("The Shared World helper discovery file is invalid.");
		return false;
	}
	BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), Port);
	Token = NewToken;
	return true;
}

bool USharedWorldIPCClient::LaunchHelper()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SharedWorld"));
	if (!Plugin.IsValid())
	{
		return false;
	}
	const FString Exe = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("ThirdParty"), TEXT("SharedWorldHelper"), TEXT("Win64"), TEXT("shared-world-helper.exe")));
	if (!FPaths::FileExists(Exe))
	{
		LastError = TEXT("The Shared World helper program is missing from the mod installation.");
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=helper_missing path=%s"), *Exe);
		return false;
	}
	uint32 Pid = 0;
	// Detached and hidden: the helper outlives short game hiccups and has no console window.
	FProcHandle Handle = FPlatformProcess::CreateProc(*Exe, TEXT(""), /*bLaunchDetached*/ true, /*bLaunchHidden*/ true,
		/*bLaunchReallyHidden*/ true, &Pid, /*PriorityModifier*/ 0, /*OptionalWorkingDirectory*/ nullptr, /*PipeWriteChild*/ nullptr);
	if (!Handle.IsValid())
	{
		LastError = TEXT("Could not start the Shared World helper.");
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=helper_launch_failed path=%s"), *Exe);
		return false;
	}
	FPlatformProcess::CloseProc(Handle);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=helper_launched pid=%u"), Pid);
	return true;
}

void USharedWorldIPCClient::CheckHealth()
{
	++Attempts;
	if (!LoadDiscovery())
	{
		if (!bLaunchedHelper)
		{
			bLaunchedHelper = true;
			LaunchHelper();
		}
		ScheduleRetry();
		return;
	}
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(BaseUrl + TEXT("/v1/health"));
	Req->SetVerb(TEXT("GET"));
	Req->SetTimeout(3.0f);
	TWeakObjectPtr<USharedWorldIPCClient> WeakThis(this);
	Req->OnProcessRequestComplete().BindLambda([WeakThis](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		USharedWorldIPCClient* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		int32 ApiVersion = 0;
		TSharedPtr<FJsonObject> Json;
		if (bOk && Resp.IsValid() && Resp->GetResponseCode() == 200)
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
			{
				Json->TryGetNumberField(TEXT("apiVersion"), ApiVersion);
			}
		}
		if (ApiVersion == ExpectedApiVersion)
		{
			Self->bConnected = true;
			Self->bConnecting = false;
			Self->LastError.Reset();
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=helper_connected url=%s"), *Self->BaseUrl);
			return;
		}
		if (ApiVersion != 0)
		{
			Self->bConnecting = false;
			Self->LastError = FString::Printf(TEXT("The Shared World helper is incompatible (API %d, mod expects %d). Update the mod."), ApiVersion, ExpectedApiVersion);
			UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=helper_incompatible api=%d"), ApiVersion);
			return;
		}
		// Stale discovery file (helper crashed): try launching once, then retry.
		if (!Self->bLaunchedHelper)
		{
			Self->bLaunchedHelper = true;
			Self->LaunchHelper();
		}
		Self->LastError = TEXT("Waiting for the Shared World helper...");
		Self->ScheduleRetry();
	});
	Req->ProcessRequest();
}

void USharedWorldIPCClient::ScheduleRetry()
{
	if (Attempts >= MaxConnectAttempts)
	{
		bConnecting = false;
		if (LastError.IsEmpty())
		{
			LastError = TEXT("The Shared World helper did not start.");
		}
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=helper_connect_failed attempts=%d reason=\"%s\""), Attempts, *LastError);
		return;
	}
	TWeakObjectPtr<USharedWorldIPCClient> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
	{
		if (USharedWorldIPCClient* Self = WeakThis.Get())
		{
			Self->CheckHealth();
		}
		return false; // one-shot
	}), RetryDelaySeconds);
}

void USharedWorldIPCClient::OnDisconnected(const FString& Reason)
{
	if (bConnected)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=helper_disconnected reason=\"%s\""), *Reason);
	}
	bConnected = false;
	bLaunchedHelper = false; // allow one relaunch after a helper crash
	LastError = TEXT("Lost connection to the Shared World helper. Reconnecting...");
	Connect();
}

void USharedWorldIPCClient::Request(const FString& Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FSharedWorldIPCResponse OnDone)
{
	if (!bConnected)
	{
		Connect();
		OnDone.ExecuteIfBound(false, 0, nullptr);
		return;
	}
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(BaseUrl + Path);
	Req->SetVerb(Verb);
	Req->SetTimeout(RequestTimeoutSeconds);
	Req->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
	if (Body.IsValid())
	{
		FString Payload;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Req->SetContentAsString(Payload);
	}
	TWeakObjectPtr<USharedWorldIPCClient> WeakThis(this);
	Req->OnProcessRequestComplete().BindLambda([WeakThis, OnDone, Path](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
	{
		USharedWorldIPCClient* Self = WeakThis.Get();
		if (!bOk || !Resp.IsValid())
		{
			if (Self)
			{
				Self->OnDisconnected(FString::Printf(TEXT("request to %s failed"), *Path));
			}
			OnDone.ExecuteIfBound(false, 0, nullptr);
			return;
		}
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		FJsonSerializer::Deserialize(Reader, Json);
		const int32 Code = Resp->GetResponseCode();
		if (Code == 401 && Self)
		{
			// Helper restarted with a new token.
			Self->OnDisconnected(TEXT("token rejected"));
		}
		OnDone.ExecuteIfBound(Code >= 200 && Code < 300, Code, Json);
	});
	Req->ProcessRequest();
}
