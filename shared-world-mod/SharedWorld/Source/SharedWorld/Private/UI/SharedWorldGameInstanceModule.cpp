#include "UI/SharedWorldGameInstanceModule.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBoxSlot.h"
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
#include "UObject/UnrealType.h"

USharedWorldGameInstanceModule::USharedWorldGameInstanceModule()
{
	bRootModule = true;
}

void USharedWorldGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		RegisterMenuHooks();
	}
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

void USharedWorldGameInstanceModule::RegisterMenuHooks()
{
	if (bHooksRegistered) return;

	UWidgetBlueprintHookManager* Manager = GEngine ? GEngine->GetEngineSubsystem<UWidgetBlueprintHookManager>() : nullptr;
	if (!Manager)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=menu_hook_manager_missing"));
		bHooksRegistered = true;
		return;
	}

	// Same integration path SML uses for Mods: archetype insert into mMainMenuList.
	// Indirect_Child + mButtonJoinGame → parent panel is mMainMenuList.
	// Slot index 3 = after Continue(0), New Game(1), Join Game(2).
	UClass* FrontEndClass = SharedWorldNativeMenu::LoadFrontEndButtonClass();
	if (FrontEndClass)
	{
		MainMenuHook = NewObject<UWidgetBlueprintHookData>(this, NAME_None, RF_Transient);
		MainMenuHook->DeveloperComment = TEXT("Shared Worlds under Join Game (native FrontEnd button).");
		MainMenuHook->WidgetClass = TSoftClassPtr<UUserWidget>(FSoftObjectPath(
			TEXT("/Game/FactoryGame/Interface/UI/Menu/MainMenu/BP_MainMenuWidget.BP_MainMenuWidget_C")));
		MainMenuHook->NewWidgetClass = FrontEndClass;
		MainMenuHook->NewWidgetName = TEXT("mButtonSharedWorlds");
		MainMenuHook->ParentWidgetType = EWidgetBlueprintHookParentType::Indirect_Child;
		MainMenuHook->ParentWidgetName = TEXT("mButtonJoinGame");
		MainMenuHook->ParentSlotIndex = 3;
		// Do NOT set SlotConfiguration: SML's Generic slot helper has a VerticalBox
		// bug (calls HorizontalBoxSlot->SetSize on a null) and crashes here.
		MainMenuHook->SlotConfiguration = nullptr;

		Manager->RegisterWidgetBlueprintHook(MainMenuHook);
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=menu_hook_main ok class=%s slot=3"), *FrontEndClass->GetName());
	}
	else
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=menu_hook_main_fail reason=\"FrontEnd button class missing\""));
	}

	// Pause Manage Session: runtime inject only (FrontEnd OnClicked is unreliable here).
	// Hook disabled — SessionMenuButton uses a real UButton.

	bHooksRegistered = true;
}

void USharedWorldGameInstanceModule::OpenSharedWorldsBrowser()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		if (APlayerController* PC = GI->GetFirstLocalPlayerController())
		{
			UUserWidget* MainMenu = nullptr;
			if (UWorld* World = GI->GetWorld())
			{
				if (AFGMainMenuHUD* HUD = Cast<AFGMainMenuHUD>(PC->GetHUD()))
				{
					MainMenu = HUD->mMainMenu.Get();
				}
			}

			// One browser instance per menu open — prefer parenting into the
			// same WidgetSwitcher Join Game / Mods use.
			USharedWorldBrowserWidget* Browser = nullptr;
			if (MainMenu && MainMenu->WidgetTree)
			{
				TArray<UWidget*> All;
				MainMenu->WidgetTree->GetAllWidgets(All);
				for (UWidget* W : All)
				{
					if (USharedWorldBrowserWidget* Existing = Cast<USharedWorldBrowserWidget>(W))
					{
						Browser = Existing;
						break;
					}
				}
			}
			// Also find a prior browser parented under the switcher / SubMenu shell (not in WidgetTree).
			if (!Browser && MainMenu)
			{
				if (UObject* ExistingObj = StaticFindObject(
					USharedWorldBrowserWidget::StaticClass(), MainMenu, TEXT("SharedWorld_Browser")))
				{
					Browser = Cast<USharedWorldBrowserWidget>(ExistingObj);
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
				Browser->AddToViewport(200);
				Browser->SetVisibility(ESlateVisibility::Visible);
			}
		}
	}
}

