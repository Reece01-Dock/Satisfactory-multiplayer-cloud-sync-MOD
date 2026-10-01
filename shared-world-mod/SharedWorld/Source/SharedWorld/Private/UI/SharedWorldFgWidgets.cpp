#include "UI/SharedWorldFgWidgets.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlot.h"
#include "Components/Overlay.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Components/Button.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/UnrealType.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldNoticeOverlay.h"
#include "Widgets/Layout/Anchors.h"

namespace SharedWorldFg
{
	namespace
	{
		void ClearMulticast(UObject* Obj, FName Name)
		{
			if (!Obj) return;
			if (FMulticastDelegateProperty* DP = FindFProperty<FMulticastDelegateProperty>(Obj->GetClass(), Name))
			{
				DP->ClearDelegate(Obj);
			}
		}

		bool BindMulticast(UObject* Obj, FName PropName, UObject* Receiver, FName FuncName)
		{
			if (!Obj || !Receiver) return false;
			ClearMulticast(Obj, PropName);
			if (FMulticastDelegateProperty* DP = FindFProperty<FMulticastDelegateProperty>(Obj->GetClass(), PropName))
			{
				FScriptDelegate Del;
				Del.BindUFunction(Receiver, FuncName);
				DP->AddDelegate(Del, Obj);
				return true;
			}
			return false;
		}

		void CallBoolSetter(UObject* Obj, FName FuncName, bool Value)
		{
			if (!Obj) return;
			if (UFunction* Fn = Obj->FindFunction(FuncName))
			{
				struct FParams { bool bValue; };
				FParams P{Value};
				Obj->ProcessEvent(Fn, &P);
			}
		}

		UObject* LoadFontObj(const TCHAR* Path)
		{
			return StaticLoadObject(UFont::StaticClass(), nullptr, Path);
		}

		/** Force UMG construction so WidgetTree / NamedSlots exist (SML BPs get this for free). */
		void EnsureConstructed(UUserWidget* W)
		{
			if (!W) return;
			if (!W->GetCachedWidget().IsValid())
			{
				W->TakeWidget();
			}
			W->SynchronizeProperties();
		}

		UWidget* ResolveContentSlot(UUserWidget* Bg)
		{
			if (!Bg) return nullptr;

			// Prefer the Blueprint property mContent (NamedSlot on Widget_SubMenuBackground).
			if (FObjectPropertyBase* OP = FindFProperty<FObjectPropertyBase>(Bg->GetClass(), TEXT("mContent")))
			{
				if (UWidget* W = Cast<UWidget>(OP->GetObjectPropertyValue_InContainer(Bg)))
				{
					return W;
				}
			}
			if (Bg->WidgetTree)
			{
				if (UWidget* W = Bg->WidgetTree->FindWidget(TEXT("mContent"))) return W;
				if (UWidget* W = Bg->WidgetTree->FindWidget(TEXT("Content"))) return W;
			}
			return nullptr;
		}

		bool PlaceIntoSlot(UWidget* SlotWidget, UWidget* Content)
		{
			if (!SlotWidget || !Content) return false;
			Content->RemoveFromParent();

			if (UContentWidget* ContentHost = Cast<UContentWidget>(SlotWidget))
			{
				ContentHost->SetContent(Content);
				return true;
			}
			if (UNamedSlot* Named = Cast<UNamedSlot>(SlotWidget))
			{
				Named->ClearChildren();
				Named->AddChild(Content);
				return true;
			}
			if (UPanelWidget* Panel = Cast<UPanelWidget>(SlotWidget))
			{
				Panel->ClearChildren();
				Panel->AddChild(Content);
				return true;
			}
			return false;
		}

		bool CallOverwritePanel(UUserWidget* Bg, UWidget* Content)
		{
			if (!Bg || !Content) return false;
			static const FName Names[] = {
				TEXT("OverwritePanelWidget"),
				TEXT("Overwrite Panel Widget"),
			};
			for (FName N : Names)
			{
				if (UFunction* Fn = Bg->FindFunction(N))
				{
					// CustomEvent OverwritePanelWidget(PanelWidget)
					struct FParams { UWidget* PanelWidget; };
					FParams P{Content};
					Bg->ProcessEvent(Fn, &P);
					return true;
				}
			}
			return false;
		}

