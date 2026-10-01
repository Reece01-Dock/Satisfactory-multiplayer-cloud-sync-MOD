#pragma once
// Top-of-screen transparent toast for player errors / version notices.
// Replaces the centered Widget_ErrorMessage modal for Shared World messages.

#include "Blueprint/UserWidget.h"
#include "SharedWorldNoticeOverlay.generated.h"

class UTextBlock;
class UBorder;
class UButton;
class UCanvasPanel;
class USizeBox;

UCLASS()
class SHAREDWORLD_API USharedWorldNoticeOverlay : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void ShowNotice(const FText& Body, const FText& ButtonText, float AutoHideSeconds = 0.f);
	void HideNotice();

	/** Bound from ShowErrorMessage when a dismiss receiver is provided. */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnNoticeDismissed);
	UPROPERTY()
	FOnNoticeDismissed OnDismissed;

private:
	UFUNCTION()
	void HandleDismissClicked();

	UPROPERTY() TObjectPtr<UCanvasPanel> RootCanvas;
	UPROPERTY() TObjectPtr<USizeBox> BannerSize;
	UPROPERTY() TObjectPtr<UBorder> BannerBorder;
	UPROPERTY() TObjectPtr<UBorder> AccentBar;
	UPROPERTY() TObjectPtr<UTextBlock> BodyText;
	UPROPERTY() TObjectPtr<UButton> DismissButton;
	UPROPERTY() TObjectPtr<UTextBlock> DismissLabel;
	float AutoHideAt = 0.f;
};
