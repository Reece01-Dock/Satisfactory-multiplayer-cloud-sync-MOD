#pragma once
// Binds a FrontEnd list-row click to a Shared World id.

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SharedWorldRowBinder.generated.h"

class USharedWorldBrowserWidget;

UCLASS()
class USharedWorldRowBinder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY() TWeakObjectPtr<USharedWorldBrowserWidget> Browser;
	FString WorldId;
	bool bPlayOnClick = false;
	/** Remove from this PC's list (does not delete cloud data). */
	bool bRemoveFromList = false;
	/** Set for invitation buttons: 1 = accept, 2 = decline (WorldId unused). */
	FString InviteId;
	uint8 InviteAction = 0;
	/** TabKind 4: connect form choice (ConnectOption = ConnectValue; empty option toggles advanced settings).
	 *  TabKind 5: create wizard save storage (ConnectValue = rclone remote, empty = default).
	 *  TabKind 6: link a world's saves (ConnectValue = rclone remote, empty = unlink). */
	FString ConnectOption;
	FString ConnectValue;
	/** Tab buttons: TabKind 1 = join page, 2 = settings, 3 = world details. */
	int32 TabKind = 0;
	int32 TabIndex = 0;

	UFUNCTION()
	void OnClicked();
};
