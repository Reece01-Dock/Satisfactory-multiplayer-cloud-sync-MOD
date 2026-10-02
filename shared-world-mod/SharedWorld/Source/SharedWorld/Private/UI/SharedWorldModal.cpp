#include "UI/SharedWorldModal.h"

#include "Blueprint/WidgetTree.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "TimerManager.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

USharedWorldModal* USharedWorldModal::Show(APlayerController* PC, const FSharedWorldModalSpec& InSpec)
{
	if (!PC) return nullptr;
	USharedWorldModal* M = CreateWidget<USharedWorldModal>(PC, USharedWorldModal::StaticClass());
	if (!M) return nullptr;
	M->Spec = InSpec;
	M->Build();
	M->AddToViewport(10000); // above the menu, the browser and the toast layer
	if (!PC->bShowMouseCursor)
	{
		// In-game the cursor is hidden and input is game-only: take UI input while the dialog is open.
		M->InputOwner = PC;
		M->bRestoreGameInput = true;
		PC->SetShowMouseCursor(true);
		FInputModeUIOnly Mode;
		Mode.SetWidgetToFocus(M->TakeWidget());
		PC->SetInputMode(Mode);
	}
	// Focus after the widget is in the tree. A destructive dialog starts on Cancel so one stray press cannot confirm.
	TWeakObjectPtr<USharedWorldModal> Weak(M);
	if (UWorld* World = PC->GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([Weak]()
		{
			if (USharedWorldModal* Self = Weak.Get())
			{
				UButton* Target = (Self->Spec.ConfirmRole == ESharedWorldButtonRole::Danger && Self->CancelButton) ? Self->CancelButton.Get() : Self->ConfirmButton.Get();
				if (Target) Target->SetKeyboardFocus();
			}
		}));
	}
	return M;
}

