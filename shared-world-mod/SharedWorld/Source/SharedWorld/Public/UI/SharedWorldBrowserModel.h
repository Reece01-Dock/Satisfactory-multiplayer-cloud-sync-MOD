#pragma once
// View model for the Shared World browser. Pure data: no widgets, no network.
// Both the list rows and the grid tiles consume FSharedWorldBrowserItem, and the
// details panel reads the same item, so there is exactly one place that decides
// "what state is this world in and what does the main button do".

#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldUiStyle.h" // ESharedWorldTone

enum class ESharedWorldSection : uint8
{
	FriendsPlaying, // somebody else is hosting right now
	YourWorlds,
	SharedWithYou,
};

enum class ESharedWorldStatus : uint8
{
	Offline,     // nobody hosting
	Online,      // a host is up, players connected
	Hosting,     // host is up (or this PC is hosting), nobody else yet
	Starting,    // lease acquired / save installed / world loading
	Joining,     // connecting to a host
	Syncing,     // downloading / uploading / checking the cloud save
	Migrating,   // host handover in progress
	Recovering,  // electing a new host / recovering progress (local session is busy doing it)
	NeedsRecovery, // previous host stopped responding; Play performs the recovery
	Creating,    // world is still being created
	Unreachable, // cloud metadata could not be read
	Error,       // local session failed; backend message is player-facing
};

enum class ESharedWorldAction : uint8
{
	Play, // nobody hosting: acquire the lease and host (backend decides)
	Join, // somebody is hosting: join them
};

struct FSharedWorldBrowserItem
{
	FSharedWorldEntryView View;
	ESharedWorldSection Section = ESharedWorldSection::YourWorlds;
	ESharedWorldStatus Status = ESharedWorldStatus::Offline;
	FText StatusLabel;
	ESharedWorldTone Tone = ESharedWorldTone::Inactive;
	FLinearColor StatusColor = FLinearColor::White; // == ToneColor(Tone)
	/** One short secondary line for rows/tiles. */
	FString Subtitle;
	/** Status sentence for the details panel ("Available · Nobody hosting"). */
	FString DetailStatus;
	ESharedWorldAction Action = ESharedWorldAction::Play;
	FText ActionLabel;
	bool bActionEnabled = true;
	/** True while the local session for this world is doing something (blocks removal). */
	bool bLocalBusy = false;
	bool bMostRecent = false;
	/** Offered only when ForgetWorld would accept it. */
	bool bCanRemove = true;
	/** Owner/admin only in the backend; we only know "owned" locally. */
	bool bCanInvite = false;

	const FString& Id() const { return View.WorldId; }
	FString DisplayName() const { return View.WorldName.IsEmpty() ? View.WorldId : View.WorldName; }
};

struct FSharedWorldBrowserSections
{
	TArray<FSharedWorldBrowserItem> FriendsPlaying;
	TArray<FSharedWorldBrowserItem> YourWorlds;
	TArray<FSharedWorldBrowserItem> SharedWithYou;
	/** Worlds known before the search filter was applied. */
	int32 TotalBeforeFilter = 0;

	int32 Num() const { return FriendsPlaying.Num() + YourWorlds.Num() + SharedWithYou.Num(); }
	const FSharedWorldBrowserItem* Find(const FString& WorldId) const;
	/** First visible item in display order, or null. */
	const FSharedWorldBrowserItem* First() const;
};

namespace SharedWorldBrowserModel
{
	FSharedWorldBrowserItem MakeItem(const FSharedWorldEntryView& View, const FString& MostRecentWorldId);

	/**
	 * Groups worlds. A world appears once: hosted-by-someone worlds go to FriendsPlaying,
	 * everything else to Your/Shared by ownership. Filter matches name, save name,
	 * host and online player names (case-insensitive), as the user types.
	 */
	FSharedWorldBrowserSections Build(const TArray<FSharedWorldEntryView>& Views, const FString& Filter, const FString& MostRecentWorldId);

	bool Matches(const FSharedWorldEntryView& View, const FString& Filter);
}
