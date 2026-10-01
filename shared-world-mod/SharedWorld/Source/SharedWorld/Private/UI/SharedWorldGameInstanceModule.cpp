#include "UI/SharedWorldGameInstanceModule.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FGMainMenuHUD.h"
#include "Patching/WidgetBlueprintHookManager.h"
#include "SharedWorldTypes.h"
#include "TimerManager.h"
#include "UI/FGUserWidget.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldFgWidgets.h"
#include "UI/SharedWorldNativeMenu.h"
#include "UI/SharedWorldSessionMenuButton.h"
#include "UI/SharedWorldSessionWidget.h"

namespace
{
	const TCHAR* MainMenuClassPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/MainMenu/BP_MainMenuWidget.BP_MainMenuWidget_C");
	const TCHAR* ManageSessionClassPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_ManageSession.Widget_ManageSession_C");
	const TCHAR* FrontEndButtonClassPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_FrontEnd_Button.Widget_FrontEnd_Button_C");
	const TCHAR* SubMenuBackgroundClassPath =
		TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_SubMenuBackground.Widget_SubMenuBackground_C");
}

USharedWorldGameInstanceModule::USharedWorldGameInstanceModule()
{
	bRootModule = true;
}

bool USharedWorldGameInstanceModule::ArchetypeHasNamedWidget(const TCHAR* WidgetClassPath, FName WidgetName)
{
	UClass* Cls = LoadClass<UUserWidget>(nullptr, WidgetClassPath);
	const UWidgetBlueprintGeneratedClass* WBGC = Cast<UWidgetBlueprintGeneratedClass>(Cls);
	if (!WBGC) return false;
	const UWidgetTree* Tree = WBGC->GetWidgetTreeArchetype();
	return Tree && Tree->FindWidget(WidgetName) != nullptr;
}

UWidgetBlueprintHookData* USharedWorldGameInstanceModule::MakeHook(
	const FString& Comment,
	const FSoftObjectPath& TargetWidgetClass,
	UClass* NewWidgetClass,
	FName NewWidgetName,
	FName ParentWidgetName,
	int32 ParentSlotIndex)
{
	if (!NewWidgetClass) return nullptr;

	UWidgetBlueprintHookData* Hook = NewObject<UWidgetBlueprintHookData>(this, NAME_None, RF_Transient);
	Hook->DeveloperComment = Comment;
	Hook->WidgetClass = TSoftClassPtr<UUserWidget>(TargetWidgetClass);
	Hook->NewWidgetClass = NewWidgetClass;
	Hook->NewWidgetName = NewWidgetName;
	Hook->ParentWidgetType = EWidgetBlueprintHookParentType::Direct;
	Hook->ParentWidgetName = ParentWidgetName;
	Hook->ParentSlotIndex = ParentSlotIndex;
	Hook->SlotConfiguration = nullptr;
	return Hook;
}

