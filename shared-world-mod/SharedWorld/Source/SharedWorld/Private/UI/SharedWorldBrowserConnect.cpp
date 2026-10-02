// Settings > Storage > Connect an rclone-backed provider.
//
// The form is generated from rclone's own description of the provider (FRcloneProviders): which settings exist, which are
// required, which are secret, which are advanced and what the valid choices are. Nothing here is specific to one provider,
// so every backend rclone ships can be connected, and new ones appear without UI work.

#include "UI/SharedWorldBrowserWidget.h"

#include "Async/Async.h"
#include "Blueprint/WidgetTree.h"
#include "Components/EditableTextBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/ScrollBox.h"
#include "HAL/PlatformProcess.h"
#include "Rclone/RcloneProviders.h"
#include "Rclone/RcloneRuntime.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldModal.h"
#include "UI/SharedWorldRowBinder.h"
#include "UI/SharedWorldUiStyle.h"

using namespace SharedWorldUi;

namespace
{
	const TCHAR* const FolderKey = TEXT("__folder");
	const TCHAR* const DefaultFolder = TEXT("SharedWorlds");

	const FRcloneBackend* FindBackend(const FString& Type)
	{
		const TSharedPtr<const TArray<FRcloneBackend>> All = FRcloneProviders::Get();
		if (!All.IsValid()) return nullptr;
		for (const FRcloneBackend& B : *All)
		{
			if (B.Name == Type) return &B;
		}
		return nullptr;
	}

	/** "access_key_id" -> "Access key id". */
	FString Prettify(const FString& Name)
	{
		FString Out = Name.Replace(TEXT("_"), TEXT(" "));
		if (Out.Len() > 0) Out[0] = FChar::ToUpper(Out[0]);
		return Out;
	}

	/** First paragraph of rclone's help text, trimmed to something that fits under a field. */
	FString ShortHelp(const FString& Help)
	{
		FString H = Help;
		int32 Cut = INDEX_NONE;
		if (H.FindChar(TEXT('\n'), Cut)) H.LeftInline(Cut);
		H.TrimStartAndEndInline();
		if (H.Len() > 200) H = H.Left(197).TrimEnd() + TEXT("...");
		return H;
	}

	bool NeedsBucket(const FString& Type)
	{
		return Type == TEXT("s3") || Type == TEXT("b2") || Type == TEXT("azureblob") || Type == TEXT("google cloud storage")
			|| Type == TEXT("oracleobjectstorage") || Type == TEXT("qingstor") || Type == TEXT("swift");
	}

	/** Settings the player must see for this provider. */
	bool IsOptionShown(const FRcloneBackend& B, const FRcloneOption& O, const FSharedWorldStorageProvider& P, bool bAdvanced)
	{
		if (O.bHidden) return false;
		if (O.Name == TEXT("provider") && !P.RclonePreset.IsEmpty()) return false; // fixed by the card (e.g. Cloudflare)
		if (!O.AppliesTo(P.RclonePreset)) return false;
		if (B.Name == TEXT("drive") && (O.Name == TEXT("scope") || O.Name == TEXT("service_account_file") || O.Name == TEXT("service_account_credentials"))) return false;
		// Browser sign-in providers use rclone's own app keys; letting the player supply their own is advanced
		// (except Google Drive, where the shared key is being retired).
		const bool bOwnKeyRequired = B.Name == TEXT("drive") && (O.Name == TEXT("client_id") || O.Name == TEXT("client_secret"));
		if (bOwnKeyRequired) return true;
		const bool bAdvancedByNature = O.bAdvanced || (B.bOAuth && (O.Name == TEXT("client_id") || O.Name == TEXT("client_secret")));
		return bAdvanced || !bAdvancedByNature;
	}

	bool IsOptionRequired(const FRcloneBackend& B, const FRcloneOption& O)
	{
		if (B.Name == TEXT("drive") && (O.Name == TEXT("client_id") || O.Name == TEXT("client_secret"))) return true;
		return O.bRequired && O.Default.IsEmpty();
	}

