#pragma once
// Main-menu list entry: "Shared Worlds" — styled like Continue / Join Game.

#include "Blueprint/UserWidget.h"
#include "SharedWorldMainMenuButton.generated.h"

class UButton;
class UTextBlock;

UCLASS()
class SHAREDWORLD_API USharedWorldMainMenuButton : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;

private:
	UFUNCTION()
	void OnClicked();

	UPROPERTY() TObjectPtr<UButton> Button;
	UPROPERTY() TObjectPtr<UTextBlock> Label;
};
