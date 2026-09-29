#pragma once
// Visual tokens matched to Satisfactory main-menu surfaces (Load / Mods / Join Game).

#include "Blueprint/UserWidget.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "UI/SharedWorldFgWidgets.h"

namespace SharedWorldUi
{
	inline const FLinearColor BgDeep(0.02f, 0.022f, 0.028f, 0.72f);
	inline const FLinearColor BgPanel(0.04f, 0.045f, 0.055f, 0.55f);
	inline const FLinearColor BgCard(0.055f, 0.06f, 0.075f, 0.35f);
	inline const FLinearColor Accent(0.98f, 0.52f, 0.12f, 1.0f);
	inline const FLinearColor AccentSelected(0.94f, 0.48f, 0.18f, 1.0f);
	inline const FLinearColor SecondaryBtn(0.14f, 0.145f, 0.16f, 0.85f);
	inline const FLinearColor SearchBg(0.08f, 0.085f, 0.095f, 0.75f);
	inline const FLinearColor TextPrimary(1.f, 1.f, 1.f, 1.0f);
	inline const FLinearColor TextOnAccent(0.08f, 0.08f, 0.08f, 1.f);
	inline const FLinearColor TextMuted(0.72f, 0.74f, 0.76f, 1.0f);
	inline const FLinearColor Ok(0.35f, 0.85f, 0.4f, 1.0f);
	inline const FLinearColor Warn(0.95f, 0.75f, 0.2f, 1.0f);
	inline const FLinearColor Err(1.0f, 0.38f, 0.32f, 1.0f);
	inline const FLinearColor MenuEntry(0.0f, 0.0f, 0.0f, 0.0f);
	inline const FLinearColor RowIdle(1.f, 1.f, 1.f, 0.0f);
	inline const FLinearColor RowHover(1.f, 1.f, 1.f, 0.06f);
	/** Join Game hovered row orange (~#E5924B). */
	inline const FLinearColor JoinRowOrange(0.90f, 0.57f, 0.29f, 1.0f);
	inline const FLinearColor JoinRowOrangePressed(0.82f, 0.48f, 0.20f, 1.0f);

	inline UTextBlock* MakeText(UWidgetTree* Tree, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
		SharedWorldFg::ApplyMenuFont(T, Size, bBold);
		T->SetColorAndOpacity(FSlateColor(Color));
		T->SetAutoWrapText(true);
		return T;
	}

	/** Match Join Game: transparent idle, full-width orange bar on hover. */
	inline void StyleAsJoinRow(UButton* B)
	{
		if (!B) return;
		FButtonStyle Style = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
		const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
		auto Solid = [&](const FLinearColor& C) -> FSlateBrush
		{
			FSlateBrush Brush = White ? *White : Style.Normal;
			Brush.TintColor = FSlateColor(C);
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Brush.Margin = FMargin(0.f);
			return Brush;
		};
		Style.SetNormal(Solid(FLinearColor(1.f, 1.f, 1.f, 0.f)));
		Style.SetHovered(Solid(JoinRowOrange));
		Style.SetPressed(Solid(JoinRowOrangePressed));
		Style.SetDisabled(Solid(FLinearColor(0.25f, 0.25f, 0.25f, 0.4f)));
		Style.SetNormalPadding(FMargin(18.f, 0.f, 12.f, 0.f));
		Style.SetPressedPadding(FMargin(18.f, 0.f, 12.f, 0.f));
		B->SetStyle(Style);
		B->SetBackgroundColor(FLinearColor::White);
	}

	/** SizeBox + Join-style button. Add the returned SizeBox to the panel. */
	inline USizeBox* MakeJoinMenuRowBox(UWidgetTree* Tree, UButton*& OutButton, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, float Height = 48.f, int32 FontSize = 18)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetMinDesiredHeight(Height);
		Box->SetHeightOverride(Height);
		OutButton = Tree->ConstructWidget<UButton>();
		StyleAsJoinRow(OutButton);
		OutLabel = MakeText(Tree, FontSize, TextPrimary, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetJustification(ETextJustify::Left);
		OutButton->AddChild(OutLabel);
		Box->AddChild(OutButton);
		return Box;
	}

