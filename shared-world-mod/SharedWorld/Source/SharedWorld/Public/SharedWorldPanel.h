#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "SharedWorldPanel.generated.h"

class UButton;
class UTextBlock;
class UVerticalBox;
class USharedWorldSubsystem;

/**
 * One shared world in the main-menu list:
 *
 *   Our Factory
 *   Status: Online / Host: Reece / Players: 2 / Revision: 184
 *   [ Play Shared World ]   [Dismiss|Cancel|Retry]   [Details]
 *
 * Built entirely in C++ so the mod needs no cooked widget assets.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldEntry : public UUserWidget
{
	GENERATED_BODY()

public:
	void Update(const FSharedWorldStatus& Status);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void Build();
	USharedWorldSubsystem* GetSharedWorld() const;

	UFUNCTION()
	void OnPlayClicked();
	UFUNCTION()
	void OnSecondaryClicked();
	UFUNCTION()
	void OnDetailsClicked();

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UTextBlock> InfoText;
	UPROPERTY() TObjectPtr<UTextBlock> ProgressText;
	UPROPERTY() TObjectPtr<UTextBlock> ErrorText;
	UPROPERTY() TObjectPtr<UTextBlock> DetailText;
	UPROPERTY() TObjectPtr<UButton> PlayButton;
	UPROPERTY() TObjectPtr<UTextBlock> PlayLabel;
	UPROPERTY() TObjectPtr<UButton> SecondaryButton;
	UPROPERTY() TObjectPtr<UTextBlock> SecondaryLabel;
	UPROPERTY() TObjectPtr<UButton> DetailsButton;

	FSharedWorldStatus Current;
	bool bShowDetails = false;
	/** What the secondary button does in the current state. */
	enum class ESecondary : uint8 { None, Dismiss, Cancel, Retry } Secondary = ESecondary::None;
};

/** Main-menu panel listing all configured shared worlds. */
UCLASS()
class SHAREDWORLD_API USharedWorldPanel : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	void Refresh();
	USharedWorldSubsystem* GetSharedWorld() const;

	UPROPERTY() TObjectPtr<UVerticalBox> List;
	UPROPERTY() TObjectPtr<UTextBlock> ConnectionText;
	UPROPERTY() TArray<TObjectPtr<USharedWorldEntry>> Entries;

	FDelegateHandle ChangedHandle;
};
