#pragma once
// SML-style menu integration: WidgetBlueprintHooks inject Shared Worlds into
// stock BP_MainMenuWidget / Widget_ManageSession (same idea as ModsButton_SML).

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "SharedWorldGameInstanceModule.generated.h"

class UWidgetBlueprintHookData;
class UUserWidget;
class UWidget;
class UPanelWidget;
class UWidgetSwitcher;

UCLASS()
class SHAREDWORLD_API USharedWorldGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	USharedWorldGameInstanceModule();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	UFUNCTION()
	void OpenSharedWorldsBrowser();

	UFUNCTION()
	void OpenSharedWorldSession();

	void TryEnsureMenuEntries(UWorld* World);

	/** Wire FrontEnd button like SML ModsButton_SML Construct (switcher + target + label). */
	void WireMainMenuSharedWorldsButton(UUserWidget* MainMenuRoot, UUserWidget* Button);

private:
	void RegisterMenuHooks();
	UWidgetBlueprintHookData* MakeHook(
		const FString& Comment,
		const FSoftObjectPath& TargetWidgetClass,
		UClass* NewWidgetClass,
		FName NewWidgetName,
		FName ParentWidgetName,
		int32 ParentSlotIndex);

	void TryInjectMainMenuButton(UUserWidget* MainMenuRoot);
	void TryInjectPauseSessionEntry(UUserWidget* ManageSessionRoot);
	int32 IndexOfChild(UPanelWidget* Panel, UWidget* Child) const;

	static UWidgetSwitcher* FindAncestorSwitcher(UWidget* Child);
	static UWidget* SwitcherChildContaining(UWidgetSwitcher* Switcher, UWidget* Descendant);
	static bool ActivateInSwitcher(UWidget* Target);
	static bool ArchetypeHasNamedWidget(const TCHAR* WidgetClassPath, FName WidgetName);

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> MainMenuButtonHook;

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> MainMenuBrowserHook;

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> ManageSessionButtonHook;

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> ManageSessionPageHook;

	UPROPERTY()
	TWeakObjectPtr<UUserWidget> PauseSessionButton;

	bool bHooksRegistered = false;
};
