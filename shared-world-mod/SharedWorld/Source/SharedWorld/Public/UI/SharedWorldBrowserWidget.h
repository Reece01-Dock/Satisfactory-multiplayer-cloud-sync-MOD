#pragma once
// Shared Worlds browser and its sub-pages (create wizard, join, details, settings, storage link).
// Layout/behaviour per page lives in SharedWorldBrowserWidget.cpp (main browser) and
// SharedWorldBrowserPages.cpp / SharedWorldBrowserDetails.cpp (sub-pages). All visuals come from
// SharedWorldUiStyle.h; see docs/shared-worlds-ui-system.md.

#include "Blueprint/UserWidget.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "UI/SharedWorldBrowserModel.h"
#include "UI/SharedWorldStorageModel.h"
#include "SharedWorldBrowserWidget.generated.h"

class UTexture2D;
class UButton;
class UEditableTextBox;
class UScrollBox;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class UWidget;
class USizeBox;
class USharedWorldSubsystem;

UCLASS(Blueprintable)
class SHAREDWORLD_API USharedWorldBrowserWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	/** Escape / gamepad Back: close "..." menu, then step back out of a sub-page. */
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	void ActivateInMainMenu(UUserWidget* MainMenuRoot);
	void SelectWorld(const FString& WorldId);
	/** Primary card/panel button: Join if somebody hosts it, otherwise Play (the backend decides host vs join). */
	void PlayWorld(const FString& WorldId);
	/** Accept/decline one specific pending invitation (called from per-row binders). */
	void HandleInvite(const FString& InviteId, bool bAccept);
	/** Tab buttons: Kind 1 = Join page, 2 = Settings, 3 = World details. */
	void SetTab(int32 Kind, int32 Index);
	/** Selects the world and opens/closes its "..." action list. */
	void ToggleMoreMenu(const FString& WorldId);
	/** Removes a world from this PC's list (cloud copy is kept). */
	void RemoveWorldFromList(const FString& WorldId);
	void ShowMain();
	void ShowCreateWizard();
	void ShowJoinFriend();
	void ShowAdvanced(); // opens Settings
	void ShowJoinCode();
	void ShowDetails();

	// ---- Settings > Storage (called by provider cards)
	void SelectProvider(const FString& ProviderId);
	/** "Connect" on a card: GitHub starts the real link flow; display-only providers show a development-safe message. */
	void ConnectProvider(const FString& ProviderId);
	void ViewAllProviders();
	/** Connect form: picks a value for a choice / yes-no setting, or toggles the advanced settings (Name empty). */
	void SetConnectOption(const FString& Name, const FString& Value);
	/** Create wizard: where the new world's saves go (rclone remote name; empty = the default storage). */
	void SetCreateSaveTarget(const FString& RemoteName);
	/** World Details > Storage: link this PC to the world's save storage (rclone remote name + folder; empty = unlink). */
	void LinkWorldSaves(const FString& RemoteName);

	/** Called by save picker rows. */
	void OnSavePicked(const FString& SaveName, const FString& DisplayName);

	UFUNCTION() void Close();

	/** Keep binders alive for button clicks. */
	UPROPERTY() TArray<TObjectPtr<class USharedWorldRowBinder>> RowBinders;

