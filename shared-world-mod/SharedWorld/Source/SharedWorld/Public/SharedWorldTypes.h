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
	FString LastPlayed; // "3 hours ago", empty if unknown
	FString LastHostName;
	/** Original save name / map label from world.json when known. */
	FString OriginalSaveName;
	/** Currently connected session players (lease), display names. */
	TArray<FString> OnlinePlayerNames;
	int32 RequiredModCount = 0;
	int32 MaxPlayers = 4;
	FString MapName;
	FString MapLabel; // "Grass Fields"
	int32 PlayDurationSeconds = 0;
	FString PlaytimeText; // "87h 22m"
	FString GamePhase;
	/** -1 = unknown (not in session / never measured). */
	int32 PingMs = -1;
	bool bCreating = false; // being created / verified: not playable yet
	/** owned = Your Worlds, shared = Shared With You */
	bool bOwned = true;
	FString InviteCode;

	/** Local session, mirrored from sw::SessionState. Empty/"IDLE" when nothing is happening locally. */
	FString LocalState;
	FString LocalMessage;
	TArray<FString> Steps; // recent session notifications, oldest first
	bool bHasError = false;
	FString ErrorCode;
	FString ErrorMessage;
	FString ErrorDetail;
	bool bErrorRetryable = false;
	FString BackupPath;
	bool bJoinReady = false;

	bool IsLocalIdle() const { return LocalState.IsEmpty() || LocalState == TEXT("IDLE"); }
	bool IsHostingNow() const { return CloudStatus == TEXT("ONLINE") || CloudStatus == TEXT("SAVING") || CloudStatus == TEXT("STARTING"); }
	bool NeedsFriendsListFallback() const
	{
		return bJoinReady && LocalMessage.Contains(TEXT("friends list"));
	}

	/** Compact player-facing status line (no revision/hash). */
	FString FriendlyStatusLine() const
	{
		if (bCreating) return TEXT("Setting up...");
		if (LocalState == TEXT("CHECKING_HOST")) return TEXT("Checking host...");
		if (LocalState == TEXT("WAITING_FOR_SESSION") || LocalState == TEXT("WAITING_FOR_HOST"))
		{
			return HostName.IsEmpty() ? TEXT("Waiting for host to finish loading...") : FString::Printf(TEXT("Waiting for %s to finish loading..."), *HostName);
		}
		if (LocalState == TEXT("JOINING") || LocalState == TEXT("JOIN_READY") || LocalState == TEXT("HOST_VERIFIED"))
		{
			return HostName.IsEmpty() ? TEXT("Connecting...") : FString::Printf(TEXT("Connecting to %s..."), *HostName);
		}
		if (LocalState == TEXT("JOIN_RETRY") || LocalState == TEXT("HOST_UNREACHABLE"))
		{
			return HostName.IsEmpty() ? TEXT("Host unreachable. Retrying...") : FString::Printf(TEXT("Could not reach %s. Retrying..."), *HostName);
		}
		if (LocalState == TEXT("RECOVERING_HOST") || LocalState == TEXT("RECONNECTING"))
		{
			return TEXT("Host connection lost. Recovering Shared World...");
		}
		if (LocalState == TEXT("ELECTING_HOST") || LocalState == TEXT("ACQUIRING"))
		{
			return TEXT("Selecting a new host...");
		}
		if (LocalState == TEXT("UPLOADING")) return LocalMessage.IsEmpty() ? TEXT("Uploading Shared World...") : LocalMessage;
		if (LocalState == TEXT("MIGRATING") || CloudStatus == TEXT("MIGRATING"))
		{
			return HostName.IsEmpty() ? TEXT("Migrating host...") : FString::Printf(TEXT("Migrating host... %s starting world"), *HostName);
		}
		if (IsHostingNow() && !HostName.IsEmpty())
		{
			if (PlayerCount > 0) return FString::Printf(TEXT("%s hosting · %d players"), *HostName, PlayerCount);
			return FString::Printf(TEXT("%s hosting"), *HostName);
		}
		if (!LastPlayed.IsEmpty()) return FString::Printf(TEXT("Available · Last played %s"), *LastPlayed);
		return TEXT("Available · Nobody hosting");
	}
};
