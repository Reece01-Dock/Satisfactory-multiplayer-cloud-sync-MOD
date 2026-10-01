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

UCLASS(Blueprintable)
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
	void ShowDetails();

	/** Called by save picker rows. */
	void OnSavePicked(const FString& SaveName, const FString& DisplayName);

	UFUNCTION() void Close();

	/** Keep binders alive for FrontEnd row clicks. */
	UPROPERTY() TArray<TObjectPtr<class USharedWorldRowBinder>> RowBinders;

private:
	enum class EPage : uint8 { Main, Details, CreateChoice, CreatePickSave, CreateName, JoinFriend, JoinCode, Advanced, Welcome, LinkGitHub };

	void RebuildMain();
	void RebuildDetails();
	void RebuildPage();
	void RebuildPageNow();
	void ScheduleRebuild();
	void CaptureListScroll();
	void RestoreListScroll(EPage ForPage);
	void TickScrollRestore();
	/** Refresh list/detail contents without destroying the ScrollBox (keeps scrollbar position). */
	bool TrySoftRefresh();
	bool SoftRefreshMain();
	bool SoftRefreshDetails();
	void PopulateMainList(UVerticalBox* List, FSharedWorldDiscoverySnapshot& Snap, FSharedWorldEntryView& OutSelected, bool& bOutHasSelected);
	void PopulateMainDetail(UVerticalBox* Detail, const FSharedWorldEntryView& SelectedView, bool bHasSelected);
	void PopulateDetailsBody(UVerticalBox* Body, const FSharedWorldEntryView& View);
	void RebuildLinkGitHubPage();
	void SetFlash(bool bOk, const FString& Message);
	void UpdateBottomPlay();
	void AddDetailStatRow(UVerticalBox* Col, const FText& Label, const FText& Value, const FLinearColor& ValueColor);
	void RequestDetailsMembers();
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
	UFUNCTION() void OnToggleTechnicalDetails();
	UFUNCTION() void OnManageSelected();
	UFUNCTION() void OnDetailsHistory();
	UFUNCTION() void OnDetailsRestore();

	UPROPERTY() TObjectPtr<UVerticalBox> PageRoot;
	UPROPERTY() TObjectPtr<UTextBlock> FlashText;
	UPROPERTY() TObjectPtr<UTextBlock> StatusText;
	UPROPERTY() TObjectPtr<UEditableTextBox> FilterInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> NameInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> CodeInput;
	UPROPERTY() TObjectPtr<UEditableTextBox> RestoreInput;
	UPROPERTY() TObjectPtr<UButton> PlayButton;
	UPROPERTY() TObjectPtr<UTextBlock> PlayLabel;
	UPROPERTY() TObjectPtr<UScrollBox> ListScroll;
	UPROPERTY() TObjectPtr<UVerticalBox> MainListBox;
	UPROPERTY() TObjectPtr<UVerticalBox> MainDetailBox;
	UPROPERTY() TObjectPtr<UVerticalBox> ScrollContentBox;
	/** FG Widget_SubMenuBackground shell hosting this browser (SML ModList pattern). */
	UPROPERTY() TObjectPtr<UUserWidget> SubMenuShell;
	UPROPERTY() TObjectPtr<UUserWidget> PlayStandardButton;
	UPROPERTY() TObjectPtr<UUserWidget> CreateStandardButton;

	EPage Page = EPage::Main;
	/** Which page currently owns ListScroll (survives until next rebuild clears it). */
	EPage ListScrollOwner = EPage::Main;
	float MainListScrollOffset = 0.f;
	float DetailsListScrollOffset = 0.f;
	float OtherListScrollOffset = 0.f;
	float PendingScrollRestoreOffset = 0.f;
	int32 ScrollRestoreGeneration = 0;
	int32 ScrollRestoreAttempts = 0;
	EPage PendingScrollRestorePage = EPage::Main;
	FString SelectedWorldId;
	FString SelectedSaveName;
	FString SuggestedWorldName;
	FString FilterText;
	FString PendingInviteId;
	FString DetailsMembersText;
	FString DetailsHistoryText;
	FDelegateHandle ChangedHandle;
	bool bBusy = false;
	bool bRebuildQueued = false;
	bool bPendingScrollRestore = false;
	bool bShowTechnicalDetails = false;
	bool bDetailsMembersLoading = false;
	bool bDetailsMembersLoaded = false;
};
