#pragma once
// A save in the Create Shared World wizard: same card treatment as a world row.
#include "Blueprint/UserWidget.h"
#include "SharedWorldSavePickRow.generated.h"

class UBorder;
class USharedWorldBrowserWidget;

UCLASS()
class SHAREDWORLD_API USharedWorldSavePickRow : public UUserWidget
{
	GENERATED_BODY()
public:
	/** Builds the card. Subtitle is the pre-composed "Session · Last played X · 245 MB" line. */
	void Setup(const FString& InSaveName, const FString& InDisplayName, const FString& InSubtitle, USharedWorldBrowserWidget* Owner, bool bSelected);

protected:
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;

private:
	UFUNCTION() void OnClicked();
	void UpdateChrome();
	UPROPERTY() TObjectPtr<UBorder> RootBorder;
	TWeakObjectPtr<USharedWorldBrowserWidget> OwnerBrowser;
	FString SaveName;
	FString DisplayName;
	bool bIsSelected = false;
	bool bIsFocused = false;
};
