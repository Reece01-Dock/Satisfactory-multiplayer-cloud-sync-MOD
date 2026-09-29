#pragma once
// Simplified Shared Worlds browser: Create / Join Friend / world sections.
// Technical storage/Git settings live under Advanced only.

#include "Blueprint/UserWidget.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "SharedWorldBrowserWidget.generated.h"

class UButton;
class UEditableTextBox;
class UScrollBox;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class USharedWorldSubsystem;

UCLASS()
class SHAREDWORLD_API USharedWorldBrowserWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	void ActivateInMainMenu(UUserWidget* MainMenuRoot);
	void SelectWorld(const FString& WorldId);
	/** Removes a world from this PC's list (cloud copy is kept). */
	void RemoveWorldFromList(const FString& WorldId);
	void ShowMain();
	void ShowCreateWizard();
	void ShowJoinFriend();
	void ShowAdvanced();
	void ShowJoinCode();

	/** Called by save picker rows. */
	void OnSavePicked(const FString& SaveName, const FString& DisplayName);

	UFUNCTION() void Close();

	/** Keep binders alive for FrontEnd row clicks. */
	UPROPERTY() TArray<TObjectPtr<class USharedWorldRowBinder>> RowBinders;

private:
	enum class EPage : uint8 { Main, CreateChoice, CreatePickSave, CreateName, JoinFriend, JoinCode, Advanced, Welcome, LinkGitHub };

	void RebuildMain();
	void RebuildPage();
	void RebuildLinkGitHubPage();
	void SetFlash(bool bOk, const FString& Message);
	void UpdateBottomPlay();
	USharedWorldSubsystem* SW() const;
	UWidgetSwitcher* FindSwitcher(UUserWidget* MainMenuRoot) const;
	void AddSectionHeader(UVerticalBox* Col, const FText& Title);
	void AddWorldRows(UVerticalBox* Col, const TArray<FSharedWorldEntryView>& Worlds, bool bJoinLabel, bool bShowRemove = false);
	void AddInviteRows(UVerticalBox* Col, const TArray<FSharedWorldPendingInviteView>& Invites);

	UFUNCTION() void OnCreateClicked();
	UFUNCTION() void OnJoinFriendClicked();
	UFUNCTION() void OnSettingsClicked();
	UFUNCTION() void OnJoinCodeClicked();
	UFUNCTION() void OnRefresh();
	UFUNCTION() void OnPlaySelected();
	UFUNCTION() void OnDetailsSelected();
	UFUNCTION() void OnRemoveSelected();
	UFUNCTION() void OnBack();
	UFUNCTION() void OnUseExistingSave();
	UFUNCTION() void OnCreateNewGameHint();
	UFUNCTION() void OnCreateConfirm();
	UFUNCTION() void OnJoinCodeSubmit();
	UFUNCTION() void OnWelcomeContinue();
	UFUNCTION() void OnWelcomeConnect();
	UFUNCTION() void OnLinkGitHubClicked();
	UFUNCTION() void OnOpenGitHubVerify();
	UFUNCTION() void OnCopyGitHubCode();
	UFUNCTION() void OnCancelGitHubLink();
	UFUNCTION() void OnDisconnectGitHub();
	UFUNCTION() void OnTestGitHubAccess();
	UFUNCTION() void OnFilterChanged(const FText& Text);
	UFUNCTION() void OnAcceptInvite();
	UFUNCTION() void OnDeclineInvite();

	UPROPERTY() TObjectPtr<UVerticalBox> PageRoot;
	UPROPERTY() TObjectPtr<UTextBlock> FlashText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UEditableTextBox> FilterInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> NameInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> CodeInput;
	UPROPERTY() TObjectPtr<UButton> PlayButton;
	UPROPERTY() TObjectPtr<UTextBlock> PlayLabel;
	UPROPERTY() TObjectPtr<UScrollBox> ListScroll;
	/** FG Widget_SubMenuBackground shell hosting this browser (SML ModList pattern). */
	UPROPERTY() TObjectPtr<UUserWidget> SubMenuShell;
	UPROPERTY() TObjectPtr<UUserWidget> PlayStandardButton;
	UPROPERTY() TObjectPtr<UUserWidget> CreateStandardButton;

	EPage Page = EPage::Main;
	FString SelectedWorldId;
	FString SelectedSaveName;
	FString SuggestedWorldName;
	FString FilterText;
	FString PendingInviteId;
	FDelegateHandle ChangedHandle;
	bool bBusy = false;
};
