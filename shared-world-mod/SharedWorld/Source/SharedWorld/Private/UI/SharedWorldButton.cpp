#include "UI/SharedWorldButton.h"

#include "Input/Reply.h"
#include "Layout/WidgetPath.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

namespace
{
	/**
	 * Wraps the real SButton. It is itself focusable only to forward focus to the button, so
	 * UWidget::SetKeyboardFocus() (which targets the outermost widget) still lands on the button.
	 */
	class SSharedWorldFocusRing : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SSharedWorldFocusRing) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, TSharedRef<SWidget> InInner)
		{
			Inner = InInner;
			RingBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
			RingBrush.TintColor = FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.f)); // outline only, never a fill
			RingBrush.OutlineSettings.CornerRadii = FVector4(4.f, 4.f, 4.f, 4.f);
			RingBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
			RingBrush.OutlineSettings.Color = FSlateColor(FLinearColor(1.f, 0.82f, 0.55f, 1.f));
			RingBrush.OutlineSettings.Width = 2.f;
			ChildSlot[InInner];
		}

		virtual bool SupportsKeyboardFocus() const override { return true; }

		virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override
		{
			if (TSharedPtr<SWidget> Target = Inner.Pin())
			{
				return FReply::Handled().SetUserFocus(Target.ToSharedRef(), InFocusEvent.GetCause());
			}
			return FReply::Unhandled();
		}

		virtual void OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent) override
		{
			const bool bNow = NewWidgetPath.ContainsWidget(this);
			if (bNow != bFocusWithin)
			{
				bFocusWithin = bNow;
				Invalidate(EInvalidateWidgetReason::Paint);
			}
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const int32 Layer = SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
			if (bFocusWithin)
			{
				FSlateDrawElement::MakeBox(OutDrawElements, Layer + 1, AllottedGeometry.ToPaintGeometry(), &RingBrush, ESlateDrawEffect::None, FLinearColor::White);
				return Layer + 1;
			}
			return Layer;
		}

	private:
		TWeakPtr<SWidget> Inner;
		FSlateBrush RingBrush;
		bool bFocusWithin = false;
	};
}

TSharedRef<SWidget> USharedWorldButton::RebuildWidget()
{
	// Super builds MyButton and its content slot; we only wrap it, so every UButton property keeps working.
	TSharedRef<SWidget> Button = Super::RebuildWidget();
	return SNew(SSharedWorldFocusRing, Button);
}
