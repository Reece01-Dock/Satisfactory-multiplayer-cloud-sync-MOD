#pragma once
#include "Blueprint/UserWidget.h"
#include "UI/SharedWorldBrowserModel.h"
#include "SharedWorldWorldCard.generated.h"

class UBorder;
class UButton;
class UWidget;
class UWidgetTree;
class UTexture2D;
class USharedWorldBrowserWidget;

enum class ESharedWorldCardLayout : uint8 { Row, Tile };

/**
 * One world in the browser. The same widget renders the list row and the grid tile
 * from the same FSharedWorldBrowserItem, so list and grid never diverge.
 * Three focus stops per card (select / primary / more) so it works with a pad.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldWorldCard : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Builds the widget tree; call once right after CreateWidget. */
	void Setup(const FSharedWorldBrowserItem& Item, USharedWorldBrowserWidget* Owner, bool bSelected,
		ESharedWorldCardLayout InLayout, UTexture2D* Thumbnail, bool bShowMore = true);

	/** Thumbnail image if we have one, otherwise a tinted tile with the world's initial. Sized by the caller's SizeBox. */
	static UWidget* BuildThumbnail(UWidgetTree* Tree, const FString& WorldId, const FString& Name, UTexture2D* Thumbnail, int32 LetterSize);

protected:
	virtual void NativeOnAddedToFocusPath(const FFocusEvent& InFocusEvent) override;
	virtual void NativeOnRemovedFromFocusPath(const FFocusEvent& InFocusEvent) override;

private:
	UFUNCTION() void OnSelect();
	UFUNCTION() void OnPrimary();
	UFUNCTION() void OnMore();
	void UpdateChrome();

	UPROPERTY() TObjectPtr<UBorder> RootBorder;
	TWeakObjectPtr<USharedWorldBrowserWidget> OwnerBrowser;
	FString WorldId;
	bool bIsSelected = false;
	bool bIsFocused = false;
};
