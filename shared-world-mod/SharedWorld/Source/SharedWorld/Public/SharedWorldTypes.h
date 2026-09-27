#pragma once

#include "CoreMinimal.h"
#include "SharedWorldTypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSharedWorld, Log, All);

/**
 * Mirrors of the helper's IPC JSON (docs/ipc-protocol.md). Field names match
 * the JSON keys so FJsonObjectConverter can fill them. These are read-only
 * views: the helper owns the session state machine, the mod only renders it
 * and reports game events.
 */

USTRUCT()
struct FSharedWorldJoinInfo
{
	GENERATED_BODY()

	/** "online-session-id" or "address" */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Value;
	UPROPERTY() FString Backend;
};

USTRUCT()
struct FSharedWorldErrorInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** Player-facing message, already written for display. */
	UPROPERTY() FString Message;
	/** Technical detail for "View details". */
	UPROPERTY() FString Detail;
	UPROPERTY() bool LocalSaveUnchanged = true;
	UPROPERTY() FString BackupPath;
	UPROPERTY() int64 CloudRevision = 0;
	UPROPERTY() int64 LocalRevision = 0;
	UPROPERTY() bool Retryable = false;
};

USTRUCT()
struct FSharedWorldStep
{
	GENERATED_BODY()

	UPROPERTY() FString Message;
};

/** The helper's local session for one world (SessionView). */
USTRUCT()
struct FSharedWorldSession
{
	GENERATED_BODY()

	UPROPERTY() FString WorldId;
	/** IDLE, CHECKING, WAITING_FOR_HOST, JOIN_READY, ACQUIRING, RECOVERING, DOWNLOADING,
	 *  READY_TO_HOST, HOSTING, UPLOADING, RELEASING, LEASE_LOST, ERROR */
	UPROPERTY() FString State;
	/** "", "HOST" or "JOIN" */
	UPROPERTY() FString Decision;
	UPROPERTY() FString Message;
	UPROPERTY() TArray<FSharedWorldStep> Steps;
	UPROPERTY() FSharedWorldErrorInfo Error;
	UPROPERTY() FString HostName;
	UPROPERTY() FSharedWorldJoinInfo Join;
	UPROPERTY() FString SaveName;
	UPROPERTY() int64 Generation = 0;
	UPROPERTY() int64 Revision = 0;

	/** Set by the parser: whether "error" / "join" were present in the JSON. */
	bool bHasError = false;
	bool bHasJoin = false;

	bool IsIdle() const { return State.IsEmpty() || State == TEXT("IDLE"); }
};

USTRUCT()
struct FSharedWorldPlayer
{
	GENERATED_BODY()

	UPROPERTY() FString DisplayName;
	UPROPERTY() FString PlayerId;
};

/** Shared-world status for the main menu (WorldStatus). */
USTRUCT()
struct FSharedWorldStatus
{
	GENERATED_BODY()

	UPROPERTY() FString WorldId;
	UPROPERTY() FString WorldName;
	/** AVAILABLE, STARTING, ONLINE, SAVING, STOPPING, RECOVERABLE, NO_SAVE, UNREACHABLE */
	UPROPERTY() FString Status;
	UPROPERTY() FString StatusText;
	UPROPERTY() FString HostName;
	UPROPERTY() int32 PlayerCount = 0;
	UPROPERTY() TArray<FSharedWorldPlayer> Players;
	UPROPERTY() int64 Revision = 0;
	UPROPERTY() int64 Generation = 0;
	UPROPERTY() FString LastPlayedAt;
	UPROPERTY() FString LastHostName;
	UPROPERTY() FString Error;
	UPROPERTY() FSharedWorldSession Local;
};
