#pragma once
// Registers Shared Worlds into Satisfactory's main menu / pause menus.
// Prefers SML WidgetBlueprintHooks; falls back to runtime tree injection
// (find "Join Game" / Manage Session list and insert after it).

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "SharedWorldGameInstanceModule.generated.h"

class UWidgetBlueprintHookData;
class UUserWidget;
class UWidget;
class UPanelWidget;

UCLASS()
class SHAREDWORLD_API USharedWorldGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	USharedWorldGameInstanceModule();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

	/** Opens the Shared Worlds browser from a main-menu button. */
	UFUNCTION()
	void OpenSharedWorldsBrowser();

	/** Opens the in-session Shared World management screen. */
	UFUNCTION()
	void OpenSharedWorldSession();

	/** Runtime inject after Join Game / Manage Session (safe to call repeatedly). */
	void TryEnsureMenuEntries(UWorld* World);

private:
	void RegisterMenuHooks();
	void TryInjectMainMenuButton(UUserWidget* MainMenuRoot);
	void TryInjectPauseSessionEntry(UUserWidget* PauseRoot);
	UWidget* FindWidgetByText(UWidget* Root, const FString& ExactText) const;
	UPanelWidget* FindParentPanel(UWidget* Child) const;
	int32 IndexOfChild(UPanelWidget* Panel, UWidget* Child) const;

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> MainMenuHook;

	UPROPERTY()
	TObjectPtr<UWidgetBlueprintHookData> PauseMenuHook;

	/** Pause Manage Session FrontEnd button we already bound (avoid rebinding every tick). */
	UPROPERTY()
	TWeakObjectPtr<UUserWidget> PauseSessionButton;

	bool bHooksRegistered = false;
};