	FRcloneConnection* FindConnection(TArray<FRcloneConnection>& Conns, const FString& CatalogId)
	{
		for (FRcloneConnection& C : Conns)
		{
			if (C.CatalogId == CatalogId) return &C;
		}
		return nullptr;
	}
}

// ============================================================================ entry points

void USharedWorldBrowserWidget::BeginConnect(const FString& ProviderId)
{
	if (ConnectProviderId != ProviderId)
	{
		ConnectValues.Reset();
		bConnectAdvanced = false;
	}
	ConnectProviderId = ProviderId;
	ConnectError.Reset();
	ConnectStatus.Reset();
	bConnectBusy = false;
	Page = EPage::ConnectProvider;
	ListScroll = nullptr;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::HarvestConnectFields()
{
	for (const TPair<FString, TObjectPtr<UEditableTextBox>>& It : ConnectBoxes)
	{
		if (It.Value) ConnectValues.Add(It.Key, It.Value->GetText().ToString());
	}
}

void USharedWorldBrowserWidget::SetConnectOption(const FString& Name, const FString& Value)
{
	HarvestConnectFields();
	if (Name.IsEmpty()) bConnectAdvanced = !bConnectAdvanced;
	else ConnectValues.Add(Name, Value);
	ConnectError.Reset();
	ListScroll = nullptr;
	ScheduleRebuild();
}

void USharedWorldBrowserWidget::OnConnectLearnMore()
{
	const FString Type = [&]() -> FString
	{
		if (USharedWorldSubsystem* S = SW())
		{
			if (const FSharedWorldStorageProvider* P = FSharedWorldStorageCatalog::Build(*S).Find(ConnectProviderId)) return P->RcloneType;
		}
		return FString();
	}();
	FPlatformProcess::LaunchURL(Type == TEXT("drive")
		? TEXT("https://rclone.org/drive/#making-your-own-client-id")
		: *FString::Printf(TEXT("https://rclone.org/%s/"), *Type.Replace(TEXT(" "), TEXT(""))), nullptr, nullptr);
}

// ============================================================================ the form

void USharedWorldBrowserWidget::RebuildConnectPage()
{
	ConnectBoxes.Reset();
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
	const FSharedWorldStorageProvider* P = Catalog.Find(ConnectProviderId);
	const FRcloneBackend* B = P ? FindBackend(P->RcloneType) : nullptr;
	if (!P || !B)
	{
		AddPageHeader(NSLOCTEXT("SharedWorld", "ConnectTitleFallback", "Connect storage"), FText::GetEmpty());
		UButton* Unused = nullptr;
		PageBody->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
			NSLOCTEXT("SharedWorld", "ConnectNoEngine", "This provider can't be set up right now"),
			NSLOCTEXT("SharedWorld", "ConnectNoEngineBody", "The storage engine didn't load. Go back and open Diagnostics to see why."), Unused));
		return;
	}

	AddPageHeader(FText::Format(NSLOCTEXT("SharedWorld", "ConnectTitle", "Connect {0}"), FText::FromString(P->DisplayName)),
		B->bOAuth
			? NSLOCTEXT("SharedWorld", "ConnectSubOAuth", "Your browser opens so you can approve access. Shared Worlds never sees your password.")
			: NSLOCTEXT("SharedWorld", "ConnectSubKeys", "Enter the details for this storage. They stay on this PC."));

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	PageBody->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Col = WidgetTree->ConstructWidget<UVerticalBox>();
	Scroll->AddChild(Col);

	UButton* Guide = nullptr;
	if (B->Name == TEXT("drive"))
	{
		Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Warning,
			NSLOCTEXT("SharedWorld", "DriveOwnKey", "Google Drive needs your own Google client ID"),
			NSLOCTEXT("SharedWorld", "DriveOwnKeyBody",
				"Google is retiring the shared sign-in that rclone ships with. Create a free client ID (about five minutes), then paste it below. "
				"To share a world with friends, share a Drive folder with them and put its ID under advanced settings."),
			Guide, NSLOCTEXT("SharedWorld", "OpenGuide", "Open the guide")))->SetPadding(FMargin(0.f, 0.f, 0.f, 16.f));
		if (Guide) Guide->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnConnectLearnMore);
	}

	auto Label = [&](const FString& Text, bool bRequired, const FString& Help)
	{
		UTextBlock* L = MakeText(WidgetTree, FontBody, TextPrimary, true);
		L->SetText(FText::FromString(bRequired ? Text + TEXT("  *") : Text));
		L->SetAutoWrapText(false);
		L->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
		L->SetToolTipText(FText::FromString(Text));
		Col->AddChildToVerticalBox(L)->SetPadding(FMargin(0.f, 0.f, 0.f, Help.IsEmpty() ? 6.f : 2.f));
		if (!Help.IsEmpty())
		{
			UTextBlock* H = MakeText(WidgetTree, FontSmall, TextMuted);
			H->SetText(FText::FromString(Help));
			Col->AddChildToVerticalBox(H)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		}
	};
	// Choice buttons sit in a fixed-column grid: every cell has a bounded width, so long values end in "..." instead of
	// widening the page. The full value and rclone's explanation are on the tooltip.
	auto NewChipGrid = [&]()
	{
		UUniformGridPanel* G = WidgetTree->ConstructWidget<UUniformGridPanel>();
		G->SetSlotPadding(FMargin(0.f, 0.f, 8.f, 8.f));
		Col->AddChildToVerticalBox(G)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		return G;
	};
	auto Chip = [&](UUniformGridPanel* Row, int32 Index, const FString& OptionName, const FString& Value, const FString& Text, bool bActive, const FString& Tip)
	{
		UButton* Btn = MakeTabButton(WidgetTree, FText::FromString(Text), bActive);
		USharedWorldRowBinder* Binder = NewObject<USharedWorldRowBinder>(this);
		Binder->Browser = this;
		Binder->TabKind = 4;
		Binder->ConnectOption = OptionName;
		Binder->ConnectValue = Value;
		RowBinders.Add(Binder);
		Btn->OnClicked.AddDynamic(Binder, &USharedWorldRowBinder::OnClicked);
		Btn->SetToolTipText(FText::FromString(Tip.IsEmpty() ? Text : Text + TEXT("\n") + Tip));
		if (UUniformGridSlot* Cell = Row->AddChildToUniformGrid(Btn, Index / 4, Index % 4))
		{
			Cell->SetHorizontalAlignment(HAlign_Fill);
		}
	};

	bool bAnyAdvanced = false;
	for (const FRcloneOption& O : B->Options)
	{
		const bool bBaseVisible = IsOptionShown(*B, O, *P, false);
		const bool bAdvVisible = IsOptionShown(*B, O, *P, true);
		bAnyAdvanced |= (bAdvVisible && !bBaseVisible);
		if (!(bConnectAdvanced ? bAdvVisible : bBaseVisible)) continue;

		const bool bReq = IsOptionRequired(*B, O);
		Label(Prettify(O.Name), bReq, ShortHelp(O.Help));

		const FString* Typed = ConnectValues.Find(O.Name);
		if (O.IsBool())
		{
			const FString Current = Typed ? *Typed : O.Default;
			UUniformGridPanel* Row = NewChipGrid();
			Chip(Row, 0, O.Name, TEXT("true"), TEXT("Yes"), Current == TEXT("true"), FString());
			Chip(Row, 1, O.Name, TEXT("false"), TEXT("No"), Current != TEXT("true"), FString());
		}
		else if (O.bExclusive && O.Examples.Num() > 0 && O.Examples.Num() <= 8)
		{
			const FString Current = Typed ? *Typed : O.Default;
			UUniformGridPanel* Row = NewChipGrid();
			int32 Index = 0;
			for (const FRcloneOptionExample& E : O.Examples)
			{
				const FString Shown = E.Value.IsEmpty() ? FString(TEXT("Default")) : E.Value;
				Chip(Row, Index++, O.Name, E.Value, Shown, E.Value == Current, ShortHelp(E.Help));
			}
		}
		else
		{
			FString Hint = O.Default;
			if (Hint.IsEmpty() && O.Examples.Num() > 0) Hint = O.Examples[0].Value;
			UEditableTextBox* Box = AddTextField(Col, FText::FromString(Hint), Typed ? *Typed : FString());
			if (O.bPassword) Box->SetIsPassword(true);
			ConnectBoxes.Add(O.Name, Box);
		}
	}

	// ---- where inside the storage the worlds live
	{
		Label(TEXT("Folder for Shared Worlds"), false,
			NeedsBucket(B->Name) ? FString(TEXT("Start with your bucket name, for example my-bucket/SharedWorlds."))
				: FString(TEXT("Created if it doesn't exist. Only Shared Worlds files go in here.")));
		const FString* Typed = ConnectValues.Find(FolderKey);
		UEditableTextBox* Box = AddTextField(Col, FText::FromString(NeedsBucket(B->Name) ? TEXT("my-bucket/SharedWorlds") : DefaultFolder), Typed ? *Typed : FString());
		ConnectBoxes.Add(FolderKey, Box);
	}

	if (bAnyAdvanced)
	{
		Chip(NewChipGrid(), 0, FString(), FString(), bConnectAdvanced ? TEXT("Hide advanced settings") : TEXT("Show advanced settings"), bConnectAdvanced, FString());
	}

	UButton* Unused = nullptr;
	if (!ConnectError.IsEmpty())
	{
		Col->AddChildToVerticalBox(MakeNoticePanel(WidgetTree, ESharedWorldTone::Problem,
			NSLOCTEXT("SharedWorld", "ConnectFailed", "Couldn't connect"), FText::FromString(ConnectError), Unused))
			->SetPadding(FMargin(0.f, 0.f, 0.f, 14.f));
	}
	if (bConnectBusy)
	{
		Col->AddChildToVerticalBox(MakeProgressBar(WidgetTree, 0.f, true))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		UTextBlock* St = MakeText(WidgetTree, FontSmall + 1, TextMuted);
		St->SetText(FText::FromString(ConnectStatus));
		Col->AddChildToVerticalBox(St)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	}

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>();
	if (UVerticalBoxSlot* BS = Col->AddChildToVerticalBox(Buttons))
	{
		BS->SetPadding(FMargin(0.f, 6.f, 0.f, 8.f));
		BS->SetHorizontalAlignment(HAlign_Right);
	}
	TObjectPtr<UTextBlock> L1, L2;
	UButton* Cancel = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Secondary, L1,
		bConnectBusy ? NSLOCTEXT("SharedWorld", "StopWaiting", "Stop waiting") : NSLOCTEXT("SharedWorld", "Cancel", "Cancel"), 16, FMargin(22.f, 10.f));
	Cancel->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnBack);
	Buttons->AddChildToHorizontalBox(Cancel)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	UButton* Go = MakeRoleButton(WidgetTree, ESharedWorldButtonRole::Config, L2, NSLOCTEXT("SharedWorld", "ConnectGo", "Connect"), 16, FMargin(26.f, 10.f));
	Go->SetIsEnabled(!bConnectBusy);
	Go->OnClicked.AddDynamic(this, &USharedWorldBrowserWidget::OnConnectSubmit);
	Buttons->AddChildToHorizontalBox(Go);
}

