#pragma once
// In-game Manage Session → Shared World screen.

#include "Blueprint/UserWidget.h"
#include "SharedWorldSessionWidget.generated.h"

class UButton;
class UEditableTextBox;
class UScrollBox;
class UTextBlock;
class UVerticalBox;
class USharedWorldSubsystem;
class USharedWorldInviteRowBinder;

UCLASS(Blueprintable)
class SHAREDWORLD_API USharedWorldSessionWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeDestruct() override;

	UFUNCTION() void Close();

	/** Status line for host migration / sync (shown in Manage Session → Shared World). */
	void SetStatusMessage(const FText& Message);

	/** Called by invite row binders (in-session players or Steam friends). */
	void InvitePlayer(const FString& WorldId, const FString& PlayerId, const FString& DisplayName);

private:
	void Refresh();
	void RebuildPlayersPanel(bool bForceMembersReload);
	void RefreshSessionPlayersIfChanged();
	/** Backend event (OnChanged): refresh whatever tab is showing. Replaces per-frame polling. */
	void HandleBackendChanged();
	/** Show text inside its card; collapse the card when there is nothing to say. */
	static void SetCardText(UTextBlock* Text, const FText& Value);
	void SetOverviewVisible(bool bVisible);
	/** Re-style the four tab buttons so the active one is outlined in orange. */
	void UpdateTabs();
	void AddInviteRow(UVerticalBox* List, const FString& WorldId, const FString& PlayerId, const FString& DisplayName, const FString& SubLabel);
	USharedWorldSubsystem* SW() const;

	UFUNCTION() void OnMigrate();
	UFUNCTION() void OnReevaluate();
	UFUNCTION() void OnSaveSync();
	UFUNCTION() void OnStopHosting();
	UFUNCTION() void OnToggleAdvanced();
	UFUNCTION() void OnBack();
	UFUNCTION() void OnTabOverview();
	UFUNCTION() void OnTabPlayers();
	UFUNCTION() void OnTabHistory();
	UFUNCTION() void OnTabBackups();
	UFUNCTION() void OnMakeSharedWorld();
	UFUNCTION() void OnInviteFriend();
	UFUNCTION() void OnInviteConfirm();
	UFUNCTION() void OnCopyInviteCode();

	UPROPERTY() TArray<TObjectPtr<UButton>> TabButtons;
	UPROPERTY() TObjectPtr<UTextBlock> TitleText;
	UPROPERTY() TObjectPtr<UTextBlock> OverviewText;
	UPROPERTY() TObjectPtr<UTextBlock> HostText;
	UPROPERTY() TObjectPtr<UTextBlock> RankingText;
	UPROPERTY() TObjectPtr<UTextBlock> SectionText;
	UPROPERTY() TObjectPtr<UTextBlock> AdvancedText;
	UPROPERTY() TObjectPtr<UWidget> AdvancedBox;
	UPROPERTY() TObjectPtr<UButton> MigrateButton;
	UPROPERTY() TObjectPtr<UButton> SaveButton;
	UPROPERTY() TObjectPtr<UButton> StopButton;
	UPROPERTY() TObjectPtr<UButton> MakeSharedButton;
	UPROPERTY() TObjectPtr<UButton> InviteButton;
	UPROPERTY() TObjectPtr<UEditableTextBox> InviteNameInput;
	UPROPERTY() TObjectPtr<UVerticalBox> InviteFriendsList;
	UPROPERTY() TObjectPtr<UVerticalBox> SessionPlayersList;
	UPROPERTY() TObjectPtr<UTextBlock> InviteCodeText;
	UPROPERTY() TObjectPtr<UTextBlock> MembersText;
	UPROPERTY() TObjectPtr<UTextBlock> FlashText;
	UPROPERTY() TArray<TObjectPtr<USharedWorldInviteRowBinder>> InviteBinders;

	bool bShowAdvanced = false;
	float RefreshAccum = 0.f;
	FDelegateHandle ChangedHandle;
	int32 ActiveTab = 0;
	FString LastSessionPlayerKey;
	bool bFriendsExpanded = false;
	bool bMembersLoaded = false;
	bool bFriendsRetryPending = false;
};
