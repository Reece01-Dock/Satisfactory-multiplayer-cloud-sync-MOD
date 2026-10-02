#pragma once
// Shared Worlds UI design system: tokens + component factories.
// Every Shared Worlds screen builds from this file; do not introduce one-off colours,
// radii, font sizes or button looks elsewhere. See docs/shared-worlds-ui-system.md.

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/EditableTextBox.h"
#include "Engine/Font.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/ProgressBar.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "UI/SharedWorldButton.h"
#include "UI/SharedWorldFgWidgets.h"

/** One meaning per colour, always paired with a distinct shape so state never relies on colour alone. */
enum class ESharedWorldTone : uint8
{
	Healthy,  // green  dot      : hosting, online, ready, synced, connected
	Working,  // blue   dotted ring: syncing, downloading, uploading, joining, preparing
	Warning,  // orange diamond  : migrating, waiting for host, needs recovery, update available
	Problem,  // red    square   : conflict, failed sync, host lost, auth failed
	Inactive, // grey   ring     : offline, nobody hosting, not linked
};

enum class ESharedWorldButtonRole : uint8
{
	Game,      // green : Play / Join / Start / Resume / Connect
	Config,    // orange: Create / Add / Invite / Confirm / Save / Link
	Secondary, // grey  : Details / Cancel / Back / Refresh / Close
	Danger,    // red   : Delete / Remove / Leave / Revoke / Reset / Disconnect
};

enum class ESharedWorldStep : uint8 { Pending, Active, Done };

namespace SharedWorldUi
{
	// ===================================================== tokens: colour
	inline const FLinearColor BgDeep(0.02f, 0.022f, 0.028f, 0.72f);
	inline const FLinearColor BgPanel(0.04f, 0.045f, 0.055f, 0.55f);
	inline const FLinearColor BgCard(0.055f, 0.06f, 0.075f, 0.35f);
	inline const FLinearColor Accent(0.98f, 0.52f, 0.12f, 1.0f);
	inline const FLinearColor AccentHover(1.0f, 0.64f, 0.26f, 1.0f);
	inline const FLinearColor AccentSelected(0.94f, 0.48f, 0.18f, 1.0f);
	inline const FLinearColor SecondaryBtn(0.14f, 0.145f, 0.16f, 0.85f);
	inline const FLinearColor SearchBg(0.08f, 0.085f, 0.095f, 0.75f);
	inline const FLinearColor TextPrimary(1.f, 1.f, 1.f, 1.0f);
	inline const FLinearColor TextOnAccent(0.08f, 0.08f, 0.08f, 1.f);
	inline const FLinearColor TextMuted(0.72f, 0.74f, 0.76f, 1.0f);
	inline const FLinearColor Ok(0.35f, 0.85f, 0.4f, 1.0f);
	inline const FLinearColor Warn(0.95f, 0.75f, 0.2f, 1.0f);
	inline const FLinearColor Err(1.0f, 0.38f, 0.32f, 1.0f);
	inline const FLinearColor Info(0.36f, 0.66f, 1.0f, 1.0f);
	inline const FLinearColor ToneIdle(0.58f, 0.60f, 0.63f, 1.0f);
	inline const FLinearColor MenuEntry(0.0f, 0.0f, 0.0f, 0.0f);
	inline const FLinearColor RowIdle(1.f, 1.f, 1.f, 0.0f);
	inline const FLinearColor RowHover(1.f, 1.f, 1.f, 0.06f);
	/** Join Game hovered row orange (~#E5924B). */
	inline const FLinearColor JoinRowOrange(0.90f, 0.57f, 0.29f, 1.0f);
	inline const FLinearColor JoinRowOrangePressed(0.82f, 0.48f, 0.20f, 1.0f);

	inline const FLinearColor PlayGreen(0.13f, 0.62f, 0.25f, 1.0f);
	inline const FLinearColor PlayGreenHover(0.18f, 0.74f, 0.32f, 1.0f);
	inline const FLinearColor PlayGreenPressed(0.10f, 0.50f, 0.20f, 1.0f);
	inline const FLinearColor GreyBtn(0.20f, 0.21f, 0.23f, 0.95f);
	inline const FLinearColor GreyBtnHover(0.30f, 0.31f, 0.34f, 1.0f);
	inline const FLinearColor GreyBtnPressed(0.15f, 0.16f, 0.18f, 1.0f);
	inline const FLinearColor DangerBg(0.30f, 0.07f, 0.08f, 0.75f);
	inline const FLinearColor DangerBgHover(0.45f, 0.10f, 0.11f, 0.9f);
	inline const FLinearColor DangerBgPressed(0.22f, 0.05f, 0.06f, 0.9f);
	inline const FLinearColor PanelFill(0.03f, 0.035f, 0.045f, 0.80f);
	inline const FLinearColor PanelEdge(1.f, 1.f, 1.f, 0.08f);
	inline const FLinearColor RowFill(0.07f, 0.075f, 0.085f, 0.70f);
	inline const FLinearColor RowFillFocus(0.16f, 0.12f, 0.08f, 0.85f);
	inline const FLinearColor ModalScrim(0.f, 0.f, 0.f, 0.62f);
	inline const FLinearColor Clear(0.f, 0.f, 0.f, 0.f);

