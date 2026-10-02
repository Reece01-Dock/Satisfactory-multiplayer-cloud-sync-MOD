#pragma once
// Migration / crash-recovery / cloud-save HUD. Saving sits bottom-left;
// host migration is a centered card (not a full-screen takeover).

#include "Blueprint/UserWidget.h"
#include "UI/SharedWorldUiStyle.h"
#include "SharedWorldMigrationOverlay.generated.h"

class UTextBlock;
class UProgressBar;
class UBorder;
class UCanvasPanel;
class USizeBox;

UCLASS()
class SHAREDWORLD_API USharedWorldMigrationOverlay : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	void ShowMigration(const FText& Headline, const FText& Detail);
	void ShowRecovery(const FText& Headline, const FText& Detail);
	/** In-world / leave save: compact bottom-left toast. */
	void ShowUploading(const FText& Headline, const FText& Detail);
	void ShowConnected(const FText& HostName);
	void HideOverlay();

	/**
	 * Determinate host-migration progress.
	 * Percent01 in [0,1]; StepLabel is the current stage (e.g. "Waiting for host lock · 18s").
	 * Pass bIndeterminate=true for upload-style pulse when exact progress is unknown.
	 */
	void SetMigrationProgress(float Percent01, const FText& StepLabel, bool bIndeterminate = false);

private:
	enum class ELayoutMode : uint8 { None, CornerSave, CenterMigration };
	void SetProgressVisible(bool bVisible);
	void ApplyLayout(ELayoutMode Mode);
	/** Card edge, headline colour, bar colour and icon all come from one tone. */
	void ApplyTone(ESharedWorldTone Tone);

	UPROPERTY() TObjectPtr<UCanvasPanel> RootCanvas;
	UPROPERTY() TObjectPtr<UBorder> DimBackdrop;
	UPROPERTY() TObjectPtr<USizeBox> CardSize;
	UPROPERTY() TObjectPtr<UBorder> CardBorder;
	UPROPERTY() TObjectPtr<UBorder> ToneIconHolder;
	UPROPERTY() TObjectPtr<UTextBlock> HeadlineText;
	UPROPERTY() TObjectPtr<UTextBlock> DetailText;
	UPROPERTY() TObjectPtr<UTextBlock> StepText;
	UPROPERTY() TObjectPtr<UTextBlock> ProgressLabelText;
	UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
	UPROPERTY() TObjectPtr<UBorder> ProgressTrack;
	float ConnectedHideAt = 0.f;
	bool bIndeterminateProgress = false;
	float ProgressPulse = 0.f;
	float DisplayPercent = 0.f;
	float TargetPercent = 0.f;
	ELayoutMode LayoutMode = ELayoutMode::None;
};