// ============================================================================ connect

void USharedWorldBrowserWidget::OnConnectSubmit() { RunConnect(); }

void USharedWorldBrowserWidget::RunConnect()
{
	if (bConnectBusy) return;
	HarvestConnectFields();
	USharedWorldSubsystem* S = SW();
	if (!S) return;
	const FSharedWorldStorageCatalog Catalog = FSharedWorldStorageCatalog::Build(*S);
	const FSharedWorldStorageProvider* P = Catalog.Find(ConnectProviderId);
	const FRcloneBackend* B = P ? FindBackend(P->RcloneType) : nullptr;
	if (!P || !B) return;

	// ---- validate what the player typed before touching the engine
	TMap<FString, FString> Params;
	TArray<FString> Missing;
	for (const FRcloneOption& O : B->Options)
	{
		if (!IsOptionShown(*B, O, *P, true)) continue;
		const FString* V = ConnectValues.Find(O.Name);
		const FString Value = V ? V->TrimStartAndEnd() : FString();
		if (Value.IsEmpty())
		{
			if (IsOptionRequired(*B, O)) Missing.Add(Prettify(O.Name));
			continue;
		}
		Params.Add(O.Name, Value);
	}
	if (Missing.Num() > 0)
	{
		ConnectError = FString::Printf(TEXT("Fill in: %s."), *FString::Join(Missing, TEXT(", ")));
		ListScroll = nullptr;
		ScheduleRebuild();
		return;
	}
	if (!P->RclonePreset.IsEmpty() && B->FindOption(TEXT("provider"))) Params.Add(TEXT("provider"), P->RclonePreset);
	if (B->Name == TEXT("drive")) Params.Add(TEXT("scope"), TEXT("drive"));

	FString Folder = ConnectValues.FindRef(FolderKey).TrimStartAndEnd();
	if (Folder.IsEmpty()) Folder = NeedsBucket(B->Name) ? FString() : FString(DefaultFolder);
	if (NeedsBucket(B->Name) && Folder.IsEmpty())
	{
		ConnectError = TEXT("Enter the bucket and folder to use, for example my-bucket/SharedWorlds.");
		ListScroll = nullptr;
		ScheduleRebuild();
		return;
	}

	const TArray<FRcloneConnection> Existing = FRcloneConnections::Load();
	FRcloneConnection Conn;
	Conn.RemoteName = FRcloneConnections::MakeRemoteName(B->Name, Existing);
	Conn.CatalogId = P->ProviderId;
	Conn.BackendType = B->Name;
	Conn.Label = P->DisplayName;
	Conn.Folder = Folder;
	Conn.CreatedUtc = FDateTime::UtcNow();

	bConnectBusy = true;
	ConnectError.Reset();
	ConnectStatus = B->bOAuth ? TEXT("Waiting for you to approve access in your browser...") : TEXT("Connecting and testing...");
	const int32 Attempt = ++ConnectAttempt;
	ListScroll = nullptr;
	ScheduleRebuild();

	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FRcloneRuntime::RunDetached([Weak, Attempt, Conn, Params, BackendName = B->Name]() mutable
	{
		FString Error;
		bool bOk = false;
		FRcloneResult Created = RcloneActions::CreateRemote(Conn.RemoteName, BackendName, Params);
		if (!Created.bOk)
		{
			Error = Created.ErrorText();
		}
		else
		{
			FString ProbeError;
			if (RcloneActions::ProbeStorage(Conn.Fs(), ProbeError))
			{
				bOk = true;
				Conn.bVerified = true;
				Conn.VerifiedUtc = FDateTime::UtcNow();
			}
			else
			{
				Error = ProbeError;
			}
			if (!bOk) (void)RcloneActions::DeleteRemote(Conn.RemoteName); // never leave a half-working connection behind
		}
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/rclone] event=connect type=%s ok=%d"), *BackendName, bOk ? 1 : 0);
		if (IsEngineExitRequested())
		{
			// The game is closing: nobody can save this connection, so undo it here rather than leave it orphaned.
			if (bOk) (void)RcloneActions::DeleteRemote(Conn.RemoteName);
			return;
		}

		FRcloneRuntime::PostToGameThread([Weak, Attempt, Conn, bOk, Error]()
		{
			USharedWorldBrowserWidget* Self = Weak.Get();
			if (!Self || Self->ConnectAttempt != Attempt)
			{
				// The player stopped waiting (or the menu closed) before this finished: undo it.
				if (bOk) (void)RcloneActions::DeleteRemote(Conn.RemoteName);
				return;
			}
			Self->bConnectBusy = false;
			if (bOk)
			{
				TArray<FRcloneConnection> All = FRcloneConnections::Load();
				All.Add(Conn);
				FRcloneConnections::Save(All);
				Self->ConnectValues.Reset();
				Self->PendingFlash = FString::Printf(TEXT("%s is connected and passed a read/write test."), *Conn.Label);
				Self->bPendingFlashOk = true;
				Self->SelectedProviderId = Conn.CatalogId;
				Self->StorageNotice.Reset();
				Self->Page = EPage::Settings;
				Self->SettingsTab = 0;
			}
			else
			{
				FString Shown = Error;
				if (Shown.Contains(TEXT("address already in use"), ESearchCase::IgnoreCase) || Shown.Contains(TEXT("bind"), ESearchCase::IgnoreCase))
				{
					Shown = TEXT("A previous sign-in is still waiting. Finish it in your browser, or restart the game, then try again.");
				}
				Self->ConnectError = Shown.IsEmpty() ? FString(TEXT("The storage didn't respond.")) : Shown.Left(400);
			}
			Self->ListScroll = nullptr;
			Self->ScheduleRebuild();
		});
	});
}