	// ===================================================== tokens: spacing / radius / type
	inline constexpr float SpaceS = 6.f;
	inline constexpr float SpaceM = 12.f;
	inline constexpr float SpaceL = 20.f;
	inline constexpr float RadiusS = 3.f;
	inline constexpr float RadiusM = 5.f;
	inline constexpr float RadiusL = 8.f;
	inline constexpr int32 FontTitle = 32;
	inline constexpr int32 FontHeader = 24;
	inline constexpr int32 FontSection = 15;
	inline constexpr int32 FontBody = 15;
	inline constexpr int32 FontSmall = 13;

	// ===================================================== primitives
	/** Flat rounded rectangle brush, optionally outlined. */
	inline FSlateBrush RoundedBrush(const FLinearColor& Fill, float Radius = 4.f, const FLinearColor& Outline = FLinearColor(0.f, 0.f, 0.f, 0.f), float OutlineWidth = 0.f)
	{
		FSlateBrush B;
		B.DrawAs = ESlateBrushDrawType::RoundedBox;
		B.TintColor = FSlateColor(Fill);
		B.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		B.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		B.OutlineSettings.Color = FSlateColor(Outline);
		B.OutlineSettings.Width = OutlineWidth;
		return B;
	}

	/** Rounded translucent panel (the dark Satisfactory-style plates). */
	inline UBorder* MakePanel(UWidgetTree* Tree, const FLinearColor& Fill, const FLinearColor& Edge, const FMargin& Padding, float Radius = 6.f, float EdgeWidth = 1.f)
	{
		UBorder* P = Tree->ConstructWidget<UBorder>();
		P->SetBrush(RoundedBrush(Fill, Radius, Edge, EdgeWidth));
		P->SetPadding(Padding);
		return P;
	}

	inline UTextBlock* MakeText(UWidgetTree* Tree, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
		SharedWorldFg::ApplyMenuFont(T, Size, bBold);
		T->SetColorAndOpacity(FSlateColor(Color));
		T->SetAutoWrapText(true);
		return T;
	}

	/** Solid, rounded button with explicit hover/press colours (hover also reads as controller highlight on pad-driven focus). */
	inline void StyleSolidButton(UButton* B, const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Pressed,
		const FLinearColor& Disabled, const FMargin& Padding = FMargin(14.f, 6.f), float Radius = 3.f,
		const FLinearColor& Outline = FLinearColor(0.f, 0.f, 0.f, 0.f), float OutlineWidth = 0.f)
	{
		if (!B) return;
		FButtonStyle Style = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
		Style.SetNormal(RoundedBrush(Normal, Radius, Outline, OutlineWidth));
		Style.SetHovered(RoundedBrush(Hover, Radius, Outline, OutlineWidth));
		Style.SetPressed(RoundedBrush(Pressed, Radius, Outline, OutlineWidth));
		Style.SetDisabled(RoundedBrush(Disabled, Radius, Outline, OutlineWidth));
		Style.SetNormalPadding(Padding);
		Style.SetPressedPadding(Padding);
		B->SetStyle(Style);
		B->SetBackgroundColor(FLinearColor::White);
	}

