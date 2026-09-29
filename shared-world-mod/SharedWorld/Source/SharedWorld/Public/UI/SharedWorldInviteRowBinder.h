#pragma once
// Binds an Invite / Add click in the in-game Shared World Players tab.

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SharedWorldInviteRowBinder.generated.h"

class USharedWorldSessionWidget;

UCLASS()
class USharedWorldInviteRowBinder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY() TWeakObjectPtr<USharedWorldSessionWidget> Session;
	FString WorldId;
	FString PlayerId;
	FString DisplayName;

	UFUNCTION()
	void OnInviteClicked();
};
