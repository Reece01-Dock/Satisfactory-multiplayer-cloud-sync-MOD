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

	UFUNCTION()
	void OnClicked();
};