void USharedWorldGameInstanceModule::OpenSharedWorldSession()
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=open_session_menu"));
	UGameInstance* GI = GetGameInstance();
	if (!GI) return;
	APlayerController* PC = GI->GetFirstLocalPlayerController();
	if (!PC) return;

	// Reuse an existing screen if already open.
	TArray<UUserWidget*> Existing;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GI->GetWorld(), Existing, USharedWorldSessionWidget::StaticClass(), false);
	for (UUserWidget* W : Existing)
	{
		if (USharedWorldSessionWidget* Screen = Cast<USharedWorldSessionWidget>(W))
		{
			Screen->SetVisibility(ESlateVisibility::Visible);
			Screen->AddToViewport(20000);
			return;
		}
	}

	USharedWorldSessionWidget* Screen = CreateWidget<USharedWorldSessionWidget>(PC, USharedWorldSessionWidget::StaticClass());
	if (Screen)
	{
		Screen->AddToViewport(20000);
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

	// Manage Session only exists while that pause submenu is open — search every time.
	TArray<UUserWidget*> Widgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, Widgets, UUserWidget::StaticClass(), /*TopLevelOnly=*/false);
	int32 ManageSessionCount = 0;
	for (UUserWidget* W : Widgets)
	{
		if (!W) continue;
		const FString ClassName = W->GetClass()->GetName();
		if (ClassName.Contains(TEXT("ManageSession")))
		{
			++ManageSessionCount;
			TryInjectPauseSessionEntry(W);
		}
	}
	if (ManageSessionCount == 0)
	{
		static double LastMissLog = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (Now - LastMissLog > 5.0)
		{
			LastMissLog = Now;
			UE_LOG(LogSharedWorld, Verbose, TEXT("[SharedWorld] event=pause_inject_idle reason=\"no ManageSession widget\" widgets=%d"), Widgets.Num());
		}
	}
}

UWidget* USharedWorldGameInstanceModule::FindWidgetByText(UWidget* Root, const FString& ExactText) const
{
	if (!Root) return nullptr;
	if (UTextBlock* Text = Cast<UTextBlock>(Root))
	{
		if (Text->GetText().ToString().Equals(ExactText, ESearchCase::IgnoreCase))
		{
			return Text;
		}
	}
	if (UUserWidget* UW = Cast<UUserWidget>(Root))
	{
		if (UWidget* Found = FindWidgetByText(UW->GetRootWidget(), ExactText)) return Found;
	}
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
	{
		const int32 N = Panel->GetChildrenCount();
		for (int32 i = 0; i < N; ++i)
		{
			if (UWidget* Found = FindWidgetByText(Panel->GetChildAt(i), ExactText)) return Found;
		}
	}
	return nullptr;
}

UPanelWidget* USharedWorldGameInstanceModule::FindParentPanel(UWidget* Child) const
{
	for (UWidget* It = Child; It; It = It->GetParent())
	{
		if (UPanelWidget* Panel = Cast<UPanelWidget>(It->GetParent()))
		{
			return Panel;
		}
	}
	return nullptr;
}

int32 USharedWorldGameInstanceModule::IndexOfChild(UPanelWidget* Panel, UWidget* Child) const
{
	return SharedWorldNativeMenu::ChildIndex(Panel, Child);
}

void USharedWorldGameInstanceModule::TryInjectMainMenuButton(UUserWidget* MainMenuRoot)
{
	SharedWorldNativeMenu::EnsureMainMenuEntry(MainMenuRoot, this);
}