void USharedWorldGameInstanceModule::RegisterMenuHooks()
{
	if (bHooksRegistered) return;
	bHooksRegistered = true;

	UWidgetBlueprintHookManager* Manager = GEngine ? GEngine->GetEngineSubsystem<UWidgetBlueprintHookManager>() : nullptr;
	if (!Manager)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=menu_hook_manager_missing"));
		return;
	}

	UClass* FrontEndClass = LoadClass<UUserWidget>(nullptr, FrontEndButtonClassPath);
	if (!FrontEndClass)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=menu_hook_fail reason=\"FrontEnd button class missing\""));
		return;
	}
	UClass* SubMenuClass = LoadClass<UUserWidget>(nullptr, SubMenuBackgroundClassPath);
	if (!SubMenuClass)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=menu_hook_fail reason=\"SubMenuBackground class missing\""));
		return;
	}

	const FSoftObjectPath MainMenuPath(MainMenuClassPath);
	const FSoftObjectPath ManageSessionPath(ManageSessionClassPath);

	// Same pattern as SML Mods, but use stock FrontEnd + SubMenuBackground so the
	// entry matches Join Game (not the smaller ModsButton_SML / Extensions look).
	// BP bake: Shared Worlds immediately after Play (slot 2).
	if (!ArchetypeHasNamedWidget(MainMenuClassPath, TEXT("mButtonSharedWorlds")))
	{
		MainMenuButtonHook = MakeHook(
			TEXT("Shared Worlds FrontEnd button after Play (Join Game style)"),
			MainMenuPath, FrontEndClass, TEXT("mButtonSharedWorlds"),
			TEXT("mMainMenuList"), 2);
		if (MainMenuButtonHook)
		{
			Manager->RegisterWidgetBlueprintHook(MainMenuButtonHook);
			UE_LOG(LogSharedWorld, Log,
				TEXT("[SharedWorld] event=menu_hook_button ok parent=mMainMenuList slot=2 class=FrontEnd"));
		}
	}
	else
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_button_skip reason=\"already in archetype\""));
	}

	if (!ArchetypeHasNamedWidget(MainMenuClassPath, TEXT("SharedWorldsBrowser")))
	{
		// Join Game / Load / Options pages are SubMenuBackground shells in mSwitcher.
		MainMenuBrowserHook = MakeHook(
			TEXT("Shared Worlds SubMenuBackground page (Join Game panel chrome)"),
			MainMenuPath, SubMenuClass, TEXT("SharedWorldsBrowser"),
			TEXT("mSwitcher"), INDEX_NONE);
		if (MainMenuBrowserHook)
		{
			Manager->RegisterWidgetBlueprintHook(MainMenuBrowserHook);
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_browser ok parent=mSwitcher class=SubMenuBackground"));
		}
	}
	else
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_browser_skip reason=\"already in archetype\""));
	}

	if (!ArchetypeHasNamedWidget(ManageSessionClassPath, TEXT("mSharedWorld")))
	{
		ManageSessionButtonHook = MakeHook(
			TEXT("Shared World button in Manage Session"),
			ManageSessionPath, FrontEndClass, TEXT("mSharedWorld"),
			TEXT("OptionsList"), INDEX_NONE);
		if (ManageSessionButtonHook)
		{
			Manager->RegisterWidgetBlueprintHook(ManageSessionButtonHook);
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_session_button ok"));
		}
	}

	if (!ArchetypeHasNamedWidget(ManageSessionClassPath, TEXT("SharedWorldSession")))
	{
		ManageSessionPageHook = MakeHook(
			TEXT("Shared World session SubMenuBackground in Manage Session switcher"),
			ManageSessionPath, SubMenuClass, TEXT("SharedWorldSession"),
			TEXT("mSwitcher"), INDEX_NONE);
		if (ManageSessionPageHook)
		{
			Manager->RegisterWidgetBlueprintHook(ManageSessionPageHook);
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_session_page ok"));
		}
	}
}

void USharedWorldGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// Register hooks before Super so they install with other mod hooks during INITIALIZATION.
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		RegisterMenuHooks();
	}

	Super::DispatchLifecycleEvent(Phase);

	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			auto TryInject = FTimerDelegate::CreateLambda([this]()
			{
				if (UGameInstance* GI2 = GetGameInstance())
				{
					if (UWorld* World = GI2->GetWorld())
					{
						TryEnsureMenuEntries(World);
					}
				}
			});
			FTimerHandle H1, H2, H3;
			GI->GetTimerManager().SetTimer(H1, TryInject, 0.25f, false);
			GI->GetTimerManager().SetTimer(H2, TryInject, 1.0f, false);
			GI->GetTimerManager().SetTimer(H3, TryInject, 2.5f, false);
		}
	}
}

