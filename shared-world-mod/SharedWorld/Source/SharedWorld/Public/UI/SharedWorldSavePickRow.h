#pragma once
#include "Blueprint/UserWidget.h"
#include "SharedWorldSavePickRow.generated.h"

class UButton;
class UTextBlock;
class USharedWorldBrowserWidget;

UCLASS()
class SHAREDWORLD_API USharedWorldSavePickRow : public UUserWidget
{
	GENERATED_BODY()
public:
	void Setup(const FString& InSaveName, const FString& InDisplayName, const FString& InLastPlayed, USharedWorldBrowserWidget* Owner);
	virtual TSharedRef<SWidget> RebuildWidget() override;
private:
	UFUNCTION() void OnClicked();
	UPROPERTY() TObjectPtr<UButton> Button;
	UPROPERTY() TObjectPtr<UTextBlock> NameText;
	UPROPERTY() TObjectPtr<UTextBlock> MetaText;
	TWeakObjectPtr<USharedWorldBrowserWidget> OwnerBrowser;
	FString SaveName;
	FString DisplayName;
};