static UPanelWidget* FindNamedPanel(UUserWidget* Root, FName Name)
{
	if (!Root) return nullptr;
	if (Root->WidgetTree)
	{
		if (UPanelWidget* P = Cast<UPanelWidget>(Root->WidgetTree->FindWidget(Name))) return P;
	}
	if (FObjectProperty* OP = FindFProperty<FObjectProperty>(Root->GetClass(), Name))
	{
		return Cast<UPanelWidget>(OP->GetObjectPropertyValue_InContainer(Root));
	}
	return nullptr;
}

static UUserWidget* FindDirectFrontEndChild(UPanelWidget* List, const FString& LabelSubstring)
{
	if (!List) return nullptr;
	for (int32 i = 0; i < List->GetChildrenCount(); ++i)
	{
		UUserWidget* Child = Cast<UUserWidget>(List->GetChildAt(i));
		if (!Child) continue;
		if (FTextProperty* TP = FindFProperty<FTextProperty>(Child->GetClass(), TEXT("mDisplayName")))
		{
			if (TP->GetPropertyValue_InContainer(Child).ToString().Contains(LabelSubstring))
			{
				return Child;
			}
		}
		if (Child->GetName().Contains(TEXT("ManagePlayers")) && LabelSubstring.Contains(TEXT("Manage")))
		{
			return Child;
		}
	}
	return nullptr;
}

void USharedWorldGameInstanceModule::TryInjectPauseSessionEntry(UUserWidget* ManageSessionRoot)
{
	if (!ManageSessionRoot) return;

	// Left nav = parent of Manage Players (OptionsList). Never mButtonsBox (Apply/Reset).
	UUserWidget* ManagePlayers = nullptr;
	if (FObjectProperty* OP = FindFProperty<FObjectProperty>(ManageSessionRoot->GetClass(), TEXT("mManagePlayers")))
	{
		ManagePlayers = Cast<UUserWidget>(OP->GetObjectPropertyValue_InContainer(ManageSessionRoot));
	}
	UUserWidget* SessionSettingsBtn = nullptr;
	if (FObjectProperty* OP = FindFProperty<FObjectProperty>(ManageSessionRoot->GetClass(), TEXT("mSessionSettings")))
	{
		SessionSettingsBtn = Cast<UUserWidget>(OP->GetObjectPropertyValue_InContainer(ManageSessionRoot));
	}

	UPanelWidget* NavList = ManagePlayers ? Cast<UPanelWidget>(ManagePlayers->GetParent()) : nullptr;
	if (!NavList && SessionSettingsBtn) NavList = Cast<UPanelWidget>(SessionSettingsBtn->GetParent());
	if (!NavList)
	{
		UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=pause_inject_skip reason=\"nav list missing\" class=%s"),
			*ManageSessionRoot->GetClass()->GetName());
		return;
	}

	auto IsStaleFrontEnd = [](UWidget* Child) -> bool
	{
		if (!Child || Cast<USharedWorldSessionMenuButton>(Child)) return false;
		if (Child->GetName().Contains(TEXT("mButtonSharedWorldSession"))
			|| Child->GetName().Contains(TEXT("SharedWorld_Session")))
		{
			return true;
		}
		if (FTextProperty* TP = FindFProperty<FTextProperty>(Child->GetClass(), TEXT("mDisplayName")))
		{
			return TP->GetPropertyValue_InContainer(Child).ToString().Equals(TEXT("Shared World"), ESearchCase::IgnoreCase);
		}
		return false;
	};

	// Strip non-clickable FrontEnd leftovers from nav + bottom Apply bar.
	auto StripStale = [&](UPanelWidget* Panel)
	{
		if (!Panel) return;
		for (int32 i = Panel->GetChildrenCount() - 1; i >= 0; --i)
		{
			UWidget* Child = Panel->GetChildAt(i);
			if (!IsStaleFrontEnd(Child)) continue;
			Child->RemoveFromParent();
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=pause_strip_stale name=%s"), *Child->GetName());
		}
	};
	StripStale(NavList);
	StripStale(FindNamedPanel(ManageSessionRoot, TEXT("mButtonsBox")));

	// Already have a real clickable SessionMenuButton?
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
