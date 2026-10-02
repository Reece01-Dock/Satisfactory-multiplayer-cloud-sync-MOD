#pragma once
// UButton that shows where keyboard / gamepad focus is. A plain SButton only restyles on hover, so a controller
// player has no visible cursor. This draws a rounded outline over the button while it (or its content) has focus.
// Every button made by the Shared Worlds design system uses it, so focus looks the same on every screen.

#include "Components/Button.h"
#include "SharedWorldButton.generated.h"

UCLASS()
class SHAREDWORLD_API USharedWorldButton : public UButton
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
};
