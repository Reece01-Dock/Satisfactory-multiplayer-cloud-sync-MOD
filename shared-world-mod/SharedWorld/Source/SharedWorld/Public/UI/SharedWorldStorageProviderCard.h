#pragma once
// One storage provider in the Settings > Storage browser. Renders from FSharedWorldStorageProvider only,
// as a grid tile or a list row. Also hosts the "View All Providers" tile.

#include "Blueprint/UserWidget.h"
#include "UI/SharedWorldStorageModel.h"
#include "UI/SharedWorldWorldCard.h" // ESharedWorldCardLayout
#include "SharedWorldStorageProviderCard.generated.h"

class UBorder;
class USharedWorldBrowserWidget;

UCLASS()
class SHAREDWORLD_API USharedWorldStorageProviderCard : public UUserWidget
{
	GENERATED_BODY()

public:
	void Setup(const FSharedWorldStorageProvider& Provider, USharedWorldBrowserWidget* Owner, bool bSelected, ESharedWorldCardLayout InLayout);
	/** The trailing "View All Providers" tile. */
	void SetupViewAll(USharedWorldBrowserWidget* Owner, ESharedWorldCardLayout InLayout);

	/**
	 * Provider logo slot. Placeholder tile with a monogram today; replace the body with a brush lookup
	 * keyed by Provider.ProviderId and every card, the active card and the details panel update together.
	 */
	static UWidget* BuildIcon(UWidgetTree* Tree, const FSharedWorldStorageProvider& Provider, float Size);

protected:
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;

private:
	UFUNCTION() void OnSelect();
	UFUNCTION() void OnConnect();
	UFUNCTION() void OnViewAll();
	void UpdateChrome();

	UPROPERTY() TObjectPtr<UBorder> RootBorder;
	TWeakObjectPtr<USharedWorldBrowserWidget> OwnerBrowser;
	FString ProviderId;
	bool bIsSelected = false;
	bool bIsFocused = false;
};
