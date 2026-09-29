#include "UI/SharedWorldMigrationOverlay.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

TSharedRef<SWidget> USharedWorldMigrationOverlay::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = RootCanvas;
		RootCanvas->SetVisibility(ESlateVisibility::HitTestInvisible);

		// Soft dim behind the centered migration card only (toggled in ApplyLayout).
		DimBackdrop = WidgetTree->ConstructWidget<UBorder>();
		DimBackdrop->SetBrushColor(FLinearColor(0.01f, 0.012f, 0.018f, 0.55f));
		DimBackdrop->SetVisibility(ESlateVisibility::Collapsed);
		if (UCanvasPanelSlot* DimSlot = RootCanvas->AddChildToCanvas(DimBackdrop))
		{
			DimSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			DimSlot->SetOffsets(FMargin(0.f));
		}

		CardSize = WidgetTree->ConstructWidget<USizeBox>();
		CardBorder = WidgetTree->ConstructWidget<UBorder>();
		CardBorder->SetBrushColor(FLinearColor(0.04f, 0.045f, 0.055f, 0.92f));
		CardBorder->SetPadding(FMargin(20.f, 16.f));
		CardSize->AddChild(CardBorder);

		UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
		CardBorder->SetContent(Col);

		HeadlineText = MakeText(WidgetTree, 22, Accent, true);
		Col->AddChildToVerticalBox(HeadlineText)->SetPadding(FMargin(0, 0, 0, 6));
		DetailText = MakeText(WidgetTree, 14, TextPrimary);
		Col->AddChildToVerticalBox(DetailText)->SetPadding(FMargin(0, 0, 0, 8));

		ProgressLabelText = MakeText(WidgetTree, 12, TextMuted);
		Col->AddChildToVerticalBox(ProgressLabelText)->SetPadding(FMargin(0, 0, 0, 6));

		ProgressTrack = WidgetTree->ConstructWidget<UBorder>();
		ProgressTrack->SetBrushColor(FLinearColor(0.08f, 0.09f, 0.11f, 0.95f));
		ProgressTrack->SetPadding(FMargin(0.f));
		ProgressTrack->SetVisibility(ESlateVisibility::Collapsed);
		Col->AddChildToVerticalBox(ProgressTrack)->SetPadding(FMargin(0, 0, 0, 8));
		USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>();
		BarBox->SetHeightOverride(10.f);
		BarBox->SetWidthOverride(280.f);
		ProgressTrack->SetContent(BarBox);
		ProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
		ProgressBar->SetPercent(0.f);
		ProgressBar->SetFillColorAndOpacity(Accent);
		BarBox->AddChild(ProgressBar);

		StepText = MakeText(WidgetTree, 12, TextMuted);
		Col->AddChildToVerticalBox(StepText);

		if (UCanvasPanelSlot* CardSlot = RootCanvas->AddChildToCanvas(CardSize))
		{
			CardSlot->SetAutoSize(true);
			CardSlot->SetZOrder(1);
		}
		ApplyLayout(ELayoutMode::CenterMigration);
	}
	return Super::RebuildWidget();
}

void USharedWorldMigrationOverlay::ApplyLayout(ELayoutMode Mode)
{
	LayoutMode = Mode;
	if (!CardSize) return;
	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(CardSize->Slot);
	if (!CanvasSlot) return;

	if (Mode == ELayoutMode::CornerSave)
	{
		// Bottom-left toast, above the health / status cluster.
		if (DimBackdrop) DimBackdrop->SetVisibility(ESlateVisibility::Collapsed);
		CardSize->ClearWidthOverride();
		CardSize->SetWidthOverride(320.f);
		if (CardBorder)
		{
			CardBorder->SetBrushColor(FLinearColor(0.03f, 0.035f, 0.045f, 0.88f));
			CardBorder->SetPadding(FMargin(14.f, 10.f));
		}
		if (HeadlineText) SharedWorldFg::ApplyMenuFont(HeadlineText, 16, true);
		if (DetailText) SharedWorldFg::ApplyMenuFont(DetailText, 12, false);
		if (StepText) SharedWorldFg::ApplyMenuFont(StepText, 11, false);
		if (ProgressLabelText) SharedWorldFg::ApplyMenuFont(ProgressLabelText, 11, false);
		if (USizeBox* BarBox = ProgressTrack ? Cast<USizeBox>(ProgressTrack->GetContent()) : nullptr)
		{
			BarBox->SetWidthOverride(260.f);
			BarBox->SetHeightOverride(8.f);
		}
		CanvasSlot->SetAnchors(FAnchors(0.f, 1.f, 0.f, 1.f));
		CanvasSlot->SetAlignment(FVector2D(0.f, 1.f));
		CanvasSlot->SetPosition(FVector2D(28.f, -110.f));
		CanvasSlot->SetAutoSize(true);
	}
	else // CenterMigration
	{
		if (DimBackdrop) DimBackdrop->SetVisibility(ESlateVisibility::HitTestInvisible);
		CardSize->ClearWidthOverride();
		CardSize->SetWidthOverride(520.f);
		if (CardBorder)
		{
			CardBorder->SetBrushColor(FLinearColor(0.04f, 0.045f, 0.055f, 0.94f));
			CardBorder->SetPadding(FMargin(28.f, 22.f));
		}
		if (HeadlineText) SharedWorldFg::ApplyMenuFont(HeadlineText, 26, true);
		if (DetailText) SharedWorldFg::ApplyMenuFont(DetailText, 15, false);
		if (StepText) SharedWorldFg::ApplyMenuFont(StepText, 13, false);
		if (ProgressLabelText) SharedWorldFg::ApplyMenuFont(ProgressLabelText, 13, false);
		if (USizeBox* BarBox = ProgressTrack ? Cast<USizeBox>(ProgressTrack->GetContent()) : nullptr)
		{
			BarBox->SetWidthOverride(440.f);
			BarBox->SetHeightOverride(14.f);
		}
		CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
		CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		CanvasSlot->SetPosition(FVector2D(0.f, 0.f));
		CanvasSlot->SetAutoSize(true);
	}
}

