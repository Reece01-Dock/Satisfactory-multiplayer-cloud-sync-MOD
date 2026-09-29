#pragma once
// Pause / Manage Session list entry that opens USharedWorldSessionWidget.

#include "Blueprint/UserWidget.h"
#include "SharedWorldSessionMenuButton.generated.h"

class UButton;
class UTextBlock;

UCLASS()
class SHAREDWORLD_API USharedWorldSessionMenuButton : public UUserWidget
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
