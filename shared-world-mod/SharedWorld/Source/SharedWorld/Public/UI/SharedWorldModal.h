#pragma once
// The one Shared Worlds dialog: confirmations (destructive actions) and error/notice dialogs.
// icon + short title + sentence + optional Details expander + Cancel / action.
// Controller/keyboard: focus is trapped on its buttons; Escape / gamepad Back cancels.

#include "Blueprint/UserWidget.h"
#include "UI/SharedWorldUiStyle.h"
#include "SharedWorldModal.generated.h"

class UButton;
class UTextBlock;
class UVerticalBox;

struct FSharedWorldModalSpec
{
	ESharedWorldTone Tone = ESharedWorldTone::Warning;
	FText Title;
	FText Body;
	/** Technical text shown only after the player expands "Details". Empty = no expander. */
	FText Detail;
	FText ConfirmLabel;
	/** Empty = single-button dialog (ConfirmLabel only). */
	FText CancelLabel;
	ESharedWorldButtonRole ConfirmRole = ESharedWorldButtonRole::Config;
	TFunction<void()> OnConfirm;
	TFunction<void()> OnCancel;
};

UCLASS()
class SHAREDWORLD_API USharedWorldModal : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Builds and shows the dialog above everything else. Returns null if no player controller. */
	static USharedWorldModal* Show(APlayerController* PC, const FSharedWorldModalSpec& InSpec);
	void Close();

protected:
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	void Build();
	UFUNCTION() void OnConfirmClicked();
	UFUNCTION() void OnCancelClicked();
	UFUNCTION() void OnToggleDetails();

	FSharedWorldModalSpec Spec;
	UPROPERTY() TObjectPtr<UButton> ConfirmButton;
	UPROPERTY() TObjectPtr<UButton> CancelButton;
	UPROPERTY() TObjectPtr<UTextBlock> DetailText;
	bool bClosing = false;
	/** Set when the dialog had to turn on the cursor / UI input (in-game); undone on close. */
	TWeakObjectPtr<APlayerController> InputOwner;
	bool bRestoreGameInput = false;
};
