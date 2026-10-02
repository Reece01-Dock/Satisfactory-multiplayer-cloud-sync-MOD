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
	/** Connect form choice buttons (TabKind 4): sets ConnectOption = ConnectValue; empty option toggles advanced settings. */
	FString ConnectOption;
	FString ConnectValue;
	/** Tab buttons: TabKind 1 = join page, 2 = settings, 3 = world details. */
	int32 TabKind = 0;
	int32 TabIndex = 0;

	UFUNCTION()
	void OnClicked();
};
