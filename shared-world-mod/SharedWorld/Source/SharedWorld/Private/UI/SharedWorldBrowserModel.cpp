#include "UI/SharedWorldBrowserModel.h"

#include "UI/SharedWorldUiStyle.h"

#define LOCTEXT_NAMESPACE "SharedWorldBrowserModel"

namespace
{
	bool In(const FString& State, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* N : Names)
		{
			if (State == N) return true;
		}
		return false;
	}

	/** This PC is in the middle of becoming (or being) the host. */
	bool LocalIsHost(const FString& S)
	{
		return In(S, { TEXT("ACQUIRING"), TEXT("READY_TO_HOST"), TEXT("STARTING_SESSION"), TEXT("PUBLISHING_SESSION"), TEXT("HOSTING") });
	}

	bool ContainsNoCase(const FString& Haystack, const FString& Needle)
	{
		return Haystack.Contains(Needle, ESearchCase::IgnoreCase);
	}
}

const FSharedWorldBrowserItem* FSharedWorldBrowserSections::Find(const FString& WorldId) const
{
	for (const TArray<FSharedWorldBrowserItem>* Arr : { &FriendsPlaying, &YourWorlds, &SharedWithYou })
	{
		for (const FSharedWorldBrowserItem& It : *Arr)
		{
			if (It.Id() == WorldId) return &It;
		}
	}
	return nullptr;
}

const FSharedWorldBrowserItem* FSharedWorldBrowserSections::First() const
{
	// Display order matches the browser: Your worlds, Shared with you, Friends playing.
	if (YourWorlds.Num() > 0) return &YourWorlds[0];
	if (SharedWithYou.Num() > 0) return &SharedWithYou[0];
	if (FriendsPlaying.Num() > 0) return &FriendsPlaying[0];
	return nullptr;
}

namespace SharedWorldBrowserModel
{
	using namespace SharedWorldUi;