	inline UVerticalBoxSlot* AddFillRow(UVerticalBox* Col, UWidget* Row, float BottomPad = 0.f)
	{
		if (!Col || !Row) return nullptr;
		UVerticalBoxSlot* Slot = Col->AddChildToVerticalBox(Row);
		Slot->SetHorizontalAlignment(HAlign_Fill);
		Slot->SetPadding(FMargin(0.f, 0.f, 0.f, BottomPad));
		return Slot;
	}

	inline UButton* MakePrimaryButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 16)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(Accent);
		OutLabel = MakeText(Tree, FontSize, TextOnAccent, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetJustification(ETextJustify::Center);
		B->AddChild(OutLabel);
		return B;
	}

	inline UButton* MakeSecondaryButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(SecondaryBtn);
		OutLabel = MakeText(Tree, FontSize, TextPrimary, false);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetJustification(ETextJustify::Center);
		B->AddChild(OutLabel);
		return B;
	}

	inline UButton* MakeTextLink(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.f));
		OutLabel = MakeText(Tree, FontSize, TextPrimary, false);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		B->AddChild(OutLabel);
		return B;
	}

	inline UButton* MakeButton(UWidgetTree* Tree, const FLinearColor& Color, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(Color);
		const bool bOnAccent = Color.R > 0.7f && Color.G > 0.3f && Color.G < 0.7f;
		OutLabel = MakeText(Tree, FontSize, bOnAccent ? TextOnAccent : TextPrimary, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		B->AddChild(OutLabel);
		return B;
	}

	inline UButton* MakeMenuListButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label)
	{
		UButton* B = Tree->ConstructWidget<UButton>();
		B->SetBackgroundColor(MenuEntry);
		OutLabel = MakeText(Tree, 22, TextPrimary, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		B->AddChild(OutLabel);
		return B;
	}

	inline FString StatusBadge(const FString& CloudStatus, const FString& LocalState)
	{
		if (LocalState == TEXT("MIGRATING") || CloudStatus == TEXT("MIGRATING")) return TEXT("MIGRATING");
		if (LocalState == TEXT("RECONNECTING") || LocalState == TEXT("RECOVERING")) return TEXT("RECOVERING");
		if (LocalState == TEXT("DOWNLOADING") || LocalState == TEXT("UPLOADING") || LocalState == TEXT("CHECKING")) return TEXT("SYNCING");
		if (LocalState == TEXT("JOIN_READY") || LocalState == TEXT("JOINED") || LocalState == TEXT("WAITING_FOR_HOST")) return TEXT("JOINING");
		if (LocalState == TEXT("READY_TO_HOST") || LocalState == TEXT("ACQUIRING") || LocalState == TEXT("HOSTING")) return TEXT("STARTING");
		if (LocalState == TEXT("ERROR")) return TEXT("ERROR");
		if (CloudStatus == TEXT("ONLINE") || CloudStatus == TEXT("SAVING")) return TEXT("ACTIVE");
		if (CloudStatus == TEXT("STARTING")) return TEXT("STARTING");
		if (CloudStatus == TEXT("UNREACHABLE")) return TEXT("OFFLINE");
		if (CloudStatus == TEXT("RECOVERABLE")) return TEXT("RECOVERING");
		return TEXT("AVAILABLE");
	}

	inline FLinearColor StatusColor(const FString& Badge)
	{
		if (Badge == TEXT("ACTIVE") || Badge == TEXT("AVAILABLE")) return Ok;
		if (Badge == TEXT("MIGRATING") || Badge == TEXT("RECOVERING") || Badge == TEXT("SYNCING") || Badge == TEXT("STARTING") || Badge == TEXT("JOINING")) return Warn;
		if (Badge == TEXT("ERROR") || Badge == TEXT("OFFLINE")) return Err;
		return TextMuted;
	}
}