void USharedWorldGameInstanceModule::WireMainMenuSharedWorldsButton(UUserWidget* MainMenuRoot, UUserWidget* Button)
{
	if (!MainMenuRoot || !Button) return;

	// Same Construct sequence as Widget_MainMenuButtonExtensions (Mods).
	UWidget* Switcher = SharedWorldFg::FindNamedWidget(MainMenuRoot, TEXT("mSwitcher"));
	UWidget* List = SharedWorldFg::FindNamedWidget(MainMenuRoot, TEXT("mMainMenuList"));
	UWidget* Target = SharedWorldFg::FindNamedWidget(MainMenuRoot, TEXT("SharedWorldsBrowser"));

	// Match BP_MainMenuWidget bake: Continue → Play → Shared Worlds → Join Game…
	if (UPanelWidget* NavList = Cast<UPanelWidget>(List))
	{
		UWidget* Play = SharedWorldFg::FindNamedWidget(MainMenuRoot, TEXT("mButtonPlay"));
		const int32 PlayIdx = IndexOfChild(NavList, Play);
		const int32 CurIdx = IndexOfChild(NavList, Button);
		const int32 WantIdx = (PlayIdx == INDEX_NONE) ? 2 : PlayIdx + 1;
		if (CurIdx != INDEX_NONE && CurIdx != WantIdx)
		{
			NavList->RemoveChild(Button);
			const int32 InsertAt = FMath::Clamp(WantIdx, 0, NavList->GetChildrenCount());
			NavList->InsertChildAt(InsertAt, Button);
			UE_LOG(LogSharedWorld, Log,
				TEXT("[SharedWorld] event=menu_reorder_button from=%d to=%d (after Play)"),
				CurIdx, InsertAt);
		}
	}

	SharedWorldFg::SetFrontEndTitle(Button, NSLOCTEXT("SharedWorld", "MenuEntry", "Shared Worlds"));
	// Match Join Game / New Game: big FrontEnd row, transparent plate (no Mods-style compact bar).
	SharedWorldFg::SetBoolProp(Button, TEXT("IsBigButton"), true);
	SharedWorldFg::SetBoolProp(Button, TEXT("mIsBigButton"), true);
	SharedWorldFg::SetBoolProp(Button, TEXT("mUseTransparentBackground"), true);
	SharedWorldFg::SetBoolProp(Button, TEXT("UseTransparentBackground"), true);
	SharedWorldFg::SetBoolProp(Button, TEXT("mShineOnHover"), true);
	if (UFunction* Fn = Button->FindFunction(TEXT("SetIsBigButton")))
	{
		struct FParamsBool { bool bValue; };
		FParamsBool P{true};
		Button->ProcessEvent(Fn, &P);
	}
	if (UFunction* Fn = Button->FindFunction(TEXT("SetUseTransparentBackground")))
	{
		struct FParamsBool { bool bValue; };
		FParamsBool P{true};
		Button->ProcessEvent(Fn, &P);
	}
	if (Button->WidgetTree)
	{
		TArray<UWidget*> All;
		Button->WidgetTree->GetAllWidgets(All);
		for (UWidget* W : All)
		{
			if (USizeBox* SB = Cast<USizeBox>(W))
			{
				SB->SetMinDesiredHeight(56.f);
				SB->ClearWidthOverride();
				SB->SetMinDesiredWidth(280.f);
			}
		}
	}

	if (List) SharedWorldFg::SetObjectProp(Button, TEXT("mParentList"), List);
	if (Switcher) SharedWorldFg::SetObjectProp(Button, TEXT("mSwitcherWidget"), Switcher);
	if (Target) SharedWorldFg::SetObjectProp(Button, TEXT("mTargetWidget"), Target);

	// Also bind Open so C++ browser content is ready / filled when clicked.
	SharedWorldFg::BindFrontEndClicked(Button, this,
		GET_FUNCTION_NAME_CHECKED(USharedWorldGameInstanceModule, OpenSharedWorldsBrowser));

	Button->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	Button->SynchronizeProperties();
	Button->InvalidateLayoutAndVolatility();

	UE_LOG(LogSharedWorld, Log,
		TEXT("[SharedWorld] event=menu_wire_button name=%s switcher=%d target=%d list=%d"),
		*Button->GetName(), Switcher ? 1 : 0, Target ? 1 : 0, List ? 1 : 0);
}

