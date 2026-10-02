#pragma once
// Provider discovery and connection management on top of the rclone engine.
//
// Nothing here knows about a specific provider. rclone describes every backend it ships (config/providers: name, every
// setting with help text, required/password/advanced flags and example values), and builds the sign-in flow itself
// (config/create runs OAuth for the backends that need it). The UI renders from this schema, so a provider rclone adds
// later shows up without code changes.

#include "CoreMinimal.h"
#include "Rclone/RcloneRuntime.h"

struct FRcloneOptionExample
{
	FString Value;
	FString Help;
};

struct FRcloneOption
{
	FString Name;
	FString Help;
	FString Default;
	/** rclone type name: string, bool, int, SizeSuffix, Duration, CommaSepList... */
	FString Type;
	/** Comma list of sub-providers this setting applies to; a leading '!' means "all except". Empty = always. */
	FString ProviderFilter;
	bool bRequired = false;
	bool bPassword = false;
	bool bAdvanced = false;
	/** Hidden from interactive configuration by rclone (internal tokens and the like). */
	bool bHidden = false;
	/** Only the listed examples are valid values. */
	bool bExclusive = false;
	TArray<FRcloneOptionExample> Examples;

	bool IsBool() const { return Type == TEXT("bool"); }
	/** Does this setting matter for the selected value of the backend's "provider" setting (e.g. S3 flavour)? */
	bool AppliesTo(const FString& ProviderValue) const;
};

struct FRcloneBackend
{
	/** rclone type name, e.g. "drive", "s3", "sftp". */
	FString Name;
	FString Description;
	TArray<FRcloneOption> Options;
	/** Signs in through the browser (has client_id, client_secret, token and auth_url settings). */
	bool bOAuth = false;

	const FRcloneOption* FindOption(const FString& OptionName) const;
	/** Short, player-facing name ("Google Drive" rather than rclone's multi-line description). */
	FString DisplayName() const;
};

class SHAREDWORLD_API FRcloneProviders
{
public:
	/** Cached result of the last successful Load(), or null. Cheap; safe from any thread. */
	static TSharedPtr<const TArray<FRcloneBackend>> Get();

	/** Parses rclone's provider schema (large JSON). Blocking: call from a background thread. Cached after success. */
	static bool Load(FString& OutError);

	/** Backends that make sense as save storage (excludes aliases, wrappers, read-only and in-memory backends). */
	static bool IsStorageBackend(const FString& BackendName);
};

/** A remote the player set up through the mod (stored in the mod's own rclone.conf; this file holds the non-secret metadata). */
struct FRcloneConnection
{
	FString RemoteName;  // rclone remote name, e.g. "sw-drive-1"
	FString CatalogId;   // provider card it was created from, e.g. "google-drive"
	FString BackendType; // rclone type, e.g. "drive"
	FString Label;       // shown to the player
	FString Folder;      // folder inside the remote that holds Shared World data
	FDateTime CreatedUtc;
	/** A write / read / delete probe succeeded when the connection was made (or last tested). */
	bool bVerified = false;
	FDateTime VerifiedUtc;

	/** rclone path of the folder: "sw-drive-1:SharedWorlds". */
	FString Fs() const;
};

class SHAREDWORLD_API FRcloneConnections
{
public:
	static TArray<FRcloneConnection> Load();
	static bool Save(const TArray<FRcloneConnection>& Connections);
	/** Unused remote name for a new connection of this backend type. */
	static FString MakeRemoteName(const FString& BackendType, const TArray<FRcloneConnection>& Existing);
};

struct FRcloneAbout
{
	bool bOk = false;
	int64 Free = -1;
	int64 Total = -1;
	int64 Used = -1;
};

namespace RcloneActions
{
	/**
	 * config/create. Passwords are obscured by rclone. For OAuth backends this opens the player's browser and BLOCKS until
	 * they approve (or the sign-in times out): background thread only.
	 */
	SHAREDWORLD_API FRcloneResult CreateRemote(const FString& RemoteName, const FString& BackendType, const TMap<FString, FString>& Parameters);
	SHAREDWORLD_API FRcloneResult DeleteRemote(const FString& RemoteName);
	/** Free/used/total space when the provider reports it (real values only). */
	SHAREDWORLD_API FRcloneAbout About(const FString& Fs);
	/** Writes, reads back and deletes a small object under Fs: proves the connection can really store saves. */
	SHAREDWORLD_API bool ProbeStorage(const FString& Fs, FString& OutError);
}
