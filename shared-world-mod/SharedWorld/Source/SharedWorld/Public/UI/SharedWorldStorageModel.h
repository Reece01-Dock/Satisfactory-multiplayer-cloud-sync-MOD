#pragma once
// Display model for the Settings > Storage provider browser.
//
// Pure data. Today it is filled from (a) the real GitHub / local-folder state held by
// USharedWorldSubsystem and (b) display-only placeholder entries. When rclone discovery arrives it
// only has to produce more FSharedWorldStorageProvider values; the cards, grid, search and details
// panel read nothing but this struct.

#include "CoreMinimal.h"

class USharedWorldSubsystem;

enum class ESharedWorldStorageDifficulty : uint8
{
	Easy,     // green: sign in and go
	Advanced, // orange: needs keys / a server address. Describes setup effort only, not quality.
};

struct FSharedWorldStorageProvider
{
	FString ProviderId;
	FString DisplayName;
	FString Description;

	// ---- icon (swap for a brush/texture lookup later without touching the cards)
	/** Two/three letter tile text used until a real logo asset exists. Empty = first letter of the name. */
	FString IconMonogram;
	FLinearColor IconColor = FLinearColor(0.35f, 0.37f, 0.42f, 1.f);

	ESharedWorldStorageDifficulty Difficulty = ESharedWorldStorageDifficulty::Advanced;
	bool bRecommended = false;

	// ---- state
	/** Search metadata (UI-only until rclone supplies real provider info). */
	FString Category;
	TArray<FString> Tags;

	/** The mod can actually use this provider today (GitHub, local folder). Everything else is display-only ("coming soon"). */
	bool bAvailable = false;
	bool bConnected = false;
	/**
	 * This is the storage new Shared Worlds use. NOT the same as connected: later several providers can be connected
	 * at once with exactly one of them active. "Selected" is UI-only state held by the browser.
	 */
	bool bActive = false;
	/** Account/location/connected come from live backend state, not preview metadata. */
	bool bRuntimeBacked = false;

	// ---- identity (empty rows are hidden by the UI)
	FString AccountName;
	FString RepositoryName;
	FString Location;

	// ---- capabilities. Only meaningful when bHasCapabilityInfo; the UI labels them as provider metadata, not live checks.
	bool bHasCapabilityInfo = false;
	bool bSupportsRead = false;
	bool bSupportsWrite = false;
	bool bSupportsDelete = false;
	bool bSupportsMove = false;
	bool bSupportsList = false;
	bool bSupportsHash = false;
	bool bSupportsQuota = false;
	bool bSupportsAtomicWrites = false;
	bool bSupportsServerSideCopy = false;
	FString ProviderTier;

	// ---- rclone wiring (empty RcloneType = GitHub / local folder / not wired)
	FString RcloneType;   // rclone backend this card configures, e.g. "drive", "s3"
	FString RclonePreset; // pre-selected sub-provider, e.g. S3 "provider" = "Cloudflare"
	bool bOAuth = false;
	/** Shown only in the expanded "All providers" view (every rclone backend that has no curated card). */
	bool bViewAllOnly = false;
	int32 ConnectionCount = 0;
	bool IsRcloneBacked() const { return !RcloneType.IsEmpty(); }
	/** Atomic writes / server-side copy / quota are known for this provider (false = shown as Unknown). */
	bool bExtendedCapsKnown = true;
	/** Last successful read/write test of the connection (rclone providers). */
	FDateTime VerifiedUtc;
	bool bVerified = false;

	bool IsComingSoon() const { return !bAvailable; }
	/** "Connect" for sign-in style providers, "Configure" for ones needing keys or an address. */
	FText ConnectVerb() const;

	/** "Read, Write, Delete, List" style summary of the supported file operations. */
	FString FileOperationsText() const;
};

struct FSharedWorldStorageCatalog
{
	TArray<FSharedWorldStorageProvider> Providers;
	/** Provider the mod is using for new Shared Worlds right now. */
	FString ActiveProviderId;

	const FSharedWorldStorageProvider* Find(const FString& ProviderId) const;
	const FSharedWorldStorageProvider* Active() const { return Find(ActiveProviderId); }

	/** Case-insensitive match on name and description. Empty query matches everything. */
	static bool Matches(const FSharedWorldStorageProvider& Provider, const FString& Query);

	/** Live state from the subsystem + display-only entries. */
	static FSharedWorldStorageCatalog Build(USharedWorldSubsystem& SW);
};