UWidgetSwitcher* USharedWorldGameInstanceModule::FindAncestorSwitcher(UWidget* Child)
{
	for (UWidget* It = Child; It; It = It->GetParent())
	{
		if (UWidgetSwitcher* Sw = Cast<UWidgetSwitcher>(It->GetParent()))
		{
			return Sw;
		}
	}
	return nullptr;
}

UWidget* USharedWorldGameInstanceModule::SwitcherChildContaining(UWidgetSwitcher* Switcher, UWidget* Descendant)
{
	if (!Switcher || !Descendant) return nullptr;
	for (UWidget* It = Descendant; It; It = It->GetParent())
	{
		if (It->GetParent() == Switcher)
		{
			return It;
		}
	}
	return nullptr;
}

bool USharedWorldGameInstanceModule::ActivateInSwitcher(UWidget* Target)
{
	if (!Target) return false;
	UWidgetSwitcher* Switcher = FindAncestorSwitcher(Target);
	if (!Switcher) return false;
	UWidget* Child = SwitcherChildContaining(Switcher, Target);
	if (!Child) return false;
	Switcher->SetActiveWidget(Child);
	Target->SetVisibility(ESlateVisibility::Visible);
	Child->SetVisibility(ESlateVisibility::Visible);
	return true;
}

void USharedWorldGameInstanceModule::OpenSharedWorldsBrowser()
{
	UGameInstance* GI = GetGameInstance();
	if (!GI) return;
	APlayerController* PC = GI->GetFirstLocalPlayerController();
	if (!PC) return;

	UUserWidget* MainMenu = nullptr;
	if (AFGMainMenuHUD* HUD = Cast<AFGMainMenuHUD>(PC->GetHUD()))
	{
		MainMenu = Cast<UUserWidget>(HUD->mMainMenu.Get());
	}

	// Prefer the hooked SharedWorldsBrowser page (SML ModList_SML equivalent).
	USharedWorldBrowserWidget* Browser = nullptr;
	if (MainMenu)
	{
		Browser = Cast<USharedWorldBrowserWidget>(
			SharedWorldFg::FindNamedWidget(MainMenu, TEXT("SharedWorldsBrowser")));
	}
	if (!Browser && MainMenu && MainMenu->WidgetTree)
	{
		TArray<UWidget*> All;
		MainMenu->WidgetTree->GetAllWidgets(All);
		for (UWidget* W : All)
		{
			Browser = Cast<USharedWorldBrowserWidget>(W);
			if (Browser) break;
		}
	}
	if (!Browser)
	{
		TArray<UUserWidget*> Existing;
		UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GI->GetWorld(), Existing, USharedWorldBrowserWidget::StaticClass(), false);
		for (UUserWidget* W : Existing)
		{
			Browser = Cast<USharedWorldBrowserWidget>(W);
			if (Browser) break;
		}
	}
	if (!Browser)
	{
		if (MainMenu)
		{
			const FName BrowserName = MakeUniqueObjectName(
				MainMenu, USharedWorldBrowserWidget::StaticClass(), TEXT("SharedWorld_Browser"));
			Browser = Cast<USharedWorldBrowserWidget>(
				UUserWidget::CreateWidgetInstance(*MainMenu, USharedWorldBrowserWidget::StaticClass(), BrowserName));
		}
		else
		{
			Browser = CreateWidget<USharedWorldBrowserWidget>(PC, USharedWorldBrowserWidget::StaticClass());
		}
	}
	if (!Browser) return;

	if (MainMenu)
	{
		Browser->ActivateInMainMenu(MainMenu);
	}
	else
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=browser_open_fail reason=\"no main menu\""));
	}
}

