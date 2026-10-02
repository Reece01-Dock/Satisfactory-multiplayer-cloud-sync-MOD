#pragma once

#include "CoreMinimal.h"

/** Where a new world keeps its save files. Empty Remote = the repository's own store (GitHub releases / folder). */
struct FSharedWorldSaveTarget
{
	/** rclone path (remote:folder) on this PC; the world's files go in its <worldId> subfolder. */
	FString Remote;
	FString Backend; // rclone type, e.g. "dropbox"
	FString Label;   // e.g. "Dropbox"
	bool IsDefault() const { return Remote.IsEmpty(); }
};
