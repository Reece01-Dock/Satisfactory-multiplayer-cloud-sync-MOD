#pragma once
// Creates a native-looking main-menu entry using Satisfactory's own
// Widget_FrontEnd_Button (same class as Continue / Join Game / Mods).

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

class UPanelWidget;
class USharedWorldGameInstanceModule;

namespace SharedWorldNativeMenu
{
	/** Soft path to the game's front-end list button. */
	inline const TCHAR* FrontEndButtonPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_FrontEnd_Button.Widget_FrontEnd_Button_C");

	/** Load Widget_FrontEnd_Button_C (null if assets unavailable). */
	UClass* LoadFrontEndButtonClass();

	/**
	 * Build a FrontEnd button labelled Title, bind click to Receiver::ClickFuncName.
	 * Sets IsBigButton + transparent background to match Continue / Join Game.
	 */
	UUserWidget* CreateFrontEndMenuButton(APlayerController* PC, const FText& Title,
		UObject* ClickReceiver, FName ClickFuncName);

	/** Find a named widget in a user widget tree (e.g. mMainMenuList). */
	UWidget* FindNamed(UUserWidget* Root, FName Name);

	/** Index of Child inside Panel, or INDEX_NONE. */
	int32 ChildIndex(UPanelWidget* Panel, UWidget* Child);

	/**
	 * Insert Shared Worlds into BP_MainMenuWidget's mMainMenuList immediately
	 * after mButtonJoinGame. Returns true if the entry is present afterwards.
	 */
	bool EnsureMainMenuEntry(UUserWidget* MainMenuRoot, USharedWorldGameInstanceModule* Owner);
}