void USharedWorldGameInstanceModule::OpenSharedWorldSession()
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=open_session_menu"));
	UGameInstance* GI = GetGameInstance();
	if (!GI) return;
	APlayerController* PC = GI->GetFirstLocalPlayerController();
	if (!PC) return;

	USharedWorldSessionWidget* Screen = nullptr;
	TArray<UUserWidget*> Existing;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GI->GetWorld(), Existing, USharedWorldSessionWidget::StaticClass(), false);
	for (UUserWidget* W : Existing)
	{
		Screen = Cast<USharedWorldSessionWidget>(W);
		if (Screen) break;
	}

	UUserWidget* ManageSession = nullptr;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GI->GetWorld(), Existing, UUserWidget::StaticClass(), false);
	for (UUserWidget* W : Existing)
	{
		if (W && W->GetClass()->GetName().Contains(TEXT("ManageSession")))
		{
			ManageSession = W;
			break;
		}
	}

	if (!Screen && ManageSession)
	{
		if (UUserWidget* Hooked = Cast<UUserWidget>(SharedWorldFg::FindNamedWidget(ManageSession, TEXT("SharedWorldSession"))))
		{
			Screen = Cast<USharedWorldSessionWidget>(Hooked);
		}
	}

	if (!Screen && ManageSession)
	{
		const FName Name = MakeUniqueObjectName(
			ManageSession, USharedWorldSessionWidget::StaticClass(), TEXT("SharedWorldSessionContent"));
		Screen = Cast<USharedWorldSessionWidget>(
			UUserWidget::CreateWidgetInstance(*ManageSession, USharedWorldSessionWidget::StaticClass(), Name));
	}

	if (!Screen)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=session_missing"));
		return;
	}

	if (ManageSession)
	{
		if (UUserWidget* Shell = Cast<UUserWidget>(SharedWorldFg::FindNamedWidget(ManageSession, TEXT("SharedWorldSession"))))
		{
			if (Shell != Screen)
			{
				SharedWorldFg::FillSubMenuContent(Shell, Screen);
			}
			if (ActivateInSwitcher(Shell))
			{
				return;
			}
		}
	}

	if (!ActivateInSwitcher(Screen))
	{
		Screen->SetVisibility(ESlateVisibility::Visible);
	}
}

void USharedWorldGameInstanceModule::TryEnsureMenuEntries(UWorld* World)
{
	if (!World) return;
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC) return;

	if (AFGMainMenuHUD* HUD = Cast<AFGMainMenuHUD>(PC->GetHUD()))
	{
		if (UFGUserWidget* MainMenu = HUD->mMainMenu.Get())
		{
			TryInjectMainMenuButton(MainMenu);
		}
	}

	TArray<UUserWidget*> Widgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Widgets, UUserWidget::StaticClass(), false);
	for (UUserWidget* W : Widgets)
	{
		if (!W) continue;
		if (W->GetClass()->GetName().Contains(TEXT("ManageSession")))
		{
			TryInjectPauseSessionEntry(W);
		}
	}
}

int32 USharedWorldGameInstanceModule::IndexOfChild(UPanelWidget* Panel, UWidget* Child) const
{
	return SharedWorldNativeMenu::ChildIndex(Panel, Child);
}

void USharedWorldGameInstanceModule::TryInjectMainMenuButton(UUserWidget* MainMenuRoot)
{
	SharedWorldNativeMenu::EnsureMainMenuEntry(MainMenuRoot, this);
}