void USharedWorldModal::Build()
{
	if (!WidgetTree || WidgetTree->RootWidget) return;

	// Full-screen scrim; it is hit-testable so clicks never reach the menu underneath.
	UBorder* Scrim = WidgetTree->ConstructWidget<UBorder>();
	Scrim->SetBrush(RoundedBrush(ModalScrim, 0.f));
	Scrim->SetHorizontalAlignment(HAlign_Center);
	Scrim->SetVerticalAlignment(VAlign_Center);
	WidgetTree->RootWidget = Scrim;

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>();
	Size->SetMinDesiredWidth(520.f);
	Size->SetMaxDesiredWidth(680.f);
	Scrim->SetContent(Size);

	FLinearColor Edge = ToneColor(Spec.Tone);
	Edge.A = 0.7f;
	UBorder* Card = MakePanel(WidgetTree, FLinearColor(0.045f, 0.05f, 0.06f, 0.97f), Edge, FMargin(28.f, 24.f), RadiusL, 2.f);
	Size->AddChild(Card);
	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	Card->SetContent(Col);

	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Head)->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));
	Head->AddChildToHorizontalBox(MakeToneIcon(WidgetTree, Spec.Tone, 20.f))->SetVerticalAlignment(VAlign_Center);
	UTextBlock* Title = MakeText(WidgetTree, 24, TextPrimary, true);
	Title->SetText(Spec.Title);
	if (UHorizontalBoxSlot* S = Head->AddChildToHorizontalBox(Title))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));
		S->SetVerticalAlignment(VAlign_Center);
	}

	UTextBlock* Body = MakeText(WidgetTree, FontBody + 1, TextMuted);
	Body->SetText(Spec.Body);
	Col->AddChildToVerticalBox(Body)->SetPadding(FMargin(0.f, 0.f, 0.f, 18.f));

	UButton* DetailsToggle = nullptr;
	if (!Spec.Detail.IsEmpty())
	{
		TObjectPtr<UTextBlock> Lbl;
		DetailsToggle = MakeTextLink(WidgetTree, Lbl, NSLOCTEXT("SharedWorld", "ModalDetails", "Details"), FontSmall + 1);
		DetailsToggle->OnClicked.AddDynamic(this, &USharedWorldModal::OnToggleDetails);
		Col->AddChildToVerticalBox(DetailsToggle)->SetHorizontalAlignment(HAlign_Left);
		DetailText = MakeText(WidgetTree, FontSmall, TextMuted);
		DetailText->SetText(Spec.Detail);
		DetailText->SetVisibility(ESlateVisibility::Collapsed);
		Col->AddChildToVerticalBox(DetailText)->SetPadding(FMargin(8.f, 6.f, 0.f, 12.f));
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	Col->AddChildToVerticalBox(Buttons)->SetHorizontalAlignment(HAlign_Right);
	const bool bTwo = !Spec.CancelLabel.IsEmpty();
	if (bTwo)
	{
		TObjectPtr<UTextBlock> Lbl;
		CancelButton = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, Lbl, Spec.CancelLabel, 17, FMargin(24.f, 10.f));
		CancelButton->OnClicked.AddDynamic(this, &USharedWorldModal::OnCancelClicked);
		Buttons->AddChildToHorizontalBox(CancelButton)->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
	}
	{
		TObjectPtr<UTextBlock> Lbl;
		ConfirmButton = MakeRoleButton(WidgetTree, Spec.ConfirmRole, Lbl,
			Spec.ConfirmLabel.IsEmpty() ? NSLOCTEXT("SharedWorld", "ModalOk", "OK") : Spec.ConfirmLabel, 17, FMargin(24.f, 10.f));
		ConfirmButton->OnClicked.AddDynamic(this, &USharedWorldModal::OnConfirmClicked);
		Buttons->AddChildToHorizontalBox(ConfirmButton);
	}

	// Focus trap: pad/keyboard navigation can only move between the dialog's own buttons.
	for (UButton* B : { CancelButton.Get(), ConfirmButton.Get(), DetailsToggle })
	{
		if (!B) continue;
		B->SetNavigationRuleBase(EUINavigation::Up, EUINavigationRule::Stop);
		B->SetNavigationRuleBase(EUINavigation::Down, EUINavigationRule::Stop);
		B->SetNavigationRuleBase(EUINavigation::Left, EUINavigationRule::Stop);
		B->SetNavigationRuleBase(EUINavigation::Right, EUINavigationRule::Stop);
		B->SetNavigationRuleBase(EUINavigation::Next, EUINavigationRule::Stop);
		B->SetNavigationRuleBase(EUINavigation::Previous, EUINavigationRule::Stop);
	}
	if (CancelButton && ConfirmButton)
	{
		CancelButton->SetNavigationRuleExplicit(EUINavigation::Right, ConfirmButton);
		ConfirmButton->SetNavigationRuleExplicit(EUINavigation::Left, CancelButton);
	}
	if (DetailsToggle)
	{
		DetailsToggle->SetNavigationRuleExplicit(EUINavigation::Down, ConfirmButton);
		ConfirmButton->SetNavigationRuleExplicit(EUINavigation::Up, DetailsToggle);
		if (CancelButton) CancelButton->SetNavigationRuleExplicit(EUINavigation::Up, DetailsToggle);
	}
}

void USharedWorldModal::Close()
{
	if (bClosing) return;
	bClosing = true;
	if (bRestoreGameInput)
	{
		if (APlayerController* PC = InputOwner.Get())
		{
			PC->SetShowMouseCursor(false);
			PC->SetInputMode(FInputModeGameOnly());
		}
		bRestoreGameInput = false;
	}
	RemoveFromParent();
}

FReply USharedWorldModal::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right)
	{
		// Back always cancels the dialog, never the screen underneath it.
		OnCancelClicked();
		return FReply::Handled();
	}
	// Swallow everything else so keys do not leak to the menu below.
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void USharedWorldModal::OnConfirmClicked()
{
	if (bClosing) return;
	TFunction<void()> Cb = Spec.OnConfirm;
	Close();
	if (Cb) Cb();
}

void USharedWorldModal::OnCancelClicked()
{
	if (bClosing) return;
	TFunction<void()> Cb = Spec.OnCancel;
	Close();
	if (Cb) Cb();
}

void USharedWorldModal::OnToggleDetails()
{
	if (!DetailText) return;
	DetailText->SetVisibility(DetailText->GetVisibility() == ESlateVisibility::Collapsed ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}