	FSharedWorldBrowserItem MakeItem(const FSharedWorldEntryView& V, const FString& MostRecentWorldId)
	{
		FSharedWorldBrowserItem It;
		It.View = V;
		It.bMostRecent = !MostRecentWorldId.IsEmpty() && V.WorldId == MostRecentWorldId;
		It.bCanInvite = V.bOwned;

		const FString& L = V.LocalState;
		const bool bHostedUp = V.IsHostingNow();
		const bool bLocalHost = LocalIsHost(L);
		const bool bHostedByOther = bHostedUp && !bLocalHost;

		// ---- status (first match wins; mirrors the backend's own precedence)
		if (V.bCreating)
		{
			It.Status = ESharedWorldStatus::Creating;
			It.StatusLabel = LOCTEXT("StCreating", "Setting up");
			It.StatusColor = Info;
		}
		else if (V.bHasError)
		{
			It.Status = ESharedWorldStatus::Error;
			It.StatusLabel = LOCTEXT("StError", "Needs attention");
			It.StatusColor = Err;
		}
		else if (L == TEXT("MIGRATING") || V.CloudStatus == TEXT("MIGRATING"))
		{
			It.Status = ESharedWorldStatus::Migrating;
			It.StatusLabel = LOCTEXT("StMigrating", "Migrating host");
			It.StatusColor = Warn;
		}
		else if (In(L, { TEXT("RECONNECTING"), TEXT("RECOVERING_HOST"), TEXT("ELECTING_HOST"), TEXT("RECOVERING"), TEXT("LEASE_LOST") }))
		{
			It.Status = ESharedWorldStatus::Recovering;
			It.StatusLabel = LOCTEXT("StRecovering", "Recovering");
			It.StatusColor = Warn;
		}
		else if (In(L, { TEXT("CHECKING"), TEXT("DOWNLOADING"), TEXT("UPLOADING"), TEXT("RESTORING"), TEXT("RELEASING") }))
		{
			It.Status = ESharedWorldStatus::Syncing;
			It.StatusLabel = LOCTEXT("StSyncing", "Syncing");
			It.StatusColor = Info;
		}
		else if (In(L, { TEXT("ACQUIRING"), TEXT("READY_TO_HOST"), TEXT("STARTING_SESSION"), TEXT("PUBLISHING_SESSION") }) || (V.CloudStatus == TEXT("STARTING") && !bHostedByOther))
		{
			It.Status = ESharedWorldStatus::Starting;
			It.StatusLabel = LOCTEXT("StStarting", "Starting");
			It.StatusColor = Warn;
		}
		else if (In(L, { TEXT("CHECKING_HOST"), TEXT("WAITING_FOR_HOST"), TEXT("WAITING_FOR_SESSION"), TEXT("HOST_VERIFIED"),
			TEXT("HOST_UNREACHABLE"), TEXT("JOIN_READY"), TEXT("JOINING"), TEXT("JOIN_RETRY") }))
		{
			It.Status = ESharedWorldStatus::Joining;
			It.StatusLabel = LOCTEXT("StJoining", "Joining");
			It.StatusColor = Warn;
		}
		else if (V.CloudStatus == TEXT("RECOVERABLE"))
		{
			// Cloud says the last host went silent. Nothing is running locally; Play takes over, so it stays enabled.
			It.Status = ESharedWorldStatus::NeedsRecovery;
			It.StatusLabel = LOCTEXT("StNeedsRecovery", "Needs recovery");
			It.StatusColor = Warn;
		}
		else if (bHostedUp || L == TEXT("JOINED"))
		{
			if (V.PlayerCount > 0)
			{
				It.Status = ESharedWorldStatus::Online;
				It.StatusLabel = FText::Format(LOCTEXT("StOnline", "{0} Online"), FText::AsNumber(V.PlayerCount));
			}
			else
			{
				It.Status = ESharedWorldStatus::Hosting;
				It.StatusLabel = LOCTEXT("StHosting", "Hosting");
			}
			It.StatusColor = Ok;
		}
		else if (V.CloudStatus == TEXT("UNREACHABLE"))
		{
			It.Status = ESharedWorldStatus::Unreachable;
			It.StatusLabel = LOCTEXT("StUnreachable", "Can't reach cloud");
			It.StatusColor = Err;
		}
		else
		{
			It.Status = ESharedWorldStatus::Offline;
			It.StatusLabel = LOCTEXT("StOffline", "Offline");
			It.StatusColor = TextMuted;
		}

		// ---- tone: the single colour+shape meaning shared by every screen
		switch (It.Status)
		{
		case ESharedWorldStatus::Online:
		case ESharedWorldStatus::Hosting: It.Tone = ESharedWorldTone::Healthy; break;
		case ESharedWorldStatus::Starting:
		case ESharedWorldStatus::Joining:
		case ESharedWorldStatus::Syncing:
		case ESharedWorldStatus::Creating: It.Tone = ESharedWorldTone::Working; break;
		case ESharedWorldStatus::Migrating:
		case ESharedWorldStatus::Recovering:
		case ESharedWorldStatus::NeedsRecovery: It.Tone = ESharedWorldTone::Warning; break;
		case ESharedWorldStatus::Unreachable:
		case ESharedWorldStatus::Error: It.Tone = ESharedWorldTone::Problem; break;
		case ESharedWorldStatus::Offline:
		default: It.Tone = ESharedWorldTone::Inactive; break;
		}
		It.StatusColor = ToneColor(It.Tone);

		// ---- section: one home per world
		It.Section = bHostedByOther ? ESharedWorldSection::FriendsPlaying
			: (V.bOwned ? ESharedWorldSection::YourWorlds : ESharedWorldSection::SharedWithYou);

		// ---- subtitle / detail line
		const bool bIdle = V.IsLocalIdle();
		if (bIdle && !bHostedUp)
		{
			It.Subtitle = V.LastPlayed.IsEmpty() ? TEXT("Nobody hosting") : FString::Printf(TEXT("Last played %s"), *V.LastPlayed);
		}
		else if (bIdle && bHostedUp && !V.HostName.IsEmpty())
		{
			It.Subtitle = FString::Printf(TEXT("%s is hosting"), *V.HostName);
		}
		else
		{
			It.Subtitle = V.FriendlyStatusLine();
		}
		It.DetailStatus = V.FriendlyStatusLine();
		if (It.Status == ESharedWorldStatus::NeedsRecovery)
		{
			const FString Who = V.LastHostName.IsEmpty() ? V.HostName : V.LastHostName;
			It.Subtitle = Who.IsEmpty() ? FString(TEXT("Host stopped responding")) : FString::Printf(TEXT("%s stopped responding"), *Who);
			It.DetailStatus = TEXT("The last host stopped responding. Press Play to recover this world.");
		}
		else if (It.Status == ESharedWorldStatus::Unreachable)
		{
			It.Subtitle = TEXT("Cloud status unavailable");
			It.DetailStatus = TEXT("Unable to refresh this Shared World. Your local copy is still available.");
		}
		else if (It.Status == ESharedWorldStatus::Error && !V.ErrorMessage.IsEmpty())
		{
			// Backend messages are already player-facing; first paragraph only for the row.
			FString First = V.ErrorMessage;
			int32 Cut = INDEX_NONE;
			if (First.FindChar(TEXT('\n'), Cut)) First.LeftInline(Cut);
			It.Subtitle = First;
			It.DetailStatus = First;
		}

		// ---- primary action
		It.bLocalBusy = !bIdle && It.Status != ESharedWorldStatus::Error;
		const bool bInTransition = It.Status == ESharedWorldStatus::Starting || It.Status == ESharedWorldStatus::Joining
			|| It.Status == ESharedWorldStatus::Syncing || It.Status == ESharedWorldStatus::Migrating
			|| It.Status == ESharedWorldStatus::Recovering || It.Status == ESharedWorldStatus::Creating;
		It.Action = bHostedByOther ? ESharedWorldAction::Join : ESharedWorldAction::Play;
		It.ActionLabel = It.Action == ESharedWorldAction::Join ? LOCTEXT("Join", "Join") : LOCTEXT("Play", "Play");
		// Play() is refused while creating and would restart a running session; Unreachable cannot acquire a lease.
		It.bActionEnabled = !bInTransition && It.Status != ESharedWorldStatus::Unreachable;
		// ForgetWorld refuses while a session is active; mirror that so we never offer a dead button.
		It.bCanRemove = !V.bCreating && (bIdle || It.Status == ESharedWorldStatus::Error);
		return It;
	}