private:
	enum class EPage : uint8 { Main, Details, CreatePickSave, CreateName, CreateReview, Creating, JoinFriend, Settings, Welcome, LinkGitHub, ConnectProvider };

	// ---- core / main browser
	void RebuildMain();
	void RebuildPage();
	/** Pages that hold no typed input only rebuild when what they show actually changed (no scroll jumps on cloud refresh). */
	FString ComputePageSignature() const;
	/** Card columns that fit the space the grid really gets. Fixed columns keep layout width bounded (a wrapping box would report an unbounded width and push the menu off-screen). */
	int32 GridColumns(float MinCardWidth, float ColumnFraction, float Chrome, int32 MaxCols);
	void RebuildPageNow();
	void ScheduleRebuild();
	void OnBackendChanged();
	void CaptureListScroll();
	void RestoreListScroll(EPage ForPage);
	void TickScrollRestore();
	/** Refresh list/detail contents without destroying the ScrollBox (keeps scrollbar position). */
	bool TrySoftRefresh();
	bool SoftRefreshMain();
	bool SoftRefreshDetails();
	void PopulateMainList(UVerticalBox* List, FSharedWorldDiscoverySnapshot& Snap, FSharedWorldBrowserItem& OutSelected, bool& bOutHasSelected);
	void PopulateMainDetail(UVerticalBox* Detail, const FSharedWorldBrowserItem& SelectedItem, bool bHasSelected);
	void AddSectionTitle(UVerticalBox* Col, const FText& Title);
	void AddMutedLine(UVerticalBox* Col, const FText& Text);
	void AddWorldItems(UVerticalBox* Col, const TArray<FSharedWorldBrowserItem>& Items, bool bAllowMore = true);
	void AddContextActions(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);
	UButton* AddActionRow(UVerticalBox* Col, const FText& Label, ESharedWorldButtonRole Role, int32 Height = 40);
	UTexture2D* GetThumbnail(const FString& WorldId);
	void SetViewMode(bool bGrid);
	void SetFlash(bool bOk, const FString& Message);
	void UpdateBottomPlay();
	void AddDetailStatRow(UVerticalBox* Col, const FText& Label, const FText& Value, const FLinearColor& ValueColor);
	void RequestDetailsMembers();
	USharedWorldSubsystem* SW() const;
	UWidgetSwitcher* FindSwitcher(UUserWidget* MainMenuRoot) const;
	void AddInviteRows(UVerticalBox* Col, const TArray<FSharedWorldPendingInviteView>& Invites);
	/** Navigation: true if something was closed/stepped back. */
	bool HandleBack();

	// ---- sub-pages (SharedWorldBrowserPages.cpp)
	void AddPageHeader(const FText& Title, const FText& Subtitle, bool bShowBack = true, bool bShowGlobe = false);
	UButton* AddTabs(UVerticalBox* Col, int32 Kind, const TArray<FText>& Labels, int32 Active);
	void RebuildWelcomePage();
	void RebuildCreatePickSavePage();
	void RebuildCreateNamePage();
	void RebuildCreateReviewPage();
	void RebuildCreatingPage();
	void RebuildJoinPage();
	void RebuildSettingsPage();
	void RebuildLinkGitHubPage();
	// Connect an rclone-backed provider (SharedWorldBrowserConnect.cpp)
	void RebuildConnectPage();
	void BeginConnect(const FString& ProviderId);
	void HarvestConnectFields();
	void RunConnect();
	void AddJoinInvites(UVerticalBox* Col, const FSharedWorldDiscoverySnapshot& Snap);
	void AddJoinFriendWorlds(UVerticalBox* Col, const FSharedWorldDiscoverySnapshot& Snap);
	void AddJoinCodeForm(UVerticalBox* Col);
	// Settings > Storage (SharedWorldBrowserStorage.cpp)
	void AddStoragePage(UVerticalBox* Col);
	void AddActiveStorageCard(UVerticalBox* Col, const FSharedWorldStorageCatalog& Catalog);
	void AddProviderBrowser(UVerticalBox* Col);
	void PopulateProviderGrid(UVerticalBox* Host);
	void PopulateProviderDetails(UVerticalBox* Host);
	void QueueStorageRefresh();
	void RefreshStorageContent();
	void AddKeyValueRow(UVerticalBox* Col, const FText& Label, const FString& Value, const FLinearColor& ValueColor, bool bWrap = false, const FText& Tip = FText::GetEmpty());
	void AddSettingsSync(UVerticalBox* Col);
	void AddSettingsDiagnostics(UVerticalBox* Col);
	void AddRcloneEngineCard(UVerticalBox* Col);
	UEditableTextBox* AddTextField(UVerticalBox* Col, const FText& Hint, const FString& Initial);

	// ---- world details (SharedWorldBrowserDetails.cpp)
	void RebuildDetails();
	void PopulateDetailsBody(UVerticalBox* Body, const FSharedWorldEntryView& View);
	void AddDetailsOverview(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);
	void AddDetailsPlayers(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);
	void AddDetailsSync(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);
	void AddDetailsStorage(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);
	void AddDetailsAdvanced(UVerticalBox* Col, const FSharedWorldBrowserItem& Item);

	UFUNCTION() void OnCreateClicked();
	UFUNCTION() void OnJoinFriendClicked();
	UFUNCTION() void OnSettingsClicked();
	UFUNCTION() void OnJoinCodeClicked();
	UFUNCTION() void OnRefresh();
	UFUNCTION() void OnPlaySelected();
	UFUNCTION() void OnDetailsSelected();
	UFUNCTION() void OnRemoveSelected();
	UFUNCTION() void OnBack();
	UFUNCTION() void OnCreateNext();
	UFUNCTION() void OnCreateConfirm();
	UFUNCTION() void OnNameChanged(const FText& Text);
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
	UFUNCTION() void OnDetailsHistory();
	UFUNCTION() void OnDetailsRestore();
	UFUNCTION() void OnViewList();
	UFUNCTION() void OnViewGrid();
	UFUNCTION() void OnMoreInvite();
	UFUNCTION() void OnMoreOpenFolder();
	UFUNCTION() void OnDismissError();
	UFUNCTION() void OnCopyDiagnostics();
	UFUNCTION() void OnRcloneSelfTest();
	UFUNCTION() void OnStorageManage();
	UFUNCTION() void OnStorageChangeProvider();
	UFUNCTION() void OnStorageSearchChanged(const FText& Text);
	UFUNCTION() void OnStorageViewList();
	UFUNCTION() void OnStorageViewGrid();
	UFUNCTION() void OnStorageLearnMore();
	UFUNCTION() void OnStorageReconfigure();
	UFUNCTION() void OnStorageConnectSelected();
	UFUNCTION() void OnConnectSubmit();
	UFUNCTION() void OnConnectLearnMore();
	UFUNCTION() void OnStorageRcloneTest();
	UFUNCTION() void OnStorageRcloneDisconnect();
	/** Makes the selected rclone connection the default home for new worlds' saves (or stops). */
	UFUNCTION() void OnStorageUseForSaves();
	/** Moves the listed worlds' saves one after another, reporting progress in the storage notice. */
	void MoveWorldsSequentially(TArray<FString> WorldIds, FString RemoteName, int32 Index, int32 Failed);
	/** Asks whether to move the player's own worlds that are not on Conn yet (no-op when there are none). */
	void OfferMoveWorlds(const struct FRcloneConnection& Conn);
	TArray<FString> WorldsToMove(const struct FRcloneConnection& Conn) const;
	UFUNCTION() void OnStorageMoveWorldsHere();
	/** World Details > Storage: move the selected world to the active provider. */
	UFUNCTION() void OnDetailsMoveToActive();
	FString LastMoveError;
	/** Layout hook for later: a connected provider that is not the active one. Switching is not implemented yet. */
	UFUNCTION() void OnStorageSetActive();

	UPROPERTY() TObjectPtr<UVerticalBox> PageRoot;
	/** Caps the page's desired width to the screen so one long line can never widen the whole menu. */
	UPROPERTY() TObjectPtr<USizeBox> WidthClamp;
	void UpdateWidthClamp();
	/** Container sub-pages build into: PageRoot for the main browser, the shared page panel otherwise. */
	UPROPERTY() TObjectPtr<UVerticalBox> PageBody;
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

	/** Loaded sidecar screenshots by world id (null = looked, none found). */
	UPROPERTY() TMap<FString, TObjectPtr<UTexture2D>> ThumbCache;
	bool bGridView = false;
	/** World whose "..." list is open (only one at a time). */
	FString MoreMenuWorldId;

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
	/** What the player has typed in the create wizard; survives page rebuilds. */
	FString PendingWorldName;
	/** Create wizard: chosen save storage (rclone remote name), empty = the default. */
	FString PendingSaveConnection;
	/** World Details > Storage: folder typed for linking a world's saves. */
	UPROPERTY() TObjectPtr<UEditableTextBox> SaveLinkFolderInput;
	FString SaveLinkNotice;
	FString CreateError;
	/** Shown once on the next page build (e.g. result of an action that changed pages). */
	FString PendingFlash;
	bool bPendingFlashOk = true;
	FString FilterText;
	FString PendingInviteId;
	FString DetailsMembersText;
	FString DetailsHistoryText;
	FDelegateHandle ChangedHandle;
	FDelegateHandle SaveLinkHandle;
	/** Opens a world's Storage tab (the "link storage to host" dialog). */
	void OpenSaveLink(const FString& WorldId);
	// ---- Settings > Storage UI state
	UPROPERTY() TObjectPtr<UVerticalBox> StorageGridBox;
	UPROPERTY() TObjectPtr<UVerticalBox> StorageDetailsBox;
	UPROPERTY() TObjectPtr<UWidget> ProviderBrowserAnchor;
	UPROPERTY() TObjectPtr<UEditableTextBox> StorageSearchInput;
	FString SelectedProviderId;
	// ---- Connect form state (survives page rebuilds)
	FString ConnectProviderId;
	TMap<FString, FString> ConnectValues;
	UPROPERTY() TMap<FString, TObjectPtr<UEditableTextBox>> ConnectBoxes;
	FString ConnectError;
	FString ConnectStatus;
	int32 ConnectAttempt = 0;
	bool bConnectBusy = false;
	bool bConnectAdvanced = false;
	bool bRcloneTestRunning = false;
	/** "View all" pressed: list every storage backend rclone ships, not just the curated cards. */
	bool bShowAllProviders = false;
	FString StorageSearch;
	/** Development-safe message for display-only providers. */
	FString StorageNotice;
	/** StorageNotice reports a success (green) rather than a problem. */
	bool bStorageNoticeOk = false;
	/** Result lines of the last rclone engine self-test (Diagnostics tab). */
	FString RcloneSelfTestText;
	bool bRcloneSelfTestRunning = false;
	FString PageSignature;
	bool bStorageGrid = true;
	/** Width the selected-provider panel was given this build (grid columns are sized from the rest). */
	float StorageDetailsWidth = 0.f;
	bool bStorageRefreshQueued = false;
	/** Result of the last real Test Connection this session (nothing is claimed before one runs). */
	bool bLastTestRan = false;
	bool bLastTestOk = false;
	FDateTime LastTestTime;
	int32 JoinTab = 0;     // 0 invites, 1 friend worlds, 2 invite code
	int32 SettingsTab = 0; // 0 storage, 1 sync, 2 diagnostics
	int32 DetailsTab = 0;  // 0 overview, 1 players, 2 sync, 3 storage, 4 advanced
	bool bBusy = false;
	bool bRebuildQueued = false;
	bool bPendingScrollRestore = false;
	bool bShowTechnicalDetails = false;
	bool bDetailsMembersLoading = false;
	bool bDetailsMembersLoaded = false;
};