void USharedWorldMigrationOverlay::SetProgressVisible(bool bVisible)
{
	if (ProgressTrack)
	{
		ProgressTrack->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (ProgressLabelText)
	{
		ProgressLabelText->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void USharedWorldMigrationOverlay::SetMigrationProgress(float Percent01, const FText& StepLabel, bool bIndeterminate)
{
	bIndeterminateProgress = bIndeterminate;
	TargetPercent = FMath::Clamp(Percent01, 0.f, 1.f);
	if (bIndeterminate)
	{
		DisplayPercent = TargetPercent;
	}
	else if (DisplayPercent < TargetPercent)
	{
		DisplayPercent = FMath::Max(DisplayPercent, TargetPercent - 0.08f);
	}
	SetProgressVisible(true);
	if (ProgressBar && !bIndeterminate)
	{
		ProgressBar->SetPercent(DisplayPercent);
	}
	if (ProgressLabelText)
	{
		if (bIndeterminate)
		{
			ProgressLabelText->SetText(StepLabel);
		}
		else
		{
			const int32 Pct = FMath::RoundToInt(TargetPercent * 100.f);
			ProgressLabelText->SetText(FText::Format(
				NSLOCTEXT("SharedWorld", "MigProgressFmt", "{0}%  ·  {1}"),
				FText::AsNumber(Pct),
				StepLabel));
		}
	}
}

void USharedWorldMigrationOverlay::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (ConnectedHideAt > 0.f)
	{
		ConnectedHideAt -= InDeltaTime;
		if (ConnectedHideAt <= 0.f) HideOverlay();
	}
	if (!ProgressBar) return;

	if (bIndeterminateProgress)
	{
		ProgressPulse += InDeltaTime;
		const float Wave = 0.18f + 0.62f * (0.5f + 0.5f * FMath::Sin(ProgressPulse * 2.4f));
		ProgressBar->SetPercent(Wave);
		return;
	}

	if (DisplayPercent < TargetPercent)
	{
		DisplayPercent = FMath::FInterpTo(DisplayPercent, TargetPercent, InDeltaTime, 4.5f);
		ProgressBar->SetPercent(DisplayPercent);
	}
}

void USharedWorldMigrationOverlay::ShowMigration(const FText& Headline, const FText& Detail)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ConnectedHideAt = 0.f;
	bIndeterminateProgress = false;
	ApplyLayout(ELayoutMode::CenterMigration);
	SetProgressVisible(true);
	if (HeadlineText) HeadlineText->SetText(Headline);
	if (DetailText) DetailText->SetText(Detail);
	if (StepText) StepText->SetText(NSLOCTEXT("SharedWorld", "MigHint", "Host migration in progress — do not close the game."));
}

void USharedWorldMigrationOverlay::ShowRecovery(const FText& Headline, const FText& Detail)
{
	ShowMigration(Headline, Detail);
	if (StepText) StepText->SetText(NSLOCTEXT("SharedWorld", "RecHint", "Finding a new host and bringing the Shared World back online…"));
}

void USharedWorldMigrationOverlay::ShowUploading(const FText& Headline, const FText& Detail)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ConnectedHideAt = 0.f;
	ApplyLayout(ELayoutMode::CornerSave);
	SetMigrationProgress(0.35f, NSLOCTEXT("SharedWorld", "UploadStepShort", "Uploading…"), true);
	// Compact corner copy — keep the HUD readable.
	if (HeadlineText)
	{
		FString Short = Headline.ToString();
		if (Short.StartsWith(TEXT("UPLOADING"))) Short = TEXT("SAVING");
		else if (Short.StartsWith(TEXT("RELEASING"))) Short = TEXT("RELEASING");
		HeadlineText->SetText(FText::FromString(Short));
	}
	if (DetailText)
	{
		FString D = Detail.ToString();
		if (D.IsEmpty()) D = TEXT("Syncing Shared World…");
		DetailText->SetText(FText::FromString(D));
	}
	if (StepText) StepText->SetText(FText::GetEmpty());
}

void USharedWorldMigrationOverlay::ShowConnected(const FText& HostName)
{
	SetVisibility(ESlateVisibility::HitTestInvisible);
	ApplyLayout(ELayoutMode::CenterMigration);
	SetMigrationProgress(1.f, NSLOCTEXT("SharedWorld", "MigDone", "Complete"), false);
	if (HeadlineText) HeadlineText->SetText(NSLOCTEXT("SharedWorld", "Connected", "CONNECTED"));
	if (DetailText) DetailText->SetText(FText::Format(NSLOCTEXT("SharedWorld", "NowHosting", "{0} is now hosting."), HostName));
	if (StepText) StepText->SetText(FText::GetEmpty());
	ConnectedHideAt = 2.5f;
}

void USharedWorldMigrationOverlay::HideOverlay()
{
	ConnectedHideAt = 0.f;
	bIndeterminateProgress = false;
	DisplayPercent = 0.f;
	TargetPercent = 0.f;
	LayoutMode = ELayoutMode::None;
	SetVisibility(ESlateVisibility::Collapsed);
	RemoveFromParent();
}