	inline UButton* MakeSolidButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label,
		const FLinearColor& Normal, const FLinearColor& Hover, const FLinearColor& Pressed,
		int32 FontSize = 15, const FLinearColor& TextColor = FLinearColor(1.f, 1.f, 1.f, 1.f), const FMargin& Padding = FMargin(14.f, 6.f))
	{
		UButton* B = Tree->ConstructWidget<USharedWorldButton>();
		StyleSolidButton(B, Normal, Hover, Pressed, FLinearColor(0.2f, 0.2f, 0.2f, 0.45f), Padding);
		OutLabel = MakeText(Tree, FontSize, TextColor, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		OutLabel->SetJustification(ETextJustify::Center);
		B->AddChild(OutLabel);
		return B;
	}

	// ===================================================== button hierarchy
	/** The only place that decides what each button role looks like. */
	inline UButton* MakeRoleButton(UWidgetTree* Tree, ESharedWorldButtonRole Role, TObjectPtr<UTextBlock>& OutLabel, const FText& Label,
		int32 FontSize = 15, const FMargin& Padding = FMargin(16.f, 8.f))
	{
		switch (Role)
		{
		case ESharedWorldButtonRole::Game:
			return MakeSolidButton(Tree, OutLabel, Label, PlayGreen, PlayGreenHover, PlayGreenPressed, FontSize, TextPrimary, Padding);
		case ESharedWorldButtonRole::Config:
			return MakeSolidButton(Tree, OutLabel, Label, Accent, AccentHover, JoinRowOrangePressed, FontSize, TextOnAccent, Padding);
		case ESharedWorldButtonRole::Danger:
			return MakeSolidButton(Tree, OutLabel, Label, DangerBg, DangerBgHover, DangerBgPressed, FontSize, TextPrimary, Padding);
		case ESharedWorldButtonRole::Secondary:
		default:
			return MakeSolidButton(Tree, OutLabel, Label, GreyBtn, GreyBtnHover, GreyBtnPressed, FontSize, TextPrimary, Padding);
		}
	}

	/** Legacy names kept so older screens inherit the system look while their layouts are migrated. */
	inline UButton* MakePrimaryButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 16)
	{
		return MakeRoleButton(Tree, ESharedWorldButtonRole::Config, OutLabel, Label, FontSize);
	}

	inline UButton* MakeSecondaryButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		return MakeRoleButton(Tree, ESharedWorldButtonRole::Secondary, OutLabel, Label, FontSize);
	}

	/** Colour-keyed legacy entry point: Err -> Danger, Accent -> Config, anything else -> Secondary. */
	inline UButton* MakeButton(UWidgetTree* Tree, const FLinearColor& Color, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		ESharedWorldButtonRole Role = ESharedWorldButtonRole::Secondary;
		if (Color.R > 0.7f && Color.G > 0.3f && Color.G < 0.7f && Color.B < 0.3f) Role = ESharedWorldButtonRole::Config;
		else if (Color.R > 0.6f && Color.G < 0.5f && Color.B < 0.5f) Role = ESharedWorldButtonRole::Danger;
		return MakeRoleButton(Tree, Role, OutLabel, Label, FontSize);
	}

	inline UButton* MakeTextLink(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label, int32 FontSize = 14)
	{
		UButton* B = Tree->ConstructWidget<USharedWorldButton>();
		StyleSolidButton(B, Clear, FLinearColor(1.f, 1.f, 1.f, 0.08f), FLinearColor(1.f, 1.f, 1.f, 0.14f), Clear, FMargin(10.f, 6.f), RadiusS);
		OutLabel = MakeText(Tree, FontSize, TextPrimary, false);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		B->AddChild(OutLabel);
		return B;
	}

	/** (Re)style a tab: the active tab is outlined in Satisfactory orange. The label is the button's first child. */
	inline void StyleTabButton(UButton* B, bool bActive)
	{
		if (!B) return;
		const FLinearColor Fill = bActive ? FLinearColor(0.16f, 0.12f, 0.08f, 0.9f) : Clear;
		StyleSolidButton(B, Fill, FLinearColor(1.f, 1.f, 1.f, 0.10f), FLinearColor(1.f, 1.f, 1.f, 0.16f), Clear,
			FMargin(16.f, 8.f), RadiusM, bActive ? Accent : PanelEdge, bActive ? 2.f : 1.f);
		if (UTextBlock* T = Cast<UTextBlock>(B->GetChildAt(0)))
		{
			T->SetColorAndOpacity(FSlateColor(bActive ? TextPrimary : TextMuted));
		}
	}

	inline UButton* MakeTabButton(UWidgetTree* Tree, const FText& Label, bool bActive, int32 FontSize = 15)
	{
		UButton* B = Tree->ConstructWidget<USharedWorldButton>();
		UTextBlock* T = MakeText(Tree, FontSize, bActive ? TextPrimary : TextMuted, bActive);
		T->SetText(Label);
		T->SetAutoWrapText(false);
		T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		B->AddChild(T);
		StyleTabButton(B, bActive);
		return B;
	}

	// ===================================================== status system
	inline FLinearColor ToneColor(ESharedWorldTone Tone)
	{
		switch (Tone)
		{
		case ESharedWorldTone::Healthy: return Ok;
		case ESharedWorldTone::Working: return Info;
		case ESharedWorldTone::Warning: return Warn;
		case ESharedWorldTone::Problem: return Err;
		case ESharedWorldTone::Inactive:
		default: return ToneIdle;
		}
	}

	/** Status glyph drawn from primitives (no font/texture dependency); shape differs per tone. */
	inline UWidget* MakeToneIcon(UWidgetTree* Tree, ESharedWorldTone Tone, float Size = 12.f)
	{
		const FLinearColor C = ToneColor(Tone);
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetWidthOverride(Size);
		Box->SetHeightOverride(Size);
		UOverlay* O = Tree->ConstructWidget<UOverlay>();
		Box->AddChild(O);
		auto Centered = [&](UWidget* W, float W_, float H_)
		{
			USizeBox* S = Tree->ConstructWidget<USizeBox>();
			S->SetWidthOverride(W_);
			S->SetHeightOverride(H_);
			S->AddChild(W);
			if (UOverlaySlot* Slot = O->AddChildToOverlay(S))
			{
				Slot->SetHorizontalAlignment(HAlign_Center);
				Slot->SetVerticalAlignment(VAlign_Center);
			}
			return S;
		};
		auto Shape = [&](const FSlateBrush& Brush)
		{
			UBorder* B = Tree->ConstructWidget<UBorder>();
			B->SetBrush(Brush);
			return B;
		};
		switch (Tone)
		{
		case ESharedWorldTone::Healthy:
			Centered(Shape(RoundedBrush(C, Size * 0.5f)), Size, Size);
			break;
		case ESharedWorldTone::Inactive:
			Centered(Shape(RoundedBrush(Clear, Size * 0.5f, C, 2.f)), Size, Size);
			break;
		case ESharedWorldTone::Working:
			Centered(Shape(RoundedBrush(Clear, Size * 0.5f, C, 2.f)), Size, Size);
			Centered(Shape(RoundedBrush(C, Size * 0.2f)), Size * 0.4f, Size * 0.4f);
			break;
		case ESharedWorldTone::Warning:
		{
			USizeBox* Diamond = Centered(Shape(RoundedBrush(C, 1.f)), Size * 0.72f, Size * 0.72f);
			Diamond->SetRenderTransformAngle(45.f);
			break;
		}
		case ESharedWorldTone::Problem:
			Centered(Shape(RoundedBrush(C, 2.f)), Size * 0.86f, Size * 0.86f);
			break;
		}
		return Box;
	}

	/** Icon + label in the tone colour. The label is always present, so colour is never the only cue. */
	inline UHorizontalBox* MakeStatusBadge(UWidgetTree* Tree, ESharedWorldTone Tone, const FText& Label, int32 FontSize = 14, float IconSize = 12.f)
	{
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		H->AddChildToHorizontalBox(MakeToneIcon(Tree, Tone, IconSize))->SetVerticalAlignment(VAlign_Center);
		UTextBlock* L = MakeText(Tree, FontSize, ToneColor(Tone), false);
		L->SetText(Label);
		L->SetAutoWrapText(false);
		L->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		L->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(L))
		{
			S->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
			S->SetVerticalAlignment(VAlign_Center);
		}
		return H;
	}

	/** Legacy round dot, kept for call sites that only need a coloured bullet. */
	inline USizeBox* MakeStatusDot(UWidgetTree* Tree, const FLinearColor& Color, float Size = 10.f)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetWidthOverride(Size);
		Box->SetHeightOverride(Size);
		UBorder* Dot = Tree->ConstructWidget<UBorder>();
		Dot->SetBrush(RoundedBrush(Color, Size * 0.5f));
		Box->AddChild(Dot);
		return Box;
	}

	// ===================================================== layout atoms
	inline UTextBlock* MakeSectionHeader(UWidgetTree* Tree, const FText& Title)
	{
		UTextBlock* H = MakeText(Tree, FontSection, TextMuted, true);
		H->SetText(Title);
		return H;
	}

	inline UVerticalBoxSlot* AddFillRow(UVerticalBox* Col, UWidget* Row, float BottomPad = 0.f)
	{
		if (!Col || !Row) return nullptr;
		UVerticalBoxSlot* Slot = Col->AddChildToVerticalBox(Row);
		Slot->SetHorizontalAlignment(HAlign_Fill);
		Slot->SetPadding(FMargin(0.f, 0.f, 0.f, BottomPad));
		return Slot;
	}

	/** Label (+ optional description) on the left, control on the right. */
	inline UBorder* MakeSettingsRow(UWidgetTree* Tree, const FText& Label, const FText& Description, UWidget* Control)
	{
		UBorder* Row = MakePanel(Tree, RowFill, PanelEdge, FMargin(16.f, 12.f), RadiusM, 1.f);
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		Row->SetContent(H);
		UVerticalBox* Text = Tree->ConstructWidget<UVerticalBox>();
		UTextBlock* L = MakeText(Tree, FontBody, TextPrimary, true);
		L->SetText(Label);
		Text->AddChildToVerticalBox(L);
		if (!Description.IsEmpty())
		{
			UTextBlock* D = MakeText(Tree, FontSmall, TextMuted);
			D->SetText(Description);
			Text->AddChildToVerticalBox(D)->SetPadding(FMargin(0.f, 3.f, 0.f, 0.f));
		}
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}
		if (Control)
		{
			if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(Control))
			{
				S->SetVerticalAlignment(VAlign_Center);
				S->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
			}
		}
		return Row;
	}

	/** Title + sentence + optional action. */
	inline UBorder* MakeEmptyState(UWidgetTree* Tree, const FText& Title, const FText& Body, UButton*& OutAction,
		const FText& ActionLabel = FText::GetEmpty(), ESharedWorldButtonRole ActionRole = ESharedWorldButtonRole::Config)
	{
		UBorder* Box = MakePanel(Tree, RowFill, PanelEdge, FMargin(24.f, 26.f), RadiusM, 1.f);
		UVerticalBox* Col = Tree->ConstructWidget<UVerticalBox>();
		Box->SetContent(Col);
		UTextBlock* T = MakeText(Tree, 20, TextPrimary, true);
		T->SetText(Title);
		Col->AddChildToVerticalBox(T)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		UTextBlock* B = MakeText(Tree, FontBody, TextMuted);
		B->SetText(Body);
		Col->AddChildToVerticalBox(B)->SetPadding(FMargin(0.f, 0.f, 0.f, ActionLabel.IsEmpty() ? 0.f : 16.f));
		OutAction = nullptr;
		if (!ActionLabel.IsEmpty())
		{
			TObjectPtr<UTextBlock> Lbl;
			OutAction = MakeRoleButton(Tree, ActionRole, Lbl, ActionLabel, 17, FMargin(20.f, 12.f));
			Col->AddChildToVerticalBox(OutAction)->SetHorizontalAlignment(HAlign_Left);
		}
		return Box;
	}

	/** Inline banner for warnings/errors/info with an optional action button. */
	inline UBorder* MakeNoticePanel(UWidgetTree* Tree, ESharedWorldTone Tone, const FText& Title, const FText& Body,
		UButton*& OutAction, const FText& ActionLabel = FText::GetEmpty())
	{
		FLinearColor Fill = RowFill;
		if (Tone == ESharedWorldTone::Problem) Fill = FLinearColor(0.20f, 0.06f, 0.07f, 0.80f);
		else if (Tone == ESharedWorldTone::Warning) Fill = FLinearColor(0.20f, 0.14f, 0.05f, 0.80f);
		else if (Tone == ESharedWorldTone::Working) Fill = FLinearColor(0.05f, 0.11f, 0.20f, 0.80f);
		FLinearColor Edge = ToneColor(Tone);
		Edge.A = 0.55f;
		UBorder* Panel = MakePanel(Tree, Fill, Edge, FMargin(14.f, 10.f), RadiusM, 1.f);
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
		Panel->SetContent(Row);
		Row->AddChildToHorizontalBox(MakeToneIcon(Tree, Tone, 16.f))->SetVerticalAlignment(VAlign_Center);
		UVerticalBox* Txt = Tree->ConstructWidget<UVerticalBox>();
		UTextBlock* T = MakeText(Tree, FontBody, TextPrimary, true);
		T->SetText(Title);
		Txt->AddChildToVerticalBox(T);
		if (!Body.IsEmpty())
		{
			UTextBlock* Bd = MakeText(Tree, FontSmall, TextMuted);
			Bd->SetText(Body);
			Txt->AddChildToVerticalBox(Bd)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		}
		if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Txt))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(12.f, 0.f, 12.f, 0.f));
		}
		OutAction = nullptr;
		if (!ActionLabel.IsEmpty())
		{
			TObjectPtr<UTextBlock> Lbl;
			OutAction = MakeRoleButton(Tree, ESharedWorldButtonRole::Secondary, Lbl, ActionLabel, 15);
			Row->AddChildToHorizontalBox(OutAction)->SetVerticalAlignment(VAlign_Center);
		}
		return Panel;
	}

	/** One line of a step list: pending (ring) / active (blue) / done (green). */
	inline UHorizontalBox* MakeProgressRow(UWidgetTree* Tree, ESharedWorldStep Step, const FText& Label)
	{
		const ESharedWorldTone Tone = Step == ESharedWorldStep::Done ? ESharedWorldTone::Healthy
			: Step == ESharedWorldStep::Active ? ESharedWorldTone::Working : ESharedWorldTone::Inactive;
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		H->AddChildToHorizontalBox(MakeToneIcon(Tree, Tone, 14.f))->SetVerticalAlignment(VAlign_Center);
		UTextBlock* T = MakeText(Tree, FontBody, Step == ESharedWorldStep::Pending ? TextMuted : TextPrimary, Step == ESharedWorldStep::Active);
		T->SetText(Label);
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(T))
		{
			S->SetPadding(FMargin(12.f, 0.f, 0.f, 0.f));
			S->SetVerticalAlignment(VAlign_Center);
		}
		return H;
	}

	/** Flat text input meant to sit inside a MakePanel frame (the frame is the visible field). */
	inline void StyleTextField(UEditableTextBox* Box, int32 FontSize = 16)
	{
		if (!Box) return;
		FEditableTextBoxStyle Style = Box->WidgetStyle;
		Style.SetBackgroundImageNormal(RoundedBrush(Clear));
		Style.SetBackgroundImageHovered(RoundedBrush(Clear));
		Style.SetBackgroundImageFocused(RoundedBrush(Clear));
		Style.SetBackgroundImageReadOnly(RoundedBrush(Clear));
		Style.SetForegroundColor(FSlateColor(TextPrimary));
		Style.SetPadding(FMargin(10.f, 8.f));
		FSlateFontInfo FontInfo = Style.TextStyle.Font;
		if (UObject* FontObj = StaticLoadObject(UFont::StaticClass(), nullptr, SharedWorldFg::DescriptionFontPath))
		{
			FontInfo.FontObject = FontObj;
			FontInfo.Size = FontSize;
			FontInfo.TypefaceFontName = NAME_None;
			Style.TextStyle.SetFont(FontInfo);
		}
		Box->WidgetStyle = Style;
	}

	/** Horizontal wizard progress: done / current / upcoming steps joined by thin rules. */
	inline UHorizontalBox* MakeStepper(UWidgetTree* Tree, const TArray<FText>& Steps, int32 Current)
	{
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		for (int32 i = 0; i < Steps.Num(); ++i)
		{
			if (i > 0)
			{
				USizeBox* Rule = Tree->ConstructWidget<USizeBox>();
				Rule->SetWidthOverride(36.f);
				Rule->SetHeightOverride(2.f);
				UBorder* Line = Tree->ConstructWidget<UBorder>();
				Line->SetBrush(RoundedBrush(i <= Current ? Ok : PanelEdge, 1.f));
				Rule->AddChild(Line);
				H->AddChildToHorizontalBox(Rule)->SetPadding(FMargin(12.f, 0.f));
			}
			const ESharedWorldStep St = i < Current ? ESharedWorldStep::Done : (i == Current ? ESharedWorldStep::Active : ESharedWorldStep::Pending);
			H->AddChildToHorizontalBox(MakeProgressRow(Tree, St, Steps[i]))->SetVerticalAlignment(VAlign_Center);
		}
		return H;
	}

	/** Progress bar. Marquee when the backend has no real percentage (never a fake one). */
	inline USizeBox* MakeProgressBar(UWidgetTree* Tree, float Percent01, bool bMarquee, const FLinearColor& Fill = Info)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetHeightOverride(8.f);
		UProgressBar* Bar = Tree->ConstructWidget<UProgressBar>();
		FProgressBarStyle Style;
		Style.SetBackgroundImage(RoundedBrush(FLinearColor(1.f, 1.f, 1.f, 0.10f), 4.f));
		Style.SetFillImage(RoundedBrush(FLinearColor::White, 4.f));
		Style.SetMarqueeImage(RoundedBrush(FLinearColor::White, 4.f));
		Bar->SetWidgetStyle(Style);
		Bar->SetFillColorAndOpacity(Fill);
		Bar->SetIsMarquee(bMarquee);
		if (!bMarquee) Bar->SetPercent(FMath::Clamp(Percent01, 0.f, 1.f));
		Box->AddChild(Bar);
		return Box;
	}

	/** Circular fallback avatar with the player's initial (no platform-avatar fetch, never blocks the UI). */
	inline UWidget* MakePlayerAvatar(UWidgetTree* Tree, const FString& Name, float Size = 40.f)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetWidthOverride(Size);
		Box->SetHeightOverride(Size);
		const uint8 Hue = static_cast<uint8>(GetTypeHash(Name) % 256);
		UBorder* Disc = Tree->ConstructWidget<UBorder>();
		Disc->SetBrush(RoundedBrush(FLinearColor::MakeFromHSV8(Hue, 90, 110), Size * 0.5f));
		Disc->SetHorizontalAlignment(HAlign_Center);
		Disc->SetVerticalAlignment(VAlign_Center);
		UTextBlock* Initial = MakeText(Tree, static_cast<int32>(Size * 0.45f), TextPrimary, true);
		Initial->SetAutoWrapText(false);
		Initial->SetText(FText::FromString(Name.IsEmpty() ? TEXT("?") : Name.Left(1).ToUpper()));
		Disc->SetContent(Initial);
		Box->AddChild(Disc);
		return Box;
	}

	/** avatar | name + role | status badge (+ optional detail such as "Ping 22 ms"). */
	inline UBorder* MakePlayerRow(UWidgetTree* Tree, const FString& Name, const FString& Role, ESharedWorldTone Tone,
		const FText& StatusLabel, const FString& Detail = FString())
	{
		UBorder* Row = MakePanel(Tree, RowFill, PanelEdge, FMargin(12.f, 8.f), RadiusM, 1.f);
		UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
		Row->SetContent(H);
		H->AddChildToHorizontalBox(MakePlayerAvatar(Tree, Name, 40.f))->SetVerticalAlignment(VAlign_Center);
		UVerticalBox* Text = Tree->ConstructWidget<UVerticalBox>();
		UTextBlock* N = MakeText(Tree, FontBody, TextPrimary, true);
		N->SetText(FText::FromString(Name));
		N->SetAutoWrapText(false);
		N->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		N->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		Text->AddChildToVerticalBox(N);
		if (!Role.IsEmpty())
		{
			UTextBlock* R = MakeText(Tree, FontSmall, TextMuted);
			R->SetText(FText::FromString(Role));
			Text->AddChildToVerticalBox(R);
		}
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(Text))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(12.f, 0.f, 8.f, 0.f));
		}
		UVerticalBox* Right = Tree->ConstructWidget<UVerticalBox>();
		Right->AddChildToVerticalBox(MakeStatusBadge(Tree, Tone, StatusLabel, FontSmall + 1));
		if (!Detail.IsEmpty())
		{
			UTextBlock* D = MakeText(Tree, FontSmall, TextMuted);
			D->SetText(FText::FromString(Detail));
			D->SetJustification(ETextJustify::Right);
			Right->AddChildToVerticalBox(D);
		}
		if (UHorizontalBoxSlot* S = H->AddChildToHorizontalBox(Right))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetHorizontalAlignment(HAlign_Right);
		}
		return Row;
	}

	/** Globe drawn from primitives: ring, meridian, equator. No font/texture dependency. */
	inline UWidget* MakeGlobeIcon(UWidgetTree* Tree)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		Box->SetWidthOverride(38.f);
		Box->SetHeightOverride(38.f);
		UOverlay* O = Tree->ConstructWidget<UOverlay>();
		Box->AddChild(O);
		auto Add = [&](float W, float H, const FSlateBrush& Brush)
		{
			USizeBox* S = Tree->ConstructWidget<USizeBox>();
			S->SetWidthOverride(W);
			S->SetHeightOverride(H);
			UBorder* B = Tree->ConstructWidget<UBorder>();
			B->SetBrush(Brush);
			S->AddChild(B);
			if (UOverlaySlot* Slot = O->AddChildToOverlay(S))
			{
				Slot->SetHorizontalAlignment(HAlign_Center);
				Slot->SetVerticalAlignment(VAlign_Center);
			}
		};
		Add(36.f, 36.f, RoundedBrush(Clear, 18.f, TextPrimary, 2.f));
		Add(16.f, 36.f, RoundedBrush(Clear, 8.f, TextPrimary, 2.f));
		Add(36.f, 2.f, RoundedBrush(TextPrimary, 0.f));
		return Box;
	}

	inline UWidget* MakeListIcon(UWidgetTree* Tree, const FLinearColor& C)
	{
		UVerticalBox* V = Tree->ConstructWidget<UVerticalBox>();
		for (int32 i = 0; i < 3; ++i)
		{
			USizeBox* S = Tree->ConstructWidget<USizeBox>();
			S->SetWidthOverride(18.f);
			S->SetHeightOverride(3.f);
			UBorder* B = Tree->ConstructWidget<UBorder>();
			B->SetBrush(RoundedBrush(C, 1.f));
			S->AddChild(B);
			V->AddChildToVerticalBox(S)->SetPadding(FMargin(0.f, 2.f));
		}
		return V;
	}

	inline UWidget* MakeGridIcon(UWidgetTree* Tree, const FLinearColor& C)
	{
		UVerticalBox* V = Tree->ConstructWidget<UVerticalBox>();
		for (int32 r = 0; r < 2; ++r)
		{
			UHorizontalBox* H = Tree->ConstructWidget<UHorizontalBox>();
			for (int32 c = 0; c < 2; ++c)
			{
				USizeBox* S = Tree->ConstructWidget<USizeBox>();
				S->SetWidthOverride(9.f);
				S->SetHeightOverride(9.f);
				UBorder* B = Tree->ConstructWidget<UBorder>();
				B->SetBrush(RoundedBrush(C, 1.f));
				S->AddChild(B);
				H->AddChildToHorizontalBox(S)->SetPadding(FMargin(1.5f));
			}
			V->AddChildToVerticalBox(H);
		}
		return V;
	}

	inline UWidget* MakeSearchIcon(UWidgetTree* Tree)
	{
		USizeBox* S = Tree->ConstructWidget<USizeBox>();
		S->SetWidthOverride(15.f);
		S->SetHeightOverride(15.f);
		UBorder* B = Tree->ConstructWidget<UBorder>();
		B->SetBrush(RoundedBrush(Clear, 7.5f, TextMuted, 2.f));
		S->AddChild(B);
		return S;
	}

	/** Small rounded label with a tone icon: "Easy", "Connected", "Advanced". */
	inline UBorder* MakePill(UWidgetTree* Tree, ESharedWorldTone Tone, const FText& Label, int32 FontSize = 12)
	{
		FLinearColor Fill = ToneColor(Tone);
		Fill.A = 0.16f;
		FLinearColor Edge = ToneColor(Tone);
		Edge.A = 0.45f;
		UBorder* P = MakePanel(Tree, Fill, Edge, FMargin(9.f, 3.f), 11.f, 1.f);
		P->SetContent(MakeStatusBadge(Tree, Tone, Label, FontSize, static_cast<float>(FontSize) - 3.f));
		return P;
	}

	/** Filled, uppercase marker ("ACTIVE"). Visually distinct from the outlined status pills and from the orange selection border. */
	inline UBorder* MakeSolidBadge(UWidgetTree* Tree, ESharedWorldTone Tone, const FText& Label, int32 FontSize = 11)
	{
		UBorder* P = MakePanel(Tree, ToneColor(Tone), Clear, FMargin(8.f, 2.f), 4.f, 0.f);
		P->SetHorizontalAlignment(HAlign_Center);
		P->SetVerticalAlignment(VAlign_Center);
		UTextBlock* T = MakeText(Tree, FontSize, TextOnAccent, true);
		T->SetAutoWrapText(false);
		T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		T->SetText(Label);
		P->SetContent(T);
		return P;
	}

	/**
	 * Two-column detail row: fixed-width label column, flexible value column, so every row in a list lines up.
	 * Long identifiers ellipsize (bWrap=false) or wrap (bWrap=true); the value always carries a tooltip with the full
	 * text, and the label carries the explanation when one is given.
	 */
	inline UHorizontalBox* MakeDetailRow(UWidgetTree* Tree, const FText& Label, const FString& Value,
		const FLinearColor& ValueColor = TextPrimary, bool bWrap = false, const FText& LabelTooltip = FText::GetEmpty(),
		float LabelWidth = 176.f)
	{
		UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
		UTextBlock* L = MakeText(Tree, FontSmall + 1, TextMuted);
		L->SetText(Label);
		L->SetAutoWrapText(false);
		L->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		L->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		if (!LabelTooltip.IsEmpty()) L->SetToolTipText(LabelTooltip);
		USizeBox* LB = Tree->ConstructWidget<USizeBox>();
		LB->SetWidthOverride(LabelWidth);
		LB->AddChild(L);
		Row->AddChildToHorizontalBox(LB)->SetVerticalAlignment(VAlign_Top);
		UTextBlock* V = MakeText(Tree, FontSmall + 1, ValueColor);
		V->SetText(FText::FromString(Value));
		V->SetAutoWrapText(bWrap);
		if (!bWrap) V->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		V->SetToolTipText(FText::FromString(Value));
		if (UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(V))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Top);
			S->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
		}
		return Row;
	}

	/** Thin divider line. */
	inline USizeBox* MakeRule(UWidgetTree* Tree, bool bVertical = false)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		if (bVertical) Box->SetWidthOverride(1.f); else Box->SetHeightOverride(1.f);
		UBorder* Line = Tree->ConstructWidget<UBorder>();
		Line->SetBrush(RoundedBrush(PanelEdge, 0.f));
		Box->AddChild(Line);
		return Box;
	}

	// ===================================================== legacy: Join-Game style rows (Satisfactory-native menu rows)
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
		OutButton = Tree->ConstructWidget<USharedWorldButton>();
		StyleAsJoinRow(OutButton);
		OutLabel = MakeText(Tree, FontSize, TextPrimary, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
		OutLabel->SetJustification(ETextJustify::Left);
		OutButton->AddChild(OutLabel);
		Box->AddChild(OutButton);
		return Box;
	}

	inline UButton* MakeMenuListButton(UWidgetTree* Tree, TObjectPtr<UTextBlock>& OutLabel, const FText& Label)
	{
		UButton* B = Tree->ConstructWidget<USharedWorldButton>();
		B->SetBackgroundColor(MenuEntry);
		OutLabel = MakeText(Tree, 22, TextPrimary, true);
		OutLabel->SetText(Label);
		OutLabel->SetAutoWrapText(false);
		OutLabel->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis); // never spill out of its box
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