// ============================================================================ test / disconnect a connected provider

void USharedWorldBrowserWidget::OnStorageRcloneTest()
{
	if (bRcloneTestRunning) return;
	TArray<FRcloneConnection> Conns = FRcloneConnections::Load();
	const FRcloneConnection* Conn = FindConnection(Conns, SelectedProviderId);
	if (!Conn) return;
	bRcloneTestRunning = true;
	StorageNotice = TEXT("Testing the connection...");
	QueueStorageRefresh();

	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	const FRcloneConnection Copy = *Conn;
	FRcloneRuntime::RunDetached([Weak, Copy]()
	{
		FString Error;
		const bool bOk = RcloneActions::ProbeStorage(Copy.Fs(), Error);
		const FRcloneAbout About = bOk ? RcloneActions::About(Copy.Fs()) : FRcloneAbout();
		FRcloneRuntime::PostToGameThread([Weak, Copy, bOk, Error, About]()
		{
			USharedWorldBrowserWidget* Self = Weak.Get();
			if (!Self) return;
			Self->bRcloneTestRunning = false;
			TArray<FRcloneConnection> All = FRcloneConnections::Load();
			if (FRcloneConnection* C = FindConnection(All, Copy.CatalogId))
			{
				C->bVerified = bOk;
				if (bOk) C->VerifiedUtc = FDateTime::UtcNow();
				FRcloneConnections::Save(All);
			}
			Self->StorageNotice = bOk ? TEXT("Connection test passed.") : (TEXT("Connection test failed: ") + Error.Left(300));
			Self->QueueStorageRefresh();
		});
	});
}

