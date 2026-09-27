#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "SharedWorldTypes.h"
#include "SharedWorldPanel.generated.h"

class UButton;
class UCheckBox;
class UEditableTextBox;
class UTextBlock;
class UVerticalBox;
class USharedWorldSubsystem;
namespace sw { struct ProviderConfig; }

/**
 * One shared world in the main-menu list:
 *
 *   Our Factory
 *   Status: Online / Host: Reece / Players: 2 / Revision: 184
 *   [ Play Shared World ]   [Dismiss|Cancel|Retry]   [Details]
 *   (details: steps, error detail, [History] [Remove from list])
 *
 * Built entirely in C++ so the mod needs no cooked widget assets.
 */
UCLASS()
class SHAREDWORLD_API USharedWorldEntry : public UUserWidget
{
	GENERATED_BODY()

public:
	void Update(const FSharedWorldEntryView& View);

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
	UFUNCTION()
	void OnHistoryClicked();
	UFUNCTION()
	void OnForgetClicked();
	UFUNCTION()
	void OnRestoreClicked();

	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UTextBlock> InfoText;
	UPROPERTY() TObjectPtr<UTextBlock> ProgressText;
	UPROPERTY() TObjectPtr<UTextBlock> ErrorText;
	UPROPERTY() TObjectPtr<UTextBlock> DetailText;
	UPROPERTY() TObjectPtr<UTextBlock> HistoryText;
	UPROPERTY() TObjectPtr<UButton> PlayButton;
	UPROPERTY() TObjectPtr<UTextBlock> PlayLabel;
	UPROPERTY() TObjectPtr<UButton> SecondaryButton;
	UPROPERTY() TObjectPtr<UTextBlock> SecondaryLabel;
	UPROPERTY() TObjectPtr<UButton> DetailsButton;
	UPROPERTY() TObjectPtr<UWidget> DetailRow;
	UPROPERTY() TObjectPtr<UEditableTextBox> RestoreInput;

	FSharedWorldEntryView Current;
	bool bShowDetails = false;
	FString HistoryCache;
	/** What the secondary button does in the current state. */
	enum class ESecondary : uint8 { None, Dismiss, Cancel, Retry } Secondary = ESecondary::None;
};

/** Main-menu panel: the world list plus GitHub sign-in and world setup. */
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
	void SetResult(bool bOk, const FString& Message);
	USharedWorldSubsystem* GetSharedWorld() const;
	/** "owner/repo" -> GitHub, an absolute path -> folder. False with a message if neither. */
	bool ParseStorage(sw::ProviderConfig& Out, FString& OutError) const;

	UFUNCTION()
	void OnToggleSetup();
	UFUNCTION()
	void OnSignInClicked();
	UFUNCTION()
	void OnCreateClicked();
	UFUNCTION()
	void OnAddClicked();

	UPROPERTY() TObjectPtr<UVerticalBox> List;
	UPROPERTY() TObjectPtr<UTextBlock> ProblemText;
	UPROPERTY() TObjectPtr<UTextBlock> EmptyText;
	UPROPERTY() TArray<TObjectPtr<USharedWorldEntry>> Entries;

	UPROPERTY() TObjectPtr<UWidget> SetupBox;
	UPROPERTY() TObjectPtr<UTextBlock> AccountText;
	UPROPERTY() TObjectPtr<UTextBlock> SignInLabel;
	UPROPERTY() TObjectPtr<UEditableTextBox> StorageInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> SaveNameInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> WorldNameInput;
	UPROPERTY() TObjectPtr<UCheckBox> MembersOnlyCheck;
	UPROPERTY() TObjectPtr<UEditableTextBox> WorldIdInput;
	UPROPERTY() TObjectPtr<UTextBlock> ResultText;

	FDelegateHandle ChangedHandle;
	bool bBusy = false;
};
