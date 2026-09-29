#pragma once
// Per-world details: status, host, revision, PLAY / history / players.
// All values come from SharedWorldCore via USharedWorldSubsystem.

#include "Blueprint/UserWidget.h"
#include "SharedWorldTypes.h"
#include "SharedWorldDetailsWidget.generated.h"

class UButton;
class UEditableTextBox;
class UTextBlock;
class USharedWorldSubsystem;

UCLASS()
class SHAREDWORLD_API USharedWorldDetailsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	void OpenForWorld(const FString& InWorldId);

	UFUNCTION()
	void Close();

private:
	void Refresh();
	USharedWorldSubsystem* SW() const;

	UFUNCTION() void OnPlay();
	UFUNCTION() void OnPlayers();
	UFUNCTION() void OnHistory();
	UFUNCTION() void OnRestore();
	UFUNCTION() void OnBack();
	UFUNCTION() void OnForget();

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> BodyText;
	UPROPERTY() TObjectPtr<UTextBlock> SectionText;
	UPROPERTY() TObjectPtr<UTextBlock> FlashText;
	UPROPERTY() TObjectPtr<UEditableTextBox> RestoreInput;
	UPROPERTY() TObjectPtr<UButton> PlayButton;

	FString WorldId;
	FDelegateHandle ChangedHandle;
	bool bBusy = false;
};
