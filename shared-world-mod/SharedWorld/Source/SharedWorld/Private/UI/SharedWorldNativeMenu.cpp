#include "UI/SharedWorldNativeMenu.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "SharedWorldTypes.h"

namespace SharedWorldNativeMenu
{
	UClass* LoadFrontEndButtonClass()
	{
		return LoadClass<UUserWidget>(nullptr, FrontEndButtonPath);
	}

	static void SetBoolProp(UObject* Obj, FName Name, bool Value)
	{
		if (!Obj) return;
		if (FBoolProperty* P = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			P->SetPropertyValue_InContainer(Obj, Value);
		}
	}

	static bool GetBoolProp(UObject* Obj, FName Name, bool Default = false)
	{
		if (!Obj) return Default;
		if (FBoolProperty* P = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			return P->GetPropertyValue_InContainer(Obj);
		}
		return Default;
	}

	static void SetTextProp(UObject* Obj, FName Name, const FText& Value)
	{
		if (!Obj) return;
		if (FTextProperty* P = FindFProperty<FTextProperty>(Obj->GetClass(), Name))
		{
			P->SetPropertyValue_InContainer(Obj, Value);
		}
	}

	static void SetObjectProp(UObject* Obj, FName Name, UObject* Value)
	{
		if (!Obj) return;
		if (FObjectPropertyBase* P = FindFProperty<FObjectPropertyBase>(Obj->GetClass(), Name))
		{
			P->SetObjectPropertyValue_InContainer(Obj, Value);
		}
	}

	static void CallBoolSetter(UObject* Obj, FName FuncName, bool Value)
	{
		if (!Obj) return;
		if (UFunction* Fn = Obj->FindFunction(FuncName))
		{
			struct FParamsBool { bool bValue; };
			FParamsBool Params{Value};
			Obj->ProcessEvent(Fn, &Params);
		}
	}

	static void ClearMulticast(UObject* Obj, FName Name)
	{
		if (!Obj) return;
		if (FMulticastDelegateProperty* DP = FindFProperty<FMulticastDelegateProperty>(Obj->GetClass(), Name))
		{
			DP->ClearDelegate(Obj);
		}
	}

	static bool BindClicked(UUserWidget* ButtonWidget, UObject* Receiver, FName FuncName)
	{
		if (!ButtonWidget || !Receiver) return false;

		ClearMulticast(ButtonWidget, TEXT("OnClicked"));
		ClearMulticast(ButtonWidget, TEXT("OnClickedWithIndex"));

		if (FMulticastDelegateProperty* DP = FindFProperty<FMulticastDelegateProperty>(ButtonWidget->GetClass(), TEXT("OnClicked")))
		{
			FScriptDelegate Del;
			Del.BindUFunction(Receiver, FuncName);
			DP->AddDelegate(Del, ButtonWidget);
			return true;
		}

		static const FName Candidates[] = {
			TEXT("mFrontEndButton"), TEXT("Button"), TEXT("mButton"), TEXT("mInternalButton")
		};
		for (FName N : Candidates)
		{
			if (FObjectProperty* OP = FindFProperty<FObjectProperty>(ButtonWidget->GetClass(), N))
			{
				if (UButton* B = Cast<UButton>(OP->GetObjectPropertyValue_InContainer(ButtonWidget)))
				{
					B->OnClicked.Clear();
					FScriptDelegate Del;
					Del.BindUFunction(Receiver, FuncName);
					B->OnClicked.AddUnique(Del);
					return true;
				}
			}
		}
		return false;
	}

