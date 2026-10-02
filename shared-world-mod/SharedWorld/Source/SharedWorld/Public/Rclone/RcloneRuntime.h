#pragma once
// In-process rclone engine (librclone). rclone is Go, so there is no C++ port: the official C-callable
// librclone DLL is built from the rclone source by tools/rclone/build-librclone.ps1 and loaded here at run time.
//
// Nothing links against it. If the DLL is not installed every call degrades to a clear "not installed" result and
// the rest of the mod is unaffected. The library is never unloaded (rclone's docs forbid FreeLibrary: the Go runtime
// keeps background threads).

#include "CoreMinimal.h"
#include <atomic>

#include "HAL/CriticalSection.h"

struct FRcloneResult
{
	bool bOk = false;
	/** HTTP-style status from rclone (200 = ok). 0 means the call never reached the engine. */
	int32 Status = 0;
	/** JSON body. On failure it normally holds {"error": "..."}. */
	FString Output;

	/** The "error" text if present, otherwise the raw output. */
	FString ErrorText() const;
};

class SHAREDWORLD_API FRcloneRuntime
{
public:
	static FRcloneRuntime& Get();

	/** Loads and initialises the engine once. Safe from any thread, cheap after the first call. */
	bool EnsureLoaded();
	bool IsAvailable() const { return bReady; }
	/** Player/developer readable reason when IsAvailable() is false. */
	FString UnavailableReason() const;

	/** Blocking. Call from a background thread: network providers can take seconds. */
	FRcloneResult Rpc(const FString& Method, const FString& InputJson);

	/** Engine version string (e.g. "v1.68.2"), empty when unavailable. */
	FString Version();

	/** librclone.dll is present next to the mod (cheap file check; does not load it). */
	bool IsInstalled() const;
	FString LibraryPath() const;
	/** Per-user rclone.conf (provider tokens live here; see docs for the planned encryption). */
	FString ConfigPath() const;

private:
	FRcloneRuntime() = default;
	FRcloneResult RpcRaw(const FString& Method, const FString& InputJson);

	mutable FCriticalSection Lock;
	bool bTried = false;
	std::atomic<bool> bReadyAtomic{false};
	bool bReady = false;
	FString Reason;
	FString CachedVersion;
};
