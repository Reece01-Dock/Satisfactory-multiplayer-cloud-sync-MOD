#pragma once
#include "Blueprint/UserWidget.h"
#include "SharedWorldTypes.h"
#include "SharedWorldWorldCard.generated.h"

class UBorder;
class UButton;
class UTextBlock;
class USharedWorldBrowserWidget;

/** Load/Mods-style list row: orange when selected, name + subtitle, trailing chevron. */
UCLASS()
class SHAREDWORLD_API USharedWorldWorldCard : public UUserWidget
{
	GENERATED_BODY()

public:
	void Setup(const FSharedWorldEntryView& View, USharedWorldBrowserWidget* Owner, bool bSelected, bool bJoinLabel);
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION() void OnSelect();

	UPROPERTY() TObjectPtr<UBorder> RootBorder;
	UPROPERTY() TObjectPtr<UButton> SelectButton;
	UPROPERTY() TObjectPtr<UTextBlock> NameText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UTextBlock> ChevronText;
	TWeakObjectPtr<USharedWorldBrowserWidget> OwnerBrowser;
	FString WorldId;
};
