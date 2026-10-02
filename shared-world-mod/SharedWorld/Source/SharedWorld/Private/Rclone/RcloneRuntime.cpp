#include "Rclone/RcloneRuntime.h"

#include <thread>

#include "Async/Async.h"
#include "CoreGlobals.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SharedWorldTypes.h"

namespace
{
	// librclone's C ABI (see rclone/librclone/README.md). The generated header is deliberately not used on Windows.
	struct FRcloneRpcResult
	{
		char* Output;
		int Status;
	};
	using FRcloneInitializeFn = void (*)();
	using FRcloneRpcFn = FRcloneRpcResult (*)(char* Method, char* Input);
	using FRcloneFreeStringFn = void (*)(char* Str);

	void* GHandle = nullptr; // never freed on purpose
	FRcloneInitializeFn GInitialize = nullptr;
	FRcloneRpcFn GRpc = nullptr;
	FRcloneFreeStringFn GFreeString = nullptr;
}

FString FRcloneResult::ErrorText() const
{
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Output);
	if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
	{
		FString Err;
		if (Obj->TryGetStringField(TEXT("error"), Err) && !Err.IsEmpty()) return Err;
	}
	return Output;
}

FRcloneRuntime& FRcloneRuntime::Get()
{
	static FRcloneRuntime Instance;
	return Instance;
}

FString FRcloneRuntime::LibraryPath() const
{
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SharedWorld")))
	{
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("ThirdParty"), TEXT("rclone"), TEXT("librclone.dll"));
	}
	return FString();
}

FString FRcloneRuntime::ConfigPath() const
{
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("rclone.conf"));
}

FString FRcloneRuntime::UnavailableReason() const
{
	FScopeLock L(&Lock);
	return Reason;
}

bool FRcloneRuntime::EnsureLoaded()
{
	if (bReadyAtomic.load()) return true;
	FScopeLock L(&Lock);
	if (bTried) return bReady;
	bTried = true;

#if PLATFORM_WINDOWS
	const FString Path = LibraryPath();
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		Reason = TEXT("The rclone engine is not installed (librclone.dll is missing).");
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/rclone] event=engine_missing path=%s"), *Path);
		return false;
	}
	GHandle = FPlatformProcess::GetDllHandle(*Path);
	if (!GHandle)
	{
		Reason = TEXT("The rclone engine could not be loaded.");
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld/rclone] event=engine_load_failed path=%s"), *Path);
		return false;
	}
	GInitialize = reinterpret_cast<FRcloneInitializeFn>(FPlatformProcess::GetDllExport(GHandle, TEXT("RcloneInitialize")));
	GRpc = reinterpret_cast<FRcloneRpcFn>(FPlatformProcess::GetDllExport(GHandle, TEXT("RcloneRPC")));
	GFreeString = reinterpret_cast<FRcloneFreeStringFn>(FPlatformProcess::GetDllExport(GHandle, TEXT("RcloneFreeString")));
	if (!GInitialize || !GRpc || !GFreeString)
	{
		Reason = TEXT("The rclone engine is not a compatible librclone build.");
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld/rclone] event=engine_incompatible"));
		return false;
	}
	GInitialize();

	// Keep provider tokens in the mod's own per-user config, never in the player's own rclone setup.
	const FString Config = ConfigPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Config), true);
	{
		const FString In = FString::Printf(TEXT("{\"path\":\"%s\"}"), *Config.Replace(TEXT("\\"), TEXT("/")));
		const FRcloneResult R = RpcRaw(TEXT("config/setpath"), In);
		if (!R.bOk)
		{
			Reason = TEXT("The rclone engine could not set its config location.");
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld/rclone] event=setpath_failed detail=%s"), *R.ErrorText());
			return false;
		}
	}
	RpcRaw(TEXT("options/set"), TEXT("{\"main\":{\"LogLevel\":\"ERROR\"}}")); // keep rclone quiet in the game log
	bReady = true;
	bReadyAtomic.store(true);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/rclone] event=engine_ready"));
	return true;
#else
	Reason = TEXT("The rclone engine is only available on Windows.");
	return false;
#endif
}

FRcloneResult FRcloneRuntime::RpcRaw(const FString& Method, const FString& InputJson)
{
	FRcloneResult Out;
	if (!GRpc || !GFreeString)
	{
		Out.Output = TEXT("{\"error\":\"engine not loaded\"}");
		return Out;
	}
	FTCHARToUTF8 MethodUtf8(*Method);
	FTCHARToUTF8 InputUtf8(*InputJson);
	// librclone takes writable char*; the buffers are ours and are not modified.
	const FRcloneRpcResult R = GRpc(const_cast<char*>(MethodUtf8.Get()), const_cast<char*>(InputUtf8.Get()));
	Out.Status = R.Status;
	if (R.Output)
	{
		Out.Output = UTF8_TO_TCHAR(R.Output);
		GFreeString(R.Output);
	}
	Out.bOk = (R.Status == 200);
	return Out;
}

FRcloneResult FRcloneRuntime::Rpc(const FString& Method, const FString& InputJson)
{
	if (!EnsureLoaded())
	{
		FRcloneResult Out;
		Out.Output = FString::Printf(TEXT("{\"error\":\"%s\"}"), *UnavailableReason().ReplaceCharWithEscapedChar());
		return Out;
	}
	return RpcRaw(Method, InputJson);
}

FString FRcloneRuntime::Version()
{
	{
		FScopeLock L(&Lock);
		if (!CachedVersion.IsEmpty()) return CachedVersion;
	}
	const FRcloneResult R = Rpc(TEXT("core/version"), TEXT("{}"));
	if (!R.bOk) return FString();
	TSharedPtr<FJsonObject> Obj;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(R.Output);
	if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
	{
		FString V;
		if (Obj->TryGetStringField(TEXT("version"), V))
		{
			FScopeLock L(&Lock);
			CachedVersion = V;
			return V;
		}
	}
	return FString();
}

bool FRcloneRuntime::IsInstalled() const
{
	const FString Path = LibraryPath();
	return !Path.IsEmpty() && FPaths::FileExists(Path);
}

void FRcloneRuntime::RunDetached(TUniqueFunction<void()> Work)
{
	std::thread([Work = MoveTemp(Work)]() mutable
	{
		Work();
	}).detach();
}

void FRcloneRuntime::PostToGameThread(TUniqueFunction<void()> Fn)
{
	if (IsEngineExitRequested()) return;
	AsyncTask(ENamedThreads::GameThread, [Fn = MoveTemp(Fn)]() mutable
	{
		if (!IsEngineExitRequested()) Fn();
	});
}
