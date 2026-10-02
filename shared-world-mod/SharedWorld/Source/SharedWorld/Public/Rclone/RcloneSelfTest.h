#pragma once
// End-to-end check of the rclone plumbing that needs no account and no network: round-trips a real file through
// FRcloneObjectStore using rclone's own local-folder backend. If this passes, loading, the RPC bridge, upload,
// download, existence checks, listing and deletion all work; only provider-specific details (OAuth, quotas) remain.

#include "CoreMinimal.h"

struct FRcloneSelfTestResult
{
	bool bOk = false;
	/** One line per step: "OK ..." / "FAILED ...". Plain text, safe to show in Diagnostics. */
	TArray<FString> Lines;
	FString Summary() const;
};

/** Blocking (a few seconds with a multi-MB file). Run on a background thread. */
SHAREDWORLD_API FRcloneSelfTestResult RunRcloneSelfTest();
