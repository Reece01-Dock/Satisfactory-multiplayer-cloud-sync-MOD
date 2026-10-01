#pragma once
// Runtime helpers that reuse Satisfactory / SML menu widgets & fonts
// (same approach as Widget_ModList → BP_MenuBase + Widget_SubMenuBackground + FrontEnd buttons).

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"

class UTextBlock;
class UPanelWidget;
class UNamedSlot;
class USizeBox;
class UWidget;

namespace SharedWorldFg
{
	inline const TCHAR* SubMenuBackgroundPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_SubMenuBackground.Widget_SubMenuBackground_C");
	inline const TCHAR* FrontEndButtonPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_FrontEnd_Button.Widget_FrontEnd_Button_C");
	inline const TCHAR* StandardButtonPath =
		TEXT("/Game/FactoryGame/Interface/UI/InGame/-Shared/Widget_StandardButton.Widget_StandardButton_C");
	/** SML ModList row chrome (wraps FrontEnd_Button). Optional. */
	inline const TCHAR* ModSelectButtonPath =
		TEXT("/SML/Interface/UI/Menu/Mods/Widget_ModSelect_Button.Widget_ModSelect_Button_C");
	inline const TCHAR* DescriptionFontPath =
		TEXT("/Game/FactoryGame/Interface/Font/DescriptionText.DescriptionText");
	inline const TCHAR* HeeboBoldPath =
		TEXT("/Game/FactoryGame/Interface/Font/Heebo-Bold.Heebo-Bold");
	inline const TCHAR* HeeboRegularPath =
		TEXT("/Game/FactoryGame/Interface/Font/Heebo-Regular.Heebo-Regular");
	/** Vanilla main-menu / in-game error popup (SetBody / SetButtonText). */
	inline const TCHAR* ErrorMessagePath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/MainMenu/Widget_ErrorMessage.Widget_ErrorMessage_C");

	UClass* LoadSubMenuBackgroundClass();
	UClass* LoadFrontEndButtonClass();
	UClass* LoadStandardButtonClass();
	UClass* LoadModSelectButtonClass();
	UClass* LoadErrorMessageClass();

	/** Apply Satisfactory menu font to a text block (falls back to CoreStyle if missing). */
	void ApplyMenuFont(UTextBlock* Text, int32 Size, bool bBold);

	/** Create Widget_SubMenuBackground and place Content into its mContent slot (SML ModList). */
	UUserWidget* WrapInSubMenuBackground(UUserWidget* Outer, UWidget* Content);

	/** Place Content into an existing Widget_SubMenuBackground (baked SharedWorldsBrowser / SharedWorldSession). */
	bool FillSubMenuContent(UUserWidget* SubMenuBackground, UWidget* Content);

	/** Create a FrontEnd list row sized like Join Game submenu entries. */
	UUserWidget* CreateFrontEndRow(UUserWidget* Outer, const FText& Title, bool bBig);

	/**
	 * FrontEnd row wrapped in a full-width SizeBox (Join Game height).
	 * Prefer this for submenu actions so the orange hover bar spans the column.
	 */
	USizeBox* CreateFrontEndRowBoxed(UUserWidget* Outer, const FText& Title, bool bBig, UUserWidget*& OutButton);

	/** Create SML Widget_ModSelect_Button when available; otherwise null. */
	UUserWidget* CreateModSelectRow(UUserWidget* Outer, const FText& Title);

	/** Create Widget_StandardButton and SetText(Label). */
	UUserWidget* CreateStandardButton(UUserWidget* Outer, const FText& Label);

	/**
	 * Show a transparent top-of-screen notice overlay (not the centered FG modal).
	 * Receiver+DismissFunc are bound to OK / auto-hide so the toast can be removed.
	 */
	UUserWidget* ShowErrorMessage(UObject* WorldContext, const FText& Body, const FText& ButtonText,
		UObject* DismissReceiver, FName DismissFunc);

	void SetFrontEndTitle(UUserWidget* Button, const FText& Title);
	void BindFrontEndClicked(UUserWidget* Button, UObject* Receiver, FName FuncName);
	void BindStandardClicked(UUserWidget* Button, UObject* Receiver, FName FuncName);
	void SetBoolProp(UObject* Obj, FName Name, bool Value);
	bool GetBoolProp(UObject* Obj, FName Name, bool Default = false);
	void SetObjectProp(UObject* Obj, FName Name, UObject* Value);
	void SetTextProp(UObject* Obj, FName Name, const FText& Value);
	void CallSetText(UObject* Obj, const FText& Text);

	/** Reflected FG-widget reads. Missing props return null / empty and never crash (F11). */
	UObject* GetObjectProp(UObject* Obj, FName Name);
	FText GetTextProp(UObject* Obj, FName Name);
	/** WidgetTree name first, then reflected object property (e.g. mMainMenuList). */
	UPanelWidget* FindNamedPanel(UUserWidget* Root, FName Name);
	UWidget* FindNamedWidget(UUserWidget* Root, FName Name);
}
