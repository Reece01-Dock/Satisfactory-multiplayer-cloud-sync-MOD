#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSharedWorld, Log, All);

/**
 * One world's UI-facing snapshot: SharedWorldCore's sw::WorldSummary (cloud
 * metadata, refreshed on a background thread) plus sw::SessionView (this
 * PC's local session, if one is active) flattened into game-thread-safe
 * FStrings. Rebuilt every time either changes; never held across a frame
 * boundary by anything but the panel.
 */
struct FSharedWorldEntryView
{
	FString WorldId;
	FString WorldName;
	/** AVAILABLE, STARTING, ONLINE, SAVING, STOPPING, MIGRATING, RECOVERABLE, NO_SAVE, UNREACHABLE, NOT_CREATED */
	FString CloudStatus;
	FString HostName;
	int32 PlayerCount = 0;
	int64 Revision = 0;
	int64 Generation = 0;
	FString Problem; // set when CloudStatus == UNREACHABLE

	/** Local session, mirrored from sw::SessionState. Empty/"IDLE" when nothing is happening locally. */
	FString LocalState;
	FString LocalMessage;
	bool bHasError = false;
	FString ErrorCode;
	FString ErrorMessage;
	FString ErrorDetail;
	bool bErrorRetryable = false;
	bool bJoinReady = false;

	bool IsLocalIdle() const { return LocalState.IsEmpty() || LocalState == TEXT("IDLE"); }
};