void USharedWorldBrowserWidget::OnStorageRcloneDisconnect()
{
	TArray<FRcloneConnection> Conns = FRcloneConnections::Load();
	const FRcloneConnection* Conn = FindConnection(Conns, SelectedProviderId);
	if (!Conn) return;
	const FRcloneConnection Copy = *Conn;
	TWeakObjectPtr<USharedWorldBrowserWidget> Weak(this);
	FSharedWorldModalSpec Spec;
	Spec.Tone = ESharedWorldTone::Warning;
	Spec.Title = FText::Format(NSLOCTEXT("SharedWorld", "DisconnectProviderTitle", "Disconnect {0}?"), FText::FromString(Copy.Label));
	Spec.Body = NSLOCTEXT("SharedWorld", "DisconnectProviderBody",
		"Shared Worlds forgets this connection on this PC. Files already stored there are not deleted.");
	Spec.ConfirmLabel = NSLOCTEXT("SharedWorld", "DisconnectConfirm", "Disconnect");
	Spec.CancelLabel = NSLOCTEXT("SharedWorld", "Cancel", "Cancel");
	Spec.ConfirmRole = ESharedWorldButtonRole::Danger;
	Spec.OnConfirm = [Weak, Copy]()
	{
		USharedWorldBrowserWidget* Self = Weak.Get();
		(void)RcloneActions::DeleteRemote(Copy.RemoteName);
		TArray<FRcloneConnection> All = FRcloneConnections::Load();
		All.RemoveAll([&](const FRcloneConnection& C) { return C.RemoteName == Copy.RemoteName; });
		FRcloneConnections::Save(All);
		if (Self)
		{
			Self->StorageNotice.Reset();
			Self->QueueStorageRefresh();
		}
	};
	USharedWorldModal::Show(GetOwningPlayer(), Spec);
}