	bool Matches(const FSharedWorldEntryView& V, const FString& Filter)
	{
		if (Filter.IsEmpty()) return true;
		if (ContainsNoCase(V.WorldName, Filter) || ContainsNoCase(V.OriginalSaveName, Filter)
			|| ContainsNoCase(V.HostName, Filter) || ContainsNoCase(V.LastHostName, Filter)
			|| ContainsNoCase(V.MapLabel, Filter))
		{
			return true;
		}
		for (const FString& P : V.OnlinePlayerNames)
		{
			if (ContainsNoCase(P, Filter)) return true;
		}
		return false;
	}

	FSharedWorldBrowserSections Build(const TArray<FSharedWorldEntryView>& Views, const FString& Filter, const FString& MostRecentWorldId)
	{
		FSharedWorldBrowserSections Out;
		Out.TotalBeforeFilter = Views.Num();
		const FString Needle = Filter.TrimStartAndEnd();
		for (const FSharedWorldEntryView& V : Views)
		{
			if (!Matches(V, Needle)) continue;
			FSharedWorldBrowserItem Item = MakeItem(V, MostRecentWorldId);
			switch (Item.Section)
			{
			case ESharedWorldSection::FriendsPlaying: Out.FriendsPlaying.Add(MoveTemp(Item)); break;
			case ESharedWorldSection::YourWorlds: Out.YourWorlds.Add(MoveTemp(Item)); break;
			case ESharedWorldSection::SharedWithYou: Out.SharedWithYou.Add(MoveTemp(Item)); break;
			}
		}
		return Out;
	}
}

#undef LOCTEXT_NAMESPACE