	static void ForceLabelText(UWidget* Root, const FText& Label)
	{
		if (!Root) return;
		if (UTextBlock* T = Cast<UTextBlock>(Root))
		{
			T->SetText(Label);
			return;
		}
		if (UUserWidget* UW = Cast<UUserWidget>(Root))
		{
			ForceLabelText(UW->GetRootWidget(), Label);
			return;
		}
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
		{
			// Only force the first/largest-looking text; walk all and set title-sized ones.
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				ForceLabelText(Panel->GetChildAt(i), Label);
			}
		}
	}

	static void ConfigureAsSharedWorldsButton(UUserWidget* W, UUserWidget* JoinTemplate,
		UObject* ClickReceiver, FName ClickFuncName)
	{
		if (!W) return;
		const FText Title = NSLOCTEXT("SharedWorld", "MenuEntry", "Shared Worlds");

		SetTextProp(W, TEXT("mDisplayName"), Title);
		const bool bBig = JoinTemplate ? GetBoolProp(JoinTemplate, TEXT("IsBigButton"), true) : true;
		const bool bTransparent = JoinTemplate ? GetBoolProp(JoinTemplate, TEXT("mUseTransparentBackground"), true) : true;
		SetBoolProp(W, TEXT("IsBigButton"), bBig);
		SetBoolProp(W, TEXT("mUseTransparentBackground"), bTransparent);
		SetBoolProp(W, TEXT("UseTransparentBackground"), bTransparent);
		CallBoolSetter(W, TEXT("SetIsBigButton"), bBig);
		CallBoolSetter(W, TEXT("SetUseTransparentBackground"), bTransparent);

		SetObjectProp(W, TEXT("mSwitcherWidget"), nullptr);
		SetObjectProp(W, TEXT("mSwitcher"), nullptr);
		SetObjectProp(W, TEXT("mMenuSwitcherContainer_DEPRECATED"), nullptr);

		BindClicked(W, ClickReceiver, ClickFuncName);
		ForceLabelText(W, Title);
		W->SetVisibility(ESlateVisibility::Visible);
		W->SynchronizeProperties();
		W->InvalidateLayoutAndVolatility();
	}

	UUserWidget* CreateFrontEndMenuButton(APlayerController* /*PC*/, const FText& /*Title*/,
		UObject* /*ClickReceiver*/, FName /*ClickFuncName*/)
	{
		// Runtime CreateWidget path removed — it laid out in the footer.
		return nullptr;
	}

	UWidget* FindNamed(UUserWidget* Root, FName Name)
	{
		if (!Root) return nullptr;
		if (Root->WidgetTree)
		{
			if (UWidget* W = Root->WidgetTree->FindWidget(Name)) return W;
		}
		if (FObjectProperty* OP = FindFProperty<FObjectProperty>(Root->GetClass(), Name))
		{
			return Cast<UWidget>(OP->GetObjectPropertyValue_InContainer(Root));
		}
		if (Root->WidgetTree)
		{
			TArray<UWidget*> All;
			Root->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (W && W->GetFName().ToString().StartsWith(Name.ToString()))
				{
					return W;
				}
			}
		}
		return nullptr;
	}

	int32 ChildIndex(UPanelWidget* Panel, UWidget* Child)
	{
		if (!Panel || !Child) return INDEX_NONE;
		const int32 N = Panel->GetChildrenCount();
		for (int32 i = 0; i < N; ++i)
		{
			if (Panel->GetChildAt(i) == Child) return i;
		}
		return INDEX_NONE;
	}

	static void LogList(UPanelWidget* List)
	{
		if (!List) return;
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_list_dump parent=%s count=%d"),
			*List->GetName(), List->GetChildrenCount());
		for (int32 i = 0; i < List->GetChildrenCount(); ++i)
		{
			UWidget* C = List->GetChildAt(i);
			FString Label = TEXT("?");
			if (C)
			{
				if (FTextProperty* TP = FindFProperty<FTextProperty>(C->GetClass(), TEXT("mDisplayName")))
				{
					Label = TP->GetPropertyValue_InContainer(C).ToString();
				}
				const FVector2D Desired = C->GetDesiredSize();
				UE_LOG(LogSharedWorld, Log,
					TEXT("[SharedWorld] event=menu_list_item i=%d name=%s class=%s label=\"%s\" vis=%d desired=%.0fx%.0f"),
					i, *C->GetName(), *C->GetClass()->GetName(), *Label, (int32)C->GetVisibility(),
					Desired.X, Desired.Y);
			}
		}
	}

	static bool IsSharedWorldsLabel(UWidget* Child)
	{
		if (!Child) return false;
		if (Child->GetName().StartsWith(TEXT("mButtonSharedWorlds"))) return true;
		if (FTextProperty* TP = FindFProperty<FTextProperty>(Child->GetClass(), TEXT("mDisplayName")))
		{
			return TP->GetPropertyValue_InContainer(Child).ToString().Equals(TEXT("Shared Worlds"), ESearchCase::IgnoreCase);
		}
		return false;
	}

	bool EnsureMainMenuEntry(UUserWidget* MainMenuRoot, USharedWorldGameInstanceModule* Owner)
	{
		if (!MainMenuRoot || !Owner) return false;

		UWidget* JoinBtn = FindNamed(MainMenuRoot, TEXT("mButtonJoinGame"));
		if (!JoinBtn)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=menu_inject_skip reason=\"mButtonJoinGame missing\""));
			return false;
		}

		UPanelWidget* List = Cast<UPanelWidget>(JoinBtn->GetParent());
		if (!List)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=menu_inject_skip reason=\"JoinGame has no panel parent\""));
			return false;
		}

		UUserWidget* JoinUW = Cast<UUserWidget>(JoinBtn);
		UUserWidget* Hooked = nullptr;

		// Prefer the archetype-hooked widget (correct layout, same as Mods).
		if (UWidget* Named = FindNamed(MainMenuRoot, TEXT("mButtonSharedWorlds")))
		{
			Hooked = Cast<UUserWidget>(Named);
		}

		// Remove broken runtime inserts (footer ghosts / zero-height slots).
		for (int32 i = List->GetChildrenCount() - 1; i >= 0; --i)
		{
			UWidget* Child = List->GetChildAt(i);
			if (!IsSharedWorldsLabel(Child)) continue;
			if (Child == Hooked) continue;

			const bool bInTree = Child->GetTypedOuter<UWidgetTree>() == MainMenuRoot->WidgetTree;
			const FVector2D Desired = Child->GetDesiredSize();
			const bool bBadSize = Desired.Y < 1.f;
			if (!bInTree || bBadSize || Child != Hooked)
			{
				UE_LOG(LogSharedWorld, Log,
					TEXT("[SharedWorld] event=menu_remove_orphan name=%s inTree=%d desiredY=%.1f"),
					*Child->GetName(), bInTree ? 1 : 0, Desired.Y);
				List->RemoveChildAt(i);
				Child->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
			}
		}

		// Also purge any Shared Worlds FrontEnd buttons elsewhere under the main menu
		// (footer ghosts from earlier CreateWidget attempts).
		if (MainMenuRoot->WidgetTree)
		{
			TArray<UWidget*> All;
			MainMenuRoot->WidgetTree->GetAllWidgets(All);
			for (UWidget* W : All)
			{
				if (!IsSharedWorldsLabel(W) || W == Hooked) continue;
				if (W->GetParent() == List) continue;
				UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_remove_footer_ghost name=%s parent=%s"),
					*W->GetName(), W->GetParent() ? *W->GetParent()->GetName() : TEXT("null"));
				W->RemoveFromParent();
				W->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
			}
		}

		if (Hooked)
		{
			ConfigureAsSharedWorldsButton(Hooked, JoinUW, Owner,
				GET_FUNCTION_NAME_CHECKED(USharedWorldGameInstanceModule, OpenSharedWorldsBrowser));

			// Ensure it sits immediately after Join Game.
			const int32 JoinIdx = ChildIndex(List, JoinBtn);
			const int32 OurIdx = ChildIndex(List, Hooked);
			const int32 WantIdx = JoinIdx == INDEX_NONE ? OurIdx : JoinIdx + 1;
			if (OurIdx != INDEX_NONE && WantIdx != INDEX_NONE && OurIdx != WantIdx)
			{
				List->RemoveChild(Hooked);
				List->InsertChildAt(FMath::Clamp(WantIdx, 0, List->GetChildrenCount()), Hooked);
				if (UVerticalBoxSlot* JoinSlot = Cast<UVerticalBoxSlot>(JoinBtn->Slot))
				{
					if (UVerticalBoxSlot* EntrySlot = Cast<UVerticalBoxSlot>(Hooked->Slot))
					{
						EntrySlot->SetHorizontalAlignment(JoinSlot->GetHorizontalAlignment());
						EntrySlot->SetVerticalAlignment(JoinSlot->GetVerticalAlignment());
						EntrySlot->SetSize(JoinSlot->GetSize());
						EntrySlot->SetPadding(JoinSlot->GetPadding());
					}
				}
			}

			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_configure_hooked name=%s"), *Hooked->GetName());
			LogList(List);
			return true;
		}

		// Fallback if archetype hook did not install: CreateWidgetInstance into the list.
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=menu_hook_missing_fallback"));
		const int32 JoinIdx = ChildIndex(List, JoinBtn);
		const int32 InsertAt = JoinIdx == INDEX_NONE ? List->GetChildrenCount() : JoinIdx + 1;
		UClass* ButtonClass = JoinBtn->GetClass();
		if (!ButtonClass) ButtonClass = LoadFrontEndButtonClass();
		if (!ButtonClass) return false;

		const FName UniqueName = MakeUniqueObjectName(
			MainMenuRoot->WidgetTree ? static_cast<UObject*>(MainMenuRoot->WidgetTree) : static_cast<UObject*>(MainMenuRoot),
			ButtonClass, TEXT("mButtonSharedWorlds"));

		UUserWidget* Entry = UUserWidget::CreateWidgetInstance(*MainMenuRoot, ButtonClass, UniqueName);
		if (!Entry) return false;

		ConfigureAsSharedWorldsButton(Entry, JoinUW, Owner,
			GET_FUNCTION_NAME_CHECKED(USharedWorldGameInstanceModule, OpenSharedWorldsBrowser));
		List->InsertChildAt(InsertAt, Entry);
		if (UVerticalBoxSlot* JoinSlot = Cast<UVerticalBoxSlot>(JoinBtn->Slot))
		{
			if (UVerticalBoxSlot* EntrySlot = Cast<UVerticalBoxSlot>(Entry->Slot))
			{
				EntrySlot->SetHorizontalAlignment(JoinSlot->GetHorizontalAlignment());
				EntrySlot->SetVerticalAlignment(JoinSlot->GetVerticalAlignment());
				EntrySlot->SetSize(JoinSlot->GetSize());
				EntrySlot->SetPadding(JoinSlot->GetPadding());
			}
		}

		LogList(List);
		return true;
	}
}