void USharedWorldGameInstanceModule::TryInjectPauseSessionEntry(UUserWidget* ManageSessionRoot)
{
	if (!ManageSessionRoot) return;

	if (UWidget* Baked = SharedWorldFg::FindNamedWidget(ManageSessionRoot, TEXT("mSharedWorld")))
	{
		if (UUserWidget* BakedUW = Cast<UUserWidget>(Baked))
		{
			UWidget* Switcher = SharedWorldFg::FindNamedWidget(ManageSessionRoot, TEXT("mSwitcher"));
			UWidget* List = SharedWorldFg::FindNamedWidget(ManageSessionRoot, TEXT("OptionsList"));
			UWidget* Target = SharedWorldFg::FindNamedWidget(ManageSessionRoot, TEXT("SharedWorldSession"));
			SharedWorldFg::SetFrontEndTitle(BakedUW, NSLOCTEXT("SharedWorld", "SessionEntry", "Shared World"));
			SharedWorldFg::SetBoolProp(BakedUW, TEXT("IsBigButton"), false);
			if (List) SharedWorldFg::SetObjectProp(BakedUW, TEXT("mParentList"), List);
			if (Switcher) SharedWorldFg::SetObjectProp(BakedUW, TEXT("mSwitcherWidget"), Switcher);
			if (Target) SharedWorldFg::SetObjectProp(BakedUW, TEXT("mTargetWidget"), Target);
			SharedWorldFg::BindFrontEndClicked(BakedUW, this,
				GET_FUNCTION_NAME_CHECKED(USharedWorldGameInstanceModule, OpenSharedWorldSession));
			BakedUW->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
			BakedUW->SynchronizeProperties();
		}
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pause_baked_present name=%s"), *Baked->GetName());
		return;
	}

	UUserWidget* ManagePlayers = Cast<UUserWidget>(SharedWorldFg::GetObjectProp(ManageSessionRoot, TEXT("mManagePlayers")));
	UUserWidget* SessionSettingsBtn = Cast<UUserWidget>(SharedWorldFg::GetObjectProp(ManageSessionRoot, TEXT("mSessionSettings")));

	UPanelWidget* NavList = ManagePlayers ? Cast<UPanelWidget>(ManagePlayers->GetParent()) : nullptr;
	if (!NavList && SessionSettingsBtn) NavList = Cast<UPanelWidget>(SessionSettingsBtn->GetParent());
	if (!NavList)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=pause_inject_skip reason=\"nav list missing\" class=%s"),
			*ManageSessionRoot->GetClass()->GetName());
		return;
	}

	for (int32 i = 0; i < NavList->GetChildrenCount(); ++i)
	{
		if (Cast<USharedWorldSessionMenuButton>(NavList->GetChildAt(i)))
		{
			return;
		}
	}

	const FName UniqueName = MakeUniqueObjectName(
		ManageSessionRoot->WidgetTree ? static_cast<UObject*>(ManageSessionRoot->WidgetTree)
									  : static_cast<UObject*>(ManageSessionRoot),
		USharedWorldSessionMenuButton::StaticClass(), TEXT("mButtonSharedWorldSession"));

	USharedWorldSessionMenuButton* Entry = nullptr;
	if (APlayerController* PC = ManageSessionRoot->GetOwningPlayer())
	{
		Entry = CreateWidget<USharedWorldSessionMenuButton>(PC, USharedWorldSessionMenuButton::StaticClass(), UniqueName);
	}
	if (!Entry)
	{
		Entry = Cast<USharedWorldSessionMenuButton>(
			UUserWidget::CreateWidgetInstance(*ManageSessionRoot, USharedWorldSessionMenuButton::StaticClass(), UniqueName));
	}
	if (!Entry) return;

	const int32 AnchorIdx = ManagePlayers ? IndexOfChild(NavList, ManagePlayers) : INDEX_NONE;
	const int32 InsertAt = (AnchorIdx == INDEX_NONE) ? NavList->GetChildrenCount() : AnchorIdx + 1;
	NavList->InsertChildAt(FMath::Clamp(InsertAt, 0, NavList->GetChildrenCount()), Entry);
	if (UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(Entry->Slot))
	{
		Slot->SetHorizontalAlignment(HAlign_Fill);
	}
	PauseSessionButton = Entry;
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_inject_pause created=%s parent=%s at=%d"),
		*Entry->GetName(), *NavList->GetName(), InsertAt);
}