		UUserWidget* SpawnFgWidget(UUserWidget* Outer, UClass* Cls, const TCHAR* BaseName)
		{
			if (!Outer || !Cls) return nullptr;
			UObject* NameOuter = Outer->WidgetTree ? static_cast<UObject*>(Outer->WidgetTree) : static_cast<UObject*>(Outer);
			const FName Name = MakeUniqueObjectName(NameOuter, Cls, BaseName);
			UUserWidget* W = UUserWidget::CreateWidgetInstance(*Outer, Cls, Name);
			if (!W)
			{
				if (APlayerController* PC = Outer->GetOwningPlayer())
				{
					W = CreateWidget<UUserWidget>(PC, Cls);
				}
			}
			if (W)
			{
				EnsureConstructed(W);
			}
			return W;
		}
	}

	UClass* LoadSubMenuBackgroundClass()
	{
		UClass* C = LoadClass<UUserWidget>(nullptr, SubMenuBackgroundPath);
		if (!C)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=fg_load_fail asset=SubMenuBackground"));
		}
		return C;
	}

	UClass* LoadFrontEndButtonClass()
	{
		UClass* C = LoadClass<UUserWidget>(nullptr, FrontEndButtonPath);
		if (!C)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=fg_load_fail asset=FrontEndButton"));
		}
		return C;
	}

	UClass* LoadStandardButtonClass()
	{
		UClass* C = LoadClass<UUserWidget>(nullptr, StandardButtonPath);
		if (!C)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=fg_load_fail asset=StandardButton path=%s"), StandardButtonPath);
		}
		return C;
	}

	UClass* LoadModSelectButtonClass()
	{
		UClass* C = LoadClass<UUserWidget>(nullptr, ModSelectButtonPath);
		return C; // optional — SML may not be loaded yet in editor
	}

	UClass* LoadErrorMessageClass()
	{
		UClass* C = LoadClass<UUserWidget>(nullptr, ErrorMessagePath);
		if (!C)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=fg_load_fail asset=Widget_ErrorMessage"));
		}
		return C;
	}

	namespace
	{
		void CallNamedTextSetter(UObject* Obj, FName FuncName, FName PropName, const FText& Text)
		{
			if (!Obj) return;
			if (UFunction* Fn = Obj->FindFunction(FuncName))
			{
				struct FParams { FText Text; };
				FParams P{Text};
				Obj->ProcessEvent(Fn, &P);
				return;
			}
			SetTextProp(Obj, PropName, Text);
		}
	}

	UUserWidget* ShowErrorMessage(UObject* WorldContext, const FText& Body, const FText& ButtonText,
		UObject* DismissReceiver, FName DismissFunc)
	{
		if (!WorldContext) return nullptr;

		UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
		APlayerController* PC = nullptr;
		if (World)
		{
			PC = World->GetFirstPlayerController();
		}
		if (!PC)
		{
			if (UGameInstance* AsGI = Cast<UGameInstance>(WorldContext))
			{
				PC = AsGI->GetFirstLocalPlayerController();
			}
			else if (const UGameInstanceSubsystem* Sub = Cast<UGameInstanceSubsystem>(WorldContext))
			{
				if (UGameInstance* OwnerGI = Sub->GetGameInstance())
				{
					PC = OwnerGI->GetFirstLocalPlayerController();
				}
			}
		}
		if (!PC) return nullptr;

		USharedWorldNoticeOverlay* Toast = CreateWidget<USharedWorldNoticeOverlay>(PC);
		if (!Toast) return nullptr;
		EnsureConstructed(Toast);

		if (DismissReceiver && !DismissFunc.IsNone())
		{
			BindMulticast(Toast, TEXT("OnDismissed"), DismissReceiver, DismissFunc);
		}

		Toast->AddToViewport(1000);
		Toast->SetAnchorsInViewport(FAnchors(0.f, 0.f, 1.f, 1.f));
		Toast->SetAlignmentInViewport(FVector2D(0.f, 0.f));
		Toast->ShowNotice(
			Body,
			ButtonText.IsEmpty() ? NSLOCTEXT("SharedWorld", "ErrorOk", "OK") : ButtonText,
			12.f);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=error_popup shown=1 style=top_toast"));
		return Toast;
	}

	void SetBoolProp(UObject* Obj, FName Name, bool Value)
	{
		if (!Obj) return;
		if (FBoolProperty* P = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			P->SetPropertyValue_InContainer(Obj, Value);
		}
	}

	bool GetBoolProp(UObject* Obj, FName Name, bool Default)
	{
		if (!Obj) return Default;
		if (FBoolProperty* P = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			return P->GetPropertyValue_InContainer(Obj);
		}
		return Default;
	}

	void SetObjectProp(UObject* Obj, FName Name, UObject* Value)
	{
		if (!Obj) return;
		if (FObjectPropertyBase* P = FindFProperty<FObjectPropertyBase>(Obj->GetClass(), Name))
		{
			P->SetObjectPropertyValue_InContainer(Obj, Value);
		}
	}

	void SetTextProp(UObject* Obj, FName Name, const FText& Value)
	{
		if (!Obj) return;
		if (FTextProperty* P = FindFProperty<FTextProperty>(Obj->GetClass(), Name))
		{
			P->SetPropertyValue_InContainer(Obj, Value);
		}
	}

	void CallSetText(UObject* Obj, const FText& Text)
	{
		if (!Obj) return;
		if (UFunction* Fn = Obj->FindFunction(TEXT("SetText")))
		{
			struct FParams { FText Text; };
			FParams P{Text};
			Obj->ProcessEvent(Fn, &P);
			return;
		}
		if (UFunction* Fn = Obj->FindFunction(TEXT("SetTitle")))
		{
			struct FParams { FText Title; };
			FParams P{Text};
			Obj->ProcessEvent(Fn, &P);
		}
	}

	UObject* GetObjectProp(UObject* Obj, FName Name)
	{
		if (!Obj) return nullptr;
		if (FObjectPropertyBase* P = FindFProperty<FObjectPropertyBase>(Obj->GetClass(), Name))
		{
			return P->GetObjectPropertyValue_InContainer(Obj);
		}
		return nullptr;
	}

	FText GetTextProp(UObject* Obj, FName Name)
	{
		if (!Obj) return FText::GetEmpty();
		if (FTextProperty* P = FindFProperty<FTextProperty>(Obj->GetClass(), Name))
		{
			return P->GetPropertyValue_InContainer(Obj);
		}
		return FText::GetEmpty();
	}

	UPanelWidget* FindNamedPanel(UUserWidget* Root, FName Name)
	{
		return Cast<UPanelWidget>(FindNamedWidget(Root, Name));
	}

	UWidget* FindNamedWidget(UUserWidget* Root, FName Name)
	{
		if (!Root) return nullptr;
		if (Root->WidgetTree)
		{
			if (UWidget* W = Root->WidgetTree->FindWidget(Name)) return W;
		}
		if (UObject* Obj = GetObjectProp(Root, Name))
		{
			if (UWidget* W = Cast<UWidget>(Obj)) return W;
		}
		if (Root->WidgetTree)
		{
			TArray<UWidget*> All;
			Root->WidgetTree->GetAllWidgets(All);
			const FString Prefix = Name.ToString();
			for (UWidget* W : All)
			{
				if (W && W->GetFName().ToString().StartsWith(Prefix))
				{
					return W;
				}
			}
		}
		return nullptr;
	}

	void ApplyMenuFont(UTextBlock* Text, int32 Size, bool bBold)
	{
		if (!Text) return;
		UObject* FontObj = LoadFontObj(bBold ? HeeboBoldPath : DescriptionFontPath);
		if (!FontObj) FontObj = LoadFontObj(bBold ? HeeboBoldPath : HeeboRegularPath);
		if (FontObj)
		{
			FSlateFontInfo Info = Text->GetFont();
			Info.FontObject = FontObj;
			Info.Size = Size;
			Info.TypefaceFontName = NAME_None;
			Text->SetFont(Info);
			return;
		}
		Text->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), Size));
	}

	UUserWidget* WrapInSubMenuBackground(UUserWidget* Outer, UWidget* Content)
	{
		if (!Outer || !Content) return nullptr;
		UClass* Cls = LoadSubMenuBackgroundClass();
		if (!Cls) return nullptr;

		UObject* NameOuter = Outer->WidgetTree ? static_cast<UObject*>(Outer->WidgetTree) : static_cast<UObject*>(Outer);
		const FName Name = MakeUniqueObjectName(NameOuter, Cls, TEXT("SharedWorld_SubMenuBg"));
		UUserWidget* Bg = UUserWidget::CreateWidgetInstance(*Outer, Cls, Name);
		if (!Bg) return nullptr;

		EnsureConstructed(Bg);

		// Match BP_MenuBase / ModList: show the translucent plate + blur.
		SetBoolProp(Bg, TEXT("mShowBackground"), true);
		CallBoolSetter(Bg, TEXT("SetShowBackground"), true);
		CallBoolSetter(Bg, TEXT("Set Show Background"), true);
		CallBoolSetter(Bg, TEXT("SetUsesGradient"), true);
		SetBoolProp(Bg, TEXT("mUsesSubmenuBackgroundGradient"), true);

		bool bPlaced = CallOverwritePanel(Bg, Content);
		if (!bPlaced)
		{
			bPlaced = PlaceIntoSlot(ResolveContentSlot(Bg), Content);
		}
		if (!bPlaced && Bg->WidgetTree)
		{
			TArray<UWidget*> All;
			Bg->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (Cast<UNamedSlot>(W) || Cast<UContentWidget>(W))
				{
					if (PlaceIntoSlot(W, Content))
					{
						bPlaced = true;
						break;
					}
				}
			}
		}

		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=submenu_wrap placed=%d class=%s"),
			bPlaced ? 1 : 0, *Bg->GetClass()->GetName());

		Bg->SetVisibility(ESlateVisibility::Visible);
		EnsureConstructed(Bg);
		return Bg;
	}

	bool FillSubMenuContent(UUserWidget* SubMenuBackground, UWidget* Content)
	{
		if (!SubMenuBackground || !Content) return false;
		EnsureConstructed(SubMenuBackground);
		SetBoolProp(SubMenuBackground, TEXT("mShowBackground"), true);
		CallBoolSetter(SubMenuBackground, TEXT("SetShowBackground"), true);

		bool bPlaced = CallOverwritePanel(SubMenuBackground, Content);
		if (!bPlaced)
		{
			bPlaced = PlaceIntoSlot(ResolveContentSlot(SubMenuBackground), Content);
		}
		if (!bPlaced && SubMenuBackground->WidgetTree)
		{
			TArray<UWidget*> All;
			SubMenuBackground->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (Cast<UNamedSlot>(W) || Cast<UContentWidget>(W))
				{
					if (PlaceIntoSlot(W, Content))
					{
						bPlaced = true;
						break;
					}
				}
			}
		}
		SubMenuBackground->SetVisibility(ESlateVisibility::Visible);
		Content->SetVisibility(ESlateVisibility::Visible);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=submenu_fill placed=%d shell=%s"),
			bPlaced ? 1 : 0, *SubMenuBackground->GetName());
		return bPlaced;
	}

	void SetFrontEndTitle(UUserWidget* Button, const FText& Title)
	{
		if (!Button) return;
		SetTextProp(Button, TEXT("mDisplayName"), Title);
		SetTextProp(Button, TEXT("DisplayName"), Title);
		CallSetText(Button, Title);
		if (UFunction* Fn = Button->FindFunction(TEXT("SetTitle")))
		{
			struct FParams { FText Title; };
			FParams P{Title};
			Button->ProcessEvent(Fn, &P);
		}
		// Push label into nested text blocks (same as NativeMenu ForceLabelText).
		if (Button->WidgetTree)
		{
			TArray<UWidget*> All;
			Button->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (UTextBlock* T = Cast<UTextBlock>(W))
				{
					T->SetText(Title);
				}
			}
		}
		Button->SynchronizeProperties();
	}

	UUserWidget* CreateFrontEndRow(UUserWidget* Outer, const FText& Title, bool bBig)
	{
		if (!Outer) return nullptr;
		UClass* Cls = LoadFrontEndButtonClass();
		if (!Cls) return nullptr;
		UUserWidget* B = SpawnFgWidget(Outer, Cls, TEXT("SW_FERow"));
		if (!B) return nullptr;
		SetBoolProp(B, TEXT("IsBigButton"), bBig);
		SetBoolProp(B, TEXT("mUseTransparentBackground"), true);
		SetBoolProp(B, TEXT("UseTransparentBackground"), true);
		SetBoolProp(B, TEXT("mShineOnHover"), true);
		CallBoolSetter(B, TEXT("SetIsBigButton"), bBig);
		CallBoolSetter(B, TEXT("SetUseTransparentBackground"), true);
		SetFrontEndTitle(B, Title);

		// Stretch internal SizeBox so the orange hover bar fills the column (Join Game).
		if (B->WidgetTree)
		{
			TArray<UWidget*> All;
			B->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (USizeBox* SB = Cast<USizeBox>(W))
				{
					SB->SetMinDesiredHeight(bBig ? 56.f : 48.f);
					SB->ClearWidthOverride();
					SB->SetMinDesiredWidth(280.f);
				}
			}
		}
		B->SetVisibility(ESlateVisibility::Visible);
		EnsureConstructed(B);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=frontend_row title=\"%s\" big=%d"), *Title.ToString(), bBig ? 1 : 0);
		return B;
	}

	USizeBox* CreateFrontEndRowBoxed(UUserWidget* Outer, const FText& Title, bool bBig, UUserWidget*& OutButton)
	{
		OutButton = nullptr;
		if (!Outer || !Outer->WidgetTree) return nullptr;
		OutButton = CreateFrontEndRow(Outer, Title, bBig);
		if (!OutButton) return nullptr;
		USizeBox* Box = Outer->WidgetTree->ConstructWidget<USizeBox>();
		Box->SetMinDesiredHeight(bBig ? 56.f : 48.f);
		Box->SetHeightOverride(bBig ? 56.f : 48.f);
		Box->AddChild(OutButton);
		return Box;
	}

	UUserWidget* CreateModSelectRow(UUserWidget* Outer, const FText& Title)
	{
		if (!Outer) return nullptr;
		UClass* Cls = LoadModSelectButtonClass();
		if (!Cls) return nullptr;
		UUserWidget* B = SpawnFgWidget(Outer, Cls, TEXT("SW_ModSel"));
		if (!B) return nullptr;
		SetTextProp(B, TEXT("mDisplayName"), Title);
		SetTextProp(B, TEXT("DisplayName"), Title);
		CallSetText(B, Title);
		// Nested FrontEnd button
		if (FObjectPropertyBase* OP = FindFProperty<FObjectPropertyBase>(B->GetClass(), TEXT("mFrontEndButton")))
		{
			if (UUserWidget* Fe = Cast<UUserWidget>(OP->GetObjectPropertyValue_InContainer(B)))
			{
				SetFrontEndTitle(Fe, Title);
				SetBoolProp(Fe, TEXT("mUseTransparentBackground"), true);
				SetBoolProp(Fe, TEXT("IsBigButton"), false);
			}
		}
		B->SetVisibility(ESlateVisibility::Visible);
		return B;
	}

	UUserWidget* CreateStandardButton(UUserWidget* Outer, const FText& Label)
	{
		if (!Outer) return nullptr;
		UClass* Cls = LoadStandardButtonClass();
		if (!Cls) return nullptr;
		UUserWidget* B = SpawnFgWidget(Outer, Cls, TEXT("SW_StdBtn"));
		if (!B) return nullptr;
		SetTextProp(B, TEXT("DisplayName"), Label);
		SetTextProp(B, TEXT("mDisplayName"), Label);
		CallSetText(B, Label);
		B->SetVisibility(ESlateVisibility::Visible);
		EnsureConstructed(B);
		return B;
	}

	void BindFrontEndClicked(UUserWidget* Button, UObject* Receiver, FName FuncName)
	{
		if (!Button || !Receiver) return;

		// Bind every known surface — FrontEnd may fire the inner UButton without
		// raising the Blueprint OnClicked multicast for runtime-created instances.
		BindMulticast(Button, TEXT("OnClicked"), Receiver, FuncName);
		BindMulticast(Button, TEXT("OnClickedWithIndex"), Receiver, FuncName);
		BindMulticast(Button, TEXT("ButtonActivated"), Receiver, FuncName);
		BindMulticast(Button, TEXT("ButtonClicked"), Receiver, FuncName);

		static const FName Nested[] = {
			TEXT("mFrontEndButton"), TEXT("ButtonInternal"), TEXT("Button"),
			TEXT("mButton"), TEXT("mInternalButton")
		};
		for (FName N : Nested)
		{
			if (FObjectProperty* OP = FindFProperty<FObjectProperty>(Button->GetClass(), N))
			{
				if (UUserWidget* NestedUW = Cast<UUserWidget>(OP->GetObjectPropertyValue_InContainer(Button)))
				{
					BindFrontEndClicked(NestedUW, Receiver, FuncName);
				}
				else if (UButton* UB = Cast<UButton>(OP->GetObjectPropertyValue_InContainer(Button)))
				{
					FScriptDelegate Del;
					Del.BindUFunction(Receiver, FuncName);
					UB->OnClicked.AddUnique(Del);
				}
			}
		}
		if (Button->WidgetTree)
		{
			TArray<UWidget*> All;
			Button->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (UButton* UB = Cast<UButton>(W))
				{
					FScriptDelegate Del;
					Del.BindUFunction(Receiver, FuncName);
					UB->OnClicked.AddUnique(Del);
				}
			}
		}
	}

	void BindStandardClicked(UUserWidget* Button, UObject* Receiver, FName FuncName)
	{
		BindFrontEndClicked(Button, Receiver, FuncName);
	}
}
